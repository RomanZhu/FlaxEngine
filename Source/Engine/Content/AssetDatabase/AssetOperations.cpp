// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetOperations.h"
#include "DurableAssetFileSystem.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Utilities/Crc.h"
#include <algorithm>

namespace
{
    constexpr uint32 JournalMagic = 0x4A504F41; // AOPJ
    constexpr uint32 JournalVersion = 2;

    enum class JournalPhase : byte
    {
        Prepared,
        Applied,
        Committed,
    };

    struct OperationJournal
    {
        Guid TransactionId;
        AssetOperationKind Kind = AssetOperationKind::Create;
        JournalPhase Phase = JournalPhase::Prepared;
        Guid AssetGuid;
        Guid SourceAssetGuid;
        String SourcePath;
        String DestinationPath;
        String StageSourcePath;
        String StageMetaPath;
        String SourceFragmentsPath;
        String DestinationFragmentsPath;
        String StageFragmentsPath;
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    String MetaPath(const StringView& source)
    {
        return String(source) + TEXT(".meta");
    }

    bool EnsureDirectory(const StringView& path)
    {
        if (path.IsEmpty())
            return true;
        return DurableAssetFileSystem::EnsureDirectory(path);
    }

    bool EnsureParent(const StringView& path)
    {
        return EnsureDirectory(StringUtils::GetDirectoryName(path));
    }

    bool FlushFile(const StringView& path)
    {
        return DurableAssetFileSystem::FlushFile(path);
    }

    bool DurableMove(const StringView& destination, const StringView& source, bool overwrite)
    {
        return DurableAssetFileSystem::Move(destination, source, overwrite);
    }

    bool DurableDeleteFile(const StringView& path)
    {
        return DurableAssetFileSystem::DeleteFile(path);
    }

    bool DurableDeleteDirectory(const StringView& path)
    {
        return DurableAssetFileSystem::DeleteDirectory(path, true);
    }

    class JournalWriter
    {
    public:
        Array<byte> Data;

        void UInt32(uint32 value)
        {
            for (int32 i = 0; i < 4; i++)
                Data.Add(static_cast<byte>(value >> (i * 8)));
        }

