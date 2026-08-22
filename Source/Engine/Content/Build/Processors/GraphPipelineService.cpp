// Copyright (c) Wojciech Figat. All rights reserved.

#include "GraphPipelineService.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "GraphDocumentProcessor.h"
#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/Scripting.h"
#include "TexturePipelineService.h"
#include <memory>
#include <mutex>

namespace
{
    struct GraphExecution
    {
        PreparedAsset Prepared;
        ArtifactTarget Target;
        Guid JobID = Guid::New();
        Array<ArtifactBuildInput> Inputs;
        Array<ArtifactPublicationOutputPlan> Outputs;
        ArtifactOutputValidatorRegistry Validators;
        std::unique_ptr<ArtifactBuildContext> Context;
    };

    struct GraphPipelineState
    {
        std::mutex Locker;
        AssetProcessorRegistration Registration;
        SourceHashCache HashCache;
        Dictionary<Guid, AssetBuildRequestHandle> Handles;
        Dictionary<Guid, ArtifactKey> Fingerprints;
        uint64 ForceGeneration = 0;
        bool Initialized = false;
    };

    GraphPipelineState& State()
    {
        static GraphPipelineState state;
        return state;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.ProcessorId = GraphDocumentProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
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
        GraphPipelineState& state = State();
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
            if (!binary || !binary->IsLoaded() || !binary->IsUsingGeneratedArtifact() || binary->GetTypeName() != artifact.TypeName ||
                (binary->GetArtifactKey() == artifact.Key && binary->IsUsingExactArtifact()))
                return;
            const BinaryAssetStorageSwitchResult result = binary->SwitchStorage(artifact);
            if (result != BinaryAssetStorageSwitchResult::Success)
                LOG(Error, "Failed to hot-swap graph artifact. Asset: {0}, result: {1}.", artifact.AssetID, static_cast<int32>(result));
        });
    }
}

bool GraphPipelineService::EnsureInitialized(AssetPipelineDiagnostic& diagnostic)
{
    GraphPipelineState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    if (state.Initialized)
        return false;
    AssetProcessorDescriptor existing;
    if (!AssetProcessorRegistry::Get().TryGetDescriptor(GraphDocumentProcessor::ProcessorID(), existing) &&
        AssetProcessorRegistry::Get().Register(GraphDocumentProcessor::CreateDescriptor(), state.Registration, diagnostic))
        return true;
    state.Initialized = true;
    return false;
}

