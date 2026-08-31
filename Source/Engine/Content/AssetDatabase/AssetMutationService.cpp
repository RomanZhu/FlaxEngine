// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetMutationService.h"
#include "AssetPath.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;

    enum class JournalState : byte
    {
        Prepared,
        Staged,
        Publishing,
        Committed,
        RollingBack,
        RolledBack,
        RecoveryRequired,
    };

    struct JournalEntry
    {
        String Role;
        String SourcePath;
        String DestinationPath;
        String StagingPath;
        String PreimagePath;
        String BeforeHash;
        String StagedHash;
        bool IsDirectory = false;
        bool PreimageComplete = false;
        bool StagingComplete = false;
        bool Published = false;
    };

    struct Journal
    {
        int32 FormatVersion = 1;
        Guid ID;
        AssetMutationOperation Operation = AssetMutationOperation::Validate;
        JournalState State = JournalState::Prepared;
        String RecoveryPath;
        String LastError;
        Array<JournalEntry> Entries;
    };

    CriticalSection PathLocksLocker;
    Array<String> LockedPaths;

    String NormalizePath(const StringView& input)
    {
        String result(input);
        StringUtils::PathRemoveRelativeParts(result);
        result.Replace((Char)'\\', (Char)'/');
        while (result.Length() > 1 && result.EndsWith('/'))
            result.Resize(result.Length() - 1);
        return result;
    }

    String LockKey(const StringView& input)
    {
        String result = NormalizePath(input);
#if PLATFORM_WINDOWS || PLATFORM_UWP || PLATFORM_XBOX_ONE || PLATFORM_XBOX_SCARLETT
        result = result.ToLower();
#endif
        return result;
    }

    bool PathsOverlap(const StringView& a, const StringView& b)
    {
        if (a == b)
            return true;
        if (a.Length() > b.Length() && a.StartsWith(b) && a[b.Length()] == '/')
            return true;
        return b.Length() > a.Length() && b.StartsWith(a) && b[a.Length()] == '/';
    }

    class PathLockScope
    {
        Array<String> _keys;

    public:
        bool Acquire(const Array<String>& paths)
        {
            Array<String> keys;
            for (const String& path : paths)
            {
                if (path.IsEmpty())
                    continue;
                const String key = LockKey(path);
                if (!keys.Contains(key))
                    keys.Add(key);
            }
            if (keys.Count() > 1)
            {
                std::sort(keys.Get(), keys.Get() + keys.Count(), [](const String& a, const String& b)
                {
                    return a < b;
                });
            }

            ScopeLock lock(PathLocksLocker);
            for (const String& key : keys)
            {
                for (const String& locked : LockedPaths)
                {
                    if (PathsOverlap(key, locked))
                        return true;
                }
            }
            for (const String& key : keys)
                LockedPaths.Add(key);
            _keys = MoveTemp(keys);
            return false;
        }

        ~PathLockScope()
        {
            ScopeLock lock(PathLocksLocker);
            for (const String& key : _keys)
                LockedPaths.Remove(key);
        }
    };

    const Char* OperationName(AssetMutationOperation operation)
    {
        switch (operation)
        {
        case AssetMutationOperation::CreateAsset: return TEXT("CreateAsset");
        case AssetMutationOperation::CreateFolder: return TEXT("CreateFolder");
        case AssetMutationOperation::Copy: return TEXT("Copy");
        case AssetMutationOperation::Move: return TEXT("Move");
        case AssetMutationOperation::Rename: return TEXT("Rename");
        case AssetMutationOperation::DeleteToRecovery: return TEXT("DeleteToRecovery");
        case AssetMutationOperation::ReplaceContents: return TEXT("ReplaceContents");
        case AssetMutationOperation::ReplaceAsset: return TEXT("ReplaceAsset");
        case AssetMutationOperation::Recover: return TEXT("Recover");
        default: return TEXT("Validate");
        }
    }

    bool ParseOperation(const StringView& value, AssetMutationOperation& operation)
    {
        for (int32 i = (int32)AssetMutationOperation::Validate; i <= (int32)AssetMutationOperation::Recover; i++)
        {
            const AssetMutationOperation candidate = (AssetMutationOperation)i;
            if (value == OperationName(candidate))
            {
                operation = candidate;
                return false;
            }
        }
        return true;
    }

    const Char* StateName(JournalState state)
    {
        switch (state)
        {
        case JournalState::Prepared: return TEXT("Prepared");
        case JournalState::Staged: return TEXT("Staged");
        case JournalState::Publishing: return TEXT("Publishing");
        case JournalState::Committed: return TEXT("Committed");
        case JournalState::RollingBack: return TEXT("RollingBack");
        case JournalState::RolledBack: return TEXT("RolledBack");
        case JournalState::RecoveryRequired: return TEXT("RecoveryRequired");
        default: return TEXT("Prepared");
        }
    }

    bool ParseState(const StringView& value, JournalState& state)
    {
        for (int32 i = (int32)JournalState::Prepared; i <= (int32)JournalState::RecoveryRequired; i++)
        {
            const JournalState candidate = (JournalState)i;
            if (value == StateName(candidate))
            {
                state = candidate;
                return false;
            }
        }
        return true;
    }

    String GuidText(const Guid& id)
    {
        return id.ToString(Guid::FormatType::N).ToLower();
    }

    bool Exists(const StringView& path)
    {
        return FileSystem::FileExists(path) || FileSystem::DirectoryExists(path);
    }

    bool EnsureDirectoryFor(const StringView& path)
    {
        const String parent(StringUtils::GetDirectoryName(path));
        return parent.HasChars() && !FileSystem::DirectoryExists(parent) && FileSystem::CreateDirectory(parent);
    }

    bool DeletePath(const StringView& path)
    {
        if (FileSystem::DirectoryExists(path))
            return FileSystem::DeleteDirectory(String(path), true);
        if (FileSystem::FileExists(path))
            return FileSystem::DeleteFile(path);
        return false;
    }

    bool MovePath(const StringView& source, const StringView& destination, bool overwrite = false)
    {
        if (EnsureDirectoryFor(destination))
            return true;
        if (overwrite && Exists(destination) && FileSystem::DirectoryExists(destination))
        {
            if (FileSystem::DeleteDirectory(String(destination), true))
                return true;
        }
        return FileSystem::MoveFile(destination, source, overwrite);
    }

    bool CopyPath(const StringView& source, const StringView& destination)
    {
        if (EnsureDirectoryFor(destination))
            return true;
        if (FileSystem::DirectoryExists(source))
            return FileSystem::CopyDirectory(String(destination), String(source), true);
        return !FileSystem::FileExists(source) || FileSystem::CopyFile(destination, source);
    }

    bool HashFile(const StringView& path, String& output)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return true;
        output = String(ContentHash::Compute(bytes.Get(), bytes.Count()).ToString());
        return false;
    }

    bool CollectDirectories(const StringView& root, Array<String>& output)
    {
        Array<String> children;
        if (FileSystem::GetChildDirectories(children, String(root)))
            return true;
        for (const String& child : children)
        {
            output.Add(child);
            if (CollectDirectories(child, output))
                return true;
        }
        return false;
    }

    bool HashPath(const StringView& path, String& output)
    {
        if (FileSystem::FileExists(path))
            return HashFile(path, output);
        if (!FileSystem::DirectoryExists(path))
            return true;
        Array<String> directories;
        Array<String> files;
        if (CollectDirectories(path, directories) || FileSystem::DirectoryGetFiles(files, String(path), TEXT("*"), DirectorySearchOption::AllDirectories))
            return true;
        if (directories.Count() > 1)
            std::sort(directories.Get(), directories.Get() + directories.Count(), [](const String& a, const String& b) { return a < b; });
        if (files.Count() > 1)
            std::sort(files.Get(), files.Get() + files.Count(), [](const String& a, const String& b) { return a < b; });
        StringAnsi manifest;
        for (const String& directory : directories)
        {
            const String relative = FileSystem::ConvertAbsolutePathToRelative(String(path), directory);
            manifest += "D:";
            manifest += StringAnsi(relative);
            manifest += '\n';
        }
        for (const String& file : files)
        {
            String hash;
            if (HashFile(file, hash))
                return true;
            const String relative = FileSystem::ConvertAbsolutePathToRelative(String(path), file);
            manifest += "F:";
            manifest += StringAnsi(relative);
            manifest += ':';
            manifest += StringAnsi(hash);
            manifest += '\n';
        }
        output = String(ContentHash::Compute(manifest.Get(), manifest.Length()).ToString());
        return false;
    }

    bool FlushWrittenFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#else
        return false;
