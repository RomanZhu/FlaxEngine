// Copyright (c) Wojciech Figat. All rights reserved.

#include "CallbackImporterPipelineService.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "AssetImportService.h"
#include "Engine/Content/Artifacts/ArtifactOutputValidator.h"
#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <memory>
#include <mutex>

namespace
{
    struct CallbackImportExecution
    {
        AssetImportPlan Plan;
        AssetImportJobRequest Request;
        Guid PublicationJobID = Guid::New();
        ArtifactTarget Target;
        PreparedAsset Prepared;
        Array<ArtifactPublicationOutputPlan> PublicationOutputs;
        ArtifactOutputValidatorRegistry Validators;
        std::unique_ptr<ArtifactBuildContext> Context;

        ~CallbackImportExecution()
        {
            const String root = ArtifactStore::GetTemporaryPath(Globals::ProjectLibraryFolder) / TEXT("CallbackWorkers");
            if (!Request.OutputStagingPath.IsEmpty() && AssetPathPolicy::IsSameOrChild(Request.OutputStagingPath, root) &&
                FileSystem::DirectoryExists(Request.OutputStagingPath))
                FileSystem::DeleteDirectory(Request.OutputStagingPath, true);
        }
    };

    struct CallbackImporterPipelineState
    {
        std::mutex Locker;
        Dictionary<Guid, AssetBuildRequestHandle> Handles;
        Dictionary<Guid, ArtifactKey> Fingerprints;
    };

    CallbackImporterPipelineState& State()
    {
        static CallbackImporterPipelineState state;
        return state;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
              const Guid& assetID, const StringView& processorID, const StringView& sourcePath, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.ProcessorId = processorID;
        diagnostic.SourcePath = sourcePath;
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
        CallbackImporterPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        const ArtifactKey* value = state.Fingerprints.TryGet(assetID);
        fingerprint = value ? *value : ArtifactKey();
    }

