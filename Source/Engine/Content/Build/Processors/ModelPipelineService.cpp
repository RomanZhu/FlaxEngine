// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelPipelineService.h"

#if COMPILE_WITH_MODEL_TOOL && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "ModelArtifactValidator.h"
#include "ModelProcessor.h"
#include "ModelProcessorSettings.h"
#include "TexturePipelineService.h"
#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseFacade.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/Scripting.h"
#include <memory>
#include <mutex>

namespace
{
    struct ModelExecution
    {
        PreparedAsset Prepared;
        ArtifactTarget Target;
        Guid JobID = Guid::New();
        Array<ArtifactBuildInput> Inputs;
        Array<ArtifactPublicationOutputPlan> Outputs;
        ArtifactOutputValidatorRegistry Validators;
        std::unique_ptr<ArtifactBuildContext> Context;
    };

    struct ModelPipelineState
    {
        std::mutex Locker;
        std::mutex InvocationLocker;
        AssetProcessorRegistration Registration;
        SourceHashCache HashCache;
        Dictionary<Guid, AssetBuildRequestHandle> Handles;
        Dictionary<Guid, ArtifactKey> Fingerprints;
        uint64 ForceGeneration = 0;
        bool Initialized = false;
    };

    ModelPipelineState& State()
    {
        static ModelPipelineState state;
        return state;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.ProcessorId = ModelProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    bool EnsureInitialized(AssetPipelineDiagnostic& diagnostic)
    {
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        if (state.Initialized)
            return false;
        AssetProcessorDescriptor existing;
        if (!AssetProcessorRegistry::Get().TryGetDescriptor(ModelProcessorSettings::ProcessorID(), existing) &&
            AssetProcessorRegistry::Get().Register(ModelProcessor::CreateDescriptor(), state.Registration, diagnostic))
            return true;
        state.Initialized = true;
        return false;
    }

    void QueryCurrentState(const Guid& assetID, uint64& revision, ArtifactKey& fingerprint)
    {
        AssetRecord record;
        if (!AssetDatabase::Get().TryGetRecord(assetID, record))
        {
            revision = 0;
            fingerprint = ArtifactKey();
            return;
        }
        revision = record.DatabaseRevision;
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        const ArtifactKey* value = state.Fingerprints.TryGet(assetID);
        fingerprint = value ? *value : ArtifactKey();
    }

    void QueueHotSwap(const ArtifactManifest& manifest, const String& typeName)
    {
        const ArtifactManifestOutput* runtime = nullptr;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            if (output.Kind == StringAnsiView("runtime"))
            {
                runtime = &output;
                break;
            }
        }
        if (!runtime)
            return;
        ArtifactStoragePath storagePath;
        AssetPipelineDiagnostic diagnostic;
        if (ArtifactStore::TryResolveLibraryRelative(Globals::ProjectLibraryFolder, runtime->RelativePath, storagePath, diagnostic))
            return;
        ResolvedArtifact artifact;
        artifact.AssetID = manifest.AssetID;
        artifact.TypeName = typeName;
        artifact.StoragePath = storagePath;
        artifact.OutputKind = TEXT("runtime");
        artifact.Key = String(runtime->Key.ToString());
        artifact.StorageKind = ArtifactStorageKind::Generated;
        artifact.IsExact = true;
        Scripting::InvokeOnUpdate([artifact]()
        {
            Asset* asset = Content::GetAsset(artifact.AssetID);
            auto* binary = asset ? ScriptingObject::Cast<BinaryAsset>(asset) : nullptr;
            if (!binary || binary->GetTypeName() != artifact.TypeName ||
                (!binary->IsLoaded() && !binary->LastLoadFailed()) ||
                (binary->GetArtifactKey() == artifact.Key && binary->IsUsingExactArtifact() && binary->IsLoaded()))
                return;
            const BinaryAssetStorageSwitchResult result = binary->SwitchStorage(artifact);
            if (result != BinaryAssetStorageSwitchResult::Success)
                LOG(Error, "Failed to hot-swap model-owned artifact. Asset: {0}, result: {1}.", artifact.AssetID, static_cast<int32>(result));
        });
    }

    bool PrepareRecord(const AssetRecord& record, const AssetMeta& meta, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
    {
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> invocationLock(state.InvocationLocker);
        AssetProcessorLease lease;
        if (AssetProcessorRegistry::Get().TryAcquire(record.ProcessorID, AssetProcessorInvocationStage::Prepare, lease, diagnostic))
            return true;
        AssetCancellationSource cancellation;
        {
            std::lock_guard<std::mutex> lock(state.Locker);
            PrepareAssetContext context(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
                record, lease.Get(), meta.Processor.SettingsJson, state.HashCache, cancellation.GetToken());
            if (lease.Get().Prepare(context, prepared, diagnostic) || context.Finalize(record.DatabaseRevision, prepared, diagnostic))
                return true;
            state.Fingerprints[record.ID] = prepared.InputFingerprint;
        }
        auto* payload = static_cast<ModelPreparedPayload*>(prepared.Payload.get());
        if (!payload)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
                record.ID, TEXT("Model preparation returned no processor payload."));
        payload->RootTypeName = meta.AssetType;
        for (const auto& mapping : meta.SubAssets)
        {
            if (!mapping.Value.Removed)
                payload->AssignedIDs[mapping.Key] = mapping.Value.ID;
        }
        return false;
    }
}

