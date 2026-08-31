// Copyright (c) Wojciech Figat. All rights reserved.

#include "TexturePipelineService.h"

#if COMPILE_WITH_TEXTURE_TOOL && COMPILE_WITH_ASSETS_IMPORTER

#include "TextureArtifactValidator.h"
#include "TextureProcessor.h"
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
#include "ModelPipelineService.h"
#include "ModelProcessorSettings.h"
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "GraphDocumentProcessor.h"
#include "GraphPipelineService.h"
#endif
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Importing/AssetImportService.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Scripting/Scripting.h"
#include <memory>
#include <mutex>

namespace
{
    struct TextureExecution
    {
        PreparedAsset Prepared;
        ArtifactTarget Target;
        Guid JobID = Guid::New();
        Array<ArtifactBuildInput> Inputs;
        Array<ArtifactPublicationOutputPlan> Outputs;
        ArtifactOutputValidatorRegistry Validators;
        std::unique_ptr<ArtifactBuildContext> Context;
    };

    struct TexturePipelineState
    {
        std::mutex Locker;
        std::unique_ptr<AssetBuildService> Builds;
        AssetProcessorRegistration Registration;
        SourceHashCache HashCache;
        Dictionary<Guid, AssetBuildRequestHandle> Handles;
        Dictionary<Guid, AssetBuildRequestHandle> ThumbnailHandles;
        Dictionary<Guid, ArtifactKey> ThumbnailPlans;
        Dictionary<Guid, ArtifactKey> Fingerprints;
        uint64 ForceGeneration = 0;
        bool Initialized = false;
    };

