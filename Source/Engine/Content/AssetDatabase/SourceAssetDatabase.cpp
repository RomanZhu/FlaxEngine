// Copyright (c) Wojciech Figat. All rights reserved.

#include "SourceAssetDatabase.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Crc.h"
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    constexpr uint32 SnapshotMagic = 0x53444146; // FADS
    constexpr uint32 SnapshotVersion = 1;
    constexpr uint32 WalMagic = 0x57444146; // FADW
    constexpr uint32 WalVersion = 1;
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

    bool AtomicReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath(destination);
        const String stagingPath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, true);
#endif
    }

    bool WriteAtomic(const StringView& path, const void* data, uint32 length)
    {
        const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
        SCOPE_EXIT { FileSystem::DeleteFile(staging); };
        return File::WriteAllBytes(staging, data, length) || AtomicReplace(path, staging);
    }

    bool SaveSnapshot(const StringView& path, const SourceAssetDatabaseState& state, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> payload;
        state.Serialize(payload);
        SnapshotHeader header = { SnapshotMagic, SnapshotVersion, (uint32)payload.Count(), Crc::MemCrc32(payload.Get(), payload.Count()) };
        Array<byte> file;
        file.Resize(sizeof(header) + payload.Count(), false);
        Platform::MemoryCopy(file.Get(), &header, sizeof(header));
        if (payload.HasItems())
            Platform::MemoryCopy(file.Get() + sizeof(header), payload.Get(), payload.Count());
        if (WriteAtomic(path, file.Get(), file.Count()))
            return Fail(diagnostic, path, TEXT("Cannot atomically write the source asset database snapshot."));
        return false;
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

    bool SaveWal(const StringView& path, const SourceAssetDatabaseState& state, const AssetChangeSet& changes, AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> statePayload, changePayload;
        state.Serialize(statePayload);
        changes.Serialize(changePayload);
        WalHeader header = { WalMagic, WalVersion, (uint32)statePayload.Count(), (uint32)changePayload.Count(),
            Crc::MemCrc32(statePayload.Get(), statePayload.Count()), Crc::MemCrc32(changePayload.Get(), changePayload.Count()) };
        Array<byte> file;
        file.Resize(sizeof(header) + statePayload.Count() + changePayload.Count(), false);
        Platform::MemoryCopy(file.Get(), &header, sizeof(header));
        Platform::MemoryCopy(file.Get() + sizeof(header), statePayload.Get(), statePayload.Count());
        Platform::MemoryCopy(file.Get() + sizeof(header) + statePayload.Count(), changePayload.Get(), changePayload.Count());
        if (WriteAtomic(path, file.Get(), file.Count()))
            return Fail(diagnostic, path, TEXT("Cannot atomically write the source asset database WAL."));
        return false;
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

bool SourceAssetDatabase::Open(const StringView& libraryPath, const Guid& projectId, AssetPipelineDiagnostic& diagnostic)
{
    if (!projectId.IsValid())
        return Fail(diagnostic, libraryPath, TEXT("Source asset database requires a valid project identity."));
    Close();
    _directory = String(libraryPath) / TEXT("AssetDatabase");
    _snapshotPath = _directory / TEXT("source-database.bin");
    _walPath = _directory / TEXT("source-database.wal");
    _journalPath = _directory / TEXT("source-changes.log");
    if ((!FileSystem::DirectoryExists(_directory) && FileSystem::CreateDirectory(_directory)) || FileSystem::FileExists(_directory))
        return Fail(diagnostic, _directory, TEXT("Cannot create the source asset database directory."), AssetPipelineDiagnosticCode::LibraryCreationFailed);

    SourceAssetDatabaseState state;
    AssetChangeSet walChanges;
    SourceAssetDatabaseState walState;
    const bool hasSnapshot = FileSystem::FileExists(_snapshotPath);
    const bool hasWal = FileSystem::FileExists(_walPath);
    bool stateFromWal = false;
    if (hasSnapshot && LoadSnapshot(_snapshotPath, state, diagnostic))
    {
        if (!hasWal || LoadWal(_walPath, walState, walChanges, diagnostic))
            return true;
        state = walState;
        stateFromWal = true;
    }
    else if (!hasSnapshot)
    {
        if (hasWal)
        {
            if (LoadWal(_walPath, state, walChanges, diagnostic))
                return true;
            walState = state;
            stateFromWal = true;
        }
        else
        {
            state.Database.ProjectId = projectId;
            state.Database.CleanShutdown = true;
        }
    }
    if (state.Database.ProjectId != projectId)
        return Fail(diagnostic, _snapshotPath, TEXT("Source asset database belongs to another project."));

    const uint64 journalBase = stateFromWal && state.Database.CurrentRevision ? state.Database.CurrentRevision - 1 : state.Database.CurrentRevision;
    if (_journal.Open(_journalPath, journalBase, diagnostic))
        return true;
    if (hasWal && !stateFromWal && LoadWal(_walPath, walState, walChanges, diagnostic))
        return true;
    if (hasWal)
    {
        if (walState.Database.ProjectId != projectId || walState.Database.CurrentRevision < state.Database.CurrentRevision ||
            walState.Database.CurrentRevision > state.Database.CurrentRevision + (stateFromWal ? 0 : 1))
            return Fail(diagnostic, _walPath, TEXT("Source asset database WAL revision is inconsistent with the snapshot."));
        if (_journal.GetLastRevision() < walChanges.Revision && _journal.Append(walChanges, diagnostic))
            return true;
        if (SaveSnapshot(_snapshotPath, walState, diagnostic))
            return true;
        state = walState;
        FileSystem::DeleteFile(_walPath);
    }
    else if (_journal.GetLastRevision() != state.Database.CurrentRevision &&
        _journal.Reset(state.Database.CurrentRevision, diagnostic))
    {
        return true;
    }

    _lastShutdownWasClean = state.Database.CleanShutdown;
    state.Database.CleanShutdown = false;
    if (SaveSnapshot(_snapshotPath, state, diagnostic))
        return true;
    _state = std::make_shared<const SourceAssetDatabaseState>(MoveTemp(state));
    _open = true;
    _recoveryRequired = false;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool SourceAssetDatabase::Close(AssetPipelineDiagnostic* diagnostic)
{
    AssetPipelineDiagnostic localDiagnostic;
    _locker.Lock();
    bool failed = false;
    if (_open && _state && !_recoveryRequired)
    {
        SourceAssetDatabaseState state = *_state;
        state.Database.CleanShutdown = true;
        failed = SaveSnapshot(_snapshotPath, state, localDiagnostic);
    }
    _state.reset();
    _journal.Close();
    _open = false;
    _recoveryRequired = false;
    _directory.Clear();
    _snapshotPath.Clear();
    _walPath.Clear();
    _journalPath.Clear();
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
    return _state ? _state->Database.CurrentRevision : 0;
}

const String& SourceAssetDatabase::GetDirectory() const
{
    return _directory;
}

AssetDatabaseReadSnapshot SourceAssetDatabase::Read() const
{
    ScopeLock lock(_locker);
    return AssetDatabaseReadSnapshot(_state);
}

std::unique_ptr<AssetDatabaseTransaction> SourceAssetDatabase::BeginTransaction()
{
    ScopeLock lock(_locker);
    if (!_open || !_state || _recoveryRequired)
        return nullptr;
    return std::unique_ptr<AssetDatabaseTransaction>(new AssetDatabaseTransaction(this, *_state));
}

bool SourceAssetDatabase::ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const
{
    ScopeLock lock(_locker);
    return _journal.ReadAfter(revision, result, requiresSnapshot, diagnostic);
}

bool SourceAssetDatabase::Commit(AssetDatabaseTransaction& transaction, AssetPipelineDiagnostic& diagnostic)
{
    _locker.Lock();
    if (!_open || !_state || _recoveryRequired || transaction._owner != this || transaction._baseRevision != _state->Database.CurrentRevision)
    {
        _locker.Unlock();
        return Fail(diagnostic, _snapshotPath, TEXT("Source asset database transaction conflicts with a newer revision."));
    }
    transaction._changes.Revision = transaction._baseRevision + 1;
    transaction._state.Database.CurrentRevision = transaction._changes.Revision;
    transaction._state.Database.CleanShutdown = false;
    if (transaction._state.Validate(diagnostic) || SaveWal(_walPath, transaction._state, transaction._changes, diagnostic))
    {
        _locker.Unlock();
        return true;
    }
    if (_journal.Append(transaction._changes, diagnostic) || SaveSnapshot(_snapshotPath, transaction._state, diagnostic))
    {
        _recoveryRequired = true;
        _locker.Unlock();
        return true;
    }
    FileSystem::DeleteFile(_walPath);
    _state = std::make_shared<const SourceAssetDatabaseState>(transaction._state);
    const AssetChangeSet changes = transaction._changes;
    _locker.Unlock();
    Changed(changes);
    return false;
}