        void Byte(byte value)
        {
            Data.Add(value);
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

        bool UInt32(uint32& value)
        {
            byte bytes[4];
            if (Bytes(bytes, 4))
                return true;
            value = static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
                    (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
            return false;
        }

        bool Byte(byte& value)
        {
            return Bytes(&value, 1);
        }

        bool GuidValue(Guid& value)
        {
            return UInt32(value.A) || UInt32(value.B) || UInt32(value.C) || UInt32(value.D);
        }

        bool StringValue(String& value)
        {
            uint32 length;
            if (UInt32(length) || length > 1024 * 1024 || length > _length - _position)
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

    bool SaveJournal(const StringView& directory, const OperationJournal& journal, AssetPipelineDiagnostic& diagnostic)
    {
        if (EnsureDirectory(directory))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, directory,
                TEXT("Cannot create the asset operation transaction directory."));
        JournalWriter writer;
        writer.UInt32(JournalMagic);
        writer.UInt32(JournalVersion);
        writer.Byte(static_cast<byte>(journal.Kind));
        writer.Byte(static_cast<byte>(journal.Phase));
        writer.GuidValue(journal.TransactionId);
        writer.GuidValue(journal.AssetGuid);
        writer.GuidValue(journal.SourceAssetGuid);
        writer.StringValue(journal.SourcePath);
        writer.StringValue(journal.DestinationPath);
        writer.StringValue(journal.StageSourcePath);
        writer.StringValue(journal.StageMetaPath);
        writer.StringValue(journal.SourceFragmentsPath);
        writer.StringValue(journal.DestinationFragmentsPath);
        writer.StringValue(journal.StageFragmentsPath);
        const String destination = String(directory) / TEXT("journal.bin");
        const String staging = destination + TEXT(".tmp");
        if (File::WriteAllBytes(staging, writer.Data.Get(), writer.Data.Count()) || FlushFile(staging) ||
            DurableMove(destination, staging, true))
        {
            DurableAssetFileSystem::DeleteFile(staging);
            return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, destination,
                TEXT("Cannot durably persist the asset operation journal."));
        }
        return false;
    }

    bool LoadJournal(const StringView& directory, OperationJournal& journal, AssetPipelineDiagnostic& diagnostic)
    {
        const String path = String(directory) / TEXT("journal.bin");
        BytesContainer bytes;
        if (File::ReadAllBytes(path, bytes) || bytes.Length() > MAX_uint32)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path,
                TEXT("Asset operation journal is missing or unreadable."));
        JournalReader reader(bytes.Get(), static_cast<uint32>(bytes.Length()));
        uint32 magic;
        uint32 version;
        byte kind;
        byte phase;
        if (reader.UInt32(magic) || reader.UInt32(version) || reader.Byte(kind) || reader.Byte(phase) ||
            reader.GuidValue(journal.TransactionId) || reader.GuidValue(journal.AssetGuid) ||
            reader.GuidValue(journal.SourceAssetGuid) || reader.StringValue(journal.SourcePath) ||
            reader.StringValue(journal.DestinationPath) || reader.StringValue(journal.StageSourcePath) ||
            reader.StringValue(journal.StageMetaPath) || magic != JournalMagic ||
            (version != 1 && version != JournalVersion))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path,
                TEXT("Asset operation journal is malformed or unsupported."));
        if (version >= 2 && (reader.StringValue(journal.SourceFragmentsPath) ||
            reader.StringValue(journal.DestinationFragmentsPath) || reader.StringValue(journal.StageFragmentsPath)))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path,
                TEXT("Asset operation journal is malformed or unsupported."));
        if (!reader.AtEnd() || kind > static_cast<byte>(AssetOperationKind::Restore) ||
            phase > static_cast<byte>(JournalPhase::Committed))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, path,
                TEXT("Asset operation journal is malformed or unsupported."));
        journal.Kind = static_cast<AssetOperationKind>(kind);
        journal.Phase = static_cast<JournalPhase>(phase);
        return false;
    }

    bool HashFile(const StringView& path, ContentHash& hash)
    {
        BytesContainer bytes;
        if (File::ReadAllBytes(path, bytes))
            return true;
        hash = ContentHash::Compute(bytes.Get(), bytes.Length());
        return false;
    }

    bool GetMetaSemanticHash(const AssetMeta& meta, uint64& hash, AssetPipelineDiagnostic& diagnostic)
    {
        StringAnsi canonical;
        if (meta.ToJson(canonical, diagnostic))
            return true;
        hash = Crc::MemCrc32(canonical.Get(), canonical.Length());
        return false;
    }

    bool IsCreateKind(AssetOperationKind kind)
    {
        return kind == AssetOperationKind::Create || kind == AssetOperationKind::Import || kind == AssetOperationKind::Copy;
    }

    bool RollbackJournal(const OperationJournal& journal)
    {
        const String sourceMeta = MetaPath(journal.SourcePath);
        const String destinationMeta = MetaPath(journal.DestinationPath);
        bool failed = false;
        if (IsCreateKind(journal.Kind))
        {
            if (FileSystem::FileExists(journal.DestinationPath))
                failed |= DurableDeleteFile(journal.DestinationPath);
            if (FileSystem::FileExists(destinationMeta))
                failed |= DurableDeleteFile(destinationMeta);
            if (journal.DestinationFragmentsPath.HasChars() &&
                FileSystem::DirectoryExists(journal.DestinationFragmentsPath))
                failed |= DurableDeleteDirectory(journal.DestinationFragmentsPath);
            return failed;
        }
        if (!FileSystem::FileExists(journal.SourcePath))
        {
            if (FileSystem::FileExists(journal.DestinationPath))
                failed |= DurableMove(journal.SourcePath, journal.DestinationPath, false);
            else if (FileSystem::FileExists(journal.StageSourcePath))
                failed |= DurableMove(journal.SourcePath, journal.StageSourcePath, false);
        }
        if (!FileSystem::FileExists(sourceMeta))
        {
            if (FileSystem::FileExists(destinationMeta))
                failed |= DurableMove(sourceMeta, destinationMeta, false);
            else if (FileSystem::FileExists(journal.StageMetaPath))
                failed |= DurableMove(sourceMeta, journal.StageMetaPath, false);
        }
        if (journal.SourceFragmentsPath.HasChars() && !FileSystem::DirectoryExists(journal.SourceFragmentsPath))
        {
            if (FileSystem::DirectoryExists(journal.DestinationFragmentsPath))
                failed |= DurableMove(journal.SourceFragmentsPath, journal.DestinationFragmentsPath, false);
            else if (FileSystem::DirectoryExists(journal.StageFragmentsPath))
                failed |= DurableMove(journal.SourceFragmentsPath, journal.StageFragmentsPath, false);
        }
        return failed;
    }

    void AddSelfWrite(AssetOperationCommit& commit, const StringView& path)
    {
        AssetOperationSelfWrite write;
        write.TransactionId = commit.TransactionId;
        write.Path = path;
        if (!HashFile(path, write.Content))
            commit.SelfWrites.Add(MoveTemp(write));
    }

    bool RestoreFileAtomic(const StringView& path, const BytesContainer& bytes)
    {
        const String staging = String(path) + TEXT(".rollback-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { DurableAssetFileSystem::DeleteFile(staging); };
        return DurableAssetFileSystem::WriteFile(staging, bytes.Get(), bytes.Length()) ||
               DurableAssetFileSystem::Replace(path, staging);
    }
}

AssetOperations::AssetOperations(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
    IAssetModificationProcessor& modificationProcessor, IAssetOperationDatabaseCallbacks& databaseCallbacks)
    : _projectRoot(projectRoot)
    , _contentRoot(contentRoot)
    , _libraryRoot(libraryRoot)
    , _transactionsRoot(String(libraryRoot) / TEXT("AssetOperations/Transactions"))
    , _trashRoot(String(libraryRoot) / TEXT("AssetOperations/Trash"))
    , _rootRegistry(projectRoot, libraryRoot)
    , _modificationProcessor(modificationProcessor)
    , _databaseCallbacks(databaseCallbacks)
{
    _rootRegistryValid = !_rootRegistry.RegisterProjectRoots(contentRoot, _rootRegistryDiagnostic);
}