    TexturePipelineState& State()
    {
        static TexturePipelineState state;
        return state;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.ProcessorId = TextureProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    bool EnsureInitialized(AssetPipelineDiagnostic& diagnostic)
    {
        TexturePipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        if (state.Initialized)
            return false;

        AssetProcessorDescriptor implementation;
        if (!AssetProcessorRegistry::Get().TryGetDescriptor(TextureProcessorSettings::ProcessorID(), implementation))
        {
            implementation = TextureProcessor::CreateDescriptor();
            if (AssetProcessorRegistry::Get().Register(implementation, state.Registration, diagnostic))
                return true;
        }
        if (AssetImportService::EnsureInitialized(diagnostic))
            return true;
        if (AssetImportService::RegisterBuiltIn(implementation, diagnostic,
            [](const Guid& id, bool force, AssetPipelineDiagnostic& localDiagnostic)
            {
                return TexturePipelineService::RequestBuild(id, force, localDiagnostic);
            },
            [](const Guid& id, AssetPipelineDiagnostic& localDiagnostic)
            {
                return TexturePipelineService::GetStatus(id, localDiagnostic);
            }))
            return true;

        state.Builds = std::make_unique<AssetBuildService>();
        AssetBuildServiceLimits limits;
        limits.MaximumWorkers = 2;
        limits.MaximumMemoryBytes = 4ull * 1024ull * 1024ull * 1024ull;
        limits.MaximumExternalTools = 1;
        if (state.Builds->Initialize(Globals::ProjectLibraryFolder, limits, diagnostic))
        {
            state.Builds.reset();
            state.Registration.Reset();
            return true;
        }
        if (AssetImportService::AttachBuildService(*state.Builds, diagnostic))
        {
            state.Builds->Shutdown();
            state.Builds.reset();
            AssetImportService::Shutdown();
            state.Registration.Reset();
            return true;
        }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
        if (GraphPipelineService::EnsureInitialized(diagnostic))
        {
            AssetImportService::Shutdown();
            state.Builds.reset();
            state.Registration.Reset();
            return true;
        }
#endif
        state.Initialized = true;
        ArtifactResolutionPlanProvider provider = [](const AssetRecord& record, const ArtifactRequest& request,
            ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& planDiagnostic)
        {
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
            if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
                return ModelPipelineService::CreatePlan(record, request, plan, planDiagnostic);
#endif
            if (record.ProcessorID == GraphDocumentProcessor::ProcessorID() || GraphPipelineService::OwnsProcessor(record.ProcessorID))
                return GraphPipelineService::CreatePlan(record, request, plan, planDiagnostic);
            if (record.ProcessorID == TextureProcessorSettings::ProcessorID())
                return TexturePipelineService::CreatePlan(record, request, plan, planDiagnostic);
            const bool failed = Fail(planDiagnostic, AssetPipelineDiagnosticCode::ProcessorMissing,
                AssetPipelineDiagnosticStage::Prepare, record.ID, TEXT("No artifact plan provider owns the asset processor."));
            planDiagnostic.ProcessorId = record.ProcessorID;
            planDiagnostic.SourcePath = record.SourcePath.Get();
            return failed;
        };
        ArtifactResolver::Get().Configure(AssetDatabase::Get(), *state.Builds, Globals::ProjectLibraryFolder,
            TexturePipelineService::GetHostTarget(), provider);
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
        TexturePipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        const ArtifactKey* current = state.Fingerprints.TryGet(assetID);
        fingerprint = current ? *current : ArtifactKey();
    }

    void ApplyTextureHotSwap(const Guid assetID, const ResolvedArtifact artifact, const bool hasRuntime)
    {
        if (hasRuntime)
        {
            Asset* asset = Content::GetRuntimeObject(assetID);
            auto* texture = asset && asset->GetTypeName() == Texture::TypeName ? static_cast<Texture*>(asset) : nullptr;
            if (texture && texture->IsLoading() && !texture->IsLoaded() && !texture->LastLoadFailed())
            {
                Scripting::InvokeOnUpdate([assetID, artifact, hasRuntime]()
                {
                    ApplyTextureHotSwap(assetID, artifact, hasRuntime);
                });
                return;
            }
            if (texture && (texture->IsLoaded() || texture->LastLoadFailed()) &&
                !(texture->GetArtifactKey() == artifact.Key && texture->IsUsingExactArtifact() && texture->IsLoaded()))
            {
                const BinaryAssetStorageSwitchResult result = texture->SwitchStorage(artifact);
                if (result != BinaryAssetStorageSwitchResult::Success)
                    LOG(Error, "Failed to hot-swap texture artifact. Asset: {0}, result: {1}.", artifact.AssetID, static_cast<int32>(result));
            }
        }
        AssetPipelineService::NotifyArtifactPublished(assetID);
    }

    void QueueTextureHotSwap(const ArtifactManifest& manifest)
    {
        const Guid assetID = manifest.AssetID;
        const ArtifactManifestOutput* runtime = nullptr;
        for (const ArtifactManifestOutput& output : manifest.Outputs)
        {
            if (output.Kind == StringAnsiView("runtime"))
            {
                runtime = &output;
                break;
            }
        }
        ResolvedArtifact artifact;
        bool hasRuntime = false;
        if (runtime)
        {
            ArtifactStoragePath storagePath;
            AssetPipelineDiagnostic diagnostic;
            if (!ArtifactStore::TryResolveLibraryRelative(Globals::ProjectLibraryFolder, runtime->RelativePath, storagePath, diagnostic))
            {
                artifact.AssetID = assetID;
                artifact.TypeName = Texture::TypeName;
                artifact.StoragePath = storagePath;
                artifact.OutputKind = TEXT("runtime");
                artifact.Key = String(runtime->Key.ToString());
                artifact.StorageKind = ArtifactStorageKind::Generated;
                artifact.IsExact = true;
                artifact.IsLastGood = false;
                hasRuntime = true;
            }
        }
        Scripting::InvokeOnUpdate([assetID, artifact, hasRuntime]()
        {
            ApplyTextureHotSwap(assetID, artifact, hasRuntime);
        });
    }

    class TexturePipelineEngineService : public EngineService
    {
    public:
        TexturePipelineEngineService()
            : EngineService(TEXT("TexturePipeline"), -510)
        {
        }

        bool Init() override
        {
            AssetPipelineDiagnostic diagnostic;
            if (!TexturePipelineService::GetBuildService(diagnostic))
            {
                LOG(Error, "Cannot initialize the texture artifact pipeline: {0}", diagnostic.Message);
                return true;
            }
            return false;
        }

        void Dispose() override
        {
            TexturePipelineService::Shutdown();
        }
    };

    TexturePipelineEngineService TexturePipelineEngineServiceInstance;
}

const ArtifactTarget& TexturePipelineService::GetHostTarget()
{
    static ArtifactTarget target;
    static bool initialized = false;
    if (!initialized)
    {
        target.Platform = "Windows";
        target.Architecture = "x64";
        target.Graphics = "DirectX12";
        target.Configuration = "Development";
        target.Quality = "High";
        target.TextureCompression = "Desktop";
        target.Role = "Editor";
        initialized = true;
    }
    return target;
}

AssetBuildService* TexturePipelineService::GetBuildService(AssetPipelineDiagnostic& diagnostic)
{
    if (EnsureInitialized(diagnostic))
        return nullptr;
    return State().Builds.get();
}

bool TexturePipelineService::CreatePlan(const AssetRecord& record, const ArtifactRequest& request,
    ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic)
{
    plan = ArtifactResolutionPlan();
    if (EnsureInitialized(diagnostic))
        return true;
    if (!record.ID.IsValid() || record.ProcessorID != TextureProcessorSettings::ProcessorID())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("The asset is not owned by the texture processor."));

