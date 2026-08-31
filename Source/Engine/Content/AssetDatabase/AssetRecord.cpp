// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetRecord.h"
#include "Engine/Platform/StringUtils.h"

AssetRecord AssetRecord::FromLegacy(const AssetInfo& info)
{
    AssetRecord result;
    result.ID = info.ID;
    result.SourceAssetID = info.ID;
    result.LocalId = 1;
    result.TypeName = info.TypeName;
    result.CanonicalPath = CanonicalAssetPath(info.Path);
    result.SourcePath = SourceFilePath(info.Path);
    result.DisplayName = String(StringUtils::GetFileNameWithoutExtension(info.Path));
    result.SourceKind = AssetSourceKind::LegacyBinary;
    result.Status = AssetRecordStatus::Ready;
    return result;
}

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
