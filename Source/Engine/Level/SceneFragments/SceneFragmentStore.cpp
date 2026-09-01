// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneFragmentStore.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/JsonWriters.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    constexpr const Char* IndexFileName = TEXT("scene-fragments.index");
    constexpr const Char* FragmentExtension = TEXT(".sceneactor");
    constexpr uint32 TransactionJournalMagic = 0x4A465353; // SSFJ
    constexpr uint32 TransactionJournalVersion = 1;
    CriticalSection TransactionLocker;

    enum class TransactionPhase : byte
    {
        Staging,
        Prepared,
        Committed,
    };

    enum class TransactionEntryKind : byte
    {
        Replace,
        Delete,
    };

    struct TransactionEntry
    {
        TransactionEntryKind Kind = TransactionEntryKind::Replace;
        String Destination;
        bool Existed = false;
        Array<byte> Data;
    };

    struct TransactionJournal
    {
        Guid TransactionId;
        TransactionPhase Phase = TransactionPhase::Staging;
        bool RemoveStore = false;
        String StorePath;
        Array<TransactionEntry> Entries;
    };

    class JournalWriter
    {
    public:
        Array<byte> Data;

        void Byte(byte value)
        {
            Data.Add(value);
        }

        void UInt32(uint32 value)
        {
            for (int32 i = 0; i < 4; i++)
                Data.Add(static_cast<byte>(value >> (i * 8)));
        }

        void GuidValue(const Guid& value)
        {
            UInt32(value.A);
            UInt32(value.B);
            UInt32(value.C);
            UInt32(value.D);
        }

        void StringValue(const StringView& value)
        {
            const StringAnsi utf8(value);
            UInt32(utf8.Length());
            if (utf8.HasChars())
                Data.Add(reinterpret_cast<const byte*>(utf8.Get()), utf8.Length());
        }
    };

    class JournalReader
    {
        const byte* _data;
        uint32 _length;
        uint32 _position = 0;

    public:
        JournalReader(const byte* data, uint32 length)
            : _data(data)
            , _length(length)
        {
        }

        bool Bytes(void* output, uint32 length)
        {
            if (_position > _length || length > _length - _position)
                return true;
            Platform::MemoryCopy(output, _data + _position, length);
            _position += length;
            return false;
        }

        bool Byte(byte& value)
        {
            return Bytes(&value, 1);
        }

        bool UInt32(uint32& value)
        {
            byte bytes[4];
            if (Bytes(bytes, 4))
                return true;
            value = static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
                    (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
            return false;
        }

        bool GuidValue(Guid& value)
        {
            return UInt32(value.A) || UInt32(value.B) || UInt32(value.C) || UInt32(value.D);
        }

        bool StringValue(String& value)
        {
            uint32 length;
            if (UInt32(length) || length > 32768 || length > _length - _position)
                return true;
            value = String(StringAnsiView(reinterpret_cast<const char*>(_data + _position), length));
            _position += length;
            return false;
        }

        bool AtEnd() const
        {
            return _position == _length;
        }
    };

    bool Fail(String& error, const StringView& message)
    {
        error = message;
        return true;
    }

    bool CompareEntries(const SceneFragmentIndexEntry& a, const SceneFragmentIndexEntry& b)
    {
        return a.RootActorLocalId < b.RootActorLocalId;
    }

    bool ComparePreparedFragments(const PreparedSceneFragment& a, const PreparedSceneFragment& b)
    {
        return a.RelativePhysicalPath < b.RelativePhysicalPath;
    }

    bool SameEntries(const Array<SceneFragmentIndexEntry>& a, const Array<SceneFragmentIndexEntry>& b)
    {
        if (a.Count() != b.Count())
            return false;
        for (int32 i = 0; i < a.Count(); i++)
        {
            if (a[i].RootActorLocalId != b[i].RootActorLocalId ||
                a[i].RelativePhysicalPath != b[i].RelativePhysicalPath ||
                a[i].Content != b[i].Content ||
                a[i].Size != b[i].Size ||
                a[i].SerializerVersion != b[i].SerializerVersion)
            {
                return false;
            }
        }
        return true;
    }

    String GetTransactionsRoot()
    {
        return Globals::ProjectLibraryFolder / TEXT("SceneFragments/Transactions");
    }

    String GetTransactionDirectory(const Guid& transactionId)
    {
        return GetTransactionsRoot() / transactionId.ToString(Guid::FormatType::N).ToLower();
    }

    String GetStagePath(const TransactionJournal& journal, int32 index)
    {
        return journal.Entries[index].Destination + TEXT(".ssf-") +
               journal.TransactionId.ToString(Guid::FormatType::N).ToLower() + TEXT(".tmp");
    }

    String GetBackupPath(const StringView& transactionDirectory, int32 index)
    {
        return String(transactionDirectory) / String::Format(TEXT("{0}.backup"), index);
    }

    bool EnsureDirectory(const StringView& path)
    {
        return path.HasChars() && !FileSystem::DirectoryExists(path) && FileSystem::CreateDirectory(path);
    }

    bool FlushFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#elif PLATFORM_LINUX || PLATFORM_MAC
        const StringAnsi value(path);
        const int handle = open(value.Get(), O_RDONLY);
        if (handle == -1)
            return true;
        const bool failed = fsync(handle) != 0;
        close(handle);
        return failed;
#else
        return false;
#endif
    }

    bool WriteDurable(const StringView& path, const void* data, int32 length)
    {
        return EnsureDirectory(StringUtils::GetDirectoryName(path)) || File::WriteAllBytes(path, data, length) || FlushFile(path);
    }

    bool SameBytes(const StringView& path, const void* data, int32 length)
    {
        BytesContainer existing;
        return !File::ReadAllBytes(path, existing) && existing.Length() == length &&
               (length == 0 || Platform::MemoryCompare(existing.Get(), data, length) == 0);
    }

    bool IsAllowedDestination(const StringView& path)
    {
        String normalized(path);
        FileSystem::NormalizePath(normalized);
        return AssetPathPolicy::IsSameOrChild(normalized, Globals::ProjectContentFolder) ||
               AssetPathPolicy::IsSameOrChild(normalized, SceneFragmentStore::GetRootPath());
    }

    bool SaveJournal(const StringView& directory, const TransactionJournal& journal, String& error)
    {
        if (EnsureDirectory(directory))
            return Fail(error, TEXT("Cannot create the scene save transaction directory."));
        JournalWriter writer;
        writer.UInt32(TransactionJournalMagic);
        writer.UInt32(TransactionJournalVersion);
        writer.Byte(static_cast<byte>(journal.Phase));
        writer.Byte(journal.RemoveStore ? 1 : 0);
        writer.GuidValue(journal.TransactionId);
        writer.StringValue(journal.StorePath);
        writer.UInt32(journal.Entries.Count());
        for (const TransactionEntry& entry : journal.Entries)
        {
            writer.Byte(static_cast<byte>(entry.Kind));
            writer.Byte(entry.Existed ? 1 : 0);
            writer.StringValue(entry.Destination);
        }
        const String path = String(directory) / TEXT("journal.bin");
        const String staging = path + TEXT(".tmp");
        if (WriteDurable(staging, writer.Data.Get(), writer.Data.Count()) ||
            FileSystem::MoveFile(path, staging, true))
        {
            FileSystem::DeleteFile(staging);
            return Fail(error, TEXT("Cannot durably persist the scene save transaction journal."));
        }
        return false;
    }

    bool LoadJournal(const StringView& directory, TransactionJournal& journal, String& error)
    {
        BytesContainer bytes;
        const String path = String(directory) / TEXT("journal.bin");
        if (File::ReadAllBytes(path, bytes) || bytes.Length() > MAX_uint32)
            return Fail(error, TEXT("Scene save transaction journal is missing or unreadable."));
        JournalReader reader(bytes.Get(), static_cast<uint32>(bytes.Length()));
        uint32 magic;
        uint32 version;
        uint32 count;
        byte phase;
        byte removeStore;
        if (reader.UInt32(magic) || reader.UInt32(version) || reader.Byte(phase) || reader.Byte(removeStore) ||
            reader.GuidValue(journal.TransactionId) || reader.StringValue(journal.StorePath) || reader.UInt32(count) ||
            magic != TransactionJournalMagic || version != TransactionJournalVersion ||
            phase > static_cast<byte>(TransactionPhase::Committed) || removeStore > 1 || count > 100000)
        {
            return Fail(error, TEXT("Scene save transaction journal is malformed or unsupported."));
        }
        journal.Phase = static_cast<TransactionPhase>(phase);
        journal.RemoveStore = removeStore != 0;
        String normalizedStore(journal.StorePath);
        FileSystem::NormalizePath(normalizedStore);
        const String fragmentRoot = SceneFragmentStore::GetRootPath();
        if (!journal.TransactionId.IsValid() || normalizedStore != journal.StorePath ||
            StringUtils::GetDirectoryName(normalizedStore).Compare(StringView(fragmentRoot), StringSearchCase::IgnoreCase) != 0)
        {
            return Fail(error, TEXT("Scene save transaction journal contains invalid ownership."));
        }
        for (uint32 i = 0; i < count; i++)
        {
            byte kind;
            byte existed;
            TransactionEntry entry;
            if (reader.Byte(kind) || reader.Byte(existed) || reader.StringValue(entry.Destination) ||
                kind > static_cast<byte>(TransactionEntryKind::Delete) || existed > 1 ||
                !IsAllowedDestination(entry.Destination))
            {
                return Fail(error, TEXT("Scene save transaction journal contains an invalid entry."));
            }
            entry.Kind = static_cast<TransactionEntryKind>(kind);
            entry.Existed = existed != 0;
            journal.Entries.Add(MoveTemp(entry));
        }
        if (!reader.AtEnd())
            return Fail(error, TEXT("Scene save transaction journal has trailing data."));
        return false;
    }

    bool CleanupTransaction(const StringView& directory, const TransactionJournal& journal, bool committed)
    {
        bool failed = false;
        for (int32 i = 0; i < journal.Entries.Count(); i++)
        {
            const String staging = GetStagePath(journal, i);
            if (FileSystem::FileExists(staging))
                failed |= FileSystem::DeleteFile(staging);
        }
        for (int32 i = journal.Entries.Count() - 1; i >= 0; i--)
        {
            if (!journal.Entries[i].Existed &&
                AssetPathPolicy::IsSameOrChild(journal.Entries[i].Destination, journal.StorePath))
                FileSystem::DeleteDirectory(StringUtils::GetDirectoryName(journal.Entries[i].Destination), false);
        }
        if (committed && journal.RemoveStore && FileSystem::DirectoryExists(journal.StorePath))
            failed |= FileSystem::DeleteDirectory(journal.StorePath, true);
        else if (!committed && FileSystem::DirectoryExists(journal.StorePath))
            FileSystem::DeleteDirectory(journal.StorePath, false);
        if (!failed && FileSystem::DirectoryExists(directory))
            failed |= FileSystem::DeleteDirectory(directory, true);
        return failed;
    }

    bool RollbackJournal(const StringView& directory, const TransactionJournal& journal)
    {
        bool failed = false;
        for (int32 i = journal.Entries.Count() - 1; i >= 0; i--)
        {
            const TransactionEntry& entry = journal.Entries[i];
            if (entry.Existed)
            {
                BytesContainer backup;
                const String backupPath = GetBackupPath(directory, i);
                const String restorePath = entry.Destination + TEXT(".restore-") +
                                           journal.TransactionId.ToString(Guid::FormatType::N).ToLower();
                if (File::ReadAllBytes(backupPath, backup) ||
                    WriteDurable(restorePath, backup.Get(), static_cast<int32>(backup.Length())) ||
                    FileSystem::MoveFile(entry.Destination, restorePath, true))
                {
                    failed = true;
                    FileSystem::DeleteFile(restorePath);
                }
            }
            else if (FileSystem::FileExists(entry.Destination))
            {
                failed |= FileSystem::DeleteFile(entry.Destination);
            }
        }
        if (!failed)
            failed = CleanupTransaction(directory, journal, false);
        return failed;
    }

    bool RecoverTransaction(const StringView& directory, String& error)
    {
        if (!FileSystem::FileExists(String(directory) / TEXT("journal.bin")))
        {
            if (FileSystem::DeleteDirectory(directory, true))
                return Fail(error, TEXT("Unprepared scene save staging state could not be removed."));
            return false;
        }
        TransactionJournal journal;
        if (LoadJournal(directory, journal, error))
            return true;
        if (journal.Phase == TransactionPhase::Prepared && RollbackJournal(directory, journal))
            return Fail(error, TEXT("Interrupted scene save could not be rolled back; recovery data was preserved."));
        if (journal.Phase != TransactionPhase::Prepared && CleanupTransaction(directory, journal,
            journal.Phase == TransactionPhase::Committed))
        {
            return Fail(error, TEXT("Recovered scene save transaction state could not be removed."));
        }
        return false;
    }

    bool RecoverAllTransactions(String& error)
    {
        error.Clear();
        const String root = GetTransactionsRoot();
        if (!FileSystem::DirectoryExists(root))
            return false;
        Array<String> directories;
        if (FileSystem::GetChildDirectories(directories, root))
            return Fail(error, TEXT("Cannot enumerate scene save transaction recovery state."));
        for (const String& directory : directories)
        {
            if (RecoverTransaction(directory, error))
                return true;
        }
        return false;
    }

    bool ParseFragment(const Guid& expectedOwner, const SceneFragmentIndexEntry& entry, const Array<byte>& bytes, String& error)
    {
        rapidjson_flax::Document document;
        document.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
        if (document.HasParseError() || !document.IsObject())
            return Fail(error, TEXT("Scene fragment is not valid JSON."));
        const auto version = document.FindMember("formatVersion");
        const auto owner = document.FindMember("ownerSceneGuid");
        const auto root = document.FindMember("rootActorLocalId");
        const auto contained = document.FindMember("containedLocalIds");
        const auto serializer = document.FindMember("serializerVersion");
        const auto payload = document.FindMember("payload");
        Guid ownerGuid;
        if (version == document.MemberEnd() || !version->value.IsUint() ||
            owner == document.MemberEnd() || !owner->value.IsString() ||
            Guid::Parse(StringAnsiView(owner->value.GetString(), owner->value.GetStringLength()), ownerGuid) ||
            root == document.MemberEnd() || !root->value.IsInt64() ||
            contained == document.MemberEnd() || !contained->value.IsArray() ||
            serializer == document.MemberEnd() || !serializer->value.IsUint() ||
            payload == document.MemberEnd() || !payload->value.IsArray())
        {
            return Fail(error, TEXT("Scene fragment header is malformed."));
        }
        if (version->value.GetUint() > SceneFragmentStore::FragmentFormatVersion)
            return Fail(error, TEXT("Scene fragment uses a future format version."));
        if (version->value.GetUint() != SceneFragmentStore::FragmentFormatVersion || ownerGuid != expectedOwner ||
            root->value.GetInt64() != entry.RootActorLocalId || serializer->value.GetUint() != entry.SerializerVersion)
        {
            return Fail(error, TEXT("Scene fragment header does not match its owner index."));
        }
        HashSet<int64> localIds;
        for (const rapidjson_flax::Value& value : contained->value.GetArray())
        {
            if (!value.IsInt64() || value.GetInt64() <= 1 || !localIds.Add(value.GetInt64()))
                return Fail(error, TEXT("Scene fragment contains invalid or duplicate local IDs."));
        }
        if (!localIds.Contains(entry.RootActorLocalId))
            return Fail(error, TEXT("Scene fragment does not contain its root actor local ID."));
        return false;
    }

    void WriteIndex(const SceneFragmentIndex& index, rapidjson_flax::StringBuffer& buffer)
    {
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("formatVersion");
        writer.Uint(index.FormatVersion);
        writer.JKEY("ownerSceneGuid");
        const String owner = index.OwnerSceneGuid.ToString(Guid::FormatType::N).ToLower();
        const StringAnsi ownerAnsi(owner);
        writer.String(ownerAnsi.Get(), ownerAnsi.Length());
        writer.JKEY("indexRevision");
        writer.Uint64(index.IndexRevision);
        writer.JKEY("fragments");
        writer.StartArray();
        for (const SceneFragmentIndexEntry& entry : index.Fragments)
        {
            writer.StartObject();
            writer.JKEY("rootActorLocalId");
            writer.Int64(entry.RootActorLocalId);
            writer.JKEY("relativePhysicalPath");
            const StringAnsi relativePath(entry.RelativePhysicalPath);
            writer.String(relativePath.Get(), relativePath.Length());
            writer.JKEY("contentHash");
            const StringAnsi hash = entry.Content.ToString();
            writer.String(hash.Get(), hash.Length());
            writer.JKEY("size");
            writer.Uint64(entry.Size);
            writer.JKEY("serializerVersion");
            writer.Uint(entry.SerializerVersion);
            writer.EndObject();
        }
        writer.EndArray(index.Fragments.Count());
        writer.EndObject();
    }
}

