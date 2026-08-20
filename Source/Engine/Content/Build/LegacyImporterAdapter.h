// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactBuildContext.h"
#include "Engine/Content/Config.h"

#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/ContentImporters/Types.h"

/// <summary>Transitional bridge that runs a trusted legacy importer into artifact staging.</summary>
class FLAXENGINE_API LegacyImporterAdapter
{
public:
    /// <summary>Invokes a legacy callback without Content replacement, registration, reload, import metadata, or mtime checks.</summary>
    static bool Build(const CreateAssetFunction& callback, ArtifactBuildContext& buildContext,
        const StringView& inputIdentity, const StringAnsiView& outputKind, const StringView& outputFileName,
        const Guid& intendedAssetId, const StringView& intendedTypeName, void* customArgument,
        AssetPipelineDiagnostic& diagnostic);
};
#endif
