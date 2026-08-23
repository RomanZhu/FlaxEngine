// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactOutputValidator.h"

#if COMPILE_WITH_TEXTURE_TOOL

/// <summary>Validates staged texture processor outputs before immutable publication.</summary>
class FLAXENGINE_API TextureArtifactValidator
{
public:
    static bool Register(ArtifactOutputValidatorRegistry& registry, const Guid& expectedAssetID, const StringView& expectedType, AssetPipelineDiagnostic& diagnostic);
    static bool ValidateRuntime(const StringView& path, const ArtifactManifestOutput& output, const Guid& expectedAssetID, const StringView& expectedType, AssetPipelineDiagnostic& diagnostic);
    static bool ValidateThumbnail(const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& diagnostic);
};

#endif
