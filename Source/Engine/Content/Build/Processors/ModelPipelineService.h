// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"

#if COMPILE_WITH_MODEL_TOOL && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Production coordinator for canonical imported models and their GUID-addressed child outputs.</summary>
class FLAXENGINE_API ModelPipelineService
{
public:
    /// <summary>Registers the built-in model processor with the asset import service.</summary>
    static bool EnsureInitialized(AssetPipelineDiagnostic& diagnostic);

    static bool RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic);
    static AssetBuildJobStatus GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);
    static bool CreatePlan(const AssetRecord& record, const ArtifactRequest& request, ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Explicitly reconciles stable candidates and atomically updates the tracked root sidecar.</summary>
    static bool ReconcileMetadata(const Guid& rootAssetID, Array<SubAssetReconcileChange>& changes, AssetPipelineDiagnostic& diagnostic);

    static void Shutdown();
};

#endif
