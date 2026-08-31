// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetRecord.h"

AssetRecord AssetRecord::FromLegacy(const AssetInfo& info)
{
    AssetRecord result;
    result.ID = info.ID;
    result.SourceAssetID = info.ID;
    result.LocalId = 1;
    result.TypeName = info.TypeName;
    result.CanonicalPath = CanonicalAssetPath(info.Path);
    result.SourcePath = SourceFilePath(info.Path);
    result.SourceKind = AssetSourceKind::LegacyBinary;
    result.Status = AssetRecordStatus::Ready;
    return result;
}

AssetInfo AssetRecord::ToAssetInfo() const
{
    return AssetInfo(ID, TypeName, CanonicalPath.Get());
}

AssetObjectId AssetRecord::GetObjectId() const
{
    return AssetObjectId(SourceAssetID, LocalId);
}

bool AssetRecord::IsMainAsset() const
{
    return LocalId == 1 && SubAsset.IsEmpty();
}

bool AssetRecord::HasSameIdentityAndContent(const AssetRecord& other) const
{
    return ID == other.ID && SourceAssetID == other.SourceAssetID && LocalId == other.LocalId && TypeName == other.TypeName &&
        CanonicalPath == other.CanonicalPath && SourcePath == other.SourcePath && MetaPath == other.MetaPath &&
        SubAsset == other.SubAsset && ProcessorID == other.ProcessorID && PortabilityKey == other.PortabilityKey &&
        MetaSemanticHash == other.MetaSemanticHash && Labels == other.Labels && SourceKind == other.SourceKind &&
        BuildInputDependencies == other.BuildInputDependencies && RuntimeReferences == other.RuntimeReferences;
}