    bool ReadSnapshot(const StringView& path, Array<byte>& data, ContentHash& hash,
                      const Guid& assetID, const StringView& processorID, AssetPipelineDiagnostic& diagnostic)
    {
        if (File::ReadAllBytes(path, data))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, AssetPipelineDiagnosticStage::Prepare,
                assetID, processorID, path, TEXT("Cannot read an immutable importer input snapshot."));
        hash = ContentHash::Compute(data.Get(), data.Count());
        return false;
    }

    bool CollectAuthorizedInputs(const AssetRecord& record, AssetImportJobRequest& request, AssetPipelineDiagnostic& diagnostic)
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.ProcessorID, Globals::ProjectContentFolder,
                TEXT("Cannot enumerate controlled scripted-importer input snapshots."));
        uint64 totalBytes = request.SourceSnapshot.Count() + request.MetaSnapshot.Count();
        if (totalBytes > request.Limits.MaximumInputBytes)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, AssetPipelineDiagnosticStage::Prepare,
                record.ID, record.ProcessorID, record.SourcePath.Get(), TEXT("Source and metadata snapshots exceed the input quota."));
        for (const String& file : files)
        {
            if (file.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase) ||
                file.Compare(record.SourcePath.Get(), StringSearchCase::IgnoreCase) == 0)
                continue;
            AssetPathPolicy::ProjectPath normalized;
            if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
                Globals::ProjectLibraryFolder, file, normalized, diagnostic))
                return true;
            AssetImportWorkerInput input;
            input.Identity = normalized.ProjectRelativePath;
            input.CanonicalPath = normalized.AbsolutePath;
            if (ReadSnapshot(input.CanonicalPath, input.Snapshot, input.Hash, record.ID, record.ProcessorID, diagnostic))
                return true;
            if (static_cast<uint64>(input.Snapshot.Count()) > request.Limits.MaximumInputBytes - totalBytes)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::ResourceLimitExceeded, AssetPipelineDiagnosticStage::Prepare,
                    record.ID, record.ProcessorID, file, TEXT("Authorized importer input snapshots exceed the input quota."));
            totalBytes += input.Snapshot.Count();
            request.AuthorizedInputs.Add(MoveTemp(input));
        }
        return false;
    }

    AssetDependency ConvertDependency(const AssetImportDependency& source)
    {
        AssetDependency result;
        result.StableIdentity = source.Identity;
        result.ObjectID = source.Object;
        result.Content = source.ExpectedHash;
        result.ExactArtifact = source.ExactArtifact;
        result.Origin.Path = source.Origin;
        switch (source.Kind)
        {
        case AssetImportDependencyKind::SourceFile:
            result.Kind = AssetDependencyKind::SourceFile;
            break;
        case AssetImportDependencyKind::Toolchain:
            result.Kind = AssetDependencyKind::Toolchain;
            break;
        case AssetImportDependencyKind::LogicalPath:
            result.Kind = AssetDependencyKind::LogicalPath;
            break;
        case AssetImportDependencyKind::SourceAsset:
        case AssetImportDependencyKind::ImportedObject:
        case AssetImportDependencyKind::ImportedArtifact:
            result.Kind = AssetDependencyKind::BuildInput;
            break;
        default:
            result.Kind = AssetDependencyKind::Toolchain;
            result.StableIdentity = String::Format(TEXT("{0}:{1}"), static_cast<int32>(source.Kind), source.Identity);
            break;
        }
        return result;
    }

    bool ReconcileMetadata(const AssetImportPlan& plan, const AssetImportJobResult& workerResult, AssetMeta& meta,
                           AssetRecord& record, AssetPipelineDiagnostic& diagnostic)
    {
        Array<SubAssetCandidate> candidates;
        String mainType;
        for (const AssetImportedObjectDeclaration& object : workerResult.Objects)
        {
            if (object.IsMain)
            {
                mainType = object.TypeName;
                continue;
            }
            SubAssetCandidate candidate;
            candidate.StableKey = object.StableIdentifier;
            candidate.TypeName = object.TypeName;
            candidate.DisplayName = object.DisplayName;
            candidates.Add(MoveTemp(candidate));
        }
        if (plan.Importer.ProducesMainObject && mainType.IsEmpty())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Publication,
                record.ID, record.ProcessorID, record.SourcePath.Get(), TEXT("Scripted importer produced no main object type."));
        SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, candidates, true);
        if (reconciliation.RequiresUserReconciliation)
        {
            diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : AssetPipelineDiagnostic();
            return true;
        }
        const bool changed = reconciliation.HasTrackedChanges || (!mainType.IsEmpty() && meta.AssetType != mainType);
        meta.SubAssets = MoveTemp(reconciliation.Resolved);
        if (!mainType.IsEmpty())
            meta.AssetType = mainType;
        if (!changed)
            return false;
        if (AssetMeta::SaveAtomic(record.MetaPath.Get(), meta, diagnostic))
            return true;
        Array<String> refreshPaths;
        refreshPaths.Add(record.SourcePath.Get());
        if (AssetPipelineService::RefreshSources(refreshPaths) || !AssetDatabase::Get().TryGetRecord(record.ID, record))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, AssetPipelineDiagnosticStage::DatabaseScan,
                record.ID, record.ProcessorID, record.SourcePath.Get(),
                TEXT("Scripted importer metadata was reconciled but the parent database refresh failed."));
        return false;
    }
}

bool CallbackImporterPipelineService::OwnsProcessor(const StringView& processorID)
{
    AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
    if (!registry)
        return false;
    AssetImporterLease lease;
    AssetPipelineDiagnostic diagnostic;
    if (registry->TryAcquire(processorID, lease, diagnostic) || !lease.Get().ProcessSafe)
        return false;
    return lease.Get().ProviderKind == AssetProcessorProviderKind::Managed ||
        (lease.Get().ProviderKind == AssetProcessorProviderKind::Native && !lease.Get().WorkerExecutable.IsEmpty());
}

