// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetSourceRootRegistry.h"

/// <summary>Internal compatibility facade for callers not yet passed an explicit root registry.</summary>
class AssetSourceRoots
{
public:
    /// <summary>Gets the engine-authored asset source root.</summary>
    static String GetEngineRoot();

    /// <summary>Resolves the owning project and content roots for a canonical source path.</summary>
    static void Resolve(const StringView& sourcePath, String& projectRoot, String& contentRoot);

    /// <summary>Builds the policy registry for one legacy scanner root.</summary>
    static AssetSourceRootRegistry CreateScannerRegistry(const StringView& projectRoot, const StringView& sourceRoot,
        const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic);
};
