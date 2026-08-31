// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetChangeSet.h"
#include "AssetDatabaseSchema.h"

class SourceAssetDatabase;

/// <summary>Private mutable copy committed optimistically against one database revision.</summary>
class FLAXENGINE_API AssetDatabaseTransaction
{
    friend class SourceAssetDatabase;

private:
    SourceAssetDatabase* _owner = nullptr;
    uint64 _baseRevision = 0;
    SourceAssetDatabaseState _state;
    AssetChangeSet _changes;
    bool _completed = false;

    AssetDatabaseTransaction(SourceAssetDatabase* owner, const SourceAssetDatabaseState& state);

public:
    AssetDatabaseTransaction() = default;

    uint64 GetBaseRevision() const;
    const SourceAssetDatabaseState& GetState() const;
    const AssetChangeSet& GetChanges() const;
    bool IsCompleted() const;

    void SetLastCompleteScanId(uint64 scanId);
    void SetImporterRegistryGeneration(uint64 generation);
    void UpsertSource(const SourceAssetRow& source);
    void RemoveSource(const Guid& assetGuid);
    void ReplaceObjects(const Guid& assetGuid, const Array<SourceAssetObjectRow>& objects);
    void ReplaceDependencies(const Guid& assetGuid, const StringView& targetId, const Array<SourceAssetDependencyRow>& dependencies);
    void ReplaceDependencies(const Guid& assetGuid, int64 localFileId, const StringView& targetId,
        const Array<SourceAssetDependencyRow>& dependencies);
    void UpsertPublication(const SourceAssetPublicationRow& publication);
    void ReplaceDiagnostics(const Guid& assetGuid, const Array<SourceAssetDiagnosticRow>& diagnostics);

    /// <summary>Commits all tables and one change set atomically. Returns true on failure.</summary>
    bool Commit(AssetPipelineDiagnostic& diagnostic);
    void Rollback();
};
