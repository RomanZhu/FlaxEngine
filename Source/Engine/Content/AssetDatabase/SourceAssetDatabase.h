// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseReadSnapshot.h"
#include "AssetDatabaseTransaction.h"
#include "FileChangeJournal.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/CriticalSection.h"
#include <memory>

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
    std::shared_ptr<const SourceAssetDatabaseState> _state;
    FileChangeJournal _journal;
    File* _writerLock = nullptr;
    uint64 _checkpointGeneration = 0;
    uint64 _walBaseRevision = 0;
    uint64 _walLastRevision = 0;
    bool _open = false;
    bool _lastShutdownWasClean = true;
    bool _recoveryRequired = false;

    bool Commit(AssetDatabaseTransaction& transaction, AssetPipelineDiagnostic& diagnostic);

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

    AssetDatabaseReadSnapshot Read() const;
    std::unique_ptr<AssetDatabaseTransaction> BeginTransaction();

    /// <summary>Reads ordered changes. A true requiresSnapshot means the cursor predates retained history.</summary>
    bool ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const;
};