bool AssetOperations::Initialize(AssetPipelineDiagnostic& diagnostic)
{
    if (!_rootRegistryValid)
    {
        diagnostic = _rootRegistryDiagnostic;
        return true;
    }
    if (_projectRoot.IsEmpty() || _contentRoot.IsEmpty() || _libraryRoot.IsEmpty() ||
        EnsureDirectory(_transactionsRoot) || EnsureDirectory(_trashRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, _libraryRoot,
            TEXT("Asset operations roots are invalid or cannot be created."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::NormalizeSource(const StringView& input, AssetPathPolicy::ProjectPath& result,
    AssetPipelineDiagnostic& diagnostic) const
{
    return _rootRegistry.ResolveForGenericMutation(input, result, diagnostic);
}

bool AssetOperations::ValidateExisting(const AssetOperationTarget& target, AssetPathPolicy::ProjectPath& normalized,
    AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const
{
    if (!target.ExpectedGuid.IsValid() || NormalizeSource(target.SourcePath, normalized, diagnostic))
        return true;
    if (!FileSystem::FileExists(normalized.AbsolutePath) || AssetMeta::Load(MetaPath(normalized.AbsolutePath), meta, diagnostic))
        return true;
    if (meta.ID != target.ExpectedGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, normalized.AbsolutePath,
            TEXT("Asset operation target GUID no longer matches the exact source path."));
    return false;
}

bool AssetOperations::AcquirePaths(const Array<String>& paths, Array<String>& acquired,
    AssetPipelineDiagnostic& diagnostic)
{
    acquired.Clear();
    for (const String& path : paths)
    {
        String key(path);
        key.Replace('\\', '/');
        key = key.ToLower();
        if (!acquired.Contains(key))
            acquired.Add(MoveTemp(key));
    }
    if (acquired.Count() > 1)
        std::sort(acquired.Get(), acquired.Get() + acquired.Count());
    _stateLocker.Lock();
    bool conflict;
    do
    {
        conflict = false;
        for (const String& key : acquired)
        {
            if (_lockedPaths.Contains(key))
            {
                conflict = true;
                break;
            }
        }
        if (conflict)
            _locksChanged.Wait(_stateLocker);
    } while (conflict);
    for (const String& key : acquired)
        _lockedPaths.Add(key);
    _stateLocker.Unlock();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void AssetOperations::ReleasePaths(const Array<String>& acquired)
{
    _stateLocker.Lock();
    for (const String& key : acquired)
        _lockedPaths.Remove(key);
    _locksChanged.NotifyAll();
    _stateLocker.Unlock();
}

bool AssetOperations::PublishCommit(AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic)
{
    Array<AssetOperationCommit> commits;
    _stateLocker.Lock();
    if (commit.Kind == AssetOperationKind::ImporterSettings &&
        (_editingDepth > 0 || _pendingCommits.HasItems()))
    {
        _stateLocker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, commit.SourcePath,
            TEXT("Importer settings cannot be published while another asset operation batch is pending."));
    }
    if (_editingDepth > 0)
    {
        _pendingCommits.Add(commit);
        _stateLocker.Unlock();
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    commits = MoveTemp(_pendingCommits);
    _pendingCommits.Clear();
    commits.Add(commit);
    _stateLocker.Unlock();
    if (!_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        _stateLocker.Lock();
        for (const AssetOperationCommit& published : commits)
            _selfWrites.Add(published.SelfWrites);
        _stateLocker.Unlock();
        return false;
    }

    if (commit.Kind != AssetOperationKind::ImporterSettings)
    {
        _stateLocker.Lock();
        Array<AssetOperationCommit> pending = MoveTemp(_pendingCommits);
        _pendingCommits = MoveTemp(commits);
        _pendingCommits.Add(pending);
        _stateLocker.Unlock();
    }
    return true;
}

bool AssetOperations::CreateFromBytes(AssetOperationKind kind, const StringView& destination, const Span<byte>& sourceData,
    const AssetMeta& meta, AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (sourceData.Length() == 0 || !meta.ID.IsValid() || NormalizeSource(destination, normalized, diagnostic))
        return true;
    StringAnsi metaJson;
    if (meta.ToJson(metaJson, diagnostic))
        return true;
    const String destinationMeta = MetaPath(normalized.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(normalized.AbsolutePath);
    lockPaths.Add(destinationMeta);
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (FileSystem::FileExists(normalized.AbsolutePath) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(normalized.AbsolutePath) || EnsureParent(normalized.AbsolutePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, normalized.AbsolutePath,
            TEXT("Asset operation destination is not an exact empty source-plus-meta target."));

    OperationJournal journal;
    journal.TransactionId = Guid::New();
    journal.Kind = kind;
    journal.AssetGuid = meta.ID;
    journal.DestinationPath = normalized.AbsolutePath;
    const String transactionDirectory = _transactionsRoot / journal.TransactionId.ToString(Guid::FormatType::N);
    journal.StageSourcePath = transactionDirectory / TEXT("source.stage");
    journal.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    if (SaveJournal(transactionDirectory, journal, diagnostic) ||
        File::WriteAllBytes(journal.StageSourcePath, sourceData.Get(), sourceData.Length()) || FlushFile(journal.StageSourcePath) ||
        AssetMeta::SaveAtomic(journal.StageMetaPath, meta, diagnostic) ||
        DurableMove(normalized.AbsolutePath, journal.StageSourcePath, false) ||
        DurableMove(destinationMeta, journal.StageMetaPath, false))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, normalized.AbsolutePath,
                TEXT("Asset create/import transaction could not publish source and metadata."));
        return true;
    }
    journal.Phase = JournalPhase::Applied;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    journal.Phase = JournalPhase::Committed;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    DurableDeleteDirectory(transactionDirectory);
    commit = AssetOperationCommit();
    commit.TransactionId = journal.TransactionId;
    commit.Kind = kind;
    commit.AssetGuid = meta.ID;
    commit.DestinationPath = normalized.AbsolutePath;
    AddSelfWrite(commit, normalized.AbsolutePath);
    AddSelfWrite(commit, destinationMeta);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetOperations::CreateAsset(const StringView& destination, const Span<byte>& sourceData, const AssetMeta& meta,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (NormalizeSource(destination, normalized, diagnostic))
        return true;
    AssetOperationTarget target;
    target.SourcePath = destination;
    target.ExpectedGuid = meta.ID;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Create, target, destination, diagnostic))
        return true;
    AssetOperationCommit commit;
    return CreateFromBytes(AssetOperationKind::Create, destination, sourceData, meta, commit, diagnostic) ||
           PublishCommit(commit, diagnostic);
}

bool AssetOperations::ImportAsset(const StringView& externalSource, const StringView& destination, const AssetMeta& meta,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalized;
    if (NormalizeSource(destination, normalized, diagnostic))
        return true;
    BytesContainer bytes;
    if (!FileSystem::FileExists(externalSource) || File::ReadAllBytes(externalSource, bytes) || bytes.Length() > MAX_int32)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, externalSource,
            TEXT("Import source is missing, unreadable, or too large for this operation."));
    AssetOperationTarget target;
    target.SourcePath = destination;
    target.ExpectedGuid = meta.ID;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Import, target, destination, diagnostic))
        return true;
    AssetOperationCommit commit;
    return CreateFromBytes(AssetOperationKind::Import, destination,
               Span<byte>(bytes.Get(), static_cast<int32>(bytes.Length())), meta, commit, diagnostic) ||
           PublishCommit(commit, diagnostic);
}

bool AssetOperations::MoveExact(AssetOperationKind kind, const AssetOperationTarget& target, const StringView& destination,
    AssetTrashRecord* trash, AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath source;
    AssetPathPolicy::ProjectPath destinationPath;
    AssetMeta initialMeta;
    if (ValidateExisting(target, source, initialMeta, diagnostic))
        return true;
    String destinationAbsolute;
    if (kind == AssetOperationKind::Trash || kind == AssetOperationKind::Delete)
    {
        destinationAbsolute = destination;
    }
    else if (NormalizeSource(destination, destinationPath, diagnostic))
    {
        return true;
    }
    else
    {
        destinationAbsolute = destinationPath.AbsolutePath;
    }
    if (source.AbsolutePath.Compare(destinationAbsolute, StringSearchCase::IgnoreCase) == 0)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationAbsolute,
            TEXT("Asset move source and destination are the same exact path."));
    const String sourceMeta = MetaPath(source.AbsolutePath);
    const String destinationMeta = MetaPath(destinationAbsolute);
    const bool canMoveFragments = kind == AssetOperationKind::Trash || kind == AssetOperationKind::Delete;
    const String sourceFragments = canMoveFragments
        ? SceneFragmentStore::GetScenePath(_projectRoot, initialMeta.ID)
        : String::Empty;
    const bool hasFragments = sourceFragments.HasChars() && FileSystem::DirectoryExists(sourceFragments);
    const String destinationFragments = hasFragments
        ? String(StringUtils::GetDirectoryName(destinationAbsolute)) / TEXT("scene-fragments")
        : String::Empty;
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(sourceMeta);
    lockPaths.Add(destinationAbsolute);
    lockPaths.Add(destinationMeta);
    if (hasFragments)
    {
        lockPaths.Add(sourceFragments);
        lockPaths.Add(destinationFragments);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };

    AssetMeta currentMeta;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, currentMeta, diagnostic))
        return true;
    if (hasFragments != (sourceFragments.HasChars() && FileSystem::DirectoryExists(sourceFragments)))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, sourceFragments,
            TEXT("Private scene fragments changed during move preparation."));
    if (FileSystem::FileExists(destinationAbsolute) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(destinationAbsolute) ||
        (hasFragments && (FileSystem::FileExists(destinationFragments) || FileSystem::DirectoryExists(destinationFragments))) ||
        EnsureParent(destinationAbsolute))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationAbsolute,
            TEXT("Asset move destination is not an exact empty source-plus-meta target."));

    OperationJournal journal;
    journal.TransactionId = Guid::New();
    journal.Kind = kind;
    journal.AssetGuid = currentMeta.ID;
    journal.SourcePath = source.AbsolutePath;
    journal.DestinationPath = destinationAbsolute;
    journal.SourceFragmentsPath = sourceFragments;
    journal.DestinationFragmentsPath = destinationFragments;
    const String transactionDirectory = _transactionsRoot / journal.TransactionId.ToString(Guid::FormatType::N);
    journal.StageSourcePath = transactionDirectory / TEXT("source.stage");
    journal.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    journal.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    if (SaveJournal(transactionDirectory, journal, diagnostic) ||
        (hasFragments && DurableMove(journal.StageFragmentsPath, sourceFragments, false)) ||
        DurableMove(journal.StageSourcePath, source.AbsolutePath, false) ||
        DurableMove(journal.StageMetaPath, sourceMeta, false) ||
        DurableMove(destinationAbsolute, journal.StageSourcePath, false) ||
        DurableMove(destinationMeta, journal.StageMetaPath, false) ||
        (hasFragments && DurableMove(destinationFragments, journal.StageFragmentsPath, false)))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, source.AbsolutePath,
                TEXT("Asset move transaction could not publish source and metadata together."));
        return true;
    }
    journal.Phase = JournalPhase::Applied;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    journal.Phase = JournalPhase::Committed;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    DurableDeleteDirectory(transactionDirectory);

    AssetOperationCommit commit;
    commit.TransactionId = journal.TransactionId;
    commit.Kind = kind;
    commit.AssetGuid = currentMeta.ID;
    commit.SourcePath = source.AbsolutePath;
    commit.DestinationPath = destinationAbsolute;
    if (kind != AssetOperationKind::Trash && kind != AssetOperationKind::Delete)
    {
        AddSelfWrite(commit, destinationAbsolute);
        AddSelfWrite(commit, destinationMeta);
    }
    if (trash)
    {
        trash->TransactionId = journal.TransactionId;
        trash->AssetGuid = currentMeta.ID;
        trash->OriginalSourcePath = source.AbsolutePath;
        trash->OriginalMetaPath = sourceMeta;
        trash->TrashSourcePath = destinationAbsolute;
        trash->TrashMetaPath = destinationMeta;
        trash->OriginalFragmentsPath = sourceFragments;
        trash->TrashFragmentsPath = destinationFragments;
    }
    diagnostic = AssetPipelineDiagnostic();
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::MoveAsset(const AssetOperationTarget& target, const StringView& destination,
    AssetPipelineDiagnostic& diagnostic)
{
    AssetPathPolicy::ProjectPath normalizedSource;
    AssetPathPolicy::ProjectPath normalizedDestination;
    AssetMeta meta;
    if (ValidateExisting(target, normalizedSource, meta, diagnostic) ||
        NormalizeSource(destination, normalizedDestination, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Move, target, destination, diagnostic))
        return true;
    return MoveExact(AssetOperationKind::Move, target, destination, nullptr, diagnostic);
}

