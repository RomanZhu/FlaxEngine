// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"

/// <summary>Compatibility tags are explicit, non-empty, output-format contracts.</summary>
class FLAXENGINE_API ArtifactCompatibility
{
public:
    static bool IsCompatible(const StringAnsiView& required, const StringAnsiView& candidate)
    {
        return required.HasChars() && candidate == required;
    }
};
