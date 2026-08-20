// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactOutputValidator.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

/// <summary>Validates model compatibility and independently keyed derived outputs before publication.</summary>
class FLAXENGINE_API ModelArtifactValidator
{
public:
    static bool Register(ArtifactOutputValidatorRegistry& registry, const PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
};

#endif
