// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

/// <summary>Resolves canonical source roots shared by project and engine-authored assets.</summary>
class FLAXENGINE_API AssetSourceRoots
{
public:
    /// <summary>Gets the engine-authored asset source root.</summary>
    static String GetEngineRoot();

    /// <summary>Resolves the owning project and content roots for a canonical source path.</summary>
    static void Resolve(const StringView& sourcePath, String& projectRoot, String& contentRoot);
};
