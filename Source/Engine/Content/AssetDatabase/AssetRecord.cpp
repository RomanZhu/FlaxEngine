// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetRecord.h"

AssetInfo AssetRecord::ToAssetInfo() const
{
    return AssetInfo(ID, AssetObjectId(AssetGuid(SourceAssetID), LocalId), TypeName, CanonicalPath.Get(), DatabaseRevision);
}

bool AssetRecord::IsMainAsset() const
{
    return SourceAssetID == ID && SubAsset.IsEmpty();
}

bool AssetRecord::HasSameIdentityAndContent(const AssetRecord& other) const
{
    return ID == other.ID && SourceAssetID == other.SourceAssetID && LocalId == other.LocalId && TypeName == other.TypeName &&
        CanonicalPath == other.CanonicalPath && SourcePath == other.SourcePath && MetaPath == other.MetaPath &&
        SubAsset == other.SubAsset && DisplayName == other.DisplayName && ProcessorID == other.ProcessorID && PortabilityKey == other.PortabilityKey &&
        MetaSemanticHash == other.MetaSemanticHash && SourceKind == other.SourceKind &&
        Labels == other.Labels && BuildInputDependencies == other.BuildInputDependencies && RuntimeReferences == other.RuntimeReferences;
}
