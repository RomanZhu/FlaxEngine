// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

/// <summary>
/// Resolves an optional, user-local FMOD Studio project without putting machine paths in project assets.
/// </summary>
class FLAXENGINE_API FmodStudioLocator
{
public:
    /// <summary>
    /// Gets the configured Studio project path for the current user and project.
    /// </summary>
    static String GetUserProjectPath();

    /// <summary>
    /// Stores a Studio project path in the per-user editor cache.
    /// </summary>
    static bool SetUserProjectPath(const StringView& path);

    /// <summary>
    /// Finds a Studio executable using the configured path and standard installation locations.
    /// </summary>
    static String FindStudioExecutable();
};
