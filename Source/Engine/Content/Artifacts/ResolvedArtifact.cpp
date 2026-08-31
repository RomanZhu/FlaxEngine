// Copyright (c) Wojciech Figat. All rights reserved.

#include "ResolvedArtifact.h"

ResolvedArtifact ResolvedArtifact::Package(const AssetInfo& info)
{
    ResolvedArtifact result;
    result.AssetID = info.ID;
    result.TypeName = info.TypeName;
    result.StoragePath = ArtifactStoragePath(info.Path);
    result.OutputKind = TEXT("package");
    result.StorageKind = ArtifactStorageKind::Package;
    result.IsExact = true;
    return result;
}

bool ResolvedArtifact::IsGenerated() const
{
    return StorageKind == ArtifactStorageKind::Generated;
}

AssetLoadLocation AssetLoadLocation::Package(const AssetInfo& info)
{
    AssetLoadLocation result;
    result.Info = info;
    result.Artifact = ResolvedArtifact::Package(info);
    return result;
}