#endif
    }

    bool SameFileHash(const StringView& a, const StringView& b)
    {
        String first;
        String second;
        return !HashFile(a, first) && !HashFile(b, second) && first == second;
    }

    String StagePath(const StringView& path, const Guid& id, const Char* role)
    {
        return String(path) + TEXT(".mutation-") + role + TEXT("-") + GuidText(id);
    }

    void AddString(JsonValue& object, const char* key, const StringView& value, JsonAlloc& allocator)
    {
        const StringAnsi ansi(value);
        object.AddMember(JsonValue(key, allocator), JsonValue(ansi.Get(), ansi.Length(), allocator), allocator);
    }

    bool ReadString(const JsonValue& object, const char* key, String& value)
    {
        const auto member = object.FindMember(key);
        if (member == object.MemberEnd() || !member->value.IsString())
            return true;
        value = String(StringAnsiView(member->value.GetString(), member->value.GetStringLength()));
        return false;
    }

    bool SerializeJournal(const Journal& journal, StringAnsi& output)
    {
        JsonDocument document;
        document.SetObject();
        JsonAlloc& allocator = document.GetAllocator();
        document.AddMember("formatVersion", journal.FormatVersion, allocator);
        AddString(document, "transactionId", GuidText(journal.ID), allocator);
        AddString(document, "operation", OperationName(journal.Operation), allocator);
        AddString(document, "state", StateName(journal.State), allocator);
        AddString(document, "recoveryPath", journal.RecoveryPath, allocator);
        AddString(document, "lastError", journal.LastError, allocator);
        JsonValue entries(rapidjson::kArrayType);
        for (const JournalEntry& entry : journal.Entries)
        {
            JsonValue item(rapidjson::kObjectType);
            AddString(item, "role", entry.Role, allocator);
            AddString(item, "sourcePath", entry.SourcePath, allocator);
            AddString(item, "destinationPath", entry.DestinationPath, allocator);
            AddString(item, "stagingPath", entry.StagingPath, allocator);
            AddString(item, "preimagePath", entry.PreimagePath, allocator);
            AddString(item, "beforeHash", entry.BeforeHash, allocator);
            AddString(item, "stagedHash", entry.StagedHash, allocator);
            item.AddMember("isDirectory", entry.IsDirectory, allocator);
            item.AddMember("preimageComplete", entry.PreimageComplete, allocator);
            item.AddMember("stagingComplete", entry.StagingComplete, allocator);
            item.AddMember("published", entry.Published, allocator);
            entries.PushBack(item, allocator);
        }
        document.AddMember("entries", entries, allocator);
        Array<StringAnsi> order;
        order.Add("formatVersion");
        order.Add("transactionId");
        order.Add("operation");
        order.Add("state");
        order.Add("recoveryPath");
        order.Add("lastError");
        order.Add("entries");
        CanonicalJsonError error;
        return CanonicalJsonWriter::Write(document, output, error, &order);
    }

    bool ParseJournal(const StringAnsiView& json, Journal& journal)
    {
        JsonDocument document;
        document.Parse(json.Get(), json.Length());
        if (document.HasParseError() || !document.IsObject())
            return true;
        const auto version = document.FindMember("formatVersion");
        const auto entries = document.FindMember("entries");
        String transactionID;
        String operation;
        String state;
        if (version == document.MemberEnd() || !version->value.IsInt() || version->value.GetInt() != 1 ||
            entries == document.MemberEnd() || !entries->value.IsArray() ||
            ReadString(document, "transactionId", transactionID) || ReadString(document, "operation", operation) ||
            ReadString(document, "state", state) || ReadString(document, "recoveryPath", journal.RecoveryPath) ||
            ReadString(document, "lastError", journal.LastError) || Guid::Parse(transactionID, journal.ID) || !journal.ID.IsValid() ||
            ParseOperation(operation, journal.Operation) || ParseState(state, journal.State))
            return true;
        journal.FormatVersion = version->value.GetInt();
        for (const JsonValue& item : entries->value.GetArray())
        {
            if (!item.IsObject())
                return true;
            JournalEntry entry;
            const auto isDirectory = item.FindMember("isDirectory");
            const auto preimageComplete = item.FindMember("preimageComplete");
            const auto stagingComplete = item.FindMember("stagingComplete");
            const auto published = item.FindMember("published");
            if (isDirectory == item.MemberEnd() || !isDirectory->value.IsBool() ||
                preimageComplete == item.MemberEnd() || !preimageComplete->value.IsBool() ||
                stagingComplete == item.MemberEnd() || !stagingComplete->value.IsBool() ||
                published == item.MemberEnd() || !published->value.IsBool() ||
                ReadString(item, "role", entry.Role) || ReadString(item, "sourcePath", entry.SourcePath) ||
                ReadString(item, "destinationPath", entry.DestinationPath) || ReadString(item, "stagingPath", entry.StagingPath) ||
                ReadString(item, "preimagePath", entry.PreimagePath) || ReadString(item, "beforeHash", entry.BeforeHash) ||
                ReadString(item, "stagedHash", entry.StagedHash))
                return true;
            entry.IsDirectory = isDirectory->value.GetBool();
            entry.PreimageComplete = preimageComplete->value.GetBool();
            entry.StagingComplete = stagingComplete->value.GetBool();
            entry.Published = published->value.GetBool();
            journal.Entries.Add(MoveTemp(entry));
        }
        return journal.Entries.IsEmpty();
    }

    String JournalPath(const StringView& root, const Guid& id)
    {
        return String(root) / (GuidText(id) + TEXT(".json"));
    }

    bool SaveJournal(const StringView& root, const Journal& journal)
    {
        StringAnsi json;
        if (SerializeJournal(journal, json))
            return true;
        const String destination = JournalPath(root, journal.ID);
        const String staging = destination + TEXT(".tmp");
        if (EnsureDirectoryFor(destination) || File::WriteAllBytes(staging, json.Get(), json.Length()) || FlushWrittenFile(staging) || FileSystem::MoveFile(destination, staging, true))
        {
            FileSystem::DeleteFile(staging);
            return true;
        }
        return false;
    }

    bool LoadJournal(const StringView& path, Journal& journal)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return true;
        return ParseJournal(StringAnsiView((const char*)bytes.Get(), bytes.Count()), journal);
    }

    void DeleteJournal(const StringView& root, const Guid& id)
    {
        const String path = JournalPath(root, id);
        FileSystem::DeleteFile(path);
        FileSystem::DeleteFile(path + TEXT(".tmp"));
    }

    bool Fail(AssetMutationResult& result, AssetMutationFailure failure, const Guid& id, const StringView& source, const StringView& destination, const StringView& message, bool recovery = false)
    {
        result = AssetMutationResult();
        result.Failure = failure;
        result.TransactionID = id;
        result.SourcePath = source;
        result.DestinationPath = destination;
        result.Message = message;
        result.RequiresRecovery = recovery;
        return true;
    }

    void Succeed(AssetMutationResult& result, const Guid& id, const StringView& source, const StringView& destination)
    {
        result.Succeeded = true;
        result.Failure = AssetMutationFailure::None;
        result.TransactionID = id;
        result.SourcePath = source;
        result.DestinationPath = destination;
    }
}