bool ModelPipelineService::CreatePlan(const AssetRecord& record, const ArtifactRequest& request,
    ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic)
{
    plan = ArtifactResolutionPlan();
    if (EnsureInitialized(diagnostic))
        return true;
    if (!record.ID.IsValid() || record.ProcessorID != ModelProcessorSettings::ProcessorID())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("The asset is not owned by the model processor."));

    AssetMeta meta;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic))
        return true;
    PreparedAsset prepared;
    if (PrepareRecord(record, meta, prepared, diagnostic))
        return true;
    SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, prepared.SubAssets, false);
    if (reconciliation.RequiresUserReconciliation || reconciliation.HasTrackedChanges)
    {
        diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : AssetPipelineDiagnostic();
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Prepare,
                record.ID, TEXT("Model subasset mappings require an explicit tracked reconciliation."));
        return true;
    }

    auto execution = std::make_shared<ModelExecution>();
    execution->Prepared = prepared;
    execution->Target = request.Target;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind != AssetDependencyKind::SourceFile)
            continue;
        ArtifactBuildInput input;
        input.StableIdentity = dependency.StableIdentity;
        input.Path = Globals::ProjectFolder / dependency.StableIdentity;
        FileSystem::NormalizePath(input.Path);
        input.ExpectedContent = dependency.Content;
        execution->Inputs.Add(MoveTemp(input));
    }
    if (execution->Inputs.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("Model preparation declared no source inputs."));
    if (ModelArtifactValidator::Register(execution->Validators, prepared, diagnostic))
        return true;

    ArtifactKeyBuilder jobBuilder(StringAnsiView("flax-model-build-job-v1"));
    jobBuilder.AddGuid(StringAnsiView("asset"), prepared.AssetID);
    jobBuilder.AddUInt64(StringAnsiView("database-revision"), prepared.DatabaseRevision);
    jobBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
    jobBuilder.AddKey(StringAnsiView("manifest-target"), request.Target.BuildKey(ArtifactTargetDimension::All));
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
    {
        ArtifactPublicationOutputPlan outputPlan;
        outputPlan.Kind = output.Kind;
        Array<ArtifactKeyComponent> outputComponents;
        if (ModelProcessor::BuildOutputKey(prepared, request.Target, output.Kind, outputPlan.Key, outputComponents, diagnostic))
            return true;
        execution->Outputs.Add(outputPlan);
        jobBuilder.AddKey(StringAnsi("output-") + output.Kind, outputPlan.Key);
    }

    plan.CurrentInputFingerprint = prepared.InputFingerprint;
    plan.BuildRequest.Key.ExactPlan = jobBuilder.Finalize();
    plan.BuildRequest.KeyComponents = jobBuilder.GetComponents();
    plan.BuildRequest.AssetID = prepared.AssetID;
    plan.BuildRequest.ProcessorClass = TEXT("model");
    plan.BuildRequest.ProcessorID = record.ProcessorID;
    plan.BuildRequest.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    plan.BuildRequest.MemoryBytes = Math::Max<uint64>(1, prepared.MemoryEstimate);
    plan.BuildRequest.ProcessorConcurrencyLimit = 1;
    plan.BuildRequest.RebuildReason = TEXT("Model canonical inputs, stable mappings, or target outputs changed.");
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
        plan.BuildRequest.OutputKinds.Add(output.Kind);

    plan.BuildRequest.Build = [execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& buildDiagnostic)
    {
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> invocationLock(state.InvocationLocker);
        execution->Context = std::make_unique<ArtifactBuildContext>(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, execution->JobID, execution->Prepared, execution->Inputs, cancellation, execution->Target);
        if (execution->Context->Initialize(buildDiagnostic))
            return true;
        AssetProcessorLease lease;
        if (AssetProcessorRegistry::Get().TryAcquire(ModelProcessorSettings::ProcessorID(), AssetProcessorInvocationStage::Build, lease, buildDiagnostic))
        {
            execution->Context->Cancel();
            return true;
        }
        const bool failed = lease.Get().Build(*execution->Context, buildDiagnostic);
        if (failed)
            execution->Context->Cancel();
        return failed;
    };
    plan.BuildRequest.Publish = [execution](const AssetCancellationToken&, AssetPipelineDiagnostic& publicationDiagnostic)
    {
        if (!execution->Context)
            return Fail(publicationDiagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Publication,
                execution->Prepared.AssetID, TEXT("Model build produced no publication context."));
        ArtifactPublicationRequest publication;
        publication.Target = execution->Target;
        publication.ProcessorID = ModelProcessorSettings::ProcessorID();
        publication.ProcessorImplementationVersion = ModelProcessor::ImplementationVersion;
        publication.BuildID = execution->JobID.ToString(Guid::FormatType::N);
        publication.Outputs = execution->Outputs;
        publication.QueryCurrentState = [assetID = execution->Prepared.AssetID](uint64& revision, ArtifactKey& fingerprint)
        {
            QueryCurrentState(assetID, revision, fingerprint);
        };
        publication.Notify = [typeName = execution->Prepared.OutputType](const ArtifactManifest& manifest)
        {
            QueueHotSwap(manifest, typeName);
        };
        ArtifactPublicationResult result;
        return ArtifactPublisher::Publish(Globals::ProjectLibraryFolder, execution->Prepared, *execution->Context,
            publication, execution->Validators, result, publicationDiagnostic);
    };
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ModelPipelineService::RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic)
{
    if (EnsureInitialized(diagnostic))
        return true;
    AssetBuildService* builds = TexturePipelineService::GetBuildService(diagnostic);
    if (!builds)
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            assetID, TEXT("Model asset is not registered."));

    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = TexturePipelineService::GetHostTarget();
    request.OutputKind = "runtime";
    request.RequiredCompatibility = "flax-model-runtime-v1";
    request.Policy = ArtifactResolvePolicy::Exact;
    ArtifactResolutionPlan plan;
    if (CreatePlan(record, request, plan, diagnostic))
        return true;
    if (force)
    {
        uint64 generation;
        {
            ModelPipelineState& state = State();
            std::lock_guard<std::mutex> lock(state.Locker);
            generation = ++state.ForceGeneration;
        }
        ArtifactKeyBuilder builder(StringAnsiView("flax-model-forced-build-v1"));
        builder.AddKey(StringAnsiView("exact-plan"), plan.BuildRequest.Key.ExactPlan);
        builder.AddUInt64(StringAnsiView("generation"), generation);
        plan.BuildRequest.Key.ExactPlan = builder.Finalize();
        plan.BuildRequest.RebuildReason = TEXT("Explicit model rebuild.");
    }

    const AssetBuildRequestHandle handle = builds->Request(plan.BuildRequest);
    if (!handle.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            assetID, TEXT("Model build request was not accepted."));
    {
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        state.Handles[assetID] = handle;
    }
    AssetBuildJobResult immediate;
    if (handle.TryGetResult(immediate) && immediate.Status == AssetBuildJobStatus::Failed)
    {
        diagnostic = immediate.Diagnostic;
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

AssetBuildJobStatus ModelPipelineService::GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    {
        ModelPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        const AssetBuildRequestHandle* value = state.Handles.TryGet(assetID);
        if (!value)
        {
            diagnostic = AssetPipelineDiagnostic();
            return AssetBuildJobStatus::Invalid;
        }
        handle = *value;
    }
    AssetBuildJobResult result;
    if (handle.TryGetResult(result))
        diagnostic = result.Diagnostic;
    else
        diagnostic = AssetPipelineDiagnostic();
    return handle.GetStatus();
}

