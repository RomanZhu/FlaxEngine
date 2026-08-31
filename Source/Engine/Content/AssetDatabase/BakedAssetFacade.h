// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/ContentImporters/Types.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Creates or replaces canonical persistent tool-bake sources and their immutable Library artifacts.</summary>
class FLAXENGINE_API BakedAssetFacade
{
public:
    /// <summary>
    /// Runs a runtime-asset encoder only against Library staging, writes a deterministic authored bake document and
    /// adjacent metadata through the mutation service, then synchronously publishes the exact Library artifact.
    /// </summary>
    /// <returns>True on failure.</returns>
    static bool Create(const CreateAssetFunction& encoder, const StringView& sourcePath, Guid& assetID, void* argument = nullptr);
};

#endif
