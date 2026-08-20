// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

class Scene;
class CSGModel;

namespace CSG
{
    struct CSGGeneratedOutputPaths
    {
        String Model;
        String RawData;
        String Collision;
    };

    class FLAXENGINE_API CSGGeneratedOutputPathResolver
    {
    public:
        static bool ResolveForScene(CSGModel* model, CSGGeneratedOutputPaths& output);
        static bool ResolveForAsset(const Guid& ownerAssetId, CSGModel* model, CSGGeneratedOutputPaths& output);
    };
}
