// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImportPlanner.h"
#include "AssetImportWorkerProtocol.h"
#include "Engine/Content/Build/AssetBuildService.h"

using AssetImportJobAction = Function<bool(const AssetImportPlan&, const AssetCancellationToken&, AssetPipelineDiagnostic&)>;
using AssetImportWorkerPublishAction = Function<bool(const AssetImportPlan&, const AssetImportJobResult&,
                                                       const AssetCancellationToken&, AssetPipelineDiagnostic&)>;

/// <summary>Maps importer plans onto the shared bounded artifact scheduler.</summary>
class FLAXENGINE_API AssetImportScheduler
{
    AssetBuildService& _builds;

public:
    explicit AssetImportScheduler(AssetBuildService& builds)
        : _builds(builds)
    {
    }

    AssetBuildRequestHandle Schedule(const AssetImportPlan& plan, AssetImportJobAction action,
                                     AssetBuildJobPriority priority = AssetBuildJobPriority::Normal);
    AssetBuildRequestHandle ScheduleIsolated(const AssetImportPlan& plan, const StringView& workerExecutable,
                                             AssetImportJobRequest request, AssetImportWorkerPublishAction publish,
                                             AssetPipelineDiagnostic& diagnostic,
                                             AssetBuildJobPriority priority = AssetBuildJobPriority::Normal);
    void Cancel(const AssetBuildRequestHandle& handle);
};
