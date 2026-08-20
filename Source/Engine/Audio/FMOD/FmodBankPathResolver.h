// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/StringView.h"

/// <summary>
/// Resolves authoring bank paths in Editor and manifest-relative bank paths in cooked builds.
/// </summary>
class FmodBankPathResolver
{
public:
    static bool Resolve(const StringView& requestedPath, String& result);
};