String SceneFragmentStore::GetRootPath(const StringView& projectRoot)
{
    String result = String(projectRoot) / TEXT("ExternalActors");
    FileSystem::NormalizePath(result);
    return result;
}

String SceneFragmentStore::GetRootPath()
{
    return GetRootPath(Globals::ProjectFolder);
}

String SceneFragmentStore::GetScenePath(const StringView& projectRoot, const Guid& sceneGuid)
{
    return GetRootPath(projectRoot) / sceneGuid.ToString(Guid::FormatType::N).ToLower();
}

String SceneFragmentStore::GetScenePath(const Guid& sceneGuid)
{
    return GetScenePath(Globals::ProjectFolder, sceneGuid);
}

String SceneFragmentStore::GetIndexPath(const Guid& sceneGuid)
{
    return GetScenePath(sceneGuid) / IndexFileName;
}

String SceneFragmentStore::GetRelativeFragmentPath(int64 rootActorLocalId)
{
    const String localId = String::Format(TEXT("{0}"), rootActorLocalId);
    const String shard = localId.Length() >= 2 ? localId.Substring(0, 2) : TEXT("00");
    return shard / localId + FragmentExtension;
}

namespace
{
bool ReadIndexAt(const Guid& sceneGuid, const StringView& path, SceneFragmentIndex& index, String& error)
{
    index = SceneFragmentIndex();
    error.Clear();
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    BytesContainer bytes;
    if (File::ReadAllBytes(path, bytes))
        return Fail(error, TEXT("Scene fragment index is missing or unreadable."));
    rapidjson_flax::Document document;
    document.Parse(bytes.Get<char>(), bytes.Length());
    if (document.HasParseError() || !document.IsObject())
        return Fail(error, TEXT("Scene fragment index is malformed."));
    const auto version = document.FindMember("formatVersion");
    const auto owner = document.FindMember("ownerSceneGuid");
    const auto revision = document.FindMember("indexRevision");
    const auto fragments = document.FindMember("fragments");
    Guid ownerGuid;
    if (version == document.MemberEnd() || !version->value.IsUint() ||
        owner == document.MemberEnd() || !owner->value.IsString() ||
        Guid::Parse(StringAnsiView(owner->value.GetString(), owner->value.GetStringLength()), ownerGuid) ||
        revision == document.MemberEnd() || !revision->value.IsUint64() || revision->value.GetUint64() == 0 ||
        fragments == document.MemberEnd() || !fragments->value.IsArray())
    {
        return Fail(error, TEXT("Scene fragment index header is malformed."));
    }
    if (version->value.GetUint() > SceneFragmentIndex::CurrentFormatVersion)
        return Fail(error, TEXT("Scene fragment index uses a future format version."));
    if (version->value.GetUint() != SceneFragmentIndex::CurrentFormatVersion || ownerGuid != sceneGuid)
        return Fail(error, TEXT("Scene fragment index ownership is invalid."));

    index.OwnerSceneGuid = ownerGuid;
    index.FormatVersion = version->value.GetUint();
    index.IndexRevision = revision->value.GetUint64();
    HashSet<int64> roots;
    HashSet<String> paths;
    for (const rapidjson_flax::Value& value : fragments->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(error, TEXT("Scene fragment index entry is malformed."));
        const auto root = value.FindMember("rootActorLocalId");
        const auto relativePath = value.FindMember("relativePhysicalPath");
        const auto hash = value.FindMember("contentHash");
        const auto size = value.FindMember("size");
        const auto serializer = value.FindMember("serializerVersion");
        SceneFragmentIndexEntry entry;
        if (root == value.MemberEnd() || !root->value.IsInt64() || root->value.GetInt64() <= 1 ||
            relativePath == value.MemberEnd() || !relativePath->value.IsString() ||
            hash == value.MemberEnd() || !hash->value.IsString() ||
            size == value.MemberEnd() || !size->value.IsUint64() || size->value.GetUint64() == 0 ||
            serializer == value.MemberEnd() || !serializer->value.IsUint() || serializer->value.GetUint() == 0)
        {
            return Fail(error, TEXT("Scene fragment index entry fields are malformed."));
        }
        entry.RootActorLocalId = root->value.GetInt64();
        entry.RelativePhysicalPath = String(relativePath->value.GetString(), relativePath->value.GetStringLength());
        entry.Size = size->value.GetUint64();
        entry.SerializerVersion = serializer->value.GetUint();
        if (ContentHash::Parse(StringAnsiView(hash->value.GetString(), hash->value.GetStringLength()), entry.Content) ||
            entry.Content.IsZero() || entry.RelativePhysicalPath != SceneFragmentStore::GetRelativeFragmentPath(entry.RootActorLocalId) ||
            !roots.Add(entry.RootActorLocalId) || !paths.Add(entry.RelativePhysicalPath))
        {
            return Fail(error, TEXT("Scene fragment index contains invalid, duplicate, or misplaced entries."));
        }
        index.Fragments.Add(MoveTemp(entry));
    }
    Sorting::QuickSort(index.Fragments.Get(), index.Fragments.Count(), CompareEntries);
    return false;
}
}

