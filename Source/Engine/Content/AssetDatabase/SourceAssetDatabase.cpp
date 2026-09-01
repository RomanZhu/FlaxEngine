// Copyright (c) Wojciech Figat. All rights reserved.

#include "SourceAssetDatabase.h"
#include "DurableAssetFileSystem.h"
#include "NormalizedAssetDatabaseStore.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Utilities/Crc.h"
#include <cmath>

namespace
{
    constexpr uint32 SnapshotMagic = 0x53444146; // FADS
    constexpr uint32 SnapshotVersion = 1;
    constexpr uint32 WalMagic = 0x57444146; // FADW
    constexpr uint32 WalVersion = 2;
    constexpr uint32 MaximumDatabaseBytes = 1024 * 1024 * 1024;

    struct SnapshotHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 PayloadSize;
        uint32 PayloadCrc;
    };

    struct WalHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 StateSize;
        uint32 ChangeSize;
        uint32 StateCrc;
        uint32 ChangeCrc;
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, const StringView& path, const StringView& message,
        AssetPipelineDiagnosticCode code = AssetPipelineDiagnosticCode::SnapshotInvalid)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    bool WriteAtomic(const StringView& path, const void* data, uint32 length)
    {
        const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { DurableAssetFileSystem::DeleteFile(staging); };
        return DurableAssetFileSystem::WriteFile(staging, data, length) ||
               DurableAssetFileSystem::Replace(path, staging);
    }

    bool LoadSnapshot(const StringView& path, SourceAssetDatabaseState& state, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> file;
        if (File::ReadAllBytes(path, file) || file.Count() < (int32)sizeof(SnapshotHeader))
            return Fail(diagnostic, path, TEXT("Source asset database snapshot is missing or truncated."));
        SnapshotHeader header;
        Platform::MemoryCopy(&header, file.Get(), sizeof(header));
        if (header.Magic != SnapshotMagic || header.Version != SnapshotVersion || header.PayloadSize > MaximumDatabaseBytes ||
            header.PayloadSize != (uint32)file.Count() - sizeof(header) ||
            header.PayloadCrc != Crc::MemCrc32(file.Get() + sizeof(header), header.PayloadSize))
            return Fail(diagnostic, path, TEXT("Source asset database snapshot version or checksum is invalid."));
        return SourceAssetDatabaseState::Deserialize(file.Get() + sizeof(header), header.PayloadSize, state, diagnostic);
    }

    bool LoadWal(const StringView& path, SourceAssetDatabaseState& state, AssetChangeSet& changes, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> file;
        if (File::ReadAllBytes(path, file) || file.Count() < (int32)sizeof(WalHeader))
            return Fail(diagnostic, path, TEXT("Source asset database WAL is truncated."));
        WalHeader header;
        Platform::MemoryCopy(&header, file.Get(), sizeof(header));
        if (header.Magic != WalMagic || header.Version != WalVersion || header.StateSize > MaximumDatabaseBytes ||
            header.ChangeSize > MaximumDatabaseBytes || sizeof(header) + header.StateSize + header.ChangeSize != (uint32)file.Count())
            return Fail(diagnostic, path, TEXT("Source asset database WAL header is invalid."));
        const byte* statePayload = file.Get() + sizeof(header);
        const byte* changePayload = statePayload + header.StateSize;
        if (header.StateCrc != Crc::MemCrc32(statePayload, header.StateSize) ||
            header.ChangeCrc != Crc::MemCrc32(changePayload, header.ChangeSize) ||
            SourceAssetDatabaseState::Deserialize(statePayload, header.StateSize, state, diagnostic) ||
            AssetChangeSet::Deserialize(changePayload, header.ChangeSize, changes) ||
            changes.Revision != state.Database.CurrentRevision)
            return Fail(diagnostic, path, TEXT("Source asset database WAL payload is invalid."));
        return false;
    }
}

SourceAssetDatabase::~SourceAssetDatabase()
{
    Close();
}

