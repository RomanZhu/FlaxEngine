// Copyright (c) Wojciech Figat. All rights reserved.

#include "ResolvedArtifact.h"

ResolvedArtifact ResolvedArtifact::Legacy(const AssetInfo& info)
{
    ResolvedArtifact result;
    result.AssetID = info.ID;
    result.TypeName = info.TypeName;
    result.StoragePath = ArtifactStoragePath(info.Path);
    result.OutputKind = TEXT("legacy");
    result.StorageKind = ArtifactStorageKind::Legacy;
    result.IsExact = true;
    return result;
}

bool ResolvedArtifact::IsGenerated() const
{
    return StorageKind == ArtifactStorageKind::Generated;
}

AssetLoadLocation AssetLoadLocation::Legacy(const AssetInfo& info)
{
    AssetLoadLocation result;
    result.Info = info;
    result.Artifact = ResolvedArtifact::Legacy(info);
    return result;
}