bool SceneFragmentStore::ReadIndex(const Guid& sceneGuid, SceneFragmentIndex& index, String& error)
{
    return ReadIndexAt(sceneGuid, GetIndexPath(sceneGuid), index, error);
}

namespace
{
bool LoadAt(const Guid& sceneGuid, const StringView& scenePath, SceneFragmentIndex& index,
    Array<Array<byte>>& fragments, String& error)
{
    fragments.Clear();
    if (ReadIndexAt(sceneGuid, String(scenePath) / IndexFileName, index, error))
        return true;
    for (const SceneFragmentIndexEntry& entry : index.Fragments)
    {
        Array<byte> bytes;
        const String path = String(scenePath) / entry.RelativePhysicalPath;
        if (File::ReadAllBytes(path, bytes))
            return Fail(error, TEXT("A scene fragment referenced by the index is missing or unreadable."));
        if (static_cast<uint64>(bytes.Count()) != entry.Size || ContentHash::Compute(bytes.Get(), bytes.Count()) != entry.Content)
            return Fail(error, TEXT("A scene fragment does not match its indexed content hash or size."));
        if (ParseFragment(sceneGuid, entry, bytes, error))
            return true;
        fragments.Add(MoveTemp(bytes));
    }
    return false;
}
}

bool SceneFragmentStore::Load(const Guid& sceneGuid, SceneFragmentIndex& index, Array<Array<byte>>& fragments, String& error)
{
    return LoadAt(sceneGuid, GetScenePath(sceneGuid), index, fragments, error);
}