namespace
{
    bool ResolveContentPath(const AssetMutationService& service, const StringView& input, String& output, AssetMutationResult& result, const Guid& id, bool destination)
    {
        AssetPathPolicy::ProjectPath path;
        AssetPipelineDiagnostic diagnostic;
        if (input.IsEmpty() || input.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase) ||
            AssetPathPolicy::TryNormalizeProjectPath(service.GetProjectRoot(), service.GetContentRoot(), service.GetJournalRoot(), input, path, diagnostic) ||
            NormalizePath(path.AbsolutePath) == NormalizePath(service.GetContentRoot()))
        {
            return Fail(result, destination ? AssetMutationFailure::InvalidDestination : AssetMutationFailure::InvalidSource,
                id, destination ? StringView() : input, destination ? input : StringView(),
                diagnostic.Message.HasChars() ? diagnostic.Message : TEXT("Asset mutation paths must name an entry below the canonical Content root."));
        }
        output = NormalizePath(path.AbsolutePath);
        return false;
    }

    bool IsSameOrChild(const StringView& path, const StringView& root)
    {
        return AssetPathPolicy::IsSameOrChild(NormalizePath(path), NormalizePath(root));
    }

    bool IsSameVolume(const StringView& a, const StringView& b)
    {
#if PLATFORM_WINDOWS || PLATFORM_UWP || PLATFORM_XBOX_ONE || PLATFORM_XBOX_SCARLETT
        const String first = NormalizePath(a);
        const String second = NormalizePath(b);
        if (first.Length() >= 2 && second.Length() >= 2 && first[1] == ':' && second[1] == ':')
            return StringUtils::ToLower(first[0]) == StringUtils::ToLower(second[0]);
        if (first.StartsWith(TEXT("//")) && second.StartsWith(TEXT("//")))
        {
            const int32 firstSlash = first.Find(TEXT("/"), StringSearchCase::CaseSensitive, 2);
            const int32 secondSlash = second.Find(TEXT("/"), StringSearchCase::CaseSensitive, 2);
            const int32 firstShare = firstSlash == -1 ? -1 : first.Find(TEXT("/"), StringSearchCase::CaseSensitive, firstSlash + 1);
            const int32 secondShare = secondSlash == -1 ? -1 : second.Find(TEXT("/"), StringSearchCase::CaseSensitive, secondSlash + 1);
            const String firstRoot = firstShare == -1 ? first : first.Substring(0, firstShare);
            const String secondRoot = secondShare == -1 ? second : second.Substring(0, secondShare);
            return firstRoot.Compare(secondRoot, StringSearchCase::IgnoreCase) == 0;
        }
#endif
        return true;
    }

    bool PairExists(const StringView& source)
    {
        return Exists(source) && FileSystem::FileExists(String(source) + TEXT(".meta"));
    }

    bool PairAbsent(const StringView& source)
    {
        return !Exists(source) && !Exists(String(source) + TEXT(".meta"));
    }

    bool ValidateSourcePair(const StringView& source, AssetMutationResult& result, const Guid& id)
    {
        if (!Exists(source))
            return Fail(result, AssetMutationFailure::MissingSource, id, source, StringView(), TEXT("Mutation source does not exist."));
        const String meta = String(source) + TEXT(".meta");
        if (!FileSystem::FileExists(meta))
            return Fail(result, AssetMutationFailure::MissingMetadata, id, source, StringView(), TEXT("Mutation source is missing its adjacent metadata sidecar."));
        AssetMeta value;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(meta, value, diagnostic))
            return Fail(result, AssetMutationFailure::InvalidMetadata, id, source, StringView(), diagnostic.Message);
        if (value.FolderAsset != FileSystem::DirectoryExists(source))
            return Fail(result, AssetMutationFailure::InvalidMetadata, id, source, StringView(), TEXT("Mutation source type disagrees with its adjacent metadata."));
        return false;
    }

    bool ValidateDestination(const StringView& source, const StringView& destination, bool allowEquivalent, AssetMutationResult& result, const Guid& id)
    {
        const bool equivalent = allowEquivalent && FileSystem::AreFilePathsEquivalent(source, destination);
        if (!equivalent && (!PairAbsent(destination)))
            return Fail(result, AssetMutationFailure::DestinationCollision, id, source, destination, TEXT("Mutation destination or its metadata sidecar already exists."));
        const String parent(StringUtils::GetDirectoryName(destination));
        if (parent.IsEmpty() || !FileSystem::DirectoryExists(parent))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, source, destination, TEXT("Mutation destination parent does not exist."));
        if (FileSystem::DirectoryExists(source) && !equivalent && IsSameOrChild(destination, source))
            return Fail(result, AssetMutationFailure::PathCycle, id, source, destination, TEXT("A folder cannot be copied or moved into itself or a descendant."));
        return false;
    }

    bool VerifyHandledState(AssetMutationOperation operation, const StringView& source, const StringView& destination)
    {
        switch (operation)
        {
        case AssetMutationOperation::CreateAsset:
        case AssetMutationOperation::CreateFolder:
        case AssetMutationOperation::Copy:
            return PairExists(destination);
        case AssetMutationOperation::Move:
        case AssetMutationOperation::Rename:
        case AssetMutationOperation::Recover:
            return (FileSystem::AreFilePathsEquivalent(source, destination) || PairAbsent(source)) && PairExists(destination);
        case AssetMutationOperation::DeleteToRecovery:
            return PairAbsent(source);
        case AssetMutationOperation::ReplaceContents:
        case AssetMutationOperation::ReplaceAsset:
            return PairExists(source);
        default:
            return false;
        }
    }

    bool RunDecisionHook(AssetMutationService& service, AssetMutationOperation operation, const Guid& id, const StringView& source,
        const StringView& destination, bool isDirectory, AssetMutationResult& result, String* replacementPath = nullptr)
    {
        if (!service.DecisionHook.IsBinded())
            return false;
        AssetMutationDecisionContext context;
        context.TransactionID = id;
        context.SessionID = service.GetSessionID();
        context.Operation = operation;
        context.SourcePath = source;
        context.DestinationPath = destination;
        context.IsDirectory = isDirectory;
        context.Invocation = service.GetInvocation();
        const AssetMutationDecisionResult decision = service.DecisionHook(context);
        if (decision.Decision == AssetMutationDecision::Deny)
            return Fail(result, AssetMutationFailure::CallbackRejected, id, source, destination,
                decision.Message.HasChars() ? decision.Message : TEXT("Mutation was rejected by an asset modification callback."));
        if (decision.Decision == AssetMutationDecision::AllowWithReplacementPath)
        {
            if (!replacementPath || decision.ReplacementPath.IsEmpty())
                return Fail(result, AssetMutationFailure::InvalidDestination, id, source, destination, TEXT("Mutation callback returned an empty or unsupported replacement path."));
            *replacementPath = decision.ReplacementPath;
            return false;
        }
        if (decision.Decision != AssetMutationDecision::AlreadyHandled)
            return false;
        if (!VerifyHandledState(operation, source, destination))
            return Fail(result, AssetMutationFailure::CallbackHandledInvalidState, id, source, destination, TEXT("Mutation callback reported Handled but did not leave a valid source/metadata pair state."));
        Succeed(result, id, source, destination);
        result.HandledByCallback = true;
        const String identityPath = operation == AssetMutationOperation::ReplaceContents || operation == AssetMutationOperation::ReplaceAsset
            ? String(source)
            : String(destination);
        if (identityPath.HasChars())
        {
            AssetMeta meta;
            AssetPipelineDiagnostic diagnostic;
            if (!AssetMeta::Load(identityPath + TEXT(".meta"), meta, diagnostic))
                result.AssetID = meta.ID;
        }
        if (source.HasChars() && IsSameOrChild(source, service.GetContentRoot()))
            result.ChangedPaths.Add(source);
        if (destination.HasChars() && IsSameOrChild(destination, service.GetContentRoot()))
            result.ChangedPaths.Add(destination);
        if (service.CommittedHook.IsBinded())
            service.CommittedHook(result);
        return true;
    }

    bool CloneMeta(const StringView& source, const StringView& destination, Dictionary<Guid, Guid>& remap, Guid* outputID, String* error)
    {
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        if (AssetMeta::Load(source, meta, diagnostic))
        {
            if (error)
                *error = diagnostic.Message;
            return true;
        }
        const Guid oldID = meta.ID;
        meta = meta.CloneWithNewIdentities();
        remap[oldID] = meta.ID;
        if (outputID)
            *outputID = meta.ID;
        if (AssetMeta::SaveAtomic(destination, meta, diagnostic))
        {
            if (error)
                *error = diagnostic.Message;
            return true;
        }
        return false;
    }

    bool ReplaceBytes(Array<byte>& bytes, const StringAnsiView& before, const StringAnsiView& after)
    {
        if (before.Length() == 0 || before.Length() != after.Length() || bytes.Count() < before.Length())
            return false;
        bool changed = false;
        for (int32 i = 0; i <= bytes.Count() - before.Length(); i++)
        {
            if (Platform::MemoryCompare(bytes.Get() + i, before.Get(), before.Length()) == 0)
            {
                Platform::MemoryCopy(bytes.Get() + i, after.Get(), after.Length());
                i += before.Length() - 1;
                changed = true;
            }
        }
        return changed;
    }

    bool RemapAuthoredDocument(const StringView& path, const Dictionary<Guid, Guid>& remap)
    {
        Array<byte> bytes;
        if (File::ReadAllBytes(path, bytes))
            return true;
        bool changed = false;
        for (const auto& item : remap)
        {
            const StringAnsi oldN(item.Key.ToString(Guid::FormatType::N).ToLower());
            const StringAnsi newN(item.Value.ToString(Guid::FormatType::N).ToLower());
            const StringAnsi oldD(item.Key.ToString(Guid::FormatType::D).ToLower());
            const StringAnsi newD(item.Value.ToString(Guid::FormatType::D).ToLower());
            changed |= ReplaceBytes(bytes, oldN, newN);
            changed |= ReplaceBytes(bytes, oldD, newD);
            changed |= ReplaceBytes(bytes, oldN.ToUpper(), newN.ToUpper());
            changed |= ReplaceBytes(bytes, oldD.ToUpper(), newD.ToUpper());
        }
        return changed && File::WriteAllBytes(path, bytes.Get(), bytes.Count());
    }

    bool ClonePairMetadata(const StringView& source, const StringView& stagedSource, const StringView& stagedMeta, Guid& rootID, String& error)
    {
        Dictionary<Guid, Guid> remap;
        if (CloneMeta(String(source) + TEXT(".meta"), stagedMeta, remap, &rootID, &error))
            return true;
        if (FileSystem::DirectoryExists(source))
        {
            Array<String> sidecars;
            if (FileSystem::DirectoryGetFiles(sidecars, String(source), TEXT("*.meta"), DirectorySearchOption::AllDirectories))
            {
                error = TEXT("Copied folder metadata could not be enumerated.");
                return true;
            }
            for (const String& oldMeta : sidecars)
            {
                const String relative = FileSystem::ConvertAbsolutePathToRelative(String(source), oldMeta);
                const String newMeta = String(stagedSource) / relative;
                if (CloneMeta(oldMeta, newMeta, remap, nullptr, &error))
                    return true;
            }

            for (const String& oldMeta : sidecars)
            {
                const String relative = FileSystem::ConvertAbsolutePathToRelative(String(source), oldMeta);
                const String newMeta = String(stagedSource) / relative;
                const String documentPath = newMeta.Substring(0, newMeta.Length() - 5);
                AssetMeta meta;
                AssetPipelineDiagnostic diagnostic;
                if (AssetMeta::Load(newMeta, meta, diagnostic))
                {
                    error = diagnostic.Message;
                    return true;
                }
                if (!FileSystem::DirectoryExists(documentPath) &&
                    (meta.SourceKind == AssetSourceKind::TextDocument || meta.SourceKind == AssetSourceKind::ExistingJson) &&
                    RemapAuthoredDocument(documentPath, remap))
                {
                    error = TEXT("Copied authored source GUID remap could not be written.");
                    return true;
                }
            }
        }
        else
        {
            AssetMeta meta;
            AssetPipelineDiagnostic diagnostic;
            if (AssetMeta::Load(stagedMeta, meta, diagnostic))
            {
                error = diagnostic.Message;
                return true;
            }
            if ((meta.SourceKind == AssetSourceKind::TextDocument || meta.SourceKind == AssetSourceKind::ExistingJson) &&
                RemapAuthoredDocument(stagedSource, remap))
            {
                error = TEXT("Copied authored source GUID remap could not be written.");
                return true;
            }
        }
        return false;
    }

    bool RestorePath(const JournalEntry& entry)
    {
        const bool equivalent = entry.SourcePath.HasChars() && entry.DestinationPath.HasChars() &&
                                FileSystem::AreFilePathsEquivalent(entry.SourcePath, entry.DestinationPath);
        String candidate;
        if (entry.StagingPath.HasChars() && Exists(entry.StagingPath))
            candidate = entry.StagingPath;
        else if (entry.DestinationPath.HasChars() && Exists(entry.DestinationPath))
            candidate = entry.DestinationPath;

        if (!candidate.HasChars())
            return !Exists(entry.SourcePath);
        if (equivalent)
        {
            const String proxy = StagePath(entry.SourcePath, Guid::New(), TEXT("rollback"));
            if (MovePath(candidate, proxy, false) || MovePath(proxy, entry.SourcePath, false))
                return true;
        }
        else if (!Exists(entry.SourcePath))
        {
            if (MovePath(candidate, entry.SourcePath, false))
                return true;
        }
        else if (!FileSystem::AreFilePathsEquivalent(candidate, entry.SourcePath) && DeletePath(candidate))
        {
            return true;
        }
        if (entry.DestinationPath.HasChars() && !equivalent && Exists(entry.DestinationPath) && DeletePath(entry.DestinationPath))
            return true;
        if (entry.StagingPath.HasChars() && Exists(entry.StagingPath) && DeletePath(entry.StagingPath))
            return true;
        return !Exists(entry.SourcePath);
    }

    bool RestoreReplacement(const JournalEntry& entry)
    {
        if (!FileSystem::FileExists(entry.PreimagePath))
        {
            if (!FileSystem::FileExists(entry.SourcePath))
                return true;
            if (!entry.BeforeHash.HasChars())
                return false;
            String current;
            return HashFile(entry.SourcePath, current) || current != entry.BeforeHash;
        }
        const String restore = StagePath(entry.SourcePath, Guid::New(), TEXT("restore"));
        if (CopyPath(entry.PreimagePath, restore) || MovePath(restore, entry.SourcePath, true))
        {
            DeletePath(restore);
            return true;
        }
        if (entry.BeforeHash.HasChars())
        {
            String hash;
            if (HashFile(entry.SourcePath, hash) || hash != entry.BeforeHash)
                return true;
        }
        return false;
    }

    bool RecoverOldState(const AssetMutationService& service, Journal& journal)
    {
        journal.State = JournalState::RollingBack;
        SaveJournal(service.GetJournalRoot(), journal);
        bool failed = false;
        switch (journal.Operation)
        {
        case AssetMutationOperation::CreateAsset:
        case AssetMutationOperation::CreateFolder:
        case AssetMutationOperation::Copy:
            for (int32 i = journal.Entries.Count() - 1; i >= 0; i--)
            {
                const JournalEntry& entry = journal.Entries[i];
                failed |= entry.DestinationPath.HasChars() && Exists(entry.DestinationPath) && DeletePath(entry.DestinationPath);
                failed |= entry.StagingPath.HasChars() && Exists(entry.StagingPath) && DeletePath(entry.StagingPath);
                failed |= entry.PreimagePath.HasChars() && Exists(entry.PreimagePath) && DeletePath(entry.PreimagePath);
            }
            if (journal.Entries.HasItems())
                failed |= !PairAbsent(journal.Entries[0].DestinationPath);
            break;
        case AssetMutationOperation::Move:
        case AssetMutationOperation::Rename:
        case AssetMutationOperation::DeleteToRecovery:
        case AssetMutationOperation::Recover:
            for (int32 i = journal.Entries.Count() - 1; i >= 0; i--)
                failed |= RestorePath(journal.Entries[i]);
            if (journal.Entries.HasItems())
                failed |= !PairExists(journal.Entries[0].SourcePath);
            break;
        case AssetMutationOperation::ReplaceContents:
            if (journal.Entries.IsEmpty())
                failed = true;
            else
                failed |= RestoreReplacement(journal.Entries[0]);
            for (const JournalEntry& entry : journal.Entries)
            {
                failed |= entry.StagingPath.HasChars() && Exists(entry.StagingPath) && DeletePath(entry.StagingPath);
                failed |= entry.PreimagePath.HasChars() && Exists(entry.PreimagePath) && DeletePath(entry.PreimagePath);
            }
            failed |= journal.Entries.HasItems() && !PairExists(journal.Entries[0].SourcePath);
            break;
        case AssetMutationOperation::ReplaceAsset:
            if (journal.Entries.Count() != 2)
                failed = true;
            else
            {
                for (const JournalEntry& entry : journal.Entries)
                    failed |= RestoreReplacement(entry);
            }
            for (const JournalEntry& entry : journal.Entries)
            {
                failed |= entry.StagingPath.HasChars() && Exists(entry.StagingPath) && DeletePath(entry.StagingPath);
                failed |= entry.PreimagePath.HasChars() && Exists(entry.PreimagePath) && DeletePath(entry.PreimagePath);
            }
            failed |= journal.Entries.HasItems() && !PairExists(journal.Entries[0].SourcePath);
            break;
        default:
            failed = true;
            break;
        }
        journal.State = failed ? JournalState::RecoveryRequired : JournalState::RolledBack;
        if (SaveJournal(service.GetJournalRoot(), journal))
            return true;
        if (!failed)
            DeleteJournal(service.GetJournalRoot(), journal.ID);
        return failed;
    }

    bool CompleteCommittedState(const AssetMutationService& service, Journal& journal)
    {
        bool failed = false;
        if (journal.Operation == AssetMutationOperation::ReplaceContents || journal.Operation == AssetMutationOperation::ReplaceAsset)
        {
            if (journal.Entries.IsEmpty() || !PairExists(journal.Entries[0].SourcePath))
                failed = true;
            for (const JournalEntry& entry : journal.Entries)
            {
                if (!entry.StagedHash.HasChars())
                    continue;
                String current;
                failed |= HashFile(entry.SourcePath, current) || current != entry.StagedHash;
            }
        }
        else
        {
            for (JournalEntry& entry : journal.Entries)
            {
                if (!Exists(entry.DestinationPath))
                {
                    const String candidate = Exists(entry.StagingPath) ? entry.StagingPath : entry.SourcePath;
                    if (!candidate.HasChars() || !Exists(candidate) || MovePath(candidate, entry.DestinationPath, false))
                        failed = true;
                }
            }
            if (!journal.Entries.HasItems() || !PairExists(journal.Entries[0].DestinationPath))
                failed = true;
        }
        if (failed)
            return true;
        for (const JournalEntry& entry : journal.Entries)
        {
            if (entry.StagingPath.HasChars() && Exists(entry.StagingPath))
                failed |= DeletePath(entry.StagingPath);
            if (entry.PreimagePath.HasChars() && Exists(entry.PreimagePath))
                failed |= DeletePath(entry.PreimagePath);
            if ((journal.Operation == AssetMutationOperation::Move || journal.Operation == AssetMutationOperation::Rename ||
                 journal.Operation == AssetMutationOperation::DeleteToRecovery || journal.Operation == AssetMutationOperation::Recover) &&
                !FileSystem::AreFilePathsEquivalent(entry.SourcePath, entry.DestinationPath) && Exists(entry.SourcePath))
                failed = true;
        }
        if (!failed)
            DeleteJournal(service.GetJournalRoot(), journal.ID);
        return failed;
    }

    bool AbortTransaction(const AssetMutationService& service, Journal& journal, AssetMutationResult& result,
        AssetMutationFailure failure, const StringView& message)
    {
        journal.LastError = message;
        if (RecoverOldState(service, journal))
            return Fail(result, AssetMutationFailure::RecoveryRequired, journal.ID,
                journal.Entries.HasItems() ? journal.Entries[0].SourcePath : StringView(),
                journal.Entries.HasItems() ? journal.Entries[0].DestinationPath : StringView(), message, true);
        return Fail(result, failure, journal.ID,
            journal.Entries.HasItems() ? journal.Entries[0].SourcePath : StringView(),
            journal.Entries.HasItems() ? journal.Entries[0].DestinationPath : StringView(), message);
    }

    bool CommitTransaction(AssetMutationService& service, Journal& journal, AssetMutationResult& result, const Guid& assetID = Guid())
    {
        journal.State = JournalState::Committed;
        if (SaveJournal(service.GetJournalRoot(), journal))
            return AbortTransaction(service, journal, result, AssetMutationFailure::JournalFailure, TEXT("Mutation commit marker could not be persisted."));
        bool cleaned = true;
        for (const JournalEntry& entry : journal.Entries)
        {
            if (entry.StagingPath.HasChars() && Exists(entry.StagingPath))
                cleaned &= !DeletePath(entry.StagingPath);
            if (entry.PreimagePath.HasChars() && Exists(entry.PreimagePath))
                cleaned &= !DeletePath(entry.PreimagePath);
        }
        if (cleaned)
            DeleteJournal(service.GetJournalRoot(), journal.ID);
        Succeed(result, journal.ID, journal.Entries[0].SourcePath, journal.Entries[0].DestinationPath);
        result.AssetID = assetID;
        result.RecoveryPath = journal.RecoveryPath;
        for (const JournalEntry& entry : journal.Entries)
        {
            if (entry.SourcePath.HasChars() && IsSameOrChild(entry.SourcePath, service.GetContentRoot()) && !result.ChangedPaths.Contains(entry.SourcePath))
                result.ChangedPaths.Add(entry.SourcePath);
            if (entry.DestinationPath.HasChars() && IsSameOrChild(entry.DestinationPath, service.GetContentRoot()) && !result.ChangedPaths.Contains(entry.DestinationPath))
                result.ChangedPaths.Add(entry.DestinationPath);
        }
        if (service.CommittedHook.IsBinded())
            service.CommittedHook(result);
        return false;
    }
}