bool AssetOperations::RenameAsset(const AssetOperationTarget& target, const StringView& newFileName,
    AssetPipelineDiagnostic& diagnostic)
{
    if (newFileName.IsEmpty() || newFileName.Contains(TEXT("/")) || newFileName.Contains(TEXT("\\")))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, target.SourcePath,
            TEXT("Asset rename requires one portable file name, not a path."));
    AssetPathPolicy::ProjectPath normalized;
    AssetMeta meta;
    if (ValidateExisting(target, normalized, meta, diagnostic))
        return true;
    const String destination = String(StringUtils::GetDirectoryName(normalized.AbsolutePath)) / newFileName;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Rename, target, destination, diagnostic))
        return true;
    return MoveExact(AssetOperationKind::Rename, target, destination, nullptr, diagnostic);
}

bool AssetOperations::CopyAsset(const AssetOperationTarget& target, const StringView& destination, Guid& copiedGuid,
    AssetPipelineDiagnostic& diagnostic)
{
    copiedGuid = Guid::Empty;
    AssetPathPolicy::ProjectPath source;
    AssetPathPolicy::ProjectPath destinationPath;
    AssetMeta sourceMeta;
    if (ValidateExisting(target, source, sourceMeta, diagnostic) || NormalizeSource(destination, destinationPath, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Copy, target, destination, diagnostic))
        return true;
    const Guid copiedAssetGuid = Guid::New();
    const String sourceFragments = SceneFragmentStore::GetScenePath(_projectRoot, sourceMeta.ID);
    const bool hasFragments = FileSystem::DirectoryExists(sourceFragments);
    const String destinationFragments = hasFragments
        ? SceneFragmentStore::GetScenePath(_projectRoot, copiedAssetGuid)
        : String::Empty;
    const String sourceMetaPath = MetaPath(source.AbsolutePath);
    const String destinationMeta = MetaPath(destinationPath.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(sourceMetaPath);
    lockPaths.Add(destinationPath.AbsolutePath);
    lockPaths.Add(destinationMeta);
    if (hasFragments)
    {
        lockPaths.Add(sourceFragments);
        lockPaths.Add(destinationFragments);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    AssetMeta currentMeta;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, currentMeta, diagnostic))
        return true;
    if (hasFragments != FileSystem::DirectoryExists(sourceFragments))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, sourceFragments,
            TEXT("Private scene fragments changed during copy preparation."));
    if (FileSystem::FileExists(destinationPath.AbsolutePath) || FileSystem::FileExists(destinationMeta) ||
        FileSystem::DirectoryExists(destinationPath.AbsolutePath) ||
        (hasFragments && (FileSystem::FileExists(destinationFragments) || FileSystem::DirectoryExists(destinationFragments))) ||
        EnsureParent(destinationPath.AbsolutePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, destinationPath.AbsolutePath,
            TEXT("Asset copy destination is not an exact empty source-plus-meta target."));

    AssetMeta copiedMeta = currentMeta.CloneWithNewIdentities();
    copiedMeta.ID = copiedAssetGuid;
    OperationJournal journal;
    journal.TransactionId = Guid::New();
    journal.Kind = AssetOperationKind::Copy;
    journal.AssetGuid = copiedMeta.ID;
    journal.SourceAssetGuid = currentMeta.ID;
    journal.SourcePath = source.AbsolutePath;
    journal.DestinationPath = destinationPath.AbsolutePath;
    journal.SourceFragmentsPath = hasFragments ? sourceFragments : String::Empty;
    journal.DestinationFragmentsPath = destinationFragments;
    const String transactionDirectory = _transactionsRoot / journal.TransactionId.ToString(Guid::FormatType::N);
    journal.StageSourcePath = transactionDirectory / TEXT("source.stage");
    journal.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    journal.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    String fragmentError;
    if (SaveJournal(transactionDirectory, journal, diagnostic) ||
        (hasFragments && SceneFragmentStore::PrepareCloneDirectory(_projectRoot, currentMeta.ID, copiedMeta.ID,
            journal.StageFragmentsPath, fragmentError)) ||
        FileSystem::CopyFile(journal.StageSourcePath, source.AbsolutePath) || FlushFile(journal.StageSourcePath) ||
        AssetMeta::SaveAtomic(journal.StageMetaPath, copiedMeta, diagnostic) ||
        DurableMove(destinationPath.AbsolutePath, journal.StageSourcePath, false) ||
        DurableMove(destinationMeta, journal.StageMetaPath, false) ||
        (hasFragments && DurableMove(destinationFragments, journal.StageFragmentsPath, false)))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, destinationPath.AbsolutePath,
                fragmentError.HasChars() ? fragmentError :
                    TEXT("Asset copy transaction could not publish source, cloned metadata, and private fragments."));
        return true;
    }
    journal.Phase = JournalPhase::Applied;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    journal.Phase = JournalPhase::Committed;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    DurableDeleteDirectory(transactionDirectory);

    if (_databaseCallbacks.ClearCopiedState(currentMeta.ID, copiedMeta.ID, diagnostic))
        return true;
    AssetOperationCommit commit;
    commit.TransactionId = journal.TransactionId;
    commit.Kind = AssetOperationKind::Copy;
    commit.AssetGuid = copiedMeta.ID;
    commit.SourceAssetGuid = currentMeta.ID;
    commit.SourcePath = source.AbsolutePath;
    commit.DestinationPath = destinationPath.AbsolutePath;
    AddSelfWrite(commit, destinationPath.AbsolutePath);
    AddSelfWrite(commit, destinationMeta);
    copiedGuid = copiedMeta.ID;
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::WriteImporterSettings(const AssetOperationTarget& target,
    const AssetImporterSettingsRevision& expected, int32 settingsVersion, const StringAnsiView& settingsJson,
    AssetPipelineDiagnostic& diagnostic, AssetMetaWriteFailurePoint failurePoint)
{
    if (expected.SourceRevision == 0 || expected.ImporterID.IsEmpty() ||
        expected.StoredSettingsVersion < 1 || settingsVersion < 1)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, target.SourcePath,
            TEXT("Importer settings revision is invalid."));
    if (IsAssetEditing())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, target.SourcePath,
            TEXT("Importer settings cannot be saved inside an asset editing batch."));

    AssetPathPolicy::ProjectPath source;
    if (NormalizeSource(target.SourcePath, source, diagnostic))
        return true;
    const String metaPath = MetaPath(source.AbsolutePath);
    Array<String> lockPaths;
    lockPaths.Add(source.AbsolutePath);
    lockPaths.Add(metaPath);
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };

    AssetMeta current;
    AssetPathPolicy::ProjectPath currentSource;
    if (ValidateExisting(target, currentSource, current, diagnostic) ||
        _databaseCallbacks.ValidateImporterSettingsRevision(target, expected, diagnostic))
        return true;
    uint64 currentSemanticHash;
    if (GetMetaSemanticHash(current, currentSemanticHash, diagnostic))
        return true;
    if (currentSemanticHash != expected.MetaSemanticHash || current.Processor.ID != expected.ImporterID ||
        current.Processor.SettingsVersion != expected.StoredSettingsVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, source.AbsolutePath,
            TEXT("Importer settings metadata changed after the editor revision was captured."));

    StringAnsi canonicalSettings;
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(settingsJson, canonicalSettings, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.AbsolutePath, jsonError.Message);
    rapidjson_flax::Document settingsDocument;
    settingsDocument.Parse(canonicalSettings.Get(), canonicalSettings.Length());
    if (settingsDocument.HasParseError() || !settingsDocument.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.AbsolutePath,
            TEXT("Importer settings must be a JSON object."));
    if (current.Processor.SettingsVersion == settingsVersion && current.Processor.SettingsJson == canonicalSettings)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    ReleasePaths(acquired);
    acquired.Clear();
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::ImporterSettings, target,
        StringView::Empty, diagnostic))
        return true;
    AcquirePaths(lockPaths, acquired, diagnostic);
    AssetMeta authorizedCurrent;
    if (ValidateExisting(target, currentSource, authorizedCurrent, diagnostic) ||
        _databaseCallbacks.ValidateImporterSettingsRevision(target, expected, diagnostic))
        return true;
    if (GetMetaSemanticHash(authorizedCurrent, currentSemanticHash, diagnostic))
        return true;
    if (currentSemanticHash != expected.MetaSemanticHash || authorizedCurrent.Processor.ID != expected.ImporterID ||
        authorizedCurrent.Processor.SettingsVersion != expected.StoredSettingsVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, source.AbsolutePath,
            TEXT("Importer settings metadata changed while the save was authorized."));
    current = MoveTemp(authorizedCurrent);

    AssetMeta updated = current;
    updated.Processor.SettingsVersion = settingsVersion;
    updated.Processor.SettingsJson = MoveTemp(canonicalSettings);
    BytesContainer previousMeta;
    if (File::ReadAllBytes(metaPath, previousMeta))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, metaPath,
            TEXT("Cannot capture importer metadata before atomic replacement."));
    if (AssetMeta::SaveAtomic(metaPath, updated, diagnostic, nullptr, failurePoint))
        return true;

    AssetOperationCommit commit;
    commit.TransactionId = Guid::New();
    commit.Kind = AssetOperationKind::ImporterSettings;
    commit.AssetGuid = current.ID;
    commit.SourceAssetGuid = current.ID;
    commit.SourcePath = source.AbsolutePath;
    AddSelfWrite(commit, metaPath);
    if (!PublishCommit(commit, diagnostic))
        return false;

    const AssetPipelineDiagnostic publicationDiagnostic = diagnostic;
    if (RestoreFileAtomic(metaPath, previousMeta))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
        diagnostic.SourcePath = metaPath;
        diagnostic.Message = String::Format(TEXT("Importer settings database publication failed and the prior metadata could not be restored. {0}"), publicationDiagnostic.Message);
        return true;
    }
    AssetOperationCommit rollback;
    rollback.TransactionId = commit.TransactionId;
    AddSelfWrite(rollback, metaPath);
    _stateLocker.Lock();
    _selfWrites.Add(rollback.SelfWrites);
    _stateLocker.Unlock();
    diagnostic = publicationDiagnostic;
    return true;
}

