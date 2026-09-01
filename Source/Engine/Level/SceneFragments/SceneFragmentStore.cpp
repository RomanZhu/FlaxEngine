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

namespace
{
    constexpr const Char* IndexFileName = TEXT("scene-fragments.index");
    constexpr const Char* FragmentExtension = TEXT(".sceneactor");
    CriticalSection TransactionLocker;

    enum class PublicationEntryKind : byte
    {
        Replace,
        Delete,
    };

    struct PublicationEntry
    {
        PublicationEntryKind Kind = PublicationEntryKind::Replace;
        String Destination;
        bool Existed = false;
        Array<byte> Data;
        Array<byte> PreviousData;
    };

    struct PublicationBatch
    {
        Guid TransactionId;
        String StorePath;
        Array<PublicationEntry> Entries;
        Array<String> RemovedStores;
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

    String GetStagePath(const PublicationBatch& batch, int32 index)
    {
        return batch.Entries[index].Destination + TEXT(".ssf-") +
               batch.TransactionId.ToString(Guid::FormatType::N).ToLower() + TEXT(".tmp");
    }

    bool EnsureDirectory(const StringView& path)
    {
        return path.HasChars() && FileSystem::CreateDirectory(path);
    }

    bool MoveFile(const StringView& destination, const StringView& source, bool overwrite)
    {
        return FileSystem::MoveFile(destination, source, overwrite);
    }

    bool DeleteFile(const StringView& path)
    {
        return FileSystem::DeleteFile(path);
    }

    bool DeleteDirectory(const StringView& path, bool deleteContents)
    {
        return FileSystem::DeleteDirectory(path, deleteContents);
    }

    bool WriteFile(const StringView& path, const void* data, int32 length)
    {
        return EnsureDirectory(StringUtils::GetDirectoryName(path)) || File::WriteAllBytes(path, data, length);
    }

