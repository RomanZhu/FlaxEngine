// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Platform/CriticalSection.h"

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

/// <summary>Thread-safe canonical source/metadata registry.</summary>
class FLAXENGINE_API AssetDatabase
{
private:
    mutable CriticalSection _locker;
    uint64 _revision = 0;
    Dictionary<Guid, AssetRecord> _records;
    Dictionary<String, Guid> _mainByPath;
    Dictionary<Guid, Array<Guid>> _subAssetsBySource;
    Dictionary<String, Array<Guid>> _recordsByProcessor;
    Dictionary<AssetRecordStatus, Array<Guid>> _recordsByStatus;
    Dictionary<Guid, Array<Guid>> _dependantsByBuildInput;
    Dictionary<Guid, Array<Guid>> _referencersByRuntimeReference;

public:
    Delegate<const AssetDatabaseChangeBatch&> Changed;

    static AssetDatabase& Get();

    uint64 GetRevision() const;
    AssetDatabaseSnapshot GetSnapshot() const;
    bool TryGetRecord(const Guid& id, AssetRecord& result) const;
    bool TryGetMainRecordByPath(const StringView& portabilityKey, AssetRecord& result) const;
    void GetSubAssets(const Guid& sourceId, Array<AssetRecord>& result) const;
    void GetByProcessor(const StringView& processorId, Array<AssetRecord>& result) const;
    void GetByStatus(AssetRecordStatus status, Array<AssetRecord>& result) const;
    void GetBuildDependants(const Guid& inputId, Array<AssetRecord>& result) const;
    void GetRuntimeReferencers(const Guid& referencedId, Array<AssetRecord>& result) const;

    /// <summary>Atomically replaces database truth and emits one change batch outside the database lock.</summary>
    /// <returns>True if input records violate an invariant.</returns>
    bool PublishFullSnapshot(const Array<AssetRecord>& records, AssetPipelineDiagnostic& diagnostic);

    void Clear();
};