bool SceneFragmentStore::PrepareCloneDirectory(const StringView& projectRoot, const Guid& sourceSceneGuid,
    const Guid& destinationSceneGuid, const StringView& stagingDirectory, String& error)
{
    error.Clear();
    if (!sourceSceneGuid.IsValid() || !destinationSceneGuid.IsValid() || sourceSceneGuid == destinationSceneGuid)
        return Fail(error, TEXT("Scene fragment clone identities are invalid."));
    if (stagingDirectory.IsEmpty() || FileSystem::DirectoryExists(stagingDirectory) ||
        FileSystem::FileExists(stagingDirectory))
        return Fail(error, TEXT("Scene fragment clone staging destination is not empty."));

    SceneFragmentIndex sourceIndex;
    Array<Array<byte>> sourceFragments;
    if (LoadAt(sourceSceneGuid, GetScenePath(projectRoot, sourceSceneGuid), sourceIndex, sourceFragments, error))
        return true;
    if (FileSystem::CreateDirectory(stagingDirectory))
        return Fail(error, TEXT("Cannot create the scene fragment clone staging directory."));
    bool published = false;
    SCOPE_EXIT
    {
        if (!published)
            FileSystem::DeleteDirectory(stagingDirectory, true);
    };

    SceneFragmentIndex cloneIndex;
    cloneIndex.OwnerSceneGuid = destinationSceneGuid;
    cloneIndex.IndexRevision = 1;
    Dictionary<Guid, Guid> remap;
    remap.Add(sourceSceneGuid, destinationSceneGuid);
    for (int32 i = 0; i < sourceIndex.Fragments.Count(); i++)
    {
        rapidjson_flax::Document fragment;
        fragment.Parse(reinterpret_cast<const char*>(sourceFragments[i].Get()), sourceFragments[i].Count());
        if (fragment.HasParseError() || !fragment.IsObject())
            return Fail(error, TEXT("Scene fragment clone source became malformed after validation."));
        JsonTools::ChangeIds(fragment, remap);
        const auto owner = fragment.FindMember("ownerSceneGuid");
        if (owner == fragment.MemberEnd() || !owner->value.IsString())
            return Fail(error, TEXT("Scene fragment clone source has no valid owner identity."));
        const String ownerText = destinationSceneGuid.ToString(Guid::FormatType::N).ToLower();
        const StringAnsi ownerAnsi(ownerText);
        owner->value.SetString(ownerAnsi.Get(), ownerAnsi.Length(), fragment.GetAllocator());

        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        fragment.Accept(writer.GetWriter());
        SceneFragmentIndexEntry cloneEntry = sourceIndex.Fragments[i];
        cloneEntry.Content = ContentHash::Compute(buffer.GetString(), buffer.GetSize());
        cloneEntry.Size = buffer.GetSize();
        const String destination = String(stagingDirectory) / cloneEntry.RelativePhysicalPath;
        const String parent = StringUtils::GetDirectoryName(destination);
        if ((!FileSystem::DirectoryExists(parent) && FileSystem::CreateDirectory(parent)) ||
            File::WriteAllBytes(destination, buffer.GetString(), static_cast<int32>(buffer.GetSize())))
            return Fail(error, TEXT("Cannot write a staged scene fragment clone."));
        cloneIndex.Fragments.Add(MoveTemp(cloneEntry));
    }

    rapidjson_flax::StringBuffer indexBytes;
    WriteIndex(cloneIndex, indexBytes);
    if (File::WriteAllBytes(String(stagingDirectory) / IndexFileName, indexBytes.GetString(),
        static_cast<int32>(indexBytes.GetSize())))
        return Fail(error, TEXT("Cannot write the staged scene fragment clone index."));
    published = true;
    return false;
}

