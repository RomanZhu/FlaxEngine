// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseReadSnapshot.h"

AssetDatabaseReadSnapshot::AssetDatabaseReadSnapshot(const std::shared_ptr<const SourceAssetDatabaseState>& state)
    : _state(state)
{
}

bool AssetDatabaseReadSnapshot::IsValid() const
{
    return _state != nullptr;
}

uint64 AssetDatabaseReadSnapshot::GetRevision() const
{
    return _state ? _state->Database.CurrentRevision : 0;
}

Guid AssetDatabaseReadSnapshot::GetProjectId() const
{
    return _state ? _state->Database.ProjectId : Guid::Empty;
}

const SourceAssetDatabaseState& AssetDatabaseReadSnapshot::GetState() const
{
    ASSERT(_state);
    return *_state;
}

bool AssetDatabaseReadSnapshot::TryGetSource(const Guid& assetGuid, SourceAssetRow& result) const
{
    if (_state)
    {
        for (const SourceAssetRow& source : _state->Sources)
        {
            if (source.AssetGuid == assetGuid)
            {
                result = source;
                return true;
            }
        }
    }
    return false;
}

bool AssetDatabaseReadSnapshot::TryGetObject(const Guid& assetGuid, int64 localFileId, SourceAssetObjectRow& result) const
{
    if (_state)
    {
        for (const SourceAssetObjectRow& object : _state->Objects)
        {
            if (object.AssetGuid == assetGuid && object.LocalFileId == localFileId)
            {
                result = object;
                return true;
            }
        }
    }
    return false;
}

bool AssetDatabaseReadSnapshot::TryGetPublication(const AssetObjectId& object, const StringView& targetId, SourceAssetPublicationRow& result) const
{
    if (_state)
    {
        for (const SourceAssetPublicationRow& publication : _state->Publications)
        {
            if (publication.AssetGuid == object.Asset.Value && publication.LocalFileId == object.LocalId && publication.TargetId == targetId)
            {
                result = publication;
                return true;
            }
        }
    }
    return false;
}

void AssetDatabaseReadSnapshot::GetDependencies(const Guid& ownerAssetGuid, const StringView& targetId, Array<SourceAssetDependencyRow>& result) const
{
    result.Clear();
    if (_state)
    {
        for (const SourceAssetDependencyRow& dependency : _state->Dependencies)
        {
            if (dependency.OwnerAssetGuid == ownerAssetGuid && (targetId.IsEmpty() || dependency.TargetId == targetId))
                result.Add(dependency);
        }
    }
}

void AssetDatabaseReadSnapshot::GetDependants(const Guid& targetAssetGuid, Array<SourceAssetDependencyRow>& result) const
{
    result.Clear();
    if (_state)
    {
        for (const SourceAssetDependencyRow& dependency : _state->Dependencies)
        {
            if (dependency.TargetAssetGuid == targetAssetGuid)
                result.Add(dependency);
        }
    }
}

void AssetDatabaseReadSnapshot::GetActiveDiagnostics(const Guid& assetGuid, Array<SourceAssetDiagnosticRow>& result) const
{
    result.Clear();
    if (_state)
    {
        for (const SourceAssetDiagnosticRow& diagnostic : _state->Diagnostics)
        {
            if (diagnostic.IsActive && diagnostic.AssetGuid == assetGuid)
                result.Add(diagnostic);
        }
    }
}
