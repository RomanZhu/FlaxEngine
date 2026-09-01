// Copyright (c) Wojciech Figat. All rights reserved.

#include "PostProcessStep.h"
#include "Editor/Cooker/PlatformTools.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/Build/CookedContentGeneration.h"
#include "Engine/Platform/FileSystem.h"

bool PostProcessStep::Perform(CookingData& data)
{
    // Print .NET stats
    const DotNetAOTModes aotMode = data.Tools->UseAOT();
    uint64 outputSize = FileSystem::GetDirectorySize(data.DataOutputPath / TEXT("Dotnet"));
    if (aotMode == DotNetAOTModes::None)
    {
        for (auto& binaryModule : data.BinaryModules)
            outputSize += FileSystem::GetFileSize(data.DataOutputPath / binaryModule.ManagedPath);
    }
    LOG(Info, "Output .NET files size: {0} MB", (uint32)(outputSize / (1024ull * 1024)));

    GameCooker::PostProcessFiles();
    if (GameCooker::IsCancelRequested())
        return true;
    if (data.CookedContentRoot.IsEmpty() || data.CookedContentStagingPath.IsEmpty())
    {
        data.Error(TEXT("Cooked content generation was not staged for final publication."));
        return true;
    }

    AssetPipelineDiagnostic diagnostic;
    CookedContentDeploymentState deployment;
    // Platform packaging recursively consumes either OriginalOutputPath or DataOutputPath. The rollback directory is
    // a same-volume sibling outside that package view. Cooking one output path concurrently is not supported.
    String normalizedDataOutput = data.DataOutputPath;
    String normalizedOriginalOutput = data.OriginalOutputPath;
    FileSystem::NormalizePath(normalizedDataOutput);
    FileSystem::NormalizePath(normalizedOriginalOutput);
    const String packageRoot = AssetPathPolicy::IsSameOrChild(normalizedDataOutput, normalizedOriginalOutput)
        ? normalizedOriginalOutput
        : normalizedDataOutput;
    const String rollbackRoot = packageRoot + TEXT(".flax-cook-rollback-") + Guid::New().ToString(Guid::FormatType::N);
    if (CookedContentGeneration::BeginDeployment(data.CookedContentRoot, data.CookedContentStagingPath, rollbackRoot,
        deployment, diagnostic, [] { return GameCooker::IsCancelRequested(); }))
    {
        data.Error(String::Format(TEXT("Failed to prepare cooked content for platform packaging. {0}"), diagnostic.Message));
        return true;
    }
    data.CookedContentStagingPath.Clear();
    LOG(Info, "Published cooked content generation {0}", String(deployment.NewGeneration.ToString()));

    const bool platformFailed = GameCooker::IsCancelRequested() || data.Tools->OnPostProcess(data);
    if (platformFailed || GameCooker::IsCancelRequested())
    {
        if (CookedContentGeneration::RollbackDeployment(data.CookedContentRoot, deployment, diagnostic))
            data.Error(String::Format(TEXT("Platform post-processing failed and the previous cooked-content activation could not be restored. {0}"), diagnostic.Message));
        return true;
    }
    if (CookedContentGeneration::CommitDeployment(deployment, diagnostic))
        LOG(Warning, "Platform packaging succeeded but cooked-content rollback data could not be removed. {0}", diagnostic.Message);
    return false;
}

void PostProcessStep::OnBuildEnded(CookingData& data, bool /*failed*/)
{
    if (data.CookedContentStagingPath.HasChars() && FileSystem::DirectoryExists(data.CookedContentStagingPath))
        FileSystem::DeleteDirectory(data.CookedContentStagingPath, true);
    data.CookedContentStagingPath.Clear();
    data.CookedContentRoot.Clear();
}
