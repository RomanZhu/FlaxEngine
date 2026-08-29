// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactResolver.h"

#if COMPILE_WITH_TEXTURE_TOOL && COMPILE_WITH_ASSETS_IMPORTER

/// <summary>Production coordinator for canonical texture preparation, build, publication, and resolution.</summary>
class FLAXENGINE_API TexturePipelineService
{
public:
    /// <summary>Queues an exact host-editor texture build. Returns true on failure.</summary>
    static bool RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Queues only the lightweight exact thumbnail output. Returns true on failure.</summary>
    static bool RequestThumbnailBuild(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns the latest queued build state for an asset.</summary>
    static AssetBuildJobStatus GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Returns the latest queued thumbnail-only build state for an asset.</summary>
    static AssetBuildJobStatus GetThumbnailStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Creates the exact build plan used by the resolver and cooker.</summary>
    static bool CreatePlan(const AssetRecord& record, const ArtifactRequest& request, ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic);

    static const ArtifactTarget& GetHostTarget();
    static AssetBuildService* GetBuildService(AssetPipelineDiagnostic& diagnostic);

    /// <summary>Stops texture workers and releases the processor registration before engine teardown.</summary>
    static void Shutdown();
};

#endif