bool SceneFragmentStore::PrepareSave(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments,
    SceneFragmentSavePlan& plan, String& error)
{
    plan = SceneFragmentSavePlan();
    error.Clear();
    if (RecoverIncompleteTransactions(error))
        return true;
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    const String scenePath = GetScenePath(sceneGuid);
    SceneFragmentIndex previous;
    const bool hasDirectory = FileSystem::DirectoryExists(scenePath);
    const bool hasIndex = FileSystem::FileExists(GetIndexPath(sceneGuid));
    if (hasDirectory != hasIndex)
        return Fail(error, TEXT("Private scene fragment storage exists without a complete index."));
    if (hasIndex && ReadIndex(sceneGuid, previous, error))
        return true;

    plan.OwnerSceneGuid = sceneGuid;
    plan.HadPreviousIndex = hasIndex;
    if (hasIndex)
    {
        BytesContainer indexBytes;
        if (File::ReadAllBytes(GetIndexPath(sceneGuid), indexBytes))
            return Fail(error, TEXT("Cannot capture the current scene fragment index revision."));
        plan.ExpectedIndexRevision = previous.IndexRevision;
        plan.ExpectedIndexContent = ContentHash::Compute(indexBytes.Get(), indexBytes.Length());
    }

    SceneFragmentIndex next;
    next.OwnerSceneGuid = sceneGuid;
    HashSet<int64> roots;
    for (const SceneFragmentWrite& fragment : fragments)
    {
        if (fragment.RootActorLocalId <= 1 || fragment.SerializerVersion == 0 || fragment.Payload.IsEmpty() ||
            !roots.Add(fragment.RootActorLocalId))
        {
            return Fail(error, TEXT("Scene fragment write contains an invalid or duplicate root actor local ID."));
        }
        HashSet<int64> contained;
        for (int64 localId : fragment.ContainedLocalIds)
        {
            if (localId <= 1 || !contained.Add(localId))
                return Fail(error, TEXT("Scene fragment write contains invalid or duplicate local IDs."));
        }
        if (!contained.Contains(fragment.RootActorLocalId))
            return Fail(error, TEXT("Scene fragment write does not contain its root actor local ID."));

        rapidjson_flax::Document payload;
        payload.Parse(reinterpret_cast<const char*>(fragment.Payload.Get()), fragment.Payload.Count());
        if (payload.HasParseError() || !payload.IsArray())
            return Fail(error, TEXT("Scene fragment payload must be a valid authored object array."));
        rapidjson_flax::StringBuffer buffer;
        PrettyJsonWriter writer(buffer);
        writer.StartObject();
        writer.JKEY("formatVersion");
        writer.Uint(FragmentFormatVersion);
        writer.JKEY("ownerSceneGuid");
        const String owner = sceneGuid.ToString(Guid::FormatType::N).ToLower();
        const StringAnsi ownerAnsi(owner);
        writer.String(ownerAnsi.Get(), ownerAnsi.Length());
        writer.JKEY("rootActorLocalId");
        writer.Int64(fragment.RootActorLocalId);
        writer.JKEY("containedLocalIds");
        writer.StartArray();
        for (int64 localId : fragment.ContainedLocalIds)
            writer.Int64(localId);
        writer.EndArray(fragment.ContainedLocalIds.Count());
        writer.JKEY("serializerVersion");
        writer.Uint(fragment.SerializerVersion);
        writer.JKEY("payload");
        payload.Accept(writer.GetWriter());
        writer.EndObject();

        SceneFragmentIndexEntry entry;
        entry.RootActorLocalId = fragment.RootActorLocalId;
        entry.RelativePhysicalPath = GetRelativeFragmentPath(fragment.RootActorLocalId);
        entry.Content = ContentHash::Compute(buffer.GetString(), buffer.GetSize());
        entry.Size = buffer.GetSize();
        entry.SerializerVersion = fragment.SerializerVersion;
        PreparedSceneFragment prepared;
        prepared.RelativePhysicalPath = entry.RelativePhysicalPath;
        prepared.Content = entry.Content;
        prepared.Data.Set(reinterpret_cast<const byte*>(buffer.GetString()), static_cast<int32>(buffer.GetSize()));
        plan.Fragments.Add(MoveTemp(prepared));
        next.Fragments.Add(MoveTemp(entry));
    }
    Sorting::QuickSort(next.Fragments.Get(), next.Fragments.Count(), CompareEntries);
    Sorting::QuickSort(plan.Fragments.Get(), plan.Fragments.Count(), ComparePreparedFragments);
    next.IndexRevision = hasIndex && SameEntries(previous.Fragments, next.Fragments)
                             ? previous.IndexRevision
                             : hasIndex ? previous.IndexRevision + 1 : 1;
    rapidjson_flax::StringBuffer indexBytes;
    WriteIndex(next, indexBytes);
    plan.IndexData.Set(reinterpret_cast<const byte*>(indexBytes.GetString()), static_cast<int32>(indexBytes.GetSize()));

    if (hasIndex)
    {
        HashSet<String> retained;
        for (const SceneFragmentIndexEntry& entry : next.Fragments)
            retained.Add(entry.RelativePhysicalPath);
        for (const SceneFragmentIndexEntry& entry : previous.Fragments)
        {
            if (!retained.Contains(entry.RelativePhysicalPath))
                plan.RemovedFragments.Add(entry.RelativePhysicalPath);
        }
    }
    return false;
}