AssetMutationService::AssetMutationService(const StringView& projectRoot, const StringView& contentRoot, const StringView& journalRoot, const StringView& recoveryRoot,
    AssetMutationInvocation invocation)
    : _projectRoot(NormalizePath(projectRoot))
    , _contentRoot(NormalizePath(contentRoot))
    , _journalRoot(NormalizePath(journalRoot))
    , _recoveryRoot(NormalizePath(recoveryRoot))
    , _sessionID(Guid::New())
    , _invocation(invocation)
{
}

bool AssetMutationService::Validate(AssetMutationOperation operation, const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result) const
{
    const Guid id = Guid::New();
    result = AssetMutationResult();
    if (_projectRoot.IsEmpty() || _contentRoot.IsEmpty() || _journalRoot.IsEmpty() || _recoveryRoot.IsEmpty() ||
        IsSameOrChild(_journalRoot, _contentRoot) || IsSameOrChild(_recoveryRoot, _contentRoot))
        return Fail(result, AssetMutationFailure::InvalidDestination, id, sourcePath, destinationPath, TEXT("Mutation service roots are empty or recovery storage is inside Content."));

    String source;
    String destination;
    if (operation == AssetMutationOperation::CreateFolder || operation == AssetMutationOperation::CreateAsset)
    {
        const StringView input = destinationPath.HasChars() ? destinationPath : sourcePath;
        if (ResolveContentPath(*this, input, destination, result, id, true))
            return true;
        if (!PairAbsent(destination))
            return Fail(result, AssetMutationFailure::DestinationCollision, id, StringView(), destination, TEXT("Asset destination or its metadata sidecar already exists."));
        const String parent(StringUtils::GetDirectoryName(destination));
        if (!FileSystem::DirectoryExists(parent))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, StringView(), destination, TEXT("Asset destination parent does not exist."));
        Succeed(result, id, StringView(), destination);
        return false;
    }

    if (operation == AssetMutationOperation::Recover)
    {
        source = FileSystem::IsRelative(sourcePath) ? _recoveryRoot / String(sourcePath) : String(sourcePath);
        source = NormalizePath(source);
        if (!IsSameOrChild(source, _recoveryRoot) || source == _recoveryRoot)
            return Fail(result, AssetMutationFailure::InvalidSource, id, source, destinationPath, TEXT("Recovery source is outside the configured recovery root."));
        if (ResolveContentPath(*this, destinationPath, destination, result, id, true))
            return true;
    }
    else
    {
        if (ResolveContentPath(*this, sourcePath, source, result, id, false))
            return true;
        if (operation == AssetMutationOperation::Copy || operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename)
        {
            if (ResolveContentPath(*this, destinationPath, destination, result, id, true))
                return true;
        }
    }

    if (ValidateSourcePair(source, result, id))
        return true;
    if (operation == AssetMutationOperation::Copy || operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename || operation == AssetMutationOperation::Recover)
    {
        const bool allowEquivalent = operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename;
        if (allowEquivalent && source == destination)
            return Fail(result, AssetMutationFailure::InvalidDestination, id, source, destination, TEXT("Move or rename destination is identical to the source."));
        if (ValidateDestination(source, destination, allowEquivalent, result, id))
            return true;
        if ((operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename) && !IsSameVolume(source, destination))
            return Fail(result, AssetMutationFailure::UnsupportedCrossVolumeMove, id, source, destination, TEXT("Cross-volume Content moves cannot provide rename atomicity."));
    }
    else if (operation == AssetMutationOperation::ReplaceContents)
    {
        String replacement = FileSystem::IsRelative(destinationPath) ? _projectRoot / String(destinationPath) : String(destinationPath);
        replacement = NormalizePath(replacement);
        if (!FileSystem::FileExists(replacement) || FileSystem::DirectoryExists(source))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, source, replacement, TEXT("ReplaceContents requires an existing replacement file and a file source."));
        destination = replacement;
    }
    else if (operation == AssetMutationOperation::ReplaceAsset && FileSystem::DirectoryExists(source))
    {
        return Fail(result, AssetMutationFailure::InvalidSource, id, source, StringView(), TEXT("ReplaceAsset requires a file source."));
    }
    Succeed(result, id, source, destination);
    return false;
}

