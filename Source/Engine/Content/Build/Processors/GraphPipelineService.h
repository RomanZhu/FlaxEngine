// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactResolver.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Production coordinator for canonical graph and authored documents.</summary>
class FLAXENGINE_API GraphPipelineService
{
public:
    static bool OwnsProcessor(const StringView& processorID);
    static bool RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic,
                             AssetBuildRequestHandle* resultHandle = nullptr,
                             const Guid& refreshId = Guid::Empty, uint32 pass = 0);
    static bool RequestBuildAndWait(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic);
    static AssetBuildJobStatus GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);
    static bool CreatePlan(const AssetRecord& record, const ArtifactRequest& request, ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic);
    static bool EnsureInitialized(AssetPipelineDiagnostic& diagnostic);
    static void Shutdown();
};

#endif