bool SourceAssetDatabase::CheckpointLocked(const SourceAssetDatabaseState& state, AssetPipelineDiagnostic& diagnostic)
{
    if (NormalizedAssetDatabaseStore::SaveCheckpoint(_directory, state, _checkpointGeneration, diagnostic) ||
        NormalizedAssetDatabaseStore::ResetWal(_walPath, state.Database.ProjectId,
            state.Database.CurrentRevision, diagnostic))
        return true;
    _walBaseRevision = state.Database.CurrentRevision;
    _walLastRevision = state.Database.CurrentRevision;
    _transactionsSinceCheckpoint = 0;
    _lastCheckpointTime = Platform::GetTimeSeconds();
    _checkpointRetryRevision = 0;
    _checkpointRetryTime = 0.0;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SourceAssetDatabase::ShouldCheckpointLocked() const
{
    const double now = Platform::GetTimeSeconds();
    if (_open && _revision < _checkpointRetryRevision && now < _checkpointRetryTime)
        return false;
    if (_checkpointPolicy.MaximumTransactions &&
        _transactionsSinceCheckpoint >= _checkpointPolicy.MaximumTransactions)
        return true;
    if (_checkpointPolicy.MaximumWalBytes && FileSystem::GetFileSize(_walPath) >= _checkpointPolicy.MaximumWalBytes)
        return true;
    return _checkpointPolicy.MaximumElapsedSeconds > 0.0 && _lastCheckpointTime > 0.0 &&
           now - _lastCheckpointTime >= _checkpointPolicy.MaximumElapsedSeconds;
}

bool SourceAssetDatabase::Open(const StringView& libraryPath, const Guid& projectId, AssetPipelineDiagnostic& diagnostic)
{
    if (!projectId.IsValid())
        return Fail(diagnostic, libraryPath, TEXT("Source asset database requires a valid project identity."));
    Close();
    _directory = String(libraryPath) / TEXT("AssetDatabase");
    _manifestPath = NormalizedAssetDatabaseStore::GetManifestPath(_directory);
    _walPath = NormalizedAssetDatabaseStore::GetWalPath(_directory);
    _journalPath = _directory / TEXT("source-changes.log");
    _sessionMarkerPath = _directory / TEXT("unclean-session.marker");
    if (DurableAssetFileSystem::EnsureDirectory(_directory) || FileSystem::FileExists(_directory))
        return Fail(diagnostic, _directory, TEXT("Cannot create the source asset database directory."), AssetPipelineDiagnosticCode::LibraryCreationFailed);

    const String writerLockPath = _directory / TEXT("writer.lock");
    _writerLock = File::Open(writerLockPath, FileMode::OpenAlways, FileAccess::ReadWrite, FileShare::None);
    if (!_writerLock)
        return Fail(diagnostic, writerLockPath, TEXT("Another process owns the source asset database writer."));
    bool opened = false;
    SCOPE_EXIT
    {
        if (!opened)
        {
            Delete(_writerLock);
            _writerLock = nullptr;
        }
    };

    SourceAssetDatabaseState state;
    bool rebuiltIncompatibleStore = false;
    if (FileSystem::FileExists(_manifestPath))
    {
        NormalizedAssetDatabaseLoadFailure loadFailure;
        if (NormalizedAssetDatabaseStore::LoadCheckpoint(_directory, projectId, state, _checkpointGeneration,
            diagnostic, loadFailure))
        {
            if (loadFailure != NormalizedAssetDatabaseLoadFailure::RecoverableDerivedState)
                return true;
            // Library/AssetDatabase is a derived cache. Publish a fresh generation instead of
            // failing project startup when an older schema or corrupt checkpoint cannot be read.
            state = SourceAssetDatabaseState();
            state.Database.ProjectId = projectId;
            state.Database.CleanShutdown = true;
            _checkpointGeneration = 0;
            diagnostic = AssetPipelineDiagnostic();
            if (NormalizedAssetDatabaseStore::SaveCheckpoint(_directory, state, _checkpointGeneration, diagnostic) ||
                NormalizedAssetDatabaseStore::ResetWal(_walPath, projectId, 0, diagnostic))
                return true;
            rebuiltIncompatibleStore = true;
        }
    }
    else
    {
        // One-time migration from the v2 replacement-image store. It is never used after the
        // normalized manifest has been atomically published.
        const String legacySnapshotPath = _directory / TEXT("source-database.bin");
        const String legacyWalPath = _directory / TEXT("source-database.wal");
        if (FileSystem::FileExists(legacySnapshotPath) && LoadSnapshot(legacySnapshotPath, state, diagnostic))
            return true;
        if (!FileSystem::FileExists(legacySnapshotPath))
        {
            state.Database.ProjectId = projectId;
            state.Database.CleanShutdown = true;
        }
        if (FileSystem::FileExists(legacyWalPath))
        {
            SourceAssetDatabaseState legacyWalState;
            AssetChangeSet legacyChanges;
            if (LoadWal(legacyWalPath, legacyWalState, legacyChanges, diagnostic))
                return true;
            if (legacyWalState.Database.CurrentRevision >= state.Database.CurrentRevision)
                state = MoveTemp(legacyWalState);
        }
        state.Database.SchemaVersion = AssetDatabaseSchema::Version;
        if (state.Database.ProjectId != projectId)
            return Fail(diagnostic, legacySnapshotPath, TEXT("Source asset database belongs to another project."));
        if (NormalizedAssetDatabaseStore::SaveCheckpoint(_directory, state, _checkpointGeneration, diagnostic))
            return true;
    }

    _lastShutdownWasClean = !rebuiltIncompatibleStore && state.Database.CleanShutdown && !FileSystem::FileExists(_sessionMarkerPath);
    const uint64 checkpointRevision = state.Database.CurrentRevision;
    Array<NormalizedAssetDatabaseWalRecord> walRecords;
    if (NormalizedAssetDatabaseStore::OpenWal(_walPath, projectId, checkpointRevision, _walBaseRevision,
        _walLastRevision, walRecords, diagnostic))
        return true;

    for (const NormalizedAssetDatabaseWalRecord& record : walRecords)
    {
        if (record.BaseRevision != state.Database.CurrentRevision)
            return Fail(diagnostic, _walPath, TEXT("Normalized source database WAL transaction conflicts with its checkpoint."));
        AssetDatabaseTransaction replay(this, MoveTemp(state));
        for (const AssetDatabaseMutation& mutation : record.Mutations)
        {
            SourceAssetDatabaseState payload;
            if (mutation.Payload.HasItems() && SourceAssetDatabaseState::Deserialize(mutation.Payload.Get(),
                mutation.Payload.Count(), payload, diagnostic, false))
                return Fail(diagnostic, _walPath, TEXT("Normalized source database WAL row payload is invalid."));
            switch (mutation.Kind)
            {
            case AssetDatabaseMutationKind::SetLastCompleteScanId:
                replay.SetLastCompleteScanId(mutation.Value);
                break;
            case AssetDatabaseMutationKind::SetImporterRegistryGeneration:
                replay.SetImporterRegistryGeneration(mutation.Value);
                break;
            case AssetDatabaseMutationKind::UpsertSource:
                if (!payload.Sources.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL source upsert payload is empty."));
                replay.UpsertSource(payload.Sources[0]);
                break;
            case AssetDatabaseMutationKind::RemoveSource:
                replay.RemoveSource(mutation.Key);
                break;
            case AssetDatabaseMutationKind::ReplaceObjects:
                replay.ReplaceObjects(mutation.Key, payload.Objects);
                break;
            case AssetDatabaseMutationKind::ReplaceDependencies:
                if (mutation.LocalFileId)
                    replay.ReplaceDependencies(mutation.Key, mutation.LocalFileId, mutation.TargetId, payload.Dependencies);
                else
                    replay.ReplaceDependencies(mutation.Key, mutation.TargetId, payload.Dependencies);
                break;
            case AssetDatabaseMutationKind::UpsertPublication:
                if (!payload.Publications.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL publication payload is empty."));
                replay.UpsertPublication(payload.Publications[0]);
                break;
            case AssetDatabaseMutationKind::ReplaceDiagnostics:
                replay.ReplaceDiagnostics(mutation.Key, payload.Diagnostics);
                break;
            case AssetDatabaseMutationKind::UpsertImportTarget:
                if (!payload.ImportTargets.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL import target payload is empty."));
                replay.UpsertImportTarget(payload.ImportTargets[0]);
                break;
            case AssetDatabaseMutationKind::ReplaceArtifactObjects:
                replay.ReplaceArtifactObjects(mutation.Artifact, payload.ArtifactObjects);
                break;
            case AssetDatabaseMutationKind::SetLabels:
            {
                Array<String> labels;
                for (const SourceAssetLabelRow& row : payload.Labels)
                    labels.Add(row.Label);
                replay.SetLabels(mutation.Key, labels);
                break;
            }
            case AssetDatabaseMutationKind::AppendFileJournal:
                if (!payload.FileJournal.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL file journal payload is empty."));
                replay.AppendFileJournal(payload.FileJournal[0]);
                break;
            case AssetDatabaseMutationKind::UpsertRefreshSession:
                if (!payload.RefreshSessions.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL refresh session payload is empty."));
                replay.UpsertRefreshSession(payload.RefreshSessions[0]);
                break;
            case AssetDatabaseMutationKind::UpsertImportAttempt:
                if (!payload.ImportAttempts.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL import attempt payload is empty."));
                replay.UpsertImportAttempt(payload.ImportAttempts[0]);
                break;
            case AssetDatabaseMutationKind::UpsertCustomDependency:
                if (!payload.CustomDependencies.HasItems()) return Fail(diagnostic, _walPath, TEXT("WAL custom dependency payload is empty."));
                replay.UpsertCustomDependency(payload.CustomDependencies[0]);
                break;
            case AssetDatabaseMutationKind::RemoveCustomDependency:
                replay.RemoveCustomDependency(mutation.TargetId);
                break;
            case AssetDatabaseMutationKind::ReplaceSnapshot:
                replay._state = MoveTemp(payload);
                break;
            }
        }
        replay._state.Database.CurrentRevision = record.Revision;
        replay._state.Database.CleanShutdown = false;
        if (replay._state.Validate(diagnostic))
            return true;
        state = MoveTemp(replay._state);
    }

    if (_journal.Open(_journalPath, checkpointRevision, diagnostic))
        return true;
    if (_journal.GetLastRevision() > state.Database.CurrentRevision || _journal.GetLastRevision() < checkpointRevision)
    {
        if (_journal.Reset(checkpointRevision, diagnostic))
            return true;
    }
    for (const NormalizedAssetDatabaseWalRecord& record : walRecords)
        if (_journal.GetLastRevision() < record.Revision && _journal.Append(record.Changes, diagnostic))
            return true;
    if (_journal.GetLastRevision() != state.Database.CurrentRevision && _journal.Reset(state.Database.CurrentRevision, diagnostic))
        return true;

    state.Database.CleanShutdown = false;
    const byte marker = 1;
    if (WriteAtomic(_sessionMarkerPath, &marker, sizeof(marker)))
        return Fail(diagnostic, _sessionMarkerPath, TEXT("Cannot persist the source asset database session marker."));
    _state = MoveTemp(state);
    _revision = _state.Database.CurrentRevision;
    _transactionActive = false;
    _transactionsSinceCheckpoint = walRecords.Count();
    _lastCheckpointTime = Platform::GetTimeSeconds();
    _checkpointRetryRevision = 0;
    _checkpointRetryTime = 0.0;
    _open = true;
    _recoveryRequired = false;
    opened = true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SourceAssetDatabase::Close(AssetPipelineDiagnostic* diagnostic)
{
    AssetPipelineDiagnostic localDiagnostic;
    _locker.Lock();
    bool failed = false;
    if (_open && !_transactionActive && !_recoveryRequired)
    {
        SourceAssetDatabaseState state = _state;
        state.Database.CleanShutdown = true;
        failed = CheckpointLocked(state, localDiagnostic);
        if (!failed && DurableAssetFileSystem::DeleteFile(_sessionMarkerPath))
            failed = Fail(localDiagnostic, _sessionMarkerPath, TEXT("Cannot durably clear the source asset database session marker."));
    }
    _state = SourceAssetDatabaseState();
    _revision = 0;
    _transactionActive = false;
    _journal.Close();
    if (_writerLock)
        Delete(_writerLock);
    _writerLock = nullptr;
    _open = false;
    _recoveryRequired = false;
    _directory.Clear();
    _manifestPath.Clear();
    _walPath.Clear();
    _journalPath.Clear();
    _sessionMarkerPath.Clear();
    _checkpointGeneration = 0;
    _walBaseRevision = 0;
    _walLastRevision = 0;
    _transactionsSinceCheckpoint = 0;
    _lastCheckpointTime = 0.0;
    _checkpointRetryRevision = 0;
    _checkpointRetryTime = 0.0;
    _locker.Unlock();
    if (diagnostic)
        *diagnostic = localDiagnostic;
    return failed;
}

bool SourceAssetDatabase::IsOpen() const
{
    ScopeLock lock(_locker);
    return _open;
}

bool SourceAssetDatabase::WasLastShutdownClean() const
{
    ScopeLock lock(_locker);
    return _lastShutdownWasClean;
}

uint64 SourceAssetDatabase::GetRevision() const
{
    ScopeLock lock(_locker);
    return _open ? _revision : 0;
}

const String& SourceAssetDatabase::GetDirectory() const
{
    return _directory;
}

void SourceAssetDatabase::SetCheckpointPolicy(const SourceAssetDatabaseCheckpointPolicy& policy)
{
    ScopeLock lock(_locker);
    _checkpointPolicy = policy;
    if (!std::isfinite(_checkpointPolicy.MaximumElapsedSeconds) || _checkpointPolicy.MaximumElapsedSeconds < 0.0)
        _checkpointPolicy.MaximumElapsedSeconds = 0.0;
}

SourceAssetDatabaseCheckpointPolicy SourceAssetDatabase::GetCheckpointPolicy() const
{
    ScopeLock lock(_locker);
    return _checkpointPolicy;
}

bool SourceAssetDatabase::Checkpoint(AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock lock(_locker);
    if (!_open || _transactionActive || _recoveryRequired)
        return Fail(diagnostic, _manifestPath, TEXT("Source asset database is unavailable for checkpointing."));
    return CheckpointLocked(_state, diagnostic);
}

AssetDatabaseReadSnapshot SourceAssetDatabase::Read() const
{
    ScopeLock lock(_locker);
    return !_open || _transactionActive
        ? AssetDatabaseReadSnapshot()
        : AssetDatabaseReadSnapshot(std::make_shared<const SourceAssetDatabaseState>(_state));
}

std::unique_ptr<AssetDatabaseTransaction> SourceAssetDatabase::BeginTransaction()
{
    ScopeLock lock(_locker);
    if (!_open || _transactionActive || _recoveryRequired)
        return nullptr;
    _transactionActive = true;
    return std::unique_ptr<AssetDatabaseTransaction>(new AssetDatabaseTransaction(this, MoveTemp(_state)));
}

bool SourceAssetDatabase::ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const
{
    ScopeLock lock(_locker);
    return _journal.ReadAfter(revision, result, requiresSnapshot, diagnostic);
}

bool SourceAssetDatabase::Commit(AssetDatabaseTransaction& transaction, AssetPipelineDiagnostic& diagnostic)
{
    _locker.Lock();
    if (!_open || !_transactionActive || _recoveryRequired || transaction._owner != this || transaction._baseRevision != _revision)
    {
        const Char* message = !_open
            ? TEXT("Source asset database is closed during transaction commit.")
            : !_transactionActive
                ? TEXT("Source asset database transaction is no longer active during commit.")
                : _recoveryRequired
                    ? TEXT("Source asset database requires recovery before transaction commit.")
                    : transaction._owner != this
                        ? TEXT("Source asset database transaction belongs to another database.")
                        : TEXT("Source asset database transaction conflicts with a newer revision.");
        _locker.Unlock();
        return Fail(diagnostic, _manifestPath, message);
    }
    transaction._changes.Revision = transaction._baseRevision + 1;
    transaction._state.Database.CurrentRevision = transaction._changes.Revision;
    transaction._state.Database.CleanShutdown = false;
    if (transaction._state.Validate(diagnostic))
    {
        transaction.RestoreUndo();
        transaction._state.Database.CurrentRevision = transaction._baseRevision;
        _state = MoveTemp(transaction._state);
        _transactionActive = false;
        transaction._owner = nullptr;
        _locker.Unlock();
        return true;
    }
    NormalizedAssetDatabaseWalRecord record;
    record.BaseRevision = transaction._baseRevision;
    record.Revision = transaction._changes.Revision;
    record.Mutations = transaction._mutations;
    record.Changes = transaction._changes;
    if (NormalizedAssetDatabaseStore::AppendWal(_walPath, transaction._state.Database.ProjectId,
        _walLastRevision, record, diagnostic))
    {
        _recoveryRequired = true;
        transaction.RestoreUndo();
        transaction._state.Database.CurrentRevision = transaction._baseRevision;
        _state = MoveTemp(transaction._state);
        _transactionActive = false;
        transaction._owner = nullptr;
        _locker.Unlock();
        return true;
    }
    if (_journal.Append(transaction._changes, diagnostic))
    {
        _recoveryRequired = true;
        transaction.RestoreUndo();
        transaction._state.Database.CurrentRevision = transaction._baseRevision;
        _state = MoveTemp(transaction._state);
        _transactionActive = false;
        transaction._owner = nullptr;
        _locker.Unlock();
        return true;
    }
    _state = MoveTemp(transaction._state);
    _revision = _state.Database.CurrentRevision;
    _transactionActive = false;
    transaction._owner = nullptr;
    _transactionsSinceCheckpoint++;
    if (ShouldCheckpointLocked())
    {
        AssetPipelineDiagnostic checkpointDiagnostic;
        if (CheckpointLocked(_state, checkpointDiagnostic))
        {
            _checkpointRetryRevision = _state.Database.CurrentRevision + 64;
            _checkpointRetryTime = Platform::GetTimeSeconds() + 60.0;
            LOG(Error, "Automatic source asset database checkpoint failed. {0}", checkpointDiagnostic.Message);
        }
    }
    const AssetChangeSet changes = transaction._changes;
    _locker.Unlock();
    Changed(changes);
    return false;
}

void SourceAssetDatabase::Rollback(AssetDatabaseTransaction& transaction)
{
    ScopeLock lock(_locker);
    if (_transactionActive && transaction._owner == this)
    {
        _state = MoveTemp(transaction._state);
        _transactionActive = false;
    }
    transaction._owner = nullptr;
}