bool AssetOperations::TrashAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
    AssetPipelineDiagnostic& diagnostic)
{
    trash = AssetTrashRecord();
    AssetPathPolicy::ProjectPath source;
    AssetMeta meta;
    if (ValidateExisting(target, source, meta, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Trash, target, StringView::Empty, diagnostic))
        return true;
    const String trashDirectory = _trashRoot / Guid::New().ToString(Guid::FormatType::N);
    const String destination = trashDirectory / StringUtils::GetFileName(source.AbsolutePath);
    return MoveExact(AssetOperationKind::Trash, target, destination, &trash, diagnostic);
}

bool AssetOperations::DeleteAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
    AssetPipelineDiagnostic& diagnostic)
{
    trash = AssetTrashRecord();
    AssetPathPolicy::ProjectPath source;
    AssetMeta meta;
    if (ValidateExisting(target, source, meta, diagnostic) ||
        _modificationProcessor.ValidateOperation(AssetOperationKind::Delete, target, StringView::Empty, diagnostic))
        return true;
    const String trashDirectory = _trashRoot / Guid::New().ToString(Guid::FormatType::N);
    const String destination = trashDirectory / StringUtils::GetFileName(source.AbsolutePath);
    return MoveExact(AssetOperationKind::Delete, target, destination, &trash, diagnostic);
}