bool SceneFragmentStore::PrepareDelete(const Guid& sceneGuid, SceneFragmentSavePlan& plan, String& error)
{
    plan = SceneFragmentSavePlan();
    error.Clear();
    if (RecoverIncompleteTransactions(error))
        return true;
    if (!sceneGuid.IsValid())
        return Fail(error, TEXT("Scene fragment owner GUID is invalid."));
    plan.OwnerSceneGuid = sceneGuid;
    plan.RemoveStore = true;
    const String scenePath = GetScenePath(sceneGuid);
    if (!FileSystem::DirectoryExists(scenePath))
        return false;
    if (!FileSystem::FileExists(GetIndexPath(sceneGuid)))
        return Fail(error, TEXT("Private scene fragment storage exists without its index."));
    SceneFragmentIndex previous;
    Array<Array<byte>> previousFragments;
    if (Load(sceneGuid, previous, previousFragments, error))
        return true;
    BytesContainer indexBytes;
    if (File::ReadAllBytes(GetIndexPath(sceneGuid), indexBytes))
        return Fail(error, TEXT("Cannot capture the current scene fragment index revision."));
    plan.HadPreviousIndex = true;
    plan.ExpectedIndexRevision = previous.IndexRevision;
    plan.ExpectedIndexContent = ContentHash::Compute(indexBytes.Get(), indexBytes.Length());
    for (const SceneFragmentIndexEntry& entry : previous.Fragments)
        plan.RemovedFragments.Add(entry.RelativePhysicalPath);
    return false;
}

