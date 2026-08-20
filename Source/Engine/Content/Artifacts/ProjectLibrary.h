// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>
/// Project Library root validation and bootstrap helpers.
/// </summary>
class FLAXENGINE_API ProjectLibrary
{
public:
    /// <summary>
    /// Normalizes and validates the project, Content, and Library roots.
    /// </summary>
    /// <returns>True on failure.</returns>
    static bool ValidateRoot(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, String& normalizedRoot, AssetPipelineDiagnostic& diagnostic);

    /// <summary>
    /// Validates and creates the Library root if it does not exist.
    /// </summary>
    /// <returns>True on failure.</returns>
    static bool EnsureRoot(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, String& normalizedRoot, AssetPipelineDiagnostic& diagnostic);
};
