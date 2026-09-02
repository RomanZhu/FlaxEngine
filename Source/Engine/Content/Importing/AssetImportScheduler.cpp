// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportScheduler.h"
#include "Engine/Platform/FileSystem.h"
#include <memory>

namespace
{
    constexpr int32 TextureImporterConcurrency = 2;

    void ConfigureImporterBounds(const AssetImportPlan& plan, AssetBuildJobRequest& request, bool isolated)
    {
        request.ProcessorClass = String(TEXT("asset-import/")) + plan.Importer.ID;
        request.MemoryBytes = Math::Max<uint64>(1, isolated
            ? plan.Importer.MaximumMemoryBytes
            : plan.Importer.Processor.MemoryEstimate);
        request.ExternalToolSlots = isolated || plan.Importer.Processor.UsesExternalProcess ? 1 : 0;
        request.ProcessorConcurrencyLimit = plan.Importer.SupportsParallelImport ? MAX_int32 : 1;
        if (plan.Importer.ID == TEXT("Flax.Texture"))
            request.ProcessorConcurrencyLimit = Math::Min(request.ProcessorConcurrencyLimit, TextureImporterConcurrency);
        else if (plan.Importer.ID == TEXT("Flax.Model"))
            request.SerialGroup = plan.Request.Asset.Value.ToString(Guid::FormatType::N);
    }
}

AssetBuildRequestHandle AssetImportScheduler::Schedule(const AssetImportPlan& plan, AssetImportJobAction action,
                                                       AssetBuildJobPriority priority)
{
    // A process-safe registration is an isolation requirement, not an optimization hint.
    if (plan.Importer.ProcessSafe)
        return AssetBuildRequestHandle();
    AssetBuildJobRequest request;
    request.Key.ExactPlan = plan.StaticFingerprint;
    request.AssetID = plan.Request.Asset.Value;
    request.RefreshId = plan.Request.RefreshId;
    request.Pass = plan.Request.Pass;
    request.ProcessorID = plan.Importer.ID;
    request.Target = String(plan.Request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    request.KeyComponents = plan.KeyComponents;
    request.RebuildReason = plan.Request.Reason;
    ConfigureImporterBounds(plan, request, false);
    request.Priority = priority;
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

AssetBuildRequestHandle AssetImportScheduler::ScheduleIsolated(const AssetImportPlan& plan, const StringView& workerExecutable,
                                                               AssetImportJobRequest workerRequest, AssetImportWorkerPublishAction publish,
                                                               AssetPipelineDiagnostic& diagnostic,
                                                               AssetBuildJobPriority priority)
{
    if (!plan.Importer.ProcessSafe || !publish.IsBinded())
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
        diagnostic.AssetGuid = plan.Request.Asset.Value;
        diagnostic.SourcePath = plan.Request.SourcePath;
        diagnostic.ProcessorId = plan.Importer.ID;
        diagnostic.Message = TEXT("Importer cannot be scheduled through the isolated worker path.");
        return AssetBuildRequestHandle();
    }
    workerRequest.ProtocolVersion = AssetImportJobRequest::CurrentProtocolVersion;
    if (!workerRequest.JobID.IsValid())
        workerRequest.JobID = Guid::New();
    if (!workerRequest.Capability.IsValid())
        workerRequest.Capability = Guid::New();
    workerRequest.Asset = plan.Request.Asset;
    workerRequest.SourceRevision = plan.Request.SourceRevision;
    workerRequest.SourcePath = plan.Request.SourcePath;
    workerRequest.Importer.ID = plan.Importer.ID;
    workerRequest.Importer.ProviderKind = plan.Importer.ProviderKind;
    workerRequest.Importer.ImporterVersion = plan.Importer.ImporterVersion;
    workerRequest.Importer.SettingsSchemaVersion = plan.Importer.SettingsSchemaVersion;
    workerRequest.Importer.ImplementationHash = plan.Importer.ImplementationHash;
    workerRequest.Importer.ProducesMainObject = plan.Importer.ProducesMainObject;
    workerRequest.Importer.ProducesSubObjects = plan.Importer.ProducesSubObjects;
    workerRequest.Target = plan.Request.Target;
    workerRequest.Limits.MaximumMemoryBytes = plan.Importer.MaximumMemoryBytes;
    workerRequest.Limits.MaximumOutputBytes = plan.Importer.MaximumOutputBytes;
    workerRequest.Limits.MaximumOutputFiles = plan.Importer.MaximumOutputFiles;
    workerRequest.Limits.TimeoutMilliseconds = plan.Importer.ImportTimeoutMilliseconds;
    if (AssetImportWorkerProtocol::ValidateRequest(workerRequest, diagnostic))
        return AssetBuildRequestHandle();

    struct Execution
    {
        AssetImportPlan Plan;
        String WorkerExecutable;
        AssetImportJobRequest Request;
        AssetImportJobResult Result;
        AssetImportWorkerPublishAction Publish;
    };
    auto execution = std::make_shared<Execution>();
    execution->Plan = plan;
    execution->WorkerExecutable = workerExecutable;
    execution->Request = MoveTemp(workerRequest);
    execution->Publish = MoveTemp(publish);

    AssetBuildJobRequest request;
    request.Key.ExactPlan = plan.StaticFingerprint;
    request.AssetID = plan.Request.Asset.Value;
    request.RefreshId = plan.Request.RefreshId;
    request.Pass = plan.Request.Pass;
    request.ProcessorID = plan.Importer.ID;
    request.Target = String(execution->Request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    request.KeyComponents = plan.KeyComponents;
    request.RebuildReason = plan.Request.Reason;
    ConfigureImporterBounds(plan, request, true);
    request.Priority = priority;
    request.AllowTerminalDeduplication = !plan.Request.Force;
    for (const AssetProcessorOutputDescriptor& output : plan.Importer.Processor.Outputs)
        request.OutputKinds.Add(output.Kind);
    request.Build = [execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& buildDiagnostic)
    {
        const bool failed = AssetImportWorkerProcess::Run(execution->WorkerExecutable, execution->Request, cancellation,
            execution->Result, buildDiagnostic);
        if (failed && FileSystem::DirectoryExists(execution->Request.OutputStagingPath))
            FileSystem::DeleteDirectory(execution->Request.OutputStagingPath, true);
        return failed;
    };
    request.Publish = [execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& publicationDiagnostic)
    {
        return execution->Publish(execution->Plan, execution->Result, cancellation, publicationDiagnostic);
    };
    diagnostic = AssetPipelineDiagnostic();
    return _builds.Request(request);
}

void AssetImportScheduler::Cancel(const AssetBuildRequestHandle& handle)
{
    _builds.CancelRequester(handle);
}