bool SceneFragmentStore::CaptureSourceRevision(const StringView& scenePath, SceneSourceRevision& revision, String& error)
{
    revision = SceneSourceRevision();
    error.Clear();
    String normalized(scenePath);
    FileSystem::NormalizePath(normalized);
    if (!AssetPathPolicy::IsSameOrChild(normalized, Globals::ProjectContentFolder))
        return Fail(error, TEXT("Scene save destination is outside the public Content root."));
    if (!FileSystem::FileExists(normalized))
        return false;
    BytesContainer bytes;
    if (File::ReadAllBytes(normalized, bytes))
        return Fail(error, TEXT("Cannot capture the current scene source revision."));
    revision.Exists = true;
    revision.Content = ContentHash::Compute(bytes.Get(), bytes.Length());
    return false;
}

namespace
{
    bool ValidateExpectedSource(const StringView& scenePath, const SceneSourceRevision& expected, String& error)
    {
        const bool exists = FileSystem::FileExists(scenePath);
        if (exists != expected.Exists)
            return Fail(error, TEXT("Scene source changed while it was being serialized."));
        if (!exists)
            return false;
        BytesContainer bytes;
        if (File::ReadAllBytes(scenePath, bytes) || ContentHash::Compute(bytes.Get(), bytes.Length()) != expected.Content)
            return Fail(error, TEXT("Scene source changed while it was being serialized."));
        return false;
    }

    bool ValidateExpectedIndex(const SceneFragmentSavePlan& plan, String& error)
    {
        const String scenePath = SceneFragmentStore::GetScenePath(plan.OwnerSceneGuid);
        const String indexPath = SceneFragmentStore::GetIndexPath(plan.OwnerSceneGuid);
        if (!plan.HadPreviousIndex)
        {
            if (FileSystem::DirectoryExists(scenePath) || FileSystem::FileExists(indexPath))
                return Fail(error, TEXT("Scene fragment index changed after save preparation."));
            return false;
        }
        SceneFragmentIndex index;
        if (SceneFragmentStore::ReadIndex(plan.OwnerSceneGuid, index, error))
            return true;
        BytesContainer indexBytes;
        if (index.IndexRevision != plan.ExpectedIndexRevision || File::ReadAllBytes(indexPath, indexBytes) ||
            ContentHash::Compute(indexBytes.Get(), indexBytes.Length()) != plan.ExpectedIndexContent)
        {
            return Fail(error, TEXT("Scene fragment index changed after save preparation."));
        }
        return false;
    }

    void AddReplace(TransactionJournal& journal, const StringView& destination, const void* data, int32 length)
    {
        if (SameBytes(destination, data, length))
            return;
        TransactionEntry entry;
        entry.Kind = TransactionEntryKind::Replace;
        entry.Destination = destination;
        entry.Existed = FileSystem::FileExists(destination);
        entry.Data.Set(reinterpret_cast<const byte*>(data), length);
        journal.Entries.Add(MoveTemp(entry));
    }

    void AddDelete(TransactionJournal& journal, const StringView& destination)
    {
        if (!FileSystem::FileExists(destination))
            return;
        TransactionEntry entry;
        entry.Kind = TransactionEntryKind::Delete;
        entry.Destination = destination;
        entry.Existed = true;
        journal.Entries.Add(MoveTemp(entry));
    }

    bool PublishTransaction(TransactionJournal& journal, const SceneFragmentSavePlan& plan,
        const SceneSourceRevision* expectedSource, const StringView& scenePath, String& error,
        SceneFragmentTransactionFailurePoint failurePoint)
    {
        const String directory = GetTransactionDirectory(journal.TransactionId);
        if (SaveJournal(directory, journal, error))
            return true;

        for (int32 i = 0; i < journal.Entries.Count(); i++)
        {
            const TransactionEntry& entry = journal.Entries[i];
            if (entry.Existed)
            {
                BytesContainer current;
                if (File::ReadAllBytes(entry.Destination, current) || current.Length() > MAX_int32 ||
                    WriteDurable(GetBackupPath(directory, i), current.Get(), static_cast<int32>(current.Length())))
                {
                    CleanupTransaction(directory, journal, false);
                    return Fail(error, TEXT("Cannot create a durable scene save rollback copy."));
                }
            }
        }

        if ((expectedSource && ValidateExpectedSource(scenePath, *expectedSource, error)) ||
            ValidateExpectedIndex(plan, error))
        {
            CleanupTransaction(directory, journal, false);
            return true;
        }

        for (int32 i = 0; i < journal.Entries.Count(); i++)
        {
            const TransactionEntry& entry = journal.Entries[i];
            if (entry.Kind == TransactionEntryKind::Replace &&
                WriteDurable(GetStagePath(journal, i), entry.Data.Get(), entry.Data.Count()))
            {
                CleanupTransaction(directory, journal, false);
                return Fail(error, TEXT("Cannot write a staged scene save entry."));
            }
        }

        journal.Phase = TransactionPhase::Prepared;
        if (SaveJournal(directory, journal, error))
        {
            CleanupTransaction(directory, journal, false);
            return true;
        }

        for (int32 i = 0; i < journal.Entries.Count(); i++)
        {
            const TransactionEntry& entry = journal.Entries[i];
            const bool failed = entry.Kind == TransactionEntryKind::Replace
                                    ? FileSystem::MoveFile(entry.Destination, GetStagePath(journal, i), true)
                                    : FileSystem::DeleteFile(entry.Destination);
            if (failed)
            {
                if (RollbackJournal(directory, journal))
                    return Fail(error, TEXT("Scene save failed and its prior revision could not be restored."));
                return Fail(error, TEXT("Cannot apply a staged scene save entry."));
            }
            if (i == 0 && failurePoint == SceneFragmentTransactionFailurePoint::AfterFirstApply)
                return Fail(error, TEXT("Injected interruption after the first scene save entry."));
        }

        if (failurePoint == SceneFragmentTransactionFailurePoint::AfterAllApplyBeforeCommit)
            return Fail(error, TEXT("Injected interruption before the scene save commit marker."));
        journal.Phase = TransactionPhase::Committed;
        if (SaveJournal(directory, journal, error))
        {
            if (RollbackJournal(directory, journal))
                return Fail(error, TEXT("Scene save commit failed and its prior revision could not be restored."));
            return true;
        }
        if (failurePoint == SceneFragmentTransactionFailurePoint::AfterCommitBeforeCleanup)
            return Fail(error, TEXT("Injected interruption after the scene save commit marker."));
        if (CleanupTransaction(directory, journal, true))
            return Fail(error, TEXT("Committed scene save recovery state could not be removed."));
        return false;
    }

