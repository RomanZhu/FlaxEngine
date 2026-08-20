// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/StringView.h"

/// <summary>Reads the deterministic cooked-audio manifest used for bank path selection.</summary>
class FmodBankManifest
{
public:
    /// <summary>Resolves only paths explicitly present in AudioCookManifest.json.</summary>
    static bool ResolveBank(const StringView& requestedPath, String& resolvedPath);
};
