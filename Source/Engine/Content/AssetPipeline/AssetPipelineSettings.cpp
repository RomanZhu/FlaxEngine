// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetPipelineSettings.h"
#include "Engine/Core/Log.h"

namespace
{
    bool Invalid(AssetPipelineDiagnostic& diagnostic, const Char* message, const Char* remediation)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Severity = AssetPipelineDiagnosticSeverity::Error;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.Message = message;
        diagnostic.Remediation = remediation;
        return false;
    }
}

bool AssetPipelineSettings::IsValid(AssetPipelineDiagnostic& diagnostic) const
{
    diagnostic = AssetPipelineDiagnostic();
    if (DiskQuotaGigabytes < 1 || MinimumFreeSpaceGigabytes < 0 || GarbageCollectionGracePeriodHours < 0 ||
        RetainedLastGoodCount < 0 || LogRetentionDays < 1 || WorkerLimit < 0 || MemoryLimitMegabytes < 128)
        return Invalid(diagnostic, TEXT("One or more asset pipeline resource limits are outside their supported range."), TEXT("Restore positive Library limits and a memory limit of at least 128 MB."));
    return true;
}

void AssetPipelineSettings::Apply()
{
    AssetPipelineDiagnostic diagnostic;
    if (!IsValid(diagnostic))
        LOG(Error, "[{0}] {1} {2}", GetAssetPipelineDiagnosticCodeName(diagnostic.Code), diagnostic.Message, diagnostic.Remediation);
}