bool AssetOperations::RestoreAsset(const AssetTrashRecord& trash, AssetPipelineDiagnostic& diagnostic)
{
    const bool hasFragments = trash.TrashFragmentsPath.HasChars();
    if (!trash.AssetGuid.IsValid() || !FileSystem::FileExists(trash.TrashSourcePath) ||
        !FileSystem::FileExists(trash.TrashMetaPath) ||
        (hasFragments && (trash.OriginalFragmentsPath.IsEmpty() || !FileSystem::DirectoryExists(trash.TrashFragmentsPath))))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, trash.TrashSourcePath,
            TEXT("Asset trash record is incomplete or its recoverable files are missing."));
    AssetMeta meta;
    if (AssetMeta::Load(trash.TrashMetaPath, meta, diagnostic) || meta.ID != trash.AssetGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, trash.TrashMetaPath,
            TEXT("Asset trash metadata no longer matches the exact restore target."));
    AssetPathPolicy::ProjectPath normalizedDestination;
    if (NormalizeSource(trash.OriginalSourcePath, normalizedDestination, diagnostic))
        return true;
    AssetOperationTarget callbackTarget;
    callbackTarget.SourcePath = trash.OriginalSourcePath;
    callbackTarget.ExpectedGuid = trash.AssetGuid;
    if (_modificationProcessor.ValidateOperation(AssetOperationKind::Restore, callbackTarget,
        trash.OriginalSourcePath, diagnostic))
        return true;

    Array<String> lockPaths;
    lockPaths.Add(trash.TrashSourcePath);
    lockPaths.Add(trash.TrashMetaPath);
    lockPaths.Add(trash.OriginalSourcePath);
    lockPaths.Add(trash.OriginalMetaPath);
    if (hasFragments)
    {
        lockPaths.Add(trash.TrashFragmentsPath);
        lockPaths.Add(trash.OriginalFragmentsPath);
    }
    Array<String> acquired;
    AcquirePaths(lockPaths, acquired, diagnostic);
    SCOPE_EXIT { ReleasePaths(acquired); };
    if (FileSystem::FileExists(trash.OriginalSourcePath) || FileSystem::FileExists(trash.OriginalMetaPath) ||
        (hasFragments && (FileSystem::FileExists(trash.OriginalFragmentsPath) ||
            FileSystem::DirectoryExists(trash.OriginalFragmentsPath))) ||
        EnsureParent(trash.OriginalSourcePath))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PathCollision, trash.OriginalSourcePath,
            TEXT("Asset restore destination is no longer empty."));
    AssetMeta currentMeta;
    if (AssetMeta::Load(trash.TrashMetaPath, currentMeta, diagnostic) || currentMeta.ID != trash.AssetGuid)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, trash.TrashMetaPath,
            TEXT("Asset trash metadata changed during restore validation."));

    OperationJournal journal;
    journal.TransactionId = Guid::New();
    journal.Kind = AssetOperationKind::Restore;
    journal.AssetGuid = trash.AssetGuid;
    journal.SourcePath = trash.TrashSourcePath;
    journal.DestinationPath = trash.OriginalSourcePath;
    journal.SourceFragmentsPath = trash.TrashFragmentsPath;
    journal.DestinationFragmentsPath = trash.OriginalFragmentsPath;
    const String transactionDirectory = _transactionsRoot / journal.TransactionId.ToString(Guid::FormatType::N);
    journal.StageSourcePath = transactionDirectory / TEXT("source.stage");
    journal.StageMetaPath = transactionDirectory / TEXT("source.meta.stage");
    journal.StageFragmentsPath = transactionDirectory / TEXT("fragments.stage");
    if (SaveJournal(transactionDirectory, journal, diagnostic) ||
        (hasFragments && DurableMove(journal.StageFragmentsPath, trash.TrashFragmentsPath, false)) ||
        DurableMove(journal.StageSourcePath, trash.TrashSourcePath, false) ||
        DurableMove(journal.StageMetaPath, trash.TrashMetaPath, false) ||
        DurableMove(trash.OriginalSourcePath, journal.StageSourcePath, false) ||
        DurableMove(trash.OriginalMetaPath, journal.StageMetaPath, false) ||
        (hasFragments && DurableMove(trash.OriginalFragmentsPath, journal.StageFragmentsPath, false)))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, trash.OriginalSourcePath,
                TEXT("Asset restore transaction could not publish source and metadata."));
        return true;
    }
    journal.Phase = JournalPhase::Applied;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    journal.Phase = JournalPhase::Committed;
    if (SaveJournal(transactionDirectory, journal, diagnostic))
    {
        RollbackJournal(journal);
        DurableDeleteDirectory(transactionDirectory);
        return true;
    }
    DurableDeleteDirectory(transactionDirectory);
    AssetOperationCommit commit;
    commit.TransactionId = journal.TransactionId;
    commit.Kind = AssetOperationKind::Restore;
    commit.AssetGuid = trash.AssetGuid;
    commit.SourcePath = trash.TrashSourcePath;
    commit.DestinationPath = trash.OriginalSourcePath;
    AddSelfWrite(commit, trash.OriginalSourcePath);
    AddSelfWrite(commit, trash.OriginalMetaPath);
    return PublishCommit(commit, diagnostic);
}

