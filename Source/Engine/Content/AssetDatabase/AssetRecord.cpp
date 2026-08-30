// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetRecord.h"

AssetRecord AssetRecord::FromLegacy(const AssetInfo& info)
{
    AssetRecord result;
    result.ID = info.ID;
    result.SourceAssetID = info.ID;
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

bool AssetRecord::IsMainAsset() const
{
    return SourceAssetID == ID && SubAsset.IsEmpty();
}

bool AssetRecord::HasSameIdentityAndContent(const AssetRecord& other) const
{
    return ID == other.ID && SourceAssetID == other.SourceAssetID && TypeName == other.TypeName &&
        CanonicalPath == other.CanonicalPath && SourcePath == other.SourcePath && MetaPath == other.MetaPath &&
        SubAsset == other.SubAsset && ProcessorID == other.ProcessorID && PortabilityKey == other.PortabilityKey &&
        MetaSemanticHash == other.MetaSemanticHash && SourceKind == other.SourceKind &&
        BuildInputDependencies == other.BuildInputDependencies && RuntimeReferences == other.RuntimeReferences;
}