bool AssetMutationService::CreateAsset(const StringView& path, const StringAnsiView& sourceContents, const AssetMeta& meta, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::CreateAsset, path, StringView(), validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    String destination = validation.DestinationPath;
    Array<String> paths;
    paths.Add(destination);
    paths.Add(destination + TEXT(".meta"));
    PathLockScope lock;
    if (lock.Acquire(paths))
        return Fail(result, AssetMutationFailure::PathBusy, id, StringView(), destination, TEXT("A conflicting asset mutation already owns this path."));
    if (!PairAbsent(destination))
        return Fail(result, AssetMutationFailure::DestinationCollision, id, StringView(), destination, TEXT("Asset destination appeared after preflight."));

    String replacement;
    PathLockScope replacementLock;
    if (RunDecisionHook(*this, AssetMutationOperation::CreateAsset, id, StringView(), destination, false, result, &replacement))
        return !result.Succeeded;
    if (replacement.HasChars())
    {
        String adjusted;
        if (ResolveContentPath(*this, replacement, adjusted, result, id, true))
            return true;
        Array<String> adjustedPaths;
        adjustedPaths.Add(adjusted);
        adjustedPaths.Add(adjusted + TEXT(".meta"));
        if (replacementLock.Acquire(adjustedPaths))
            return Fail(result, AssetMutationFailure::PathBusy, id, StringView(), adjusted, TEXT("Callback replacement path is owned by a conflicting mutation."));
        if (!PairAbsent(adjusted) || !FileSystem::DirectoryExists(StringUtils::GetDirectoryName(adjusted)))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, StringView(), adjusted, TEXT("Callback replacement path failed revalidation."));
        destination = adjusted;
    }
    if (!meta.ID.IsValid() || meta.FolderAsset || meta.AssetType.IsEmpty() ||
        (meta.SourceKind != AssetSourceKind::TextDocument && meta.SourceKind != AssetSourceKind::ExistingJson))
        return Fail(result, AssetMutationFailure::InvalidMetadata, id, StringView(), destination, TEXT("Authored asset metadata is invalid or not source-owned."));

    Journal journal;
    journal.ID = id;
    journal.Operation = AssetMutationOperation::CreateAsset;
    JournalEntry sourceEntry;
    sourceEntry.Role = TEXT("Source");
    sourceEntry.DestinationPath = destination;
    sourceEntry.StagingPath = StagePath(destination, id, TEXT("source"));
    journal.Entries.Add(sourceEntry);
    JournalEntry metaEntry;
    metaEntry.Role = TEXT("Metadata");
    metaEntry.DestinationPath = destination + TEXT(".meta");
    metaEntry.StagingPath = StagePath(metaEntry.DestinationPath, id, TEXT("meta"));
    journal.Entries.Add(metaEntry);
    if (SaveJournal(_journalRoot, journal))
        return Fail(result, AssetMutationFailure::JournalFailure, id, StringView(), destination, TEXT("Asset creation journal could not be persisted."));

    AssetPipelineDiagnostic metaDiagnostic;
    if (File::WriteAllBytes(journal.Entries[0].StagingPath, sourceContents.Get(), sourceContents.Length()) ||
        FlushWrittenFile(journal.Entries[0].StagingPath) || AssetMeta::SaveAtomic(journal.Entries[1].StagingPath, meta, metaDiagnostic) ||
        HashFile(journal.Entries[0].StagingPath, journal.Entries[0].StagedHash) ||
        HashFile(journal.Entries[1].StagingPath, journal.Entries[1].StagedHash))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed,
            metaDiagnostic.Message.HasChars() ? metaDiagnostic.Message : TEXT("Authored source or metadata staging failed."));
    journal.Entries[0].StagingComplete = true;
    journal.Entries[1].StagingComplete = true;
    journal.State = JournalState::Staged;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Asset creation staged state could not be persisted."));
    journal.State = JournalState::Publishing;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Asset creation publishing state could not be persisted."));
    for (int32 i = 0; i < journal.Entries.Count(); i++)
    {
        JournalEntry& entry = journal.Entries[i];
        if (MovePath(entry.StagingPath, entry.DestinationPath, false))
            return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Authored source pair could not be published."));
        entry.Published = true;
        if (SaveJournal(_journalRoot, journal))
            return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Asset creation publication step could not be journaled."));
    }
    String sourceHash;
    String metaHash;
    AssetMeta verifiedMeta;
    if (HashFile(destination, sourceHash) || sourceHash != journal.Entries[0].StagedHash ||
        HashFile(destination + TEXT(".meta"), metaHash) || metaHash != journal.Entries[1].StagedHash ||
        AssetMeta::Load(destination + TEXT(".meta"), verifiedMeta, metaDiagnostic) || verifiedMeta.ID != meta.ID)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Published authored source pair failed verification."));
    return CommitTransaction(*this, journal, result, meta.ID);
}

bool AssetMutationService::CreateFolder(const StringView& path, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::CreateFolder, path, StringView(), validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    String destination = validation.DestinationPath;
    Array<String> paths;
    paths.Add(destination);
    paths.Add(destination + TEXT(".meta"));
    PathLockScope lock;
    if (lock.Acquire(paths))
        return Fail(result, AssetMutationFailure::PathBusy, id, StringView(), destination, TEXT("A conflicting asset mutation already owns this path."));
    if (!PairAbsent(destination))
        return Fail(result, AssetMutationFailure::DestinationCollision, id, StringView(), destination, TEXT("Folder destination appeared after preflight."));
    String replacement;
    PathLockScope replacementLock;
    if (RunDecisionHook(*this, AssetMutationOperation::CreateFolder, id, StringView(), destination, true, result, &replacement))
        return !result.Succeeded;
    if (replacement.HasChars())
    {
        String adjusted;
        if (ResolveContentPath(*this, replacement, adjusted, result, id, true))
            return true;
        Array<String> adjustedPaths;
        adjustedPaths.Add(adjusted);
        adjustedPaths.Add(adjusted + TEXT(".meta"));
        if (replacementLock.Acquire(adjustedPaths))
            return Fail(result, AssetMutationFailure::PathBusy, id, StringView(), adjusted, TEXT("Callback replacement path is owned by a conflicting mutation."));
        if (!PairAbsent(adjusted) || !FileSystem::DirectoryExists(StringUtils::GetDirectoryName(adjusted)))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, StringView(), adjusted, TEXT("Callback replacement folder path failed revalidation."));
        destination = adjusted;
    }
    const String destinationMeta = destination + TEXT(".meta");

    Journal journal;
    journal.ID = id;
    journal.Operation = AssetMutationOperation::CreateFolder;
    JournalEntry sourceEntry;
    sourceEntry.Role = TEXT("Source");
    sourceEntry.DestinationPath = destination;
    sourceEntry.StagingPath = StagePath(destination, id, TEXT("stage"));
    sourceEntry.IsDirectory = true;
    journal.Entries.Add(sourceEntry);
    JournalEntry metaEntry;
    metaEntry.Role = TEXT("Metadata");
    metaEntry.DestinationPath = destinationMeta;
    metaEntry.StagingPath = StagePath(destinationMeta, id, TEXT("stage"));
    journal.Entries.Add(metaEntry);
    if (SaveJournal(_journalRoot, journal))
        return Fail(result, AssetMutationFailure::JournalFailure, id, StringView(), destination, TEXT("Create-folder journal could not be persisted."));

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.FolderAsset = true;
    meta.AssetType = TEXT("FlaxEngine.Folder");
    meta.SourceKind = AssetSourceKind::Folder;
    meta.Processor.ID = TEXT("Flax.Folder");
    AssetPipelineDiagnostic diagnostic;
    if (FileSystem::CreateDirectory(sourceEntry.StagingPath))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::PermissionDenied, TEXT("Folder staging could not be created."));
    journal.Entries[0].StagingComplete = true;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Folder staging step could not be journaled."));
    if (AssetMeta::SaveAtomic(metaEntry.StagingPath, meta, diagnostic))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::PermissionDenied, diagnostic.Message);
    journal.Entries[1].StagingComplete = true;
    journal.State = JournalState::Staged;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Create-folder staged state could not be persisted."));
    journal.State = JournalState::Publishing;
    if (SaveJournal(_journalRoot, journal) || MovePath(sourceEntry.StagingPath, destination))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Folder source could not be published."));
    journal.Entries[0].Published = true;
    if (SaveJournal(_journalRoot, journal) || MovePath(metaEntry.StagingPath, destinationMeta))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Folder metadata could not be published."));
    journal.Entries[1].Published = true;
    if (SaveJournal(_journalRoot, journal) || !PairExists(destination))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Folder source/metadata pair failed final verification."));
    return CommitTransaction(*this, journal, result, meta.ID);
}

