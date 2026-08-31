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
    static bool SynchronizeProcessorDescriptors(AssetPipelineDiagnostic& diagnostic);
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
