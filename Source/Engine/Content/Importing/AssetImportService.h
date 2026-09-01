// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImportScheduler.h"
#include "AssetModificationProcessor.h"
#include "AssetRefreshCoordinator.h"
#include "CustomDependencyRegistry.h"

/// <summary>Lifecycle owner and public access seam for the editor import framework.</summary>
class FLAXENGINE_API AssetImportService
{
public:
    static bool EnsureInitialized(AssetPipelineDiagnostic& diagnostic);
    static bool AttachBuildService(AssetBuildService& builds, AssetPipelineDiagnostic& diagnostic);
    /// <summary>Registers a built-in importer and its private build-stage implementation explicitly.</summary>
    static bool RegisterBuiltIn(const AssetProcessorDescriptor& implementation, AssetPipelineDiagnostic& diagnostic,
        AssetImporterPriorityBuildRequest requestBuild, AssetImporterBuildStatus getBuildStatus, int32 priority = 0);
    static bool IsInitialized();

    static AssetImporterRegistry* GetImporterRegistry();
    static AssetImportPlanner* GetPlanner();
    static AssetImportScheduler* GetScheduler();
    static AssetPostprocessorRegistry* GetPostprocessorRegistry();
    static AssetModificationProcessorRegistry* GetModificationProcessorRegistry();
    static CustomDependencyRegistry* GetCustomDependencyRegistry();
    static AssetRefreshCoordinator* GetRefreshCoordinator();

    static void Shutdown();
};