bool AssetMutationService::Copy(const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::Copy, sourcePath, destinationPath, validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    const String source = validation.SourcePath;
    String destination = validation.DestinationPath;
    const String sourceMeta = source + TEXT(".meta");
    Array<String> paths;
    paths.Add(source);
    paths.Add(sourceMeta);
    paths.Add(destination);
    paths.Add(destination + TEXT(".meta"));
    PathLockScope lock;
    if (lock.Acquire(paths))
        return Fail(result, AssetMutationFailure::PathBusy, id, source, destination, TEXT("A conflicting asset mutation already owns this path."));
    if (ValidateSourcePair(source, result, id) || ValidateDestination(source, destination, false, result, id))
        return true;
    String replacement;
    PathLockScope replacementLock;
    if (RunDecisionHook(*this, AssetMutationOperation::Copy, id, source, destination, FileSystem::DirectoryExists(source), result, &replacement))
        return !result.Succeeded;
    if (replacement.HasChars())
    {
        String adjusted;
        if (ResolveContentPath(*this, replacement, adjusted, result, id, true))
            return true;
        Array<String> adjustedPaths;
        adjustedPaths.Add(adjusted);
        adjustedPaths.Add(adjusted + TEXT(".meta"));
        if (replacementLock.Acquire(adjustedPaths))
            return Fail(result, AssetMutationFailure::PathBusy, id, source, adjusted, TEXT("Callback replacement path is owned by a conflicting mutation."));
        if (ValidateDestination(source, adjusted, false, result, id))
            return true;
        destination = adjusted;
    }
    const String destinationMeta = destination + TEXT(".meta");

    Journal journal;
    journal.ID = id;
    journal.Operation = AssetMutationOperation::Copy;
    JournalEntry sourceEntry;
    sourceEntry.Role = TEXT("Source");
    sourceEntry.SourcePath = source;
    sourceEntry.DestinationPath = destination;
    sourceEntry.StagingPath = StagePath(destination, id, TEXT("stage"));
    sourceEntry.IsDirectory = FileSystem::DirectoryExists(source);
    if (HashPath(source, sourceEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, destination, TEXT("Copy source observation could not be captured."));
    journal.Entries.Add(sourceEntry);
    JournalEntry metaEntry;
    metaEntry.Role = TEXT("Metadata");
    metaEntry.SourcePath = sourceMeta;
    metaEntry.DestinationPath = destinationMeta;
    metaEntry.StagingPath = StagePath(destinationMeta, id, TEXT("stage"));
    if (HashFile(sourceMeta, metaEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, destination, TEXT("Copy metadata observation could not be captured."));
    journal.Entries.Add(metaEntry);
    if (SaveJournal(_journalRoot, journal))
        return Fail(result, AssetMutationFailure::JournalFailure, id, source, destination, TEXT("Copy journal could not be persisted."));

    Guid copiedID;
    String cloneError;
    if (CopyPath(source, sourceEntry.StagingPath))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed, TEXT("Source staging copy failed."));
    String stagedPreClone;
    if (HashPath(sourceEntry.StagingPath, stagedPreClone) || stagedPreClone != sourceEntry.BeforeHash)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Source staging copy differs from the preflight source observation."));
    journal.Entries[0].StagingComplete = true;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Source staging step could not be journaled."));
    if (ClonePairMetadata(source, sourceEntry.StagingPath, metaEntry.StagingPath, copiedID, cloneError))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed, cloneError.HasChars() ? cloneError : TEXT("Metadata staging copy failed."));
    journal.Entries[1].StagingComplete = true;
    if (HashPath(sourceEntry.StagingPath, journal.Entries[0].StagedHash))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Staged copy could not be fingerprinted."));
    HashFile(metaEntry.StagingPath, journal.Entries[1].StagedHash);
    journal.State = JournalState::Staged;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Copy staged state could not be persisted."));
    journal.State = JournalState::Publishing;
    String currentSourceHash;
    if (HashPath(source, currentSourceHash) || currentSourceHash != sourceEntry.BeforeHash)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Copy source changed after preflight."));
    if (SaveJournal(_journalRoot, journal) || MovePath(sourceEntry.StagingPath, destination))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Copied source could not be published."));
    journal.Entries[0].Published = true;
    if (SaveJournal(_journalRoot, journal) || MovePath(metaEntry.StagingPath, destinationMeta))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Copied metadata could not be published."));
    journal.Entries[1].Published = true;
    if (SaveJournal(_journalRoot, journal) || !PairExists(destination))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Copied source/metadata pair failed final verification."));
    String hash;
    if (HashPath(destination, hash) || hash != journal.Entries[0].StagedHash)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Published copy differs from staged source bytes."));
    return CommitTransaction(*this, journal, result, copiedID);
}

