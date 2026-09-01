// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetBuildService.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Parent-owned prepare, isolated execution, and publication service for callback importers.</summary>
class FLAXENGINE_API CallbackImporterPipelineService
{
public:
    static bool OwnsProcessor(const StringView& processorID);
    static bool RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic,
                             AssetBuildRequestHandle* resultHandle = nullptr,
                             const Guid& refreshId = Guid::Empty, uint32 pass = 0,
                             AssetBuildJobPriority priority = AssetBuildJobPriority::Normal);
    static bool RequestBuildAndWait(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic);
    static AssetBuildJobStatus GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);
    static void Shutdown();
};

#endif