bool GraphPipelineService::CreatePlan(const AssetRecord& record, const ArtifactRequest& request,
    ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic)
{
    plan = ArtifactResolutionPlan();
    if (EnsureInitialized(diagnostic))
        return true;
    if (!record.ID.IsValid() || record.ProcessorID != GraphDocumentProcessor::ProcessorID())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("The asset is not owned by the graph document processor."));
    if (GraphDocumentPreview::IsPreviewPath(record.SourcePath.Get()))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactIncompatible, AssetPipelineDiagnosticStage::Resolution,
            record.ID, TEXT("Preview graph artifacts are never consumed by the current resolver or cooker."));

    AssetMeta meta;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic))
        return true;
    AssetProcessorLease prepareLease;
    if (AssetProcessorRegistry::Get().TryAcquire(record.ProcessorID, AssetProcessorInvocationStage::Prepare, prepareLease, diagnostic))
        return true;

    AssetCancellationSource preparationCancellation;
    PreparedAsset prepared;
    GraphPipelineState& state = State();
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        PrepareAssetContext context(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
            record, prepareLease.Get(), meta.Processor.SettingsJson, state.HashCache, preparationCancellation.GetToken());
        if (prepareLease.Get().Prepare(context, prepared, diagnostic) ||
            context.Finalize(record.DatabaseRevision, prepared, diagnostic))
            return true;
        state.Fingerprints[record.ID] = prepared.InputFingerprint;
    }

    auto execution = std::make_shared<GraphExecution>();
    execution->Prepared = prepared;
    execution->Target = request.Target;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind != AssetDependencyKind::SourceFile)
            continue;
        ArtifactBuildInput input;
        input.StableIdentity = dependency.StableIdentity;
        input.Path = record.SourcePath.Get();
        input.ExpectedContent = dependency.Content;
        execution->Inputs.Add(MoveTemp(input));
    }
    if (execution->Inputs.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("Graph preparation declared no source input."));

    ArtifactOutputValidator runtime = [expectedAssetID = record.ID, typeName = record.TypeName](const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& result)
    {
        if (output.FormatVersion != GraphDocumentProcessor::RuntimeFormatVersion || output.Compatibility != "flax-graph-document-v1" ||
            output.Size == 0 || output.Size != FileSystem::GetFileSize(path) || GraphDocumentPreview::IsPreviewPath(path))
            return Fail(result, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Publication,
                expectedAssetID, TEXT("Graph runtime artifact format, size, or preview path is invalid."));
        auto storage = ContentStorageManager::GetStorage(path);
        if (!storage)
            return Fail(result, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Publication,
                expectedAssetID, TEXT("Graph runtime artifact is not a readable Flax storage file."));
        Array<FlaxStorage::Entry> entries;
        storage->GetEntries(entries);
        if (entries.Count() != 1 || entries[0].ID != expectedAssetID || entries[0].TypeName != typeName)
            return Fail(result, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Publication,
                expectedAssetID, TEXT("Graph runtime artifact identity or type does not match the requested asset."));
        result = AssetPipelineDiagnostic();
        return false;
    };
    if (execution->Validators.Register(StringAnsiView("runtime"), record.TypeName, runtime, diagnostic))
        return true;

    ArtifactKeyBuilder jobBuilder(StringAnsiView("flax-graph-document-build-job-v2"));
    jobBuilder.AddGuid(StringAnsiView("asset"), prepared.AssetID);
    jobBuilder.AddUInt64(StringAnsiView("database-revision"), prepared.DatabaseRevision);
    jobBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
    jobBuilder.AddKey(StringAnsiView("manifest-target"), request.Target.BuildKey(ArtifactTargetDimension::All));
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
    {
        ArtifactPublicationOutputPlan outputPlan;
        outputPlan.Kind = output.Kind;
        Array<ArtifactKeyComponent> outputComponents;
        if (GraphDocumentProcessor::BuildOutputKey(prepared, request.Target, output.Kind, outputPlan.Key, outputComponents, diagnostic))
            return true;
        execution->Outputs.Add(outputPlan);
        jobBuilder.AddKey(StringAnsi::Format("output-{0}", output.Kind), outputPlan.Key);
    }

    plan.CurrentInputFingerprint = prepared.InputFingerprint;
    plan.BuildRequest.Key.ExactPlan = jobBuilder.Finalize();
    plan.BuildRequest.KeyComponents = jobBuilder.GetComponents();
    plan.BuildRequest.AssetID = prepared.AssetID;
    plan.BuildRequest.ProcessorClass = TEXT("graph-document");
    plan.BuildRequest.ProcessorID = record.ProcessorID;
    plan.BuildRequest.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    plan.BuildRequest.MemoryBytes = Math::Max<uint64>(1, prepared.MemoryEstimate);
    plan.BuildRequest.ProcessorConcurrencyLimit = 2;
    plan.BuildRequest.RebuildReason = TEXT("Graph canonical inputs changed or rebuild was requested.");
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
        plan.BuildRequest.OutputKinds.Add(output.Kind);

    plan.BuildRequest.Build = [execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& buildDiagnostic)
    {
        execution->Context = std::make_unique<ArtifactBuildContext>(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, execution->JobID, execution->Prepared, execution->Inputs, cancellation, execution->Target);
        if (execution->Context->Initialize(buildDiagnostic))
            return true;
        AssetProcessorLease buildLease;
        if (AssetProcessorRegistry::Get().TryAcquire(GraphDocumentProcessor::ProcessorID(), AssetProcessorInvocationStage::Build, buildLease, buildDiagnostic))
        {
            execution->Context->Cancel();
            return true;
        }
        const bool failed = buildLease.Get().Build(*execution->Context, buildDiagnostic);
        if (failed)
            execution->Context->Cancel();
        return failed;
    };
    plan.BuildRequest.Publish = [execution](const AssetCancellationToken&, AssetPipelineDiagnostic& publicationDiagnostic)
    {
        if (!execution->Context)
            return Fail(publicationDiagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Publication,
                execution->Prepared.AssetID, TEXT("Graph build produced no publication context."));
        ArtifactPublicationRequest publication;
        publication.Target = execution->Target;
        publication.ProcessorID = GraphDocumentProcessor::ProcessorID();
        publication.ProcessorImplementationVersion = GraphDocumentProcessor::ImplementationVersion;
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

bool GraphPipelineService::RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic)
{
    if (EnsureInitialized(diagnostic))
        return true;
    AssetBuildService* builds = TexturePipelineService::GetBuildService(diagnostic);
    if (!builds)
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            assetID, TEXT("Graph asset is not registered."));

    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = TexturePipelineService::GetHostTarget();
    request.OutputKind = "runtime";
    request.RequiredCompatibility = "flax-graph-document-v1";
    request.Policy = ArtifactResolvePolicy::Exact;
    ArtifactResolutionPlan plan;
    if (CreatePlan(record, request, plan, diagnostic))
        return true;
    if (force)
    {
        uint64 generation;
        {
            GraphPipelineState& state = State();
            std::lock_guard<std::mutex> lock(state.Locker);
            generation = ++state.ForceGeneration;
        }
        ArtifactKeyBuilder builder(StringAnsiView("flax-graph-document-forced-build-v1"));
        builder.AddKey(StringAnsiView("exact-plan"), plan.BuildRequest.Key.ExactPlan);
        builder.AddUInt64(StringAnsiView("generation"), generation);
        plan.BuildRequest.Key.ExactPlan = builder.Finalize();
        plan.BuildRequest.RebuildReason = TEXT("Explicit graph rebuild.");
    }

    const AssetBuildRequestHandle handle = builds->Request(plan.BuildRequest);
    if (!handle.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            assetID, TEXT("Graph build request was not accepted."));
    {
        GraphPipelineState& state = State();
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

AssetBuildJobStatus GraphPipelineService::GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    {
        GraphPipelineState& state = State();
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

void GraphPipelineService::Shutdown()
{
    GraphPipelineState& state = State();
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
