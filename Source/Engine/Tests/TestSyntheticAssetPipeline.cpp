// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Content/Artifacts/ArtifactPublisher.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/ObjectsRemovalService.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class SyntheticPreparedPayload : public PreparedAssetPayload
    {
    public:
        StringAnsi Settings;

        uint64 GetMemoryUsage() const override
        {
            return Settings.Length();
        }
    };

    struct SyntheticExecution
    {
        PreparedAsset Prepared;
        ArtifactTarget Target;
        Array<ArtifactBuildInput> Inputs;
        Array<ArtifactPublicationOutputPlan> OutputPlans;
        Guid ContextJobID = Guid::New();
        std::unique_ptr<ArtifactBuildContext> Context;
    };

    bool WaitForValue(const std::atomic<int32>& value, int32 expected, int32 timeoutMilliseconds = 5000)
    {
        for (int32 i = 0; i < timeoutMilliseconds; i++)
        {
            if (value.load() >= expected)
                return true;
            Platform::Sleep(1);
        }
        return false;
    }

    const ArtifactKeyComponent* FindKeyComponent(const AssetBuildJobRequest& request, const StringAnsiView& name)
    {
        for (const ArtifactKeyComponent& component : request.KeyComponents)
        {
            if (component.Name == name)
                return &component;
        }
        return nullptr;
    }

    class SyntheticPipelineFixture
    {
    private:
        std::mutex _stateMutex;
        uint64 _currentRevision = 0;
        ArtifactKey _currentFingerprint;

        bool ValidateRuntime(const StringView& path, const ArtifactManifestOutput&, AssetPipelineDiagnostic& diagnostic) const
        {
            auto storage = ContentStorageManager::GetStorage(path);
            if (!storage)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                diagnostic.Message = TEXT("Synthetic runtime output is not a Flax storage file.");
                return true;
            }
            Array<FlaxStorage::Entry> entries;
            storage->GetEntries(entries);
            AssetInitData data;
            const bool failed = entries.Count() != 1 || entries[0].ID != AssetID || entries[0].TypeName != RawDataAsset::TypeName ||
                storage->LoadAssetHeader(AssetID, data) || data.SerializedVersion != RawDataAsset::SerializedVersion || !data.Header.Chunks[0];
            storage = nullptr;
            ContentStorageManager::EnsureAccess(path);
            if (failed)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                diagnostic.Message = TEXT("Synthetic runtime output has an invalid GUID, type, version, or payload chunk.");
            }
            return failed;
        }

        static bool ValidateTrace(const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& diagnostic)
        {
            if (!FileSystem::FileExists(path) || output.Size == 0 || FileSystem::GetFileSize(path) != output.Size)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactInvalid;
                diagnostic.Message = TEXT("Synthetic trace output is empty or unreadable.");
                return true;
            }
            return false;
        }

        bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
        {
            Array<byte> source;
            ContentHash sourceHash;
            AssetDependencyOrigin sourceOrigin;
            sourceOrigin.Path = context.GetRecord().SourcePath.Get();
            if (context.ReadSourceFile(context.GetRecord().SourcePath.Get(), source, sourceHash, sourceOrigin, diagnostic))
                return true;

            AssetDependencyOrigin dependencyOrigin;
            dependencyOrigin.Path = context.GetRecord().SourcePath.Get();
            dependencyOrigin.Line = 1;
            AssetSemanticInterface semanticInterface;
            semanticInterface.Version = 1;
            semanticInterface.Hash = ContentHash::Compute("synthetic-interface-v1", 22);
            if (context.DeclareBuildInput(TEXT("synthetic-upstream-runtime"), AssetObjectId::Main(AssetGuid(BuildDependencyID)), BuildDependencyKey, semanticInterface, dependencyOrigin, diagnostic) ||
                context.DeclareRuntimeReference(TEXT("synthetic-runtime-reference"), AssetObjectId::Main(AssetGuid(RuntimeReferenceID)), dependencyOrigin, diagnostic) ||
                context.DeclareToolchain(TEXT("synthetic-toolchain"), ToolchainHash, dependencyOrigin, diagnostic) ||
                context.DeclareOutput(StringAnsiView("runtime"), Guid::Empty, diagnostic) ||
                context.DeclareOutput(StringAnsiView("trace"), Guid::Empty, diagnostic))
                return true;

            auto payload = std::make_shared<SyntheticPreparedPayload>();
            payload->Settings = context.GetSettings();
            prepared.Payload = payload;
            prepared.MemoryEstimate = payload->GetMemoryUsage();
            return false;
        }

        bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
        {
            BuildStarted++;
            while (BlockBuild.load() && !ReleaseBuild.load() && !context.GetCancellation().IsCancellationRequested())
                Platform::Sleep(1);
            if (context.GetCancellation().IsCancellationRequested())
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.AssetGuid = AssetID;
                diagnostic.Message = TEXT("Synthetic processor build was cancelled.");
                return true;
            }

            const PreparedAsset& prepared = context.GetPreparedAsset();
            const AssetDependency* sourceDependency = nullptr;
            for (const AssetDependency& dependency : prepared.Dependencies)
            {
                if (dependency.Kind == AssetDependencyKind::SourceFile)
                {
                    sourceDependency = &dependency;
                    break;
                }
            }
            if (!sourceDependency || !prepared.Payload)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.Message = TEXT("Synthetic prepared payload is incomplete.");
                return true;
            }

            Array<byte> source;
            Array<byte> upstream;
            ContentHash hash;
            if (context.ReadInput(sourceDependency->StableIdentity, source, hash, diagnostic) ||
                context.ReadInput(TEXT("synthetic-upstream-runtime"), upstream, hash, diagnostic))
                return true;
            const auto* payload = static_cast<const SyntheticPreparedPayload*>(prepared.Payload.get());
            Array<byte> result;
            result.Add(source.Get(), source.Count());
            result.Add(static_cast<byte>('\n'));
            result.Add(upstream.Get(), upstream.Count());
            result.Add(static_cast<byte>('\n'));
            result.Add(reinterpret_cast<const byte*>(payload->Settings.Get()), payload->Settings.Length());

            String scratchPath;
            if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
                return true;
            SCOPE_EXIT
            {
                ContentStorageManager::EnsureAccess(scratchPath);
                FileSystem::DeleteFile(scratchPath);
            };
            FlaxChunk chunk;
            chunk.Data.Copy(result.Get(), result.Count());
            AssetInitData data;
            data.Header.ID = prepared.AssetID;
            data.Header.TypeName = RawDataAsset::TypeName;
            data.Header.Chunks[0] = &chunk;
            data.SerializedVersion = RawDataAsset::SerializedVersion;
            if (FlaxStorage::Create(scratchPath, data, true))
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.Message = TEXT("Synthetic processor could not create its staged Flax asset.");
                return true;
            }
            Array<byte> runtimeBytes;
            if (File::ReadAllBytes(scratchPath, runtimeBytes))
                return true;
            ArtifactWriter runtimeWriter;
            ArtifactWriter traceWriter;
            if (context.OpenOutput(StringAnsiView("runtime"), runtimeWriter, diagnostic) ||
                runtimeWriter.WriteFile(TEXT("asset.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic) ||
                context.OpenOutput(StringAnsiView("trace"), traceWriter, diagnostic) ||
                traceWriter.WriteFile(TEXT("trace.bin"), result.Get(), result.Count(), diagnostic))
                return true;
            BuildCompleted++;
            return false;
        }

    public:
        String Root;
        String ContentRoot;
        String LibraryRoot;
        String SourcePath;
        String BuildInputPath;
        Guid AssetID = Guid::New();
        Guid BuildDependencyID = Guid::New();
        Guid RuntimeReferenceID = Guid::New();
        ArtifactKey BuildDependencyKey = ArtifactKey(ContentHash::Compute("synthetic-upstream-key", 22));
        ContentHash ToolchainHash = ContentHash::Compute("synthetic-toolchain-v1", 22);
        StringAnsi Settings = "{\"prefix\":\"first\"}\n";
        ArtifactTarget Target;
        SourceHashCache HashCache;
        AssetProcessorRegistry Registry;
        AssetProcessorRegistration Registration;
        ArtifactOutputValidatorRegistry Validators;
        AssetDatabase* Database = nullptr;
        std::atomic<bool> BlockBuild { false };
        std::atomic<bool> ReleaseBuild { false };
        std::atomic<int32> BuildStarted { 0 };
        std::atomic<int32> BuildCompleted { 0 };
        std::atomic<int32> Publications { 0 };
        std::atomic<int32> Superseded { 0 };

        explicit SyntheticPipelineFixture(AssetDatabase& database)
            : Database(&database)
        {
            Root = Globals::TemporaryFolder / (TEXT("SyntheticAssetPipeline-") + Guid::New().ToString(Guid::FormatType::N));
            ContentRoot = Root / TEXT("Content");
            LibraryRoot = Root / TEXT("Library");
            SourcePath = ContentRoot / TEXT("fixture.syntheticpipeline");
            BuildInputPath = LibraryRoot / TEXT("Seed/upstream.bin");
            Target.Platform = "Windows";
            Target.Architecture = "x64";
            Target.Configuration = "Development";
            Target.Quality = "High";
            Target.Role = "Editor";
        }

        bool Initialize(AssetPipelineDiagnostic& diagnostic)
        {
            if (FileSystem::CreateDirectory(ContentRoot) || FileSystem::CreateDirectory(LibraryRoot) || EnsureBuildInput() ||
                File::WriteAllText(SourcePath, TEXT("synthetic-source-one"), Encoding::ANSI) || ArtifactStore::EnsureLayout(LibraryRoot, diagnostic))
                return true;

            AssetProcessorDescriptor descriptor;
            descriptor.ID = TEXT("tests.synthetic-pipeline");
            descriptor.ProviderID = TEXT("tests");
            descriptor.SourceExtensions.Add(TEXT(".syntheticpipeline"));
            descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
            descriptor.MainOutputType = RawDataAsset::TypeName;
            descriptor.SettingsSchemaVersion = 1;
            descriptor.ImplementationVersion = 1;
            descriptor.InterfaceVersion = 1;
            descriptor.NormalizedDefaultSettings = "{\"prefix\":\"default\"}\n";
            AssetProcessorOutputDescriptor runtime;
            runtime.Kind = "runtime";
            runtime.Extension = ".flax";
            runtime.FormatVersion = RawDataAsset::SerializedVersion;
            runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Role;
            runtime.CompatibilityTag = "synthetic-runtime-v1";
            descriptor.Outputs.Add(runtime);
            AssetProcessorOutputDescriptor trace;
            trace.Kind = "trace";
            trace.Extension = ".bin";
            trace.FormatVersion = 1;
            trace.TargetDimensions = ArtifactTargetDimension::Architecture;
            trace.CompatibilityTag = "synthetic-trace-v1";
            descriptor.Outputs.Add(trace);
            descriptor.Prepare = [this](PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& callbackDiagnostic)
            {
                return Prepare(context, prepared, callbackDiagnostic);
            };
            descriptor.Build = [this](ArtifactBuildContext& context, AssetPipelineDiagnostic& callbackDiagnostic)
            {
                return Build(context, callbackDiagnostic);
            };
            if (Registry.Register(MoveTemp(descriptor), Registration, diagnostic) ||
                Validators.Register(StringAnsiView("runtime"), RawDataAsset::TypeName,
                    [this](const StringView& path, const ArtifactManifestOutput& output, AssetPipelineDiagnostic& validatorDiagnostic)
                    {
                        return ValidateRuntime(path, output, validatorDiagnostic);
                    }, diagnostic) ||
                Validators.Register(StringAnsiView("trace"), StringView::Empty, &ValidateTrace, diagnostic))
                return true;
            return false;
        }

        bool EnsureBuildInput()
        {
            const String directory = StringUtils::GetDirectoryName(BuildInputPath);
            static const byte Bytes[] = { 'u', 'p', 's', 't', 'r', 'e', 'a', 'm' };
            return (!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory)) ||
                File::WriteAllBytes(BuildInputPath, Bytes, ARRAY_COUNT(Bytes));
        }

        AssetRecord MakeRecord() const
        {
            AssetRecord record;
            record.ID = AssetID;
            record.SourceAssetID = AssetID;
            record.TypeName = RawDataAsset::TypeName;
            record.CanonicalPath = CanonicalAssetPath(SourcePath);
            record.SourcePath = SourceFilePath(SourcePath);
            record.ProcessorID = TEXT("tests.synthetic-pipeline");
            record.SourceKind = AssetSourceKind::ImportedSource;
            record.Status = AssetRecordStatus::Ready;
            record.BuildInputDependencies.Add(AssetObjectId::Main(AssetGuid(BuildDependencyID)));
            record.RuntimeReferences.Add(AssetObjectId::Main(AssetGuid(RuntimeReferenceID)));
            return record;
        }

        void QueryCurrentState(uint64& revision, ArtifactKey& fingerprint)
        {
            AssetRecord record;
            if (!Database->TryGetRecord(AssetID, record))
            {
                revision = 0;
                fingerprint = ArtifactKey();
                return;
            }
            std::lock_guard<std::mutex> lock(_stateMutex);
            revision = record.DatabaseRevision;
            fingerprint = _currentRevision == revision ? _currentFingerprint : ArtifactKey();
        }

        Array<byte> ExpectedPayload() const
        {
            Array<byte> source;
            File::ReadAllBytes(SourcePath, source);
            static const byte Upstream[] = { 'u', 'p', 's', 't', 'r', 'e', 'a', 'm' };
            Array<byte> result;
            result.Add(source.Get(), source.Count());
            result.Add(static_cast<byte>('\n'));
            result.Add(Upstream, ARRAY_COUNT(Upstream));
            result.Add(static_cast<byte>('\n'));
            result.Add(reinterpret_cast<const byte*>(Settings.Get()), Settings.Length());
            return result;
        }

        bool CreatePlan(const AssetRecord& record, const ArtifactRequest& request, ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic);
    };

    bool SyntheticPipelineFixture::CreatePlan(const AssetRecord& record, const ArtifactRequest& request,
        ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& diagnostic)
    {
        plan = ArtifactResolutionPlan();
        AssetProcessorLease prepareLease;
        if (Registry.TryAcquire(record.ProcessorID, AssetProcessorInvocationStage::Prepare, prepareLease, diagnostic))
            return true;
        AssetCancellationSource prepareCancellation;
        PreparedAsset prepared;
        PrepareAssetContext prepareContext(Root, ContentRoot, LibraryRoot, record, prepareLease.Get(), Settings,
            HashCache, prepareCancellation.GetToken());
        if (prepareLease.Get().Prepare(prepareContext, prepared, diagnostic) ||
            prepareContext.Finalize(record.DatabaseRevision, prepared, diagnostic))
            return true;

        Array<byte> buildInputBytes;
        if (File::ReadAllBytes(BuildInputPath, buildInputBytes))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.SourcePath = BuildInputPath;
            diagnostic.Message = TEXT("Synthetic upstream build input is missing.");
            return true;
        }

        auto execution = std::make_shared<SyntheticExecution>();
        execution->Prepared = prepared;
        execution->Target = request.Target;
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (dependency.Kind == AssetDependencyKind::SourceFile)
            {
                ArtifactBuildInput input;
                input.StableIdentity = dependency.StableIdentity;
                input.Path = SourcePath;
                input.ExpectedContent = dependency.Content;
                execution->Inputs.Add(MoveTemp(input));
            }
            else if (dependency.Kind == AssetDependencyKind::BuildInput)
            {
                ArtifactBuildInput input;
                input.StableIdentity = dependency.StableIdentity;
                input.Path = BuildInputPath;
                input.ExpectedContent = ContentHash::Compute(buildInputBytes.Get(), buildInputBytes.Count());
                execution->Inputs.Add(MoveTemp(input));
            }
        }

        ArtifactKeyBuilder jobBuilder(StringAnsiView("synthetic-build-job-v1"));
        jobBuilder.AddGuid(StringAnsiView("asset"), prepared.AssetID);
        jobBuilder.AddUInt64(StringAnsiView("database-revision"), prepared.DatabaseRevision);
        jobBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
        jobBuilder.AddKey(StringAnsiView("manifest-target"), request.Target.BuildKey(ArtifactTargetDimension::All));
        for (const DeclaredArtifactOutput& output : prepared.Outputs)
        {
            const AssetProcessorOutputDescriptor* descriptorOutput = nullptr;
            for (const AssetProcessorOutputDescriptor& candidate : prepareLease.Get().Outputs)
            {
                if (candidate.Kind == output.Kind)
                {
                    descriptorOutput = &candidate;
                    break;
                }
            }
            if (!descriptorOutput)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.OutputKind = String(output.Kind);
                diagnostic.Message = TEXT("Synthetic output descriptor disappeared during key construction.");
                return true;
            }
            ArtifactKeyBuilder outputBuilder(StringAnsiView("synthetic-output-v1"));
            prepareLease.Get().AppendVersionKey(outputBuilder, *descriptorOutput);
            outputBuilder.AddKey(StringAnsiView("prepared-input"), prepared.InputFingerprint);
            outputBuilder.AddTarget(request.Target, output.TargetDimensions);
            outputBuilder.AddGuid(StringAnsiView("effective-asset"), output.EffectiveAssetID);
            ArtifactPublicationOutputPlan outputPlan;
            outputPlan.Kind = output.Kind;
            outputPlan.Key = outputBuilder.Finalize();
            execution->OutputPlans.Add(outputPlan);
            jobBuilder.AddKey(StringAnsi::Format("output-{0}", output.Kind), outputPlan.Key);
        }

        plan.CurrentInputFingerprint = prepared.InputFingerprint;
        plan.BuildRequest.Key.ExactPlan = jobBuilder.Finalize();
        plan.BuildRequest.KeyComponents = jobBuilder.GetComponents();
        plan.BuildRequest.AssetID = prepared.AssetID;
        plan.BuildRequest.ProcessorClass = TEXT("synthetic-pipeline");
        plan.BuildRequest.ProcessorID = record.ProcessorID;
        plan.BuildRequest.Target = String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString());
        plan.BuildRequest.MemoryBytes = Math::Max<uint64>(1, prepared.MemoryEstimate);
        plan.BuildRequest.ProcessorConcurrencyLimit = 4;
        plan.BuildRequest.RebuildReason = TEXT("synthetic input fingerprint changed");
        for (const DeclaredArtifactOutput& output : prepared.Outputs)
            plan.BuildRequest.OutputKinds.Add(output.Kind);
        plan.BuildRequest.Build = [this, execution](const AssetCancellationToken& cancellation, AssetPipelineDiagnostic& buildDiagnostic)
        {
            execution->Context = std::make_unique<ArtifactBuildContext>(Root, ContentRoot, LibraryRoot,
                execution->ContextJobID, execution->Prepared, execution->Inputs, cancellation, execution->Target);
            if (execution->Context->Initialize(buildDiagnostic))
                return true;
            AssetProcessorLease buildLease;
            if (Registry.TryAcquire(TEXT("tests.synthetic-pipeline"), AssetProcessorInvocationStage::Build, buildLease, buildDiagnostic))
            {
                execution->Context->Cancel();
                return true;
            }
            const bool failed = buildLease.Get().Build(*execution->Context, buildDiagnostic);
            if (failed)
                execution->Context->Cancel();
            return failed;
        };
        plan.BuildRequest.Publish = [this, execution](const AssetCancellationToken&, AssetPipelineDiagnostic& publicationDiagnostic)
        {
            if (!execution->Context)
            {
                publicationDiagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                publicationDiagnostic.Stage = AssetPipelineDiagnosticStage::Publication;
                publicationDiagnostic.Message = TEXT("Synthetic build produced no publication context.");
                return true;
            }
            ArtifactPublicationRequest publication;
            publication.Target = execution->Target;
            publication.ProcessorID = TEXT("tests.synthetic-pipeline");
            publication.ProcessorImplementationVersion = 1;
            publication.BuildID = execution->ContextJobID.ToString(Guid::FormatType::N);
            publication.BuiltAtUtc = TEXT("2026-08-20T00:00:00Z");
            publication.Outputs = execution->OutputPlans;
            publication.QueryCurrentState = [this](uint64& revision, ArtifactKey& fingerprint)
            {
                QueryCurrentState(revision, fingerprint);
            };
            publication.Notify = [this](const ArtifactManifest&)
            {
                Publications++;
            };
            ArtifactPublicationResult result;
            const bool failed = ArtifactPublisher::Publish(LibraryRoot, execution->Prepared, *execution->Context,
                publication, Validators, result, publicationDiagnostic);
            if (result.WasSuperseded)
                Superseded++;
            return failed;
        };

        {
            std::lock_guard<std::mutex> lock(_stateMutex);
            _currentRevision = prepared.DatabaseRevision;
            _currentFingerprint = prepared.InputFingerprint;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
}