namespace
{
    bool MovePairOperation(AssetMutationService& service, AssetMutationOperation operation, const Guid& id,
        const StringView& source, const StringView& destination, AssetMutationResult& result)
    {
        String finalDestination(destination);
        const String sourceMeta = String(source) + TEXT(".meta");
        Array<String> paths;
        paths.Add(String(source));
        paths.Add(sourceMeta);
        paths.Add(finalDestination);
        paths.Add(finalDestination + TEXT(".meta"));
        PathLockScope lock;
        if (lock.Acquire(paths))
            return Fail(result, AssetMutationFailure::PathBusy, id, source, finalDestination, TEXT("A conflicting asset mutation already owns this path."));
        if (ValidateSourcePair(source, result, id))
            return true;
        const bool allowEquivalent = operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename;
        if (operation == AssetMutationOperation::DeleteToRecovery)
        {
            if (!PairAbsent(finalDestination))
                return Fail(result, AssetMutationFailure::DestinationCollision, id, source, finalDestination, TEXT("Delete recovery destination already exists."));
        }
        else if (ValidateDestination(source, finalDestination, allowEquivalent, result, id))
        {
            return true;
        }
        if ((operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename) && !IsSameVolume(source, finalDestination))
            return Fail(result, AssetMutationFailure::UnsupportedCrossVolumeMove, id, source, finalDestination, TEXT("Cross-volume Content moves cannot provide rename atomicity."));
        String replacement;
        PathLockScope replacementLock;
        if (RunDecisionHook(service, operation, id, source, finalDestination, FileSystem::DirectoryExists(source), result, &replacement))
            return !result.Succeeded;
        if (replacement.HasChars())
        {
            if (operation == AssetMutationOperation::DeleteToRecovery)
                return Fail(result, AssetMutationFailure::InvalidDestination, id, source, finalDestination, TEXT("Delete callbacks cannot replace the service-owned recovery path."));
            String adjusted;
            AssetMutationResult adjustedResult;
            if (ResolveContentPath(service, replacement, adjusted, adjustedResult, id, true))
            {
                result = MoveTemp(adjustedResult);
                return true;
            }
            Array<String> adjustedPaths;
            adjustedPaths.Add(adjusted);
            adjustedPaths.Add(adjusted + TEXT(".meta"));
            if (replacementLock.Acquire(adjustedPaths))
                return Fail(result, AssetMutationFailure::PathBusy, id, source, adjusted, TEXT("Callback replacement path is owned by a conflicting mutation."));
            if (ValidateDestination(source, adjusted, allowEquivalent, result, id))
                return true;
            if ((operation == AssetMutationOperation::Move || operation == AssetMutationOperation::Rename) && !IsSameVolume(source, adjusted))
                return Fail(result, AssetMutationFailure::UnsupportedCrossVolumeMove, id, source, adjusted, TEXT("Callback replacement path is on another volume."));
            finalDestination = adjusted;
        }
        const String destinationMeta = finalDestination + TEXT(".meta");

        AssetMeta originalMeta;
        AssetPipelineDiagnostic metaDiagnostic;
        if (AssetMeta::Load(sourceMeta, originalMeta, metaDiagnostic))
            return Fail(result, AssetMutationFailure::InvalidMetadata, id, source, finalDestination, metaDiagnostic.Message);

        Journal journal;
        journal.ID = id;
        journal.Operation = operation;
        if (operation == AssetMutationOperation::DeleteToRecovery)
            journal.RecoveryPath = finalDestination;
        JournalEntry sourceEntry;
        sourceEntry.Role = TEXT("Source");
        sourceEntry.SourcePath = source;
        sourceEntry.DestinationPath = finalDestination;
        sourceEntry.StagingPath = StagePath(finalDestination, id, TEXT("stage"));
        sourceEntry.IsDirectory = FileSystem::DirectoryExists(source);
        if (HashPath(source, sourceEntry.BeforeHash))
            return Fail(result, AssetMutationFailure::LockedStorage, id, source, finalDestination, TEXT("Move source observation could not be captured."));
        journal.Entries.Add(sourceEntry);
        JournalEntry metaEntry;
        metaEntry.Role = TEXT("Metadata");
        metaEntry.SourcePath = sourceMeta;
        metaEntry.DestinationPath = destinationMeta;
        metaEntry.StagingPath = StagePath(destinationMeta, id, TEXT("stage"));
        if (HashFile(sourceMeta, metaEntry.BeforeHash))
            return Fail(result, AssetMutationFailure::LockedStorage, id, source, finalDestination, TEXT("Move metadata observation could not be captured."));
        journal.Entries.Add(metaEntry);
        if (SaveJournal(service.GetJournalRoot(), journal))
            return Fail(result, AssetMutationFailure::JournalFailure, id, source, finalDestination, TEXT("Move journal could not be persisted."));
        String currentSourceHash;
        String currentMetaHash;
        if (HashPath(source, currentSourceHash) || currentSourceHash != sourceEntry.BeforeHash ||
            HashFile(sourceMeta, currentMetaHash) || currentMetaHash != metaEntry.BeforeHash)
            return AbortTransaction(service, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Move source or metadata changed after preflight."));
        if (EnsureDirectoryFor(finalDestination) || MovePath(source, sourceEntry.StagingPath))
            return AbortTransaction(service, journal, result, AssetMutationFailure::MoveFailed, TEXT("Source could not be moved into staging."));
        String stagedHash;
        if (HashPath(sourceEntry.StagingPath, stagedHash) || stagedHash != sourceEntry.BeforeHash)
            return AbortTransaction(service, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Staged move source differs from the preflight observation."));
        journal.Entries[0].StagingComplete = true;
        if (SaveJournal(service.GetJournalRoot(), journal) || MovePath(sourceMeta, metaEntry.StagingPath))
            return AbortTransaction(service, journal, result, AssetMutationFailure::MoveFailed, TEXT("Metadata could not be moved into staging."));
        String stagedMetaHash;
        if (HashFile(metaEntry.StagingPath, stagedMetaHash) || stagedMetaHash != metaEntry.BeforeHash)
            return AbortTransaction(service, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Staged metadata differs from the preflight observation."));
        journal.Entries[1].StagingComplete = true;
        journal.State = JournalState::Staged;
        if (SaveJournal(service.GetJournalRoot(), journal))
            return AbortTransaction(service, journal, result, AssetMutationFailure::JournalFailure, TEXT("Move staged state could not be persisted."));
        journal.State = JournalState::Publishing;
        if (SaveJournal(service.GetJournalRoot(), journal) || MovePath(sourceEntry.StagingPath, finalDestination))
            return AbortTransaction(service, journal, result, AssetMutationFailure::MoveFailed, TEXT("Source could not be published at its destination."));
        journal.Entries[0].Published = true;
        if (SaveJournal(service.GetJournalRoot(), journal) || MovePath(metaEntry.StagingPath, destinationMeta))
            return AbortTransaction(service, journal, result, AssetMutationFailure::MoveFailed, TEXT("Metadata could not be published at its destination."));
        journal.Entries[1].Published = true;
        if (SaveJournal(service.GetJournalRoot(), journal) || !PairExists(finalDestination))
            return AbortTransaction(service, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Published source/metadata pair failed final verification."));
        AssetMeta publishedMeta;
        if (AssetMeta::Load(destinationMeta, publishedMeta, metaDiagnostic) || publishedMeta.ID != originalMeta.ID)
            return AbortTransaction(service, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Move did not preserve the source GUID."));
        return CommitTransaction(service, journal, result, originalMeta.ID);
    }
}

bool AssetMutationService::Move(const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::Move, sourcePath, destinationPath, validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    return MovePairOperation(*this, AssetMutationOperation::Move, validation.TransactionID, validation.SourcePath, validation.DestinationPath, result);
}

bool AssetMutationService::Rename(const StringView& sourcePath, const StringView& newName, AssetMutationResult& result)
{
    if (newName.IsEmpty() || newName.Contains(TEXT("/")) || newName.Contains(TEXT("\\")))
        return Fail(result, AssetMutationFailure::InvalidDestination, Guid::New(), sourcePath, newName, TEXT("Rename requires one portable basename, not a path."));
    AssetMutationResult sourceValidation;
    String source;
    const Guid id = Guid::New();
    if (ResolveContentPath(*this, sourcePath, source, sourceValidation, id, false))
    {
        result = MoveTemp(sourceValidation);
        return true;
    }
    const String destination = String(StringUtils::GetDirectoryName(source)) / String(newName);
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::Rename, source, destination, validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    return MovePairOperation(*this, AssetMutationOperation::Rename, validation.TransactionID, validation.SourcePath, validation.DestinationPath, result);
}

bool AssetMutationService::DeleteToRecovery(const StringView& sourcePath, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::DeleteToRecovery, sourcePath, StringView(), validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    const String source = validation.SourcePath;
    const String recoveryDirectory = _recoveryRoot / GuidText(id);
    const String destination = recoveryDirectory / String(StringUtils::GetFileName(source));
    if (Exists(destination) || Exists(destination + TEXT(".meta")))
        return Fail(result, AssetMutationFailure::DestinationCollision, id, source, destination, TEXT("Delete recovery destination already exists."));
    return MovePairOperation(*this, AssetMutationOperation::DeleteToRecovery, id, source, destination, result);
}

bool AssetMutationService::Recover(const StringView& recoveryPath, const StringView& destinationPath, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::Recover, recoveryPath, destinationPath, validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    return MovePairOperation(*this, AssetMutationOperation::Recover, validation.TransactionID, validation.SourcePath, validation.DestinationPath, result);
}

bool AssetMutationService::ReplaceContents(const StringView& sourcePath, const StringView& replacementPath, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::ReplaceContents, sourcePath, replacementPath, validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    const String source = validation.SourcePath;
    String replacement = validation.DestinationPath;
    const String metaPath = source + TEXT(".meta");
    Array<String> paths;
    paths.Add(source);
    paths.Add(metaPath);
    paths.Add(replacement);
    PathLockScope lock;
    if (lock.Acquire(paths))
        return Fail(result, AssetMutationFailure::PathBusy, id, source, replacement, TEXT("A conflicting asset mutation already owns this path."));
    if (ValidateSourcePair(source, result, id) || !FileSystem::FileExists(replacement))
        return true;
    String callbackReplacement;
    PathLockScope replacementLock;
    if (RunDecisionHook(*this, AssetMutationOperation::ReplaceContents, id, source, replacement, false, result, &callbackReplacement))
        return !result.Succeeded;
    if (callbackReplacement.HasChars())
    {
        String adjusted = FileSystem::IsRelative(callbackReplacement) ? _projectRoot / callbackReplacement : callbackReplacement;
        adjusted = NormalizePath(adjusted);
        Array<String> adjustedPaths;
        adjustedPaths.Add(adjusted);
        if (replacementLock.Acquire(adjustedPaths))
            return Fail(result, AssetMutationFailure::PathBusy, id, source, adjusted, TEXT("Callback replacement source is owned by a conflicting mutation."));
        if (!FileSystem::FileExists(adjusted))
            return Fail(result, AssetMutationFailure::InvalidDestination, id, source, adjusted, TEXT("Callback replacement source does not exist."));
        replacement = adjusted;
    }

    AssetMeta meta;
    AssetPipelineDiagnostic metaDiagnostic;
    if (AssetMeta::Load(metaPath, meta, metaDiagnostic))
        return Fail(result, AssetMutationFailure::InvalidMetadata, id, source, replacement, metaDiagnostic.Message);
    Journal journal;
    journal.ID = id;
    journal.Operation = AssetMutationOperation::ReplaceContents;
    JournalEntry sourceEntry;
    sourceEntry.Role = TEXT("Source");
    sourceEntry.SourcePath = source;
    sourceEntry.DestinationPath = source;
    sourceEntry.StagingPath = StagePath(source, id, TEXT("stage"));
    sourceEntry.PreimagePath = _journalRoot / TEXT("Preimages") / GuidText(id) / String(StringUtils::GetFileName(source));
    if (HashFile(source, sourceEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, replacement, TEXT("Existing source could not be read for replacement."));
    journal.Entries.Add(sourceEntry);
    JournalEntry metaEntry;
    metaEntry.Role = TEXT("Metadata");
    metaEntry.SourcePath = metaPath;
    metaEntry.DestinationPath = metaPath;
    if (HashFile(metaPath, metaEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, replacement, TEXT("Existing metadata could not be read for replacement."));
    journal.Entries.Add(metaEntry);
    if (SaveJournal(_journalRoot, journal))
        return Fail(result, AssetMutationFailure::JournalFailure, id, source, replacement, TEXT("Replacement journal could not be persisted."));

    String replacementHashBefore;
    String replacementHashAfter;
    if (HashFile(replacement, replacementHashBefore) || CopyPath(source, sourceEntry.PreimagePath) || !SameFileHash(source, sourceEntry.PreimagePath))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed, TEXT("Verified source preimage capture failed."));
    journal.Entries[0].PreimageComplete = true;
    if (SaveJournal(_journalRoot, journal) || CopyPath(replacement, sourceEntry.StagingPath) ||
        HashFile(sourceEntry.StagingPath, journal.Entries[0].StagedHash) || HashFile(replacement, replacementHashAfter) || replacementHashBefore != replacementHashAfter)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed, TEXT("Replacement staging failed or its source changed during capture."));
    journal.Entries[0].StagingComplete = true;
    journal.State = JournalState::Staged;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Replacement staged state could not be persisted."));
    journal.State = JournalState::Publishing;
    if (SaveJournal(_journalRoot, journal) || MovePath(sourceEntry.StagingPath, source, true))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Replacement source could not be atomically published."));
    journal.Entries[0].Published = true;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Replacement publication step could not be journaled."));
    String publishedHash;
    String metaHash;
    if (HashFile(source, publishedHash) || publishedHash != journal.Entries[0].StagedHash ||
        HashFile(metaPath, metaHash) || metaHash != journal.Entries[1].BeforeHash)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Replacement publication or retained metadata failed verification."));
    return CommitTransaction(*this, journal, result, meta.ID);
}

bool AssetMutationService::ReplaceContents(const StringView& sourcePath, const StringAnsiView& sourceContents, AssetMutationResult& result)
{
    const String stagingDirectory = _journalRoot / TEXT("Replacements");
    const String replacementPath = stagingDirectory / GuidText(Guid::New()) + TEXT(".stage");
    if ((!FileSystem::DirectoryExists(stagingDirectory) && FileSystem::CreateDirectory(stagingDirectory)) ||
        File::WriteAllBytes(replacementPath, sourceContents.Get(), sourceContents.Length()))
        return Fail(result, AssetMutationFailure::CopyFailed, Guid::New(), sourcePath, replacementPath, TEXT("Replacement bytes could not be staged."));
    const bool failed = ReplaceContents(sourcePath, replacementPath, result);
    FileSystem::DeleteFile(replacementPath);
    return failed;
}

bool AssetMutationService::ReplaceAsset(const StringView& sourcePath, const StringAnsiView& sourceContents, const AssetMeta& meta, AssetMutationResult& result)
{
    AssetMutationResult validation;
    if (Validate(AssetMutationOperation::ReplaceAsset, sourcePath, StringView(), validation))
    {
        result = MoveTemp(validation);
        return true;
    }
    const Guid id = validation.TransactionID;
    const String source = validation.SourcePath;
    const String metaPath = source + TEXT(".meta");
    Array<String> paths;
    paths.Add(source);
    paths.Add(metaPath);
    PathLockScope lock;
    if (lock.Acquire(paths))
        return Fail(result, AssetMutationFailure::PathBusy, id, source, StringView(), TEXT("A conflicting asset mutation already owns this path."));
    if (ValidateSourcePair(source, result, id))
        return true;
    if (RunDecisionHook(*this, AssetMutationOperation::ReplaceAsset, id, source, source, false, result))
        return !result.Succeeded;

    AssetMeta previousMeta;
    AssetPipelineDiagnostic metaDiagnostic;
    if (AssetMeta::Load(metaPath, previousMeta, metaDiagnostic) || !meta.ID.IsValid() || meta.ID != previousMeta.ID ||
        meta.FolderAsset || (meta.SourceKind != AssetSourceKind::TextDocument && meta.SourceKind != AssetSourceKind::ExistingJson))
        return Fail(result, AssetMutationFailure::InvalidMetadata, id, source, StringView(),
            metaDiagnostic.Message.HasChars() ? metaDiagnostic.Message : TEXT("Replacement metadata must preserve a valid authored file identity."));

    Journal journal;
    journal.ID = id;
    journal.Operation = AssetMutationOperation::ReplaceAsset;
    JournalEntry sourceEntry;
    sourceEntry.Role = TEXT("Source");
    sourceEntry.SourcePath = source;
    sourceEntry.DestinationPath = source;
    sourceEntry.StagingPath = StagePath(source, id, TEXT("source"));
    sourceEntry.PreimagePath = _journalRoot / TEXT("Preimages") / GuidText(id) / String(StringUtils::GetFileName(source));
    if (HashFile(source, sourceEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, StringView(), TEXT("Existing authored source could not be read."));
    journal.Entries.Add(sourceEntry);
    JournalEntry metaEntry;
    metaEntry.Role = TEXT("Metadata");
    metaEntry.SourcePath = metaPath;
    metaEntry.DestinationPath = metaPath;
    metaEntry.StagingPath = StagePath(metaPath, id, TEXT("meta"));
    metaEntry.PreimagePath = _journalRoot / TEXT("Preimages") / GuidText(id) / String(StringUtils::GetFileName(metaPath));
    if (HashFile(metaPath, metaEntry.BeforeHash))
        return Fail(result, AssetMutationFailure::LockedStorage, id, source, StringView(), TEXT("Existing authored metadata could not be read."));
    journal.Entries.Add(metaEntry);
    if (SaveJournal(_journalRoot, journal))
        return Fail(result, AssetMutationFailure::JournalFailure, id, source, StringView(), TEXT("Authored replacement journal could not be persisted."));

    if (CopyPath(source, journal.Entries[0].PreimagePath) || !SameFileHash(source, journal.Entries[0].PreimagePath) ||
        CopyPath(metaPath, journal.Entries[1].PreimagePath) || !SameFileHash(metaPath, journal.Entries[1].PreimagePath))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed, TEXT("Verified authored pair preimage capture failed."));
    journal.Entries[0].PreimageComplete = true;
    journal.Entries[1].PreimageComplete = true;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Authored replacement preimages could not be journaled."));
    if (File::WriteAllBytes(journal.Entries[0].StagingPath, sourceContents.Get(), sourceContents.Length()) ||
        FlushWrittenFile(journal.Entries[0].StagingPath) || AssetMeta::SaveAtomic(journal.Entries[1].StagingPath, meta, metaDiagnostic) ||
        HashFile(journal.Entries[0].StagingPath, journal.Entries[0].StagedHash) ||
        HashFile(journal.Entries[1].StagingPath, journal.Entries[1].StagedHash))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::CopyFailed,
            metaDiagnostic.Message.HasChars() ? metaDiagnostic.Message : TEXT("Authored replacement staging failed."));
    journal.Entries[0].StagingComplete = true;
    journal.Entries[1].StagingComplete = true;
    journal.State = JournalState::Staged;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Authored replacement staged state could not be persisted."));
    journal.State = JournalState::Publishing;
    if (SaveJournal(_journalRoot, journal))
        return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Authored replacement publishing state could not be persisted."));
    for (int32 i = 0; i < journal.Entries.Count(); i++)
    {
        JournalEntry& entry = journal.Entries[i];
        if (MovePath(entry.StagingPath, entry.DestinationPath, true))
            return AbortTransaction(*this, journal, result, AssetMutationFailure::MoveFailed, TEXT("Authored source pair could not be atomically replaced."));
        entry.Published = true;
        if (SaveJournal(_journalRoot, journal))
            return AbortTransaction(*this, journal, result, AssetMutationFailure::JournalFailure, TEXT("Authored replacement publication step could not be journaled."));
    }
    String sourceHash;
    String metaHash;
    AssetMeta verifiedMeta;
    if (HashFile(source, sourceHash) || sourceHash != journal.Entries[0].StagedHash ||
        HashFile(metaPath, metaHash) || metaHash != journal.Entries[1].StagedHash ||
        AssetMeta::Load(metaPath, verifiedMeta, metaDiagnostic) || verifiedMeta.ID != meta.ID)
        return AbortTransaction(*this, journal, result, AssetMutationFailure::VerificationFailure, TEXT("Published authored replacement pair failed verification."));
    return CommitTransaction(*this, journal, result, meta.ID);
}

bool AssetMutationService::RecoverPending(Array<AssetMutationResult>& results)
{
    results.Clear();
    if (!FileSystem::DirectoryExists(_journalRoot))
        return false;
    Array<String> journalPaths;
    if (FileSystem::DirectoryGetFiles(journalPaths, _journalRoot, TEXT("*.json"), DirectorySearchOption::TopDirectoryOnly))
    {
        AssetMutationResult result;
        Fail(result, AssetMutationFailure::JournalFailure, Guid(), StringView(), StringView(), TEXT("Active mutation journals could not be enumerated."), true);
        results.Add(MoveTemp(result));
        return true;
    }
    if (journalPaths.Count() > 1)
    {
        std::sort(journalPaths.Get(), journalPaths.Get() + journalPaths.Count(), [](const String& a, const String& b)
        {
            return a < b;
        });
    }
    bool anyFailed = false;
    for (const String& journalPath : journalPaths)
    {
        Journal journal;
        AssetMutationResult result;
        if (LoadJournal(journalPath, journal))
        {
            Fail(result, AssetMutationFailure::RecoveryRequired, Guid(), journalPath, StringView(), TEXT("Mutation journal is malformed and was preserved."), true);
            results.Add(MoveTemp(result));
            anyFailed = true;
            continue;
        }
        Array<String> paths;
        for (const JournalEntry& entry : journal.Entries)
        {
            paths.Add(entry.SourcePath);
            paths.Add(entry.DestinationPath);
            paths.Add(entry.StagingPath);
            paths.Add(entry.PreimagePath);
        }
        PathLockScope lock;
        if (lock.Acquire(paths))
        {
            Fail(result, AssetMutationFailure::PathBusy, journal.ID, journal.Entries[0].SourcePath, journal.Entries[0].DestinationPath,
                TEXT("Interrupted mutation cannot recover while a conflicting path is locked."), true);
            results.Add(MoveTemp(result));
            anyFailed = true;
            continue;
        }

        const bool committed = journal.State == JournalState::Committed;
        const bool failed = committed ? CompleteCommittedState(*this, journal) : RecoverOldState(*this, journal);
        if (failed)
        {
            journal.State = JournalState::RecoveryRequired;
            journal.LastError = TEXT("Automatic recovery could not establish a complete old or new source/metadata pair.");
            SaveJournal(_journalRoot, journal);
            Fail(result, AssetMutationFailure::RecoveryRequired, journal.ID, journal.Entries[0].SourcePath, journal.Entries[0].DestinationPath,
                journal.LastError, true);
            anyFailed = true;
        }
        else
        {
            Succeed(result, journal.ID, journal.Entries[0].SourcePath, journal.Entries[0].DestinationPath);
            result.RecoveryPath = journal.RecoveryPath;
            result.Message = committed ? TEXT("Committed mutation cleanup completed.") : TEXT("Interrupted mutation rolled back to its verified old state.");
        }
        results.Add(MoveTemp(result));
    }
    return anyFailed;
}
