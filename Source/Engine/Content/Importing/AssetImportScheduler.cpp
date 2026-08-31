// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportScheduler.h"

AssetBuildRequestHandle AssetImportScheduler::Schedule(const AssetImportPlan& plan, AssetImportJobAction action)
{
    AssetBuildJobRequest request;
    request.Key.ExactPlan = plan.StaticFingerprint;
    request.AssetID = plan.Request.Asset.Value;
    request.ProcessorClass = TEXT("asset-import");
    request.ProcessorID = plan.Importer.ID;
    request.Target = String(plan.Request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    request.KeyComponents = plan.KeyComponents;
    request.RebuildReason = plan.Request.Reason;
    request.MemoryBytes = plan.Importer.Processor.MemoryEstimate;
    request.ExternalToolSlots = plan.Importer.Processor.UsesExternalProcess ? 1 : 0;
    request.ProcessorConcurrencyLimit = plan.Importer.SupportsParallelImport ? MAX_int32 : 1;
    request.AllowTerminalDeduplication = !plan.Request.Force;
    for (const AssetProcessorOutputDescriptor& output : plan.Importer.Processor.Outputs)
        request.OutputKinds.Add(output.Kind);
    request.Build = [plan, action = MoveTemp(action)](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& diagnostic) mutable
    {
        if (!action.IsBinded())
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.AssetGuid = plan.Request.Asset.Value;
            diagnostic.SourcePath = plan.Request.SourcePath;
            diagnostic.ProcessorId = plan.Importer.ID;
            diagnostic.Message = TEXT("Import plan has no execution callback.");
            return true;
        }
        return action(plan, cancellation, diagnostic);
    };
    return _builds.Request(request);
}

void AssetImportScheduler::Cancel(const AssetBuildRequestHandle& handle)
{
    _builds.CancelRequester(handle);
}
