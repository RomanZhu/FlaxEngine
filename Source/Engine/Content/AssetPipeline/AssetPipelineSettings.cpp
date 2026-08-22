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
    if (UseLibraryArtifacts && !UseNewAssetDatabase)
        return Invalid(diagnostic, TEXT("Library artifacts require the new asset database."), TEXT("Enable UseNewAssetDatabase or disable UseLibraryArtifacts."));
    if (UseTextGraphAssets && (!UseNewAssetDatabase || !UseLibraryArtifacts))
        return Invalid(diagnostic, TEXT("Text graph assets require the new asset database and Library artifacts."), TEXT("Enable UseNewAssetDatabase and UseLibraryArtifacts, or disable UseTextGraphAssets."));
    if (StrictAssetMetadata && !UseNewAssetDatabase)
        return Invalid(diagnostic, TEXT("Strict asset metadata requires the new asset database."), TEXT("Enable UseNewAssetDatabase or disable StrictAssetMetadata."));
    if (AutoCreateMetaInEditor && !UseNewAssetDatabase)
        return Invalid(diagnostic, TEXT("Automatic sidecar creation requires the new asset database."), TEXT("Enable UseNewAssetDatabase or disable AutoCreateMetaInEditor."));
    if (AllowLastGoodArtifacts && !UseLibraryArtifacts)
        return Invalid(diagnostic, TEXT("Last-good artifacts require Library artifacts."), TEXT("Enable UseLibraryArtifacts or disable AllowLastGoodArtifacts."));
    if (LockConvertedTypeAuthoring && !UseTextGraphAssets)
        return Invalid(diagnostic, TEXT("Converted-type lockout requires text graph assets."), TEXT("Enable UseTextGraphAssets or disable LockConvertedTypeAuthoring."));
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
