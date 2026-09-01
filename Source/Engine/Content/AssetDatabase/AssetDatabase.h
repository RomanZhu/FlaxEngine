// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "SourceAssetDatabase.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Platform/CriticalSection.h"

struct SourceHashFileState;

/// <summary>A coherent set of database changes published at one revision.</summary>
struct FLAXENGINE_API AssetDatabaseChangeBatch
{
    uint64 Revision = 0;
    Array<Guid> Added;
    Array<Guid> Removed;
    Array<Guid> Changed;
    Array<Guid> StatusChanged;
};

/// <summary>Immutable copy of all records at one database revision.</summary>
struct FLAXENGINE_API AssetDatabaseSnapshot
{
    uint64 Revision = 0;
    Array<AssetRecord> Records;
};

/// <summary>Native indexed asset record query.</summary>
struct FLAXENGINE_API AssetRecordQuery
{
    String Name;
    String PathPrefix;
    String TypeName;
    String ProcessorId;
    String Label;
    AssetRecordStatus Status = AssetRecordStatus::Ready;
    bool HasStatus = false;
    bool MainAssetsOnly = false;
    Guid ReferencedAsset = Guid::Empty;
    Guid UsedByAsset = Guid::Empty;
};

/// <summary>Thread-safe canonical source/metadata registry.</summary>
class FLAXENGINE_API AssetDatabase
{
private:
    mutable CriticalSection _locker;
    mutable CriticalSection _writeLocker;
    uint64 _revision = 0;
    Dictionary<Guid, AssetRecord> _records;
    Dictionary<AssetObjectId, Guid> _recordByObject;
    Dictionary<String, Guid> _mainByPath;
    Dictionary<Guid, Array<Guid>> _subAssetsBySource;
    Dictionary<String, Array<Guid>> _recordsByProcessor;
    Dictionary<String, Array<Guid>> _recordsByType;
    Dictionary<String, Array<Guid>> _recordsByLabel;
    Array<Guid> _recordsBySortedPath;
    Dictionary<String, Array<Guid>> _recordsBySearchGram;
    Dictionary<AssetRecordStatus, Array<Guid>> _recordsByStatus;
    Dictionary<AssetObjectId, Array<Guid>> _dependantsByBuildInput;
    Dictionary<AssetObjectId, Array<Guid>> _referencersByRuntimeReference;
    SourceAssetDatabase _sourceDatabase;

    bool PublishCache(const Array<AssetRecord>& records, uint64 revision, AssetDatabaseChangeBatch& changes, AssetPipelineDiagnostic& diagnostic);
    bool ReconcileScanRowsInternal(const Array<AssetRecord>& records,
        const Array<AssetPipelineDiagnostic>& diagnostics, const Array<SourceHashFileState>* fileStates,
        const Guid& refreshId, uint32 pass, AssetPipelineDiagnostic& diagnostic);
    void RebuildCacheFromDurable(AssetDatabaseChangeBatch* changes = nullptr);

public:
    Delegate<const AssetDatabaseChangeBatch&> Changed;

    static AssetDatabase& Get();

    /// <summary>Opens the authoritative database under the Project Library and imports its current snapshot.</summary>
    bool Open(const StringView& libraryPath, const Guid& projectId, AssetPipelineDiagnostic& diagnostic);
    bool Close(AssetPipelineDiagnostic* diagnostic = nullptr);
    bool IsOpen() const;
    void SetCheckpointPolicy(const SourceAssetDatabaseCheckpointPolicy& policy);
    SourceAssetDatabaseCheckpointPolicy GetCheckpointPolicy() const;
    bool Checkpoint(AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns true when this database owns the given Library root.</summary>
    bool IsUsingLibrary(const StringView& libraryPath) const;

    uint64 GetRevision() const;
    AssetDatabaseSnapshot GetSnapshot() const;
    bool TryGetRecord(const Guid& id, AssetRecord& result) const;
    bool TryGetRecord(const AssetObjectId& id, AssetRecord& result) const;
    bool TryGetMainRecordByPath(const StringView& portabilityKey, AssetRecord& result) const;
    void GetSubAssets(const Guid& sourceId, Array<AssetRecord>& result) const;
    void GetByProcessor(const StringView& processorId, Array<AssetRecord>& result) const;
    void GetByStatus(AssetRecordStatus status, Array<AssetRecord>& result) const;
    void GetBuildDependants(const Guid& inputId, Array<AssetRecord>& result) const;
    void GetBuildDependants(const AssetObjectId& input, Array<AssetRecord>& result) const;
    void GetRuntimeReferencers(const Guid& referencedId, Array<AssetRecord>& result) const;
    void GetRuntimeReferencers(const AssetObjectId& referenced, Array<AssetRecord>& result) const;
    void QueryRecords(const AssetRecordQuery& query, Array<AssetRecord>& result) const;
    void GetLabels(const Guid& sourceId, Array<String>& result) const;
    bool SetLabels(const Guid& sourceId, const Array<String>& labels, AssetPipelineDiagnostic& diagnostic);
    bool RegisterCustomDependency(const StringView& name, const ContentHash& hash, const StringView& provider,
        AssetPipelineDiagnostic& diagnostic);
    bool UnregisterCustomDependency(const StringView& name, AssetPipelineDiagnostic& diagnostic);
    bool TryGetCustomDependencyHash(const StringView& name, ContentHash& result) const;

    /// <summary>Atomically replaces database truth and emits one change batch outside the database lock.</summary>
    /// <returns>True if input records violate an invariant.</returns>
    bool PublishFullSnapshot(const Array<AssetRecord>& records, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Atomically publishes records and their current diagnostics into durable authority.</summary>
    bool PublishFullSnapshot(const Array<AssetRecord>& records, const Array<AssetPipelineDiagnostic>& diagnostics, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Atomically publishes records and their current source file hashes into durable authority.</summary>
    bool PublishFullSnapshot(const Array<AssetRecord>& records, const Array<SourceHashFileState>& fileStates,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Atomically publishes records, diagnostics, and current source file hashes into durable authority.</summary>
    bool PublishFullSnapshot(const Array<AssetRecord>& records, const Array<AssetPipelineDiagnostic>& diagnostics,
        const Array<SourceHashFileState>& fileStates, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Reconciles scanner output through typed source/object/dependency/diagnostic row mutations.</summary>
    bool ReconcileScanRows(const Array<AssetRecord>& records, const Array<AssetPipelineDiagnostic>& diagnostics,
        const Array<SourceHashFileState>& fileStates, AssetPipelineDiagnostic& diagnostic,
        const Guid& refreshId = Guid::Empty, uint32 pass = 0);

    AssetDatabaseReadSnapshot GetDurableSnapshot() const;
    bool ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const;
    bool RecordPublication(const SourceAssetPublicationRow& publication, const Array<SourceAssetDependencyRow>& dependencies,
        AssetPipelineDiagnostic& diagnostic, const Guid& refreshId = Guid::Empty, uint32 pass = 0);
    bool RecordRefreshSession(const SourceRefreshSessionRow& session, uint32 pass, AssetPipelineDiagnostic& diagnostic);

    void Clear();
};