bool CallbackImporterPipelineService::RequestBuild(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic,
                                                    AssetBuildRequestHandle* resultHandle,
                                                    const Guid& refreshId, uint32 pass,
                                                    AssetBuildJobPriority priority)
{
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            assetID, StringView::Empty, StringView::Empty, TEXT("Callback importer requires a registered main asset."));
    if (AssetImportService::EnsureInitialized(diagnostic))
        return true;
    AssetImportPlanner* planner = AssetImportService::GetPlanner();
    AssetImportScheduler* scheduler = AssetImportService::GetScheduler();
    if (!planner || !scheduler)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, AssetPipelineDiagnosticStage::Configuration,
            assetID, record.ProcessorID, record.SourcePath.Get(), TEXT("Callback importer scheduler is not attached to the parent build service."));

    AssetMeta meta;
    if (AssetMeta::Load(record.MetaPath.Get(), meta, diagnostic))
        return true;
    auto execution = std::make_shared<CallbackImportExecution>();
    execution->Target = ArtifactResolver::Get().IsConfigured()
        ? ArtifactResolver::Get().GetDefaultTarget()
        : ArtifactTarget();
    execution->Request.JobID = Guid::New();
    execution->Request.Capability = Guid::New();
    execution->Request.SourceRevision = record.DatabaseRevision;
    AssetPathPolicy::ProjectPath normalizedSource;
    if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
        Globals::ProjectLibraryFolder, record.SourcePath.Get(), normalizedSource, diagnostic))
        return true;
    execution->Request.SourcePath = normalizedSource.ProjectRelativePath;
    if (ReadSnapshot(record.SourcePath.Get(), execution->Request.SourceSnapshot, execution->Request.SourceHash,
        record.ID, record.ProcessorID, diagnostic) ||
        ReadSnapshot(record.MetaPath.Get(), execution->Request.MetaSnapshot, execution->Request.MetaHash,
        record.ID, record.ProcessorID, diagnostic))
        return true;

    AssetImportPlanRequest planRequest;
    planRequest.Asset = AssetGuid(record.ID);
    planRequest.RefreshId = refreshId;
    planRequest.Pass = pass;
    planRequest.SourcePath = record.SourcePath.Get();
    planRequest.ExplicitImporterID = record.ProcessorID;
    planRequest.Reason = force ? TEXT("forced-callback-import") : TEXT("callback-import");
    planRequest.Target = execution->Target;
    planRequest.SourceRevision = record.DatabaseRevision;
    planRequest.SourceHash = execution->Request.SourceHash;
    planRequest.MetadataHash = execution->Request.MetaHash;
    planRequest.Force = force;
    Array<AssetImportPlanRequest> planRequests;
    planRequests.Add(planRequest);
    Array<AssetImportPlan> plans;
    if (planner->Build(planRequests, plans, diagnostic) || plans.Count() != 1)
        return true;
    execution->Plan = plans[0];
    const bool managedWorker = execution->Plan.Importer.ProviderKind == AssetProcessorProviderKind::Managed;
    const bool externalNativeWorker = execution->Plan.Importer.ProviderKind == AssetProcessorProviderKind::Native &&
        !execution->Plan.Importer.WorkerExecutable.IsEmpty();
    if (!execution->Plan.Importer.ProcessSafe || (!managedWorker && !externalNativeWorker))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, AssetPipelineDiagnosticStage::Configuration,
            assetID, record.ProcessorID, record.SourcePath.Get(), TEXT("Callback importer is not configured for mandatory external worker isolation."));
    execution->Request.Limits.MaximumMemoryBytes = execution->Plan.Importer.MaximumMemoryBytes;
    execution->Request.Limits.MaximumOutputBytes = execution->Plan.Importer.MaximumOutputBytes;
    execution->Request.Limits.MaximumOutputFiles = execution->Plan.Importer.MaximumOutputFiles;
    execution->Request.Limits.TimeoutMilliseconds = execution->Plan.Importer.ImportTimeoutMilliseconds;
    const String workersRoot = ArtifactStore::GetTemporaryPath(Globals::ProjectLibraryFolder) / TEXT("CallbackWorkers");
    if ((!FileSystem::DirectoryExists(workersRoot) && FileSystem::CreateDirectory(workersRoot)) || FileSystem::FileExists(workersRoot))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, AssetPipelineDiagnosticStage::Prepare,
            assetID, record.ProcessorID, workersRoot, TEXT("Cannot create callback importer worker staging root."));
    execution->Request.OutputStagingPath = workersRoot / execution->Request.JobID.ToString(Guid::FormatType::N).ToLower();
    if (CollectAuthorizedInputs(record, execution->Request, diagnostic))
        return true;

    AssetImportWorkerPublishAction publish = [execution](const AssetImportPlan& plan, const AssetImportJobResult& workerResult,
        const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& publicationDiagnostic)
    {
        AssetRecord current;
        if (!AssetDatabase::Get().TryGetRecord(plan.Request.Asset.Value, current) ||
            current.DatabaseRevision != plan.Request.SourceRevision)
            return Fail(publicationDiagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Publication,
                plan.Request.Asset.Value, plan.Importer.ID, plan.Request.SourcePath,
                TEXT("Source database revision changed while the scripted importer worker was running."));
        AssetMeta currentMeta;
        if (AssetMeta::Parse(StringAnsi(reinterpret_cast<const char*>(execution->Request.MetaSnapshot.Get()),
            execution->Request.MetaSnapshot.Count()), current.MetaPath.Get(), currentMeta, publicationDiagnostic))
            return true;
        if (ReconcileMetadata(plan, workerResult, currentMeta, current, publicationDiagnostic))
            return true;
        if (workerResult.Outputs.IsEmpty())
            return Fail(publicationDiagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, AssetPipelineDiagnosticStage::Publication,
                current.ID, current.ProcessorID, current.SourcePath.Get(), TEXT("Scripted importer produced no artifact outputs."));

        execution->Prepared = PreparedAsset();
        execution->Prepared.ObjectID = AssetObjectId::Main(AssetGuid(current.ID));
        execution->Prepared.AssetID = current.ID;
        execution->Prepared.OutputType = currentMeta.AssetType;
        execution->Prepared.DatabaseRevision = current.DatabaseRevision;
        execution->Prepared.SettingsHash = ContentHash::Compute(currentMeta.Processor.SettingsJson.Get(), currentMeta.Processor.SettingsJson.Length());
        ArtifactKeyBuilder fingerprintBuilder(StringAnsiView("flax-callback-import-prepared-v1"));
        fingerprintBuilder.AddKey(StringAnsiView("plan"), plan.StaticFingerprint);
        StringAnsi currentMetaJson;
        if (currentMeta.ToJson(currentMetaJson, publicationDiagnostic))
            return true;
        fingerprintBuilder.AddHash(StringAnsiView("reconciled-meta"), ContentHash::Compute(currentMetaJson.Get(), currentMetaJson.Length()));
        execution->PublicationOutputs.Clear();
        for (const AssetImportWorkerOutput& output : workerResult.Outputs)
        {
            for (const DeclaredArtifactOutput& existing : execution->Prepared.Outputs)
            {
                if (existing.Kind == output.Kind)
                    return Fail(publicationDiagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Publication,
                        current.ID, current.ProcessorID, current.SourcePath.Get(),
                        TEXT("Current artifact manifests require one scripted output file per output kind."));
            }
            DeclaredArtifactOutput declared;
            declared.Kind = output.Kind;
            declared.Extension = StringAnsi(".") + StringAnsi(FileSystem::GetExtension(output.RelativePath).ToLower());
            declared.FormatVersion = 1;
            declared.TargetDimensions = output.TargetDimensions;
            declared.CompatibilityTag = StringAnsi("scripted-") + output.Kind;
            declared.EffectiveAssetID = current.ID;
            execution->Prepared.Outputs.Add(declared);
            ArtifactKeyBuilder outputKeyBuilder(StringAnsiView("flax-callback-import-output-v1"));
            outputKeyBuilder.AddKey(StringAnsiView("prepared"), plan.StaticFingerprint);
            outputKeyBuilder.AddString(StringAnsiView("name"), output.Name);
            outputKeyBuilder.AddString(StringAnsiView("kind"), output.Kind);
            outputKeyBuilder.AddHash(StringAnsiView("content"), output.Hash);
            outputKeyBuilder.AddTarget(execution->Target, output.TargetDimensions);
            ArtifactPublicationOutputPlan outputPlan;
            outputPlan.Kind = output.Kind;
            outputPlan.Key = outputKeyBuilder.Finalize();
            execution->PublicationOutputs.Add(outputPlan);
            fingerprintBuilder.AddKey(StringAnsi::Format("output-{0}", output.Kind), outputPlan.Key);
        }
        for (const AssetImportDependency& source : workerResult.Dependencies)
        {
            AssetDependency dependency = ConvertDependency(source);
            if (dependency.Kind == AssetDependencyKind::SourceFile || dependency.Kind == AssetDependencyKind::LogicalPath)
            {
                AssetPathPolicy::ProjectPath normalized;
                if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
                    Globals::ProjectLibraryFolder, dependency.StableIdentity, normalized, publicationDiagnostic))
                    return true;
                dependency.StableIdentity = normalized.ProjectRelativePath;
            }
            execution->Prepared.Dependencies.Add(MoveTemp(dependency));
        }
        bool hasSourceDependency = false;
        for (const AssetDependency& dependency : execution->Prepared.Dependencies)
        {
            if (dependency.Kind == AssetDependencyKind::SourceFile && dependency.Content == execution->Request.SourceHash)
            {
                hasSourceDependency = true;
                break;
            }
        }
        if (!hasSourceDependency)
        {
            AssetPathPolicy::ProjectPath normalizedSource;
            if (AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
                Globals::ProjectLibraryFolder, current.SourcePath.Get(), normalizedSource, publicationDiagnostic))
                return true;
            AssetDependency source;
            source.Kind = AssetDependencyKind::SourceFile;
            source.StableIdentity = normalizedSource.ProjectRelativePath;
            source.Content = execution->Request.SourceHash;
            source.Origin.Path = current.SourcePath.Get();
            execution->Prepared.Dependencies.Add(MoveTemp(source));
        }
        if (AssetDependency::NormalizeAndSort(execution->Prepared.Dependencies, publicationDiagnostic))
            return true;
        for (int32 i = 0; i < execution->Prepared.Dependencies.Count(); i++)
            execution->Prepared.Dependencies[i].AppendKeyComponents(fingerprintBuilder, i);
        execution->Prepared.InputFingerprint = fingerprintBuilder.Finalize();
        {
            CallbackImporterPipelineState& state = State();
            std::lock_guard<std::mutex> lock(state.Locker);
            state.Fingerprints[current.ID] = execution->Prepared.InputFingerprint;
        }

        Array<ArtifactBuildInput> noInputs;
        execution->Context = std::make_unique<ArtifactBuildContext>(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, execution->PublicationJobID, execution->Prepared, noInputs, cancellation,
            plan.Importer.MaximumMemoryBytes, plan.Importer.MaximumOutputBytes, plan.Importer.MaximumOutputFiles, execution->Target);
        if (execution->Context->Initialize(publicationDiagnostic))
            return true;
        for (const AssetImportWorkerOutput& output : workerResult.Outputs)
        {
            const String workerPath = execution->Request.OutputStagingPath / output.RelativePath;
            ArtifactWriter writer;
            if (execution->Context->OpenOutput(output.Kind, writer, publicationDiagnostic) ||
                writer.WriteFileFromPath(output.RelativePath, workerPath, output.Size, output.Hash, publicationDiagnostic))
                return true;
        }
        ArtifactOutputValidator validator = [](const StringView& path, const ArtifactManifestOutput& output,
                                               AssetPipelineDiagnostic& validatorDiagnostic)
        {
            if (output.Size == 0 || output.Size != FileSystem::GetFileSize(path))
            {
                validatorDiagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                validatorDiagnostic.Message = TEXT("Scripted importer artifact output is empty or changed size.");
                return true;
            }
            validatorDiagnostic = AssetPipelineDiagnostic();
            return false;
        };
        for (const DeclaredArtifactOutput& output : execution->Prepared.Outputs)
        {
            if (execution->Validators.Register(output.Kind, execution->Prepared.OutputType, validator, publicationDiagnostic))
                return true;
        }
        ArtifactPublicationRequest publication;
        publication.Target = execution->Target;
        publication.RefreshId = plan.Request.RefreshId;
        publication.Pass = plan.Request.Pass;
        publication.ProcessorID = plan.Importer.ID;
        publication.ProcessorImplementationVersion = plan.Importer.ImporterVersion;
        publication.BuildID = execution->PublicationJobID.ToString(Guid::FormatType::N);
        publication.Outputs = execution->PublicationOutputs;
        publication.QueryCurrentState = [assetID = current.ID](uint64& revision, ArtifactKey& fingerprint)
        {
            QueryCurrentState(assetID, revision, fingerprint);
        };
        publication.Notify = [assetID = current.ID](const ArtifactManifest&)
        {
            AssetPipelineService::NotifyArtifactPublished(assetID);
        };
        ArtifactPublicationResult publicationResult;
        return ArtifactPublisher::Publish(Globals::ProjectLibraryFolder, execution->Prepared, *execution->Context,
            publication, execution->Validators, publicationResult, publicationDiagnostic);
    };

    String workerExecutable = managedWorker ? Platform::GetExecutableFilePath() : execution->Plan.Importer.WorkerExecutable;
    if (FileSystem::IsRelative(workerExecutable))
        workerExecutable = Globals::ProjectFolder / workerExecutable;
    FileSystem::NormalizePath(workerExecutable);
    if (!FileSystem::FileExists(workerExecutable))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Configuration,
            assetID, record.ProcessorID, workerExecutable, TEXT("The isolated importer worker executable does not exist."));
    const AssetBuildRequestHandle handle = scheduler->ScheduleIsolated(execution->Plan, workerExecutable,
        execution->Request, MoveTemp(publish), diagnostic, priority);
    if (!handle.IsValid())
        return diagnostic.Code != AssetPipelineDiagnosticCode::None ||
            Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                assetID, record.ProcessorID, record.SourcePath.Get(), TEXT("Callback importer build request was not accepted."));
    if (resultHandle)
        *resultHandle = handle;
    {
        CallbackImporterPipelineState& state = State();
        std::lock_guard<std::mutex> lock(state.Locker);
        state.Handles[assetID] = handle;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool CallbackImporterPipelineService::RequestBuildAndWait(const Guid& assetID, bool force, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    if (RequestBuild(assetID, force, diagnostic, &handle))
        return true;
    if (!handle.Wait())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            assetID, StringView::Empty, StringView::Empty, TEXT("Callback importer build did not complete."));
    AssetBuildJobResult result;
    if (!handle.TryGetResult(result) || result.Status != AssetBuildJobStatus::Succeeded)
    {
        diagnostic = result.Diagnostic;
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                assetID, StringView::Empty, StringView::Empty, TEXT("Callback importer build did not succeed."));
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

AssetBuildJobStatus CallbackImporterPipelineService::GetStatus(const Guid& assetID, AssetPipelineDiagnostic& diagnostic)
{
    AssetBuildRequestHandle handle;
    {
        CallbackImporterPipelineState& state = State();
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

void CallbackImporterPipelineService::Shutdown()
{
    CallbackImporterPipelineState& state = State();
    std::lock_guard<std::mutex> lock(state.Locker);
    state.Handles.Clear();
    state.Fingerprints.Clear();
}

#endif