TEST_CASE("AssetPipeline.Artifacts synthetic processor covers end-to-end state transitions")
{
    AssetDatabase& database = AssetDatabase::Get();
    const AssetDatabaseSnapshot savedDatabase = database.GetSnapshot();
    SyntheticPipelineFixture fixture(database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(fixture.Initialize(diagnostic));
    std::unique_ptr<AssetBuildService> service = std::make_unique<AssetBuildService>();
    std::unique_ptr<AssetBuildService> secondService;
    RawDataAsset* loadedAsset = nullptr;
    SCOPE_EXIT
    {
        if (loadedAsset)
        {
            Content::UnloadAsset(loadedAsset);
            loadedAsset = nullptr;
            ObjectsRemovalService::Flush();
        }
        ArtifactResolver::Get().Reset();
        if (secondService)
            secondService->Shutdown();
        if (service)
            service->Shutdown();
        fixture.Registration.Reset();
        database.PublishFullSnapshot(savedDatabase.Records, diagnostic);
        ContentStorageManager::EnsureAccess(fixture.LibraryRoot);
        FileSystem::DeleteDirectory(fixture.Root, true);
    };

    Array<AssetRecord> records;
    records.Add(fixture.MakeRecord());
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    AssetRecord record;
    REQUIRE(database.TryGetRecord(fixture.AssetID, record));

    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 2;
    limits.MaximumMemoryBytes = 1024 * 1024;
    limits.MaximumExternalTools = 1;
    REQUIRE_FALSE(service->Initialize(fixture.LibraryRoot, limits, diagnostic));
    ArtifactResolutionPlanProvider provider = [&fixture](const AssetRecord& plannedRecord, const ArtifactRequest& request,
        ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& planDiagnostic)
    {
        return fixture.CreatePlan(plannedRecord, request, plan, planDiagnostic);
    };
    ArtifactResolver::Get().Configure(database, *service, fixture.LibraryRoot, fixture.Target, provider);
    ArtifactResolver policyResolver;
    policyResolver.Configure(database, *service, fixture.LibraryRoot, fixture.Target, provider);

    // A real Content load drives prepare, build, publication, resolution, factory creation, and binary loading.
    const Array<byte> firstExpected = fixture.ExpectedPayload();
    loadedAsset = Content::LoadRuntimeObject<RawDataAsset>(fixture.AssetID);
    REQUIRE(loadedAsset);
    REQUIRE(loadedAsset->Data.Count() == firstExpected.Count());
    CHECK(Platform::MemoryCompare(loadedAsset->Data.Get(), firstExpected.Get(), firstExpected.Count()) == 0);
    CHECK(loadedAsset->GetPath() == fixture.SourcePath);
    CHECK(loadedAsset->GetStoragePath() != fixture.SourcePath);
    CHECK(loadedAsset->IsUsingGeneratedArtifact());
    CHECK(loadedAsset->IsUsingExactArtifact());
    const String firstStoragePath = loadedAsset->GetStoragePath();

    ArtifactStoragePath manifestPath;
    REQUIRE_FALSE(ArtifactStore::TryGetManifestPath(fixture.LibraryRoot, fixture.Target, fixture.AssetID, manifestPath, diagnostic));
    StringAnsi manifestJson;
    ArtifactManifest manifest;
    REQUIRE_FALSE(File::ReadAllText(manifestPath.Get(), manifestJson));
    REQUIRE_FALSE(ArtifactManifest::Parse(manifestJson, manifestPath.Get(), manifest, diagnostic));
    CHECK(manifest.Outputs.Count() == 2);
    CHECK(manifest.Dependencies.Count() == 4);

    // Output keys honor their declared target masks.
    ArtifactRequest baseRequest;
    baseRequest.AssetID = fixture.AssetID;
    baseRequest.Target = fixture.Target;
    baseRequest.OutputKind = "runtime";
    baseRequest.RequiredCompatibility = "synthetic-runtime-v1";
    baseRequest.Policy = ArtifactResolvePolicy::Exact;
    ArtifactResolutionPlan basePlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, basePlan, diagnostic));
    ArtifactRequest architectureRequest = baseRequest;
    architectureRequest.Target.Architecture = "arm64";
    ArtifactResolutionPlan architecturePlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, architectureRequest, architecturePlan, diagnostic));
    ArtifactRequest qualityRequest = baseRequest;
    qualityRequest.Target.Quality = "Low";
    ArtifactResolutionPlan qualityPlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, qualityRequest, qualityPlan, diagnostic));
    const ArtifactKeyComponent* baseRuntime = FindKeyComponent(basePlan.BuildRequest, StringAnsiView("output-runtime"));
    const ArtifactKeyComponent* baseTrace = FindKeyComponent(basePlan.BuildRequest, StringAnsiView("output-trace"));
    const ArtifactKeyComponent* architectureRuntime = FindKeyComponent(architecturePlan.BuildRequest, StringAnsiView("output-runtime"));
    const ArtifactKeyComponent* architectureTrace = FindKeyComponent(architecturePlan.BuildRequest, StringAnsiView("output-trace"));
    const ArtifactKeyComponent* qualityRuntime = FindKeyComponent(qualityPlan.BuildRequest, StringAnsiView("output-runtime"));
    const ArtifactKeyComponent* qualityTrace = FindKeyComponent(qualityPlan.BuildRequest, StringAnsiView("output-trace"));
    REQUIRE((baseRuntime && baseTrace && architectureRuntime && architectureTrace && qualityRuntime && qualityTrace));
    CHECK(baseRuntime->Value == architectureRuntime->Value);
    CHECK(baseTrace->Value != architectureTrace->Value);
    CHECK(baseRuntime->Value == qualityRuntime->Value);
    CHECK(baseTrace->Value == qualityTrace->Value);

    // A temporarily unbuildable database state exposes compatible last-good data, then converges to exact data.
    records[0] = fixture.MakeRecord();
    records[0].Status = AssetRecordStatus::MissingSource;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    REQUIRE(database.TryGetRecord(fixture.AssetID, record));
    fixture.Settings = "{\"prefix\":\"interactive-last-good\"}\n";
    ArtifactRequest resolveRequest = baseRequest;
    resolveRequest.Policy = ArtifactResolvePolicy::Interactive;
    ArtifactResolutionPlan lastGoodPlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, resolveRequest, lastGoodPlan, diagnostic));
    REQUIRE(lastGoodPlan.CurrentInputFingerprint != manifest.InputFingerprint);
    ResolvedArtifact resolved;
    REQUIRE_FALSE(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(resolved.IsLastGood);
    CHECK_FALSE(resolved.IsExact);
    records[0] = fixture.MakeRecord();
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    REQUIRE(database.TryGetRecord(fixture.AssetID, record));
    resolveRequest.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(resolved.IsExact);

    // Source changes also converge to a distinct exact output.
    REQUIRE_FALSE(File::WriteAllText(fixture.SourcePath, TEXT("synthetic-source-two-with-a-different-size"), Encoding::ANSI));
    records[0] = fixture.MakeRecord();
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    REQUIRE(database.TryGetRecord(fixture.AssetID, record));
    resolveRequest.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(resolved.IsExact);
    CHECK(resolved.StoragePath.Get() != firstStoragePath);

    fixture.Settings = "{\"prefix\":\"settings-changed\"}\n";
    resolveRequest.Policy = ArtifactResolvePolicy::NoBuild;
    CHECK(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactRebuildRequired);
    resolveRequest.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(resolved.IsExact);

    // Two schedulers publish the same deterministic exact plan safely; each scheduler also deduplicates its requesters.
    fixture.Settings = "{\"prefix\":\"concurrent\"}\n";
    secondService = std::make_unique<AssetBuildService>();
    REQUIRE_FALSE(secondService->Initialize(fixture.LibraryRoot, limits, diagnostic));
    ArtifactResolutionPlan concurrentPlanA;
    ArtifactResolutionPlan concurrentPlanB;
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, concurrentPlanA, diagnostic));
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, concurrentPlanB, diagnostic));
    const int32 buildsBeforeConcurrency = fixture.BuildCompleted.load();
    const AssetBuildRequestHandle concurrentA = service->Request(concurrentPlanA.BuildRequest);
    const AssetBuildRequestHandle concurrentADuplicate = service->Request(concurrentPlanA.BuildRequest);
    const AssetBuildRequestHandle concurrentB = secondService->Request(concurrentPlanB.BuildRequest);
    REQUIRE(concurrentA.Wait(10000));
    REQUIRE(concurrentADuplicate.Wait(10000));
    REQUIRE(concurrentB.Wait(10000));
    CHECK(concurrentA.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(concurrentADuplicate.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(concurrentB.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(fixture.BuildCompleted.load() == buildsBeforeConcurrency + 2);
    CHECK(service->GetMetrics().DeduplicationHits >= 1);
    secondService->Shutdown();
    secondService.reset();

    // Final-requester cancellation prevents publication and cleans its job staging.
    fixture.Settings = "{\"prefix\":\"cancelled\"}\n";
    fixture.BlockBuild.store(true);
    fixture.ReleaseBuild.store(false);
    ArtifactResolutionPlan cancelledPlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, cancelledPlan, diagnostic));
    const int32 cancellationStart = fixture.BuildStarted.load() + 1;
    const AssetBuildRequestHandle cancelled = service->Request(cancelledPlan.BuildRequest);
    REQUIRE(WaitForValue(fixture.BuildStarted, cancellationStart));
    service->CancelRequester(cancelled);
    fixture.ReleaseBuild.store(true);
    REQUIRE(cancelled.Wait(10000));
    CHECK(cancelled.GetStatus() == AssetBuildJobStatus::Cancelled);
    fixture.BlockBuild.store(false);

    // A newer prepared fingerprint supersedes an in-flight build before its manifest selection point.
    fixture.Settings = "{\"prefix\":\"superseded\"}\n";
    fixture.BlockBuild.store(true);
    fixture.ReleaseBuild.store(false);
    ArtifactResolutionPlan supersededPlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, supersededPlan, diagnostic));
    const int32 supersededStart = fixture.BuildStarted.load() + 1;
    const int32 supersededBefore = fixture.Superseded.load();
    const AssetBuildRequestHandle superseded = service->Request(supersededPlan.BuildRequest);
    REQUIRE(WaitForValue(fixture.BuildStarted, supersededStart));
    fixture.Settings = "{\"prefix\":\"winner\"}\n";
    ArtifactResolutionPlan winningPlan;
    REQUIRE_FALSE(fixture.CreatePlan(record, baseRequest, winningPlan, diagnostic));
    fixture.ReleaseBuild.store(true);
    REQUIRE(superseded.Wait(10000));
    CHECK(superseded.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(fixture.Superseded.load() == supersededBefore + 1);
    fixture.BlockBuild.store(false);
    resolveRequest.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(policyResolver.Resolve(resolveRequest, resolved, diagnostic));
    CHECK(resolved.IsExact);

    // Remove every generated artifact, restart the service, and regenerate through Content without writing to Content.
    Content::UnloadAsset(loadedAsset);
    loadedAsset = nullptr;
    ObjectsRemovalService::Flush();
    ContentStorageManager::EnsureAccess(firstStoragePath);
    ArtifactResolver::Get().Reset();
    service->Shutdown();
    service.reset();
    CHECK_FALSE(ArtifactLease::HasLeaseWithin(fixture.LibraryRoot));
    Array<FlaxStorageReference> lockedStorages;
    REQUIRE_FALSE(ContentStorageManager::LockFolderAccess(fixture.LibraryRoot, lockedStorages));
    REQUIRE_FALSE(FileSystem::DeleteDirectory(fixture.LibraryRoot, true));
    ContentStorageManager::UnlockFolderAccess(lockedStorages);
    CHECK_FALSE(FileSystem::DirectoryExists(fixture.LibraryRoot));
    REQUIRE_FALSE(FileSystem::CreateDirectory(fixture.LibraryRoot));
    REQUIRE_FALSE(fixture.EnsureBuildInput());
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(fixture.LibraryRoot, diagnostic));
    service = std::make_unique<AssetBuildService>();
    REQUIRE_FALSE(service->Initialize(fixture.LibraryRoot, limits, diagnostic));
    ArtifactResolver::Get().Configure(database, *service, fixture.LibraryRoot, fixture.Target, provider);

    const int32 regenerationBuilds = fixture.BuildCompleted.load();
    loadedAsset = Content::LoadRuntimeObject<RawDataAsset>(fixture.AssetID);
    REQUIRE(loadedAsset);
    const Array<byte> regeneratedExpected = fixture.ExpectedPayload();
    REQUIRE(loadedAsset->Data.Count() == regeneratedExpected.Count());
    CHECK(Platform::MemoryCompare(loadedAsset->Data.Get(), regeneratedExpected.Get(), regeneratedExpected.Count()) == 0);
    CHECK(fixture.BuildCompleted.load() == regenerationBuilds + 1);
    CHECK(loadedAsset->GetStoragePath() != fixture.SourcePath);
    Array<String> contentFiles;
    REQUIRE_FALSE(FileSystem::DirectoryGetFiles(contentFiles, fixture.ContentRoot, TEXT("*"), DirectorySearchOption::AllDirectories));
    REQUIRE(contentFiles.Count() == 1);
    CHECK(contentFiles[0] == fixture.SourcePath);
}

#endif
