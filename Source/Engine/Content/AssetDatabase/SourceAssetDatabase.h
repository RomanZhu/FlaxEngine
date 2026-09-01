// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseReadSnapshot.h"
#include "AssetDatabaseTransaction.h"
#include "FileChangeJournal.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/CriticalSection.h"
#include <memory>

/// <summary>Commit-sampled automatic normalized-database checkpoint thresholds. A zero value disables that trigger.</summary>
struct FLAXENGINE_API SourceAssetDatabaseCheckpointPolicy
{
    uint64 MaximumWalBytes = 64ull * 1024ull * 1024ull;
    uint32 MaximumTransactions = 256;
    double MaximumElapsedSeconds = 300.0;
};

/// <summary>Durable transactional authority for normalized source asset state under Library.</summary>
class FLAXENGINE_API SourceAssetDatabase
{
    friend class AssetDatabaseTransaction;

private:
    mutable CriticalSection _locker;
    String _directory;
    String _manifestPath;
    String _walPath;
    String _journalPath;
    String _sessionMarkerPath;
    SourceAssetDatabaseState _state;
    uint64 _revision = 0;
    bool _transactionActive = false;
    FileChangeJournal _journal;
    File* _writerLock = nullptr;
    uint64 _checkpointGeneration = 0;
    uint64 _walBaseRevision = 0;
    uint64 _walLastRevision = 0;
    SourceAssetDatabaseCheckpointPolicy _checkpointPolicy;
    uint32 _transactionsSinceCheckpoint = 0;
    double _lastCheckpointTime = 0.0;
    uint64 _checkpointRetryRevision = 0;
    double _checkpointRetryTime = 0.0;
    bool _open = false;
    bool _lastShutdownWasClean = true;
    bool _recoveryRequired = false;

    bool Commit(AssetDatabaseTransaction& transaction, AssetPipelineDiagnostic& diagnostic);
    void Rollback(AssetDatabaseTransaction& transaction);
    bool CheckpointLocked(const SourceAssetDatabaseState& state, AssetPipelineDiagnostic& diagnostic);
    bool ShouldCheckpointLocked() const;

public:
    Delegate<const AssetChangeSet&> Changed;

    ~SourceAssetDatabase();

    /// <summary>Opens Library/AssetDatabase and performs WAL/torn-journal recovery. Returns true on failure.</summary>
    bool Open(const StringView& libraryPath, const Guid& projectId, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Persists a clean-shutdown marker and releases the database.</summary>
    bool Close(AssetPipelineDiagnostic* diagnostic = nullptr);
    bool IsOpen() const;
    bool WasLastShutdownClean() const;
    uint64 GetRevision() const;
    const String& GetDirectory() const;

    void SetCheckpointPolicy(const SourceAssetDatabaseCheckpointPolicy& policy);
    SourceAssetDatabaseCheckpointPolicy GetCheckpointPolicy() const;

    /// <summary>Durably checkpoints current rows and truncates the WAL only after publication succeeds.</summary>
    bool Checkpoint(AssetPipelineDiagnostic& diagnostic);

    AssetDatabaseReadSnapshot Read() const;
    std::unique_ptr<AssetDatabaseTransaction> BeginTransaction();

    /// <summary>Reads ordered changes. A true requiresSnapshot means the cursor predates retained history.</summary>
    bool ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const;
};