    AssetMeta meta;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic))
        return true;
    AssetProcessorLease prepareLease;
    if (AssetProcessorRegistry::Get().TryAcquire(record.ProcessorID, AssetProcessorInvocationStage::Prepare, prepareLease, diagnostic))
        return true;

    AssetCancellationSource preparationCancellation;
    PreparedAsset prepared;
    TexturePipelineState& state = State();
    PrepareAssetContext context(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
        record, prepareLease.Get(), meta.Processor.SettingsJson, state.HashCache, preparationCancellation.GetToken());
    if (prepareLease.Get().Prepare(context, prepared, diagnostic) ||
        context.Finalize(record.DatabaseRevision, prepared, diagnostic))
        return true;
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        state.Fingerprints[record.ID] = prepared.InputFingerprint;
    }

    auto execution = std::make_shared<TextureExecution>();
    execution->Prepared = prepared;
    execution->Target = request.Target;
    DeclaredArtifactOutput selectedOutput;
    bool hasSelectedOutput = false;
    for (const DeclaredArtifactOutput& output : prepared.Outputs)
    {
        if (output.Kind == request.OutputKind)
        {
            selectedOutput = output;
            hasSelectedOutput = true;
            break;
        }
    }
    if (!hasSelectedOutput)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, TEXT("Texture processor does not declare the requested output kind."));
    execution->Prepared.Outputs.Clear();
    execution->Prepared.Outputs.Add(selectedOutput);
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
            record.ID, TEXT("Texture preparation declared no source input."));
    if (TextureArtifactValidator::Register(execution->Validators, record.ID, record.TypeName, diagnostic))
        return true;

    ArtifactKeyBuilder jobBuilder(StringAnsiView("flax-texture-build-job-v1"));
    jobBuilder.AddGuid(StringAnsiView("asset"), prepared.AssetID);
    jobBuilder.AddUInt64(StringAnsiView("database-revision"), prepared.DatabaseRevision);
    jobBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
    jobBuilder.AddKey(StringAnsiView("manifest-target"), request.Target.BuildKey(ArtifactTargetDimension::All));
    for (const DeclaredArtifactOutput& output : execution->Prepared.Outputs)
    {
        ArtifactPublicationOutputPlan outputPlan;
        outputPlan.Kind = output.Kind;
        Array<ArtifactKeyComponent> outputComponents;
        if (TextureProcessor::BuildOutputKey(prepared, request.Target, output.Kind, outputPlan.Key, outputComponents, diagnostic))
            return true;
        execution->Outputs.Add(outputPlan);
        jobBuilder.AddKey(StringAnsi::Format("output-{0}", output.Kind), outputPlan.Key);
    }

    plan.CurrentInputFingerprint = prepared.InputFingerprint;
    plan.BuildRequest.Key.ExactPlan = jobBuilder.Finalize();
    plan.BuildRequest.KeyComponents = jobBuilder.GetComponents();
    plan.BuildRequest.AssetID = prepared.AssetID;
    plan.BuildRequest.ProcessorClass = TEXT("texture");
    plan.BuildRequest.ProcessorID = record.ProcessorID;
    plan.BuildRequest.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
    plan.BuildRequest.MemoryBytes = Math::Max<uint64>(1, prepared.MemoryEstimate);
    plan.BuildRequest.ProcessorConcurrencyLimit = 2;
    plan.BuildRequest.AllowTerminalDeduplication = false;
    plan.BuildRequest.RebuildReason = TEXT("Texture canonical inputs changed or rebuild was requested.");
    for (const DeclaredArtifactOutput& output : execution->Prepared.Outputs)
        plan.BuildRequest.OutputKinds.Add(output.Kind);

    plan.BuildRequest.Build = [execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& buildDiagnostic)
    {
        execution->Context = std::make_unique<ArtifactBuildContext>(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, execution->JobID, execution->Prepared, execution->Inputs, cancellation, execution->Target);
        if (execution->Context->Initialize(buildDiagnostic))
            return true;
        AssetProcessorLease buildLease;
        if (AssetProcessorRegistry::Get().TryAcquire(TextureProcessorSettings::ProcessorID(), AssetProcessorInvocationStage::Build, buildLease, buildDiagnostic))
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
                execution->Prepared.AssetID, TEXT("Texture build produced no publication context."));
        ArtifactPublicationRequest publication;
        publication.Target = execution->Target;
        publication.ProcessorID = TextureProcessorSettings::ProcessorID();
        publication.ProcessorImplementationVersion = TextureProcessor::ImplementationVersion;
        publication.BuildID = execution->JobID.ToString(Guid::FormatType::N);
        publication.Outputs = execution->Outputs;
        publication.QueryCurrentState = [assetID = execution->Prepared.AssetID](uint64& revision, ArtifactKey& fingerprint)
        {
            QueryCurrentState(assetID, revision, fingerprint);
        };
        publication.Notify = [](const ArtifactManifest& manifest)
        {
            QueueTextureHotSwap(manifest);
        };
        ArtifactPublicationResult result;
        return ArtifactPublisher::Publish(Globals::ProjectLibraryFolder, execution->Prepared, *execution->Context,
            publication, execution->Validators, result, publicationDiagnostic);
    };
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool TexturePipelineService::RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildService* builds = GetBuildService(diagnostic);
    if (!builds)
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            assetID, TEXT("Texture asset is not registered."));

    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = GetHostTarget();
    request.OutputKind = "runtime";
    request.RequiredCompatibility = "flax-texture-v4";
    request.Policy = ArtifactResolvePolicy::Exact;
    ArtifactResolutionPlan plan;
    if (CreatePlan(record, request, plan, diagnostic))
        return true;
    if (force)
    {
        TexturePipelineState& state = State();
        uint64 generation;
        {
            std::lock_guard<std::mutex> lock(state.Locker);
            generation = ++state.ForceGeneration;
        }
        ArtifactKeyBuilder builder(StringAnsiView("flax-texture-forced-build-v1"));
        builder.AddKey(StringAnsiView("exact-plan"), plan.BuildRequest.Key.ExactPlan);
        builder.AddUInt64(StringAnsiView("generation"), generation);
        plan.BuildRequest.Key.ExactPlan = builder.Finalize();
        plan.BuildRequest.RebuildReason = TEXT("Explicit texture rebuild.");
    }

    const AssetBuildRequestHandle handle = builds->Request(plan.BuildRequest);
    if (!handle.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            assetID, TEXT("Texture build request was not accepted."));
    {
        TexturePipelineState& state = State();
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

bool TexturePipelineService::RequestThumbnailBuild(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildService* builds = GetBuildService(diagnostic);
    if (!builds)
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            assetID, TEXT("Texture asset is not registered."));

    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = GetHostTarget();
    request.OutputKind = "thumbnail";
    request.RequiredCompatibility = "flax-texture-thumbnail-v2";
    request.Policy = ArtifactResolvePolicy::Exact;
    ArtifactResolutionPlan plan;
    if (CreatePlan(record, request, plan, diagnostic))
        return true;

    TexturePipelineState& state = State();
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        const ArtifactKey* existingPlan = state.ThumbnailPlans.TryGet(assetID);
        const AssetBuildRequestHandle* existingHandle = state.ThumbnailHandles.TryGet(assetID);
        if (existingPlan && existingHandle && *existingPlan == plan.BuildRequest.Key.ExactPlan && existingHandle->IsValid())
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    }

    const AssetBuildRequestHandle handle = builds->Request(plan.BuildRequest);
    if (!handle.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            assetID, TEXT("Texture thumbnail build request was not accepted."));
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        state.ThumbnailPlans[assetID] = plan.BuildRequest.Key.ExactPlan;
        state.ThumbnailHandles[assetID] = handle;
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

AssetBuildJobStatus TexturePipelineService::GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    {
        TexturePipelineState& state = State();
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

AssetBuildJobStatus TexturePipelineService::GetThumbnailStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    {
        TexturePipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        const AssetBuildRequestHandle* value = state.ThumbnailHandles.TryGet(assetID);
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

void TexturePipelineService::Shutdown()
{
    ArtifactResolver::Get().Reset();
    TexturePipelineState& state = State();
    std::unique_ptr<AssetBuildService> builds;
    AssetProcessorRegistration registration;
    {
        std::lock_guard<std::mutex> lock(state.Locker);
        if (!state.Initialized)
            return;
        state.Handles.Clear();
        state.ThumbnailHandles.Clear();
        state.ThumbnailPlans.Clear();
        state.Fingerprints.Clear();
        builds = MoveTemp(state.Builds);
        registration = MoveTemp(state.Registration);
        state.Initialized = false;
    }
    if (builds)
        builds->Shutdown();
    AssetImportService::Shutdown();
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
    ModelPipelineService::Shutdown();
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    GraphPipelineService::Shutdown();
#endif
    registration.Reset();
}

#endif