    bool CommitPreparedPlan(const SceneFragmentSavePlan& plan, const StringView& scenePath,
        const void* sceneData, int32 sceneDataLength, const SceneSourceRevision* expectedSource,
        String& error, SceneFragmentTransactionFailurePoint failurePoint)
    {
        error.Clear();
        if (!plan.OwnerSceneGuid.IsValid() || (plan.RemoveStore && plan.IndexData.HasItems()) ||
            (!plan.RemoveStore && plan.IndexData.IsEmpty()))
        {
            return Fail(error, TEXT("Scene fragment save plan is invalid."));
        }
        if (RecoverAllTransactions(error) || ValidateExpectedIndex(plan, error) ||
            (expectedSource && ValidateExpectedSource(scenePath, *expectedSource, error)))
        {
            return true;
        }

        TransactionJournal journal;
        journal.TransactionId = Guid::New();
        journal.RemoveStore = plan.RemoveStore;
        journal.StorePath = SceneFragmentStore::GetScenePath(plan.OwnerSceneGuid);
        HashSet<String> destinations;
        for (const PreparedSceneFragment& fragment : plan.Fragments)
        {
            const String destination = journal.StorePath / fragment.RelativePhysicalPath;
            String normalized(destination);
            FileSystem::NormalizePath(normalized);
            if (!AssetPathPolicy::IsSameOrChild(normalized, journal.StorePath) ||
                !fragment.RelativePhysicalPath.EndsWith(FragmentExtension, StringSearchCase::IgnoreCase) ||
                fragment.Data.IsEmpty() || ContentHash::Compute(fragment.Data.Get(), fragment.Data.Count()) != fragment.Content ||
                !destinations.Add(normalized))
            {
                return Fail(error, TEXT("Scene fragment save plan contains an invalid prepared fragment."));
            }
            AddReplace(journal, normalized, fragment.Data.Get(), fragment.Data.Count());
        }
        for (const String& relativePath : plan.RemovedFragments)
        {
            const String destination = journal.StorePath / relativePath;
            String normalized(destination);
            FileSystem::NormalizePath(normalized);
            if (!AssetPathPolicy::IsSameOrChild(normalized, journal.StorePath) ||
                !relativePath.EndsWith(FragmentExtension, StringSearchCase::IgnoreCase) ||
                !destinations.Add(normalized))
            {
                return Fail(error, TEXT("Scene fragment save plan contains an invalid removal."));
            }
            AddDelete(journal, normalized);
        }

        const String indexPath = SceneFragmentStore::GetIndexPath(plan.OwnerSceneGuid);
        if (plan.RemoveStore)
            AddDelete(journal, indexPath);
        else
            AddReplace(journal, indexPath, plan.IndexData.Get(), plan.IndexData.Count());
        if (expectedSource)
            AddReplace(journal, scenePath, sceneData, sceneDataLength);
        if (journal.Entries.IsEmpty() && !plan.RemoveStore)
            return false;
        return PublishTransaction(journal, plan, expectedSource, scenePath, error, failurePoint);
    }
}

bool SceneFragmentStore::CommitSceneSave(const StringView& scenePath, const void* sceneData, int32 sceneDataLength,
    const SceneSourceRevision& expectedSource, const SceneFragmentSavePlan& plan, String& error,
    SceneFragmentTransactionFailurePoint failurePoint)
{
    String normalized(scenePath);
    FileSystem::NormalizePath(normalized);
    if (!sceneData || sceneDataLength <= 0 || !AssetPathPolicy::IsSameOrChild(normalized, Globals::ProjectContentFolder))
        return Fail(error, TEXT("Scene save destination or serialized data is invalid."));
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, normalized, sceneData, sceneDataLength, &expectedSource, error, failurePoint);
}

bool SceneFragmentStore::RecoverIncompleteTransactions(String& error)
{
    ScopeLock lock(TransactionLocker);
    return RecoverAllTransactions(error);
}

bool SceneFragmentStore::Save(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments, String& error)
{
    SceneFragmentSavePlan plan;
    if (PrepareSave(sceneGuid, fragments, plan, error))
        return true;
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, StringView(), nullptr, 0, nullptr, error,
        SceneFragmentTransactionFailurePoint::None);
}

bool SceneFragmentStore::Delete(const Guid& sceneGuid, String& error)
{
    SceneFragmentSavePlan plan;
    if (PrepareDelete(sceneGuid, plan, error))
        return true;
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, StringView(), nullptr, 0, nullptr, error,
        SceneFragmentTransactionFailurePoint::None);
}