bool AssetOperations::IsAssetEditing() const
{
    _stateLocker.Lock();
    const bool result = _editingDepth > 0;
    _stateLocker.Unlock();
    return result;
}

void AssetOperations::StartAssetEditing()
{
    _stateLocker.Lock();
    _editingDepth++;
    _stateLocker.Unlock();
}

bool AssetOperations::StopAssetEditing(AssetPipelineDiagnostic& diagnostic)
{
    Array<AssetOperationCommit> commits;
    _stateLocker.Lock();
    if (_editingDepth == 0)
    {
        _stateLocker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, StringView::Empty,
            TEXT("StopAssetEditing has no matching StartAssetEditing scope."));
    }
    _editingDepth--;
    if (_editingDepth != 0)
    {
        _stateLocker.Unlock();
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    commits = MoveTemp(_pendingCommits);
    _pendingCommits.Clear();
    _stateLocker.Unlock();
    if (commits.IsEmpty())
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    if (!_databaseCallbacks.RefreshCommitted(commits, diagnostic))
    {
        _stateLocker.Lock();
        for (const AssetOperationCommit& published : commits)
            _selfWrites.Add(published.SelfWrites);
        _stateLocker.Unlock();
        return false;
    }

    // Publishing is the boundary that makes a committed filesystem batch visible. Keep a failed
    // batch pending so a later editing scope or explicit retry cannot silently lose it.
    _stateLocker.Lock();
    Array<AssetOperationCommit> pending = MoveTemp(_pendingCommits);
    _pendingCommits = MoveTemp(commits);
    _pendingCommits.Add(pending);
    _stateLocker.Unlock();
    return true;
}