bool ModelPipelineService::ReconcileMetadata(const Guid& rootAssetID, Array<SubAssetReconcileChange>& changes,
    AssetPipelineDiagnostic& diagnostic)
{
    changes.Clear();
    if (EnsureInitialized(diagnostic))
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(rootAssetID, record) || !record.IsMainAsset() ||
        record.ProcessorID != ModelProcessorSettings::ProcessorID())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            rootAssetID, TEXT("Model reconciliation requires a registered root model GUID."));
    AssetMeta meta;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic))
        return true;
    PreparedAsset prepared;
    if (PrepareRecord(record, meta, prepared, diagnostic))
        return true;
    SubAssetReconcileResult result = SubAssetReconciler::Reconcile(meta, prepared.SubAssets, true);
    if (result.RequiresUserReconciliation)
    {
        diagnostic = result.Diagnostics.HasItems() ? result.Diagnostics[0] : diagnostic;
        return true;
    }
    changes = result.Changes;
    if (!result.HasTrackedChanges)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    meta.SubAssets = MoveTemp(result.Resolved);
    if (AssetMeta::SaveAtomic(record.MetaPath.Get(), meta, diagnostic))
        return true;
    if (AssetDatabaseFacade::Scan(false))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, AssetPipelineDiagnosticStage::DatabaseScan,
            rootAssetID, TEXT("Model metadata was reconciled but the database rescan failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void ModelPipelineService::Shutdown()
{
    ModelPipelineState& state = State();
    AssetProcessorRegistration registration;
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        if (!state.Initialized)
            return;
        state.Handles.Clear();
        state.Fingerprints.Clear();
        registration = MoveTemp(state.Registration);
        state.Initialized = false;
    }
    registration.Reset();
}

#endif
