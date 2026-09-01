// Copyright (c) Wojciech Figat. All rights reserved.

#include "CubeTexture.h"
#include "Engine/Content/Factories/BinaryAssetFactory.h"

REGISTER_BINARY_ASSET(CubeTexture, "FlaxEngine.CubeTexture", true);

CubeTexture::CubeTexture(const SpawnParams& params, const AssetInfo* info)
    : TextureBase(params, info)
{
}