void AssetOperations::DrainSelfWrites(Array<AssetOperationSelfWrite>& result)
{
    _stateLocker.Lock();
    result = MoveTemp(_selfWrites);
    _selfWrites.Clear();
    _stateLocker.Unlock();
}

bool AssetOperations::RecoverIncompleteTransactions(Array<AssetPipelineDiagnostic>& diagnostics)
{
    diagnostics.Clear();
    if (!FileSystem::DirectoryExists(_transactionsRoot))
        return false;
    Array<String> directories;
    if (FileSystem::GetChildDirectories(directories, _transactionsRoot))
    {
        AssetPipelineDiagnostic diagnostic;
        Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, _transactionsRoot,
            TEXT("Cannot enumerate asset operation transaction recovery state."));
        diagnostics.Add(MoveTemp(diagnostic));
        return true;
    }
    for (const String& directory : directories)
    {
        OperationJournal journal;
        AssetPipelineDiagnostic diagnostic;
        if (LoadJournal(directory, journal, diagnostic))
        {
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        if (journal.Phase != JournalPhase::Committed && RollbackJournal(journal))
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, journal.SourcePath,
                TEXT("Incomplete asset operation could not be rolled back; recovery data was preserved."));
            diagnostic.Related.Add(directory);
            diagnostics.Add(MoveTemp(diagnostic));
            continue;
        }
        if (DurableDeleteDirectory(directory))
        {
            Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, directory,
                TEXT("Recovered asset operation directory could not be removed."));
            diagnostics.Add(MoveTemp(diagnostic));
        }
    }
    return diagnostics.HasItems();
}
