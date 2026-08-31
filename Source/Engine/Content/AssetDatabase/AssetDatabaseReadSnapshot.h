// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseSchema.h"
#include <memory>

/// <summary>Immutable, internally consistent database view fixed at one revision.</summary>
class FLAXENGINE_API AssetDatabaseReadSnapshot
{
    friend class SourceAssetDatabase;

private:
    std::shared_ptr<const SourceAssetDatabaseState> _state;

    explicit AssetDatabaseReadSnapshot(const std::shared_ptr<const SourceAssetDatabaseState>& state);

public:
    AssetDatabaseReadSnapshot() = default;

    bool IsValid() const;
    uint64 GetRevision() const;
    Guid GetProjectId() const;
    const SourceAssetDatabaseState& GetState() const;
    bool TryGetSource(const Guid& assetGuid, SourceAssetRow& result) const;
    bool TryGetObject(const Guid& assetGuid, int64 localFileId, SourceAssetObjectRow& result) const;
    bool TryGetPublication(const AssetObjectId& object, const StringView& targetId, SourceAssetPublicationRow& result) const;
    void GetDependencies(const Guid& ownerAssetGuid, const StringView& targetId, Array<SourceAssetDependencyRow>& result) const;
    void GetDependants(const Guid& targetAssetGuid, Array<SourceAssetDependencyRow>& result) const;
    void GetActiveDiagnostics(const Guid& assetGuid, Array<SourceAssetDiagnosticRow>& result) const;
    bool TryGetImportTarget(const StringView& targetId, SourceAssetImportTargetRow& result) const;
    void GetArtifactObjects(const ArtifactKey& artifact, Array<SourceArtifactObjectRow>& result) const;
    void GetLabels(const Guid& assetGuid, Array<String>& result) const;
    bool TryGetRefreshSession(const Guid& refreshId, SourceRefreshSessionRow& result) const;
    bool TryGetImportAttempt(const Guid& attemptId, SourceImportAttemptRow& result) const;
    bool TryGetCustomDependency(const StringView& name, SourceCustomDependencyRow& result) const;
};