    bool SameBytes(const StringView& path, const void* data, int32 length)
    {
        BytesContainer existing;
        return !File::ReadAllBytes(path, existing) && existing.Length() == length &&
               (length == 0 || Platform::MemoryCompare(existing.Get(), data, length) == 0);
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
        if (document.HasMember("documentVersion") || document.HasMember("settingsVersion") ||
            document.HasMember("sceneVersion") || document.HasMember("prefabVersion") ||
            version == document.MemberEnd() || !version->value.IsUint() ||
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
    if (document.HasMember("documentVersion") || document.HasMember("settingsVersion") ||
        document.HasMember("sceneVersion") || document.HasMember("prefabVersion") ||
        version == document.MemberEnd() || !version->value.IsUint() ||
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
    if (EnsureDirectory(stagingDirectory))
        return Fail(error, TEXT("Cannot create the scene fragment clone staging directory."));
    bool published = false;
    SCOPE_EXIT
    {
        if (!published)
            DeleteDirectory(stagingDirectory, true);
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
        if (WriteFile(destination, buffer.GetString(), static_cast<int32>(buffer.GetSize())))
            return Fail(error, TEXT("Cannot write a staged scene fragment clone."));
        cloneIndex.Fragments.Add(MoveTemp(cloneEntry));
    }

    rapidjson_flax::StringBuffer indexBytes;
    WriteIndex(cloneIndex, indexBytes);
    if (WriteFile(String(stagingDirectory) / IndexFileName, indexBytes.GetString(),
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

    void AddReplace(PublicationBatch& batch, const StringView& destination, const void* data, int32 length)
    {
        if (SameBytes(destination, data, length))
            return;
        PublicationEntry entry;
        entry.Kind = PublicationEntryKind::Replace;
        entry.Destination = destination;
        entry.Existed = FileSystem::FileExists(destination);
        entry.Data.Set(reinterpret_cast<const byte*>(data), length);
        batch.Entries.Add(MoveTemp(entry));
    }

    void AddDelete(PublicationBatch& batch, const StringView& destination)
    {
        if (!FileSystem::FileExists(destination))
            return;
        PublicationEntry entry;
        entry.Kind = PublicationEntryKind::Delete;
        entry.Destination = destination;
        entry.Existed = true;
        batch.Entries.Add(MoveTemp(entry));
    }

    struct PreparedCommit
    {
        const SceneFragmentSavePlan* Plan = nullptr;
        String ScenePath;
        const void* SceneData = nullptr;
        int32 SceneDataLength = 0;
        const SceneSourceRevision* ExpectedSource = nullptr;
    };

    bool ValidateExpectedState(const Array<PreparedCommit>& commits, String& error)
    {
        for (const PreparedCommit& commit : commits)
        {
            if ((commit.ExpectedSource && ValidateExpectedSource(commit.ScenePath, *commit.ExpectedSource, error)) ||
                ValidateExpectedIndex(*commit.Plan, error))
            {
                return true;
            }
        }
        return false;
    }

    bool CleanupPublication(const PublicationBatch& batch, bool committed)
    {
        bool failed = false;
        for (int32 i = 0; i < batch.Entries.Count(); i++)
        {
            const String staging = GetStagePath(batch, i);
            if (FileSystem::FileExists(staging))
                failed |= DeleteFile(staging);
        }
        if (committed)
        {
            for (const String& store : batch.RemovedStores)
            {
                if (FileSystem::DirectoryExists(store))
                    failed |= DeleteDirectory(store, true);
            }
        }
        else
        {
            for (int32 i = batch.Entries.Count() - 1; i >= 0; i--)
            {
                if (!batch.Entries[i].Existed &&
                    AssetPathPolicy::IsSameOrChild(batch.Entries[i].Destination, batch.StorePath))
                    DeleteDirectory(StringUtils::GetDirectoryName(batch.Entries[i].Destination), false);
            }
            if (FileSystem::DirectoryExists(batch.StorePath))
                DeleteDirectory(batch.StorePath, false);
        }
        return failed;
    }

    bool RollbackPublication(const PublicationBatch& batch)
    {
        bool failed = false;
        for (int32 i = batch.Entries.Count() - 1; i >= 0; i--)
        {
            const PublicationEntry& entry = batch.Entries[i];
            if (entry.Existed)
            {
                const String restorePath = entry.Destination + TEXT(".restore-") +
                                           batch.TransactionId.ToString(Guid::FormatType::N).ToLower();
                if (WriteFile(restorePath, entry.PreviousData.Get(), entry.PreviousData.Count()) ||
                    MoveFile(entry.Destination, restorePath, true))
                {
                    failed = true;
                    if (FileSystem::FileExists(restorePath))
                        DeleteFile(restorePath);
                }
            }
            else if (FileSystem::FileExists(entry.Destination))
            {
                failed |= DeleteFile(entry.Destination);
            }
        }
        failed |= CleanupPublication(batch, false);
        return failed;
    }

    bool PublishBatch(PublicationBatch& batch, const Array<PreparedCommit>& commits, String& error)
    {
        for (PublicationEntry& entry : batch.Entries)
        {
            if (entry.Existed && File::ReadAllBytes(entry.Destination, entry.PreviousData))
                return Fail(error, TEXT("Cannot capture the current scene save entry for in-process rollback."));
        }

        if (ValidateExpectedState(commits, error))
            return true;

        for (int32 i = 0; i < batch.Entries.Count(); i++)
        {
            const PublicationEntry& entry = batch.Entries[i];
            if (entry.Kind == PublicationEntryKind::Replace &&
                WriteFile(GetStagePath(batch, i), entry.Data.Get(), entry.Data.Count()))
            {
                CleanupPublication(batch, false);
                return Fail(error, TEXT("Cannot write a staged scene save entry."));
            }
        }

        for (int32 i = 0; i < batch.Entries.Count(); i++)
        {
            const PublicationEntry& entry = batch.Entries[i];
            const bool applyFailed = entry.Kind == PublicationEntryKind::Replace
                                         ? MoveFile(entry.Destination, GetStagePath(batch, i), true)
                                         : DeleteFile(entry.Destination);
            if (applyFailed)
            {
                if (RollbackPublication(batch))
                    return Fail(error, TEXT("Scene save failed and its prior revision could not be restored."));
                return Fail(error, TEXT("Cannot apply a staged scene save entry."));
            }
        }

        if (CleanupPublication(batch, true))
            return Fail(error, TEXT("Committed scene save obsolete storage could not be removed."));
        return false;
    }

    bool CommitPreparedPlans(const Array<PreparedCommit>& commits, String& error)
    {
        error.Clear();
        if (commits.IsEmpty())
            return false;
        HashSet<Guid> owners;
        HashSet<String> sourcePaths;
        HashSet<String> destinations;
        for (const PreparedCommit& commit : commits)
        {
            if (!commit.Plan)
                return Fail(error, TEXT("Scene fragment save batch contains a missing plan."));
            const SceneFragmentSavePlan& plan = *commit.Plan;
            if (!plan.OwnerSceneGuid.IsValid() || !owners.Add(plan.OwnerSceneGuid) ||
                (plan.RemoveStore && plan.IndexData.HasItems()) || (!plan.RemoveStore && plan.IndexData.IsEmpty()))
            {
                return Fail(error, TEXT("Scene fragment save batch contains an invalid or duplicate plan."));
            }
            if (commit.ExpectedSource)
            {
                if (!commit.SceneData || commit.SceneDataLength <= 0 || commit.ScenePath.IsEmpty() ||
                    !AssetPathPolicy::IsSameOrChild(commit.ScenePath, Globals::ProjectContentFolder) ||
                    !sourcePaths.Add(commit.ScenePath))
                {
                    return Fail(error, TEXT("Scene save batch contains an invalid or duplicate source."));
                }
            }
        }
        if (ValidateExpectedState(commits, error))
            return true;

        PublicationBatch batch;
        batch.TransactionId = Guid::New();
        batch.StorePath = SceneFragmentStore::GetRootPath();
        for (const PreparedCommit& commit : commits)
        {
            const SceneFragmentSavePlan& plan = *commit.Plan;
            const String storePath = SceneFragmentStore::GetScenePath(plan.OwnerSceneGuid);
            for (const PreparedSceneFragment& fragment : plan.Fragments)
            {
                const String destination = storePath / fragment.RelativePhysicalPath;
                String normalized(destination);
                FileSystem::NormalizePath(normalized);
                if (!AssetPathPolicy::IsSameOrChild(normalized, storePath) ||
                    !fragment.RelativePhysicalPath.EndsWith(FragmentExtension, StringSearchCase::IgnoreCase) ||
                    fragment.Data.IsEmpty() || ContentHash::Compute(fragment.Data.Get(), fragment.Data.Count()) != fragment.Content ||
                    !destinations.Add(normalized))
                {
                    return Fail(error, TEXT("Scene fragment save plan contains an invalid prepared fragment."));
                }
                AddReplace(batch, normalized, fragment.Data.Get(), fragment.Data.Count());
            }
            for (const String& relativePath : plan.RemovedFragments)
            {
                const String destination = storePath / relativePath;
                String normalized(destination);
                FileSystem::NormalizePath(normalized);
                if (!AssetPathPolicy::IsSameOrChild(normalized, storePath) ||
                    !relativePath.EndsWith(FragmentExtension, StringSearchCase::IgnoreCase) ||
                    !destinations.Add(normalized))
                {
                    return Fail(error, TEXT("Scene fragment save plan contains an invalid removal."));
                }
                AddDelete(batch, normalized);
            }

            const String indexPath = SceneFragmentStore::GetIndexPath(plan.OwnerSceneGuid);
            if (!destinations.Add(indexPath))
                return Fail(error, TEXT("Scene save batch contains a duplicate fragment index destination."));
            if (plan.RemoveStore)
            {
                AddDelete(batch, indexPath);
                batch.RemovedStores.Add(storePath);
            }
            else
            {
                AddReplace(batch, indexPath, plan.IndexData.Get(), plan.IndexData.Count());
            }
            if (commit.ExpectedSource)
            {
                if (!destinations.Add(commit.ScenePath))
                    return Fail(error, TEXT("Scene save batch contains a duplicate destination."));
                AddReplace(batch, commit.ScenePath, commit.SceneData, commit.SceneDataLength);
            }
        }
        if (batch.Entries.IsEmpty() && batch.RemovedStores.IsEmpty())
            return false;
        return PublishBatch(batch, commits, error);
    }

    bool CommitPreparedPlan(const SceneFragmentSavePlan& plan, const StringView& scenePath,
        const void* sceneData, int32 sceneDataLength, const SceneSourceRevision* expectedSource,
        String& error)
    {
        PreparedCommit commit;
        commit.Plan = &plan;
        commit.ScenePath = scenePath;
        commit.SceneData = sceneData;
        commit.SceneDataLength = sceneDataLength;
        commit.ExpectedSource = expectedSource;
        Array<PreparedCommit> commits;
        commits.Add(MoveTemp(commit));
        return CommitPreparedPlans(commits, error);
    }
}

bool SceneFragmentStore::CommitSceneSave(const StringView& scenePath, const void* sceneData, int32 sceneDataLength,
    const SceneSourceRevision& expectedSource, const SceneFragmentSavePlan& plan, String& error)
{
    String normalized(scenePath);
    FileSystem::NormalizePath(normalized);
    if (!sceneData || sceneDataLength <= 0 || !AssetPathPolicy::IsSameOrChild(normalized, Globals::ProjectContentFolder))
        return Fail(error, TEXT("Scene save destination or serialized data is invalid."));
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, normalized, sceneData, sceneDataLength, &expectedSource, error);
}

bool SceneFragmentStore::CommitSceneSaves(const Array<PreparedSceneSave>& saves, String& error)
{
    Array<PreparedCommit> commits;
    commits.EnsureCapacity(saves.Count());
    for (const PreparedSceneSave& save : saves)
    {
        PreparedCommit commit;
        commit.Plan = &save.FragmentPlan;
        commit.ScenePath = save.SourcePath;
        FileSystem::NormalizePath(commit.ScenePath);
        commit.SceneData = save.SourceData.Get();
        commit.SceneDataLength = save.SourceData.Count();
        commit.ExpectedSource = &save.ExpectedSource;
        commits.Add(MoveTemp(commit));
    }
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlans(commits, error);
}

bool SceneFragmentStore::Save(const Guid& sceneGuid, const Array<SceneFragmentWrite>& fragments, String& error)
{
    SceneFragmentSavePlan plan;
    if (PrepareSave(sceneGuid, fragments, plan, error))
        return true;
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, StringView(), nullptr, 0, nullptr, error);
}

bool SceneFragmentStore::Delete(const Guid& sceneGuid, String& error)
{
    SceneFragmentSavePlan plan;
    if (PrepareDelete(sceneGuid, plan, error))
        return true;
    ScopeLock lock(TransactionLocker);
    return CommitPreparedPlan(plan, StringView(), nullptr, 0, nullptr, error);
}
