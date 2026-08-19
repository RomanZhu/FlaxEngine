// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"

/// <summary>
/// Editor utility that scans FMOD Studio bank metadata and automatically generates/synchronizes engine audio event assets.
/// </summary>
class FLAXENGINE_API FmodCatalogBuilder
{
public:
    /// <summary>
    /// Synchronizes audio metadata assets from built banks in the project content directory.
    /// </summary>
    /// <param name="banksDirectory">The directory containing built FMOD banks.</param>
    /// <param name="outputDirectory">The target content directory for generated audio assets.</param>
    /// <returns>True if catalog sync succeeded, false otherwise.</returns>
    static bool BuildCatalog(const String& banksDirectory, const String& outputDirectory);
};
