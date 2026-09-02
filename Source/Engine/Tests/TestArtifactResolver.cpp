// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <atomic>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    Guid LegacyCompositeGuidForTest(const AssetObjectId& object)
    {
        if (object.IsMainObject())
            return object.Asset.Value;
        const uint64 local = static_cast<uint64>(object.LocalId);
        const uint32 low = static_cast<uint32>(local);
        const uint32 high = static_cast<uint32>(local >> 32);
        return Guid(object.Asset.Value.A ^ low, object.Asset.Value.B ^ high,
                    object.Asset.Value.C ^ ((low << 13) | (low >> 19)),
                    object.Asset.Value.D ^ ((high << 7) | (high >> 25)) ^ 0x9e3779b9u);
    }

    ArtifactTarget ResolverTarget()
    {
        ArtifactTarget target;
        target.Platform = "Windows";
        target.Architecture = "x64";
        target.Role = "Editor";
        return target;
    }

    ArtifactKey ResolverKey(const char* value)
    {
        return ArtifactKey(ContentHash::Compute(value, StringUtils::Length(value)));
    }

    bool WriteResolvedManifest(const String& library, const AssetRecord& record, const ArtifactRequest& request,
        const ArtifactKey& fingerprint, const StringAnsiView& compatibility, AssetPipelineDiagnostic& diagnostic)
    {
        const byte bytes[] = { 11, 22, 33, 44 };
        ArtifactKeyBuilder keyBuilder(StringAnsiView("resolver-output-v1"));
        keyBuilder.AddKey(StringAnsiView("input"), fingerprint);
        const ArtifactKey outputKey = keyBuilder.Finalize();
        ArtifactStoragePath outputPath;
        const AssetObjectId object = AssetObjectId::Main(AssetGuid(record.ID));
        if (ArtifactStore::TryGetArtifactPath(library, request.Target, ArtifactTargetDimension::All, record.ID, request.OutputKind,
            outputKey, StringAnsiView(".bin"), outputPath, diagnostic))
            return true;
        const String directory = StringUtils::GetDirectoryName(outputPath.Get());
        if ((!FileSystem::DirectoryExists(directory) && FileSystem::CreateDirectory(directory)) || File::WriteAllBytes(outputPath.Get(), bytes, ARRAY_COUNT(bytes)))
            return true;
        String relative;
        if (ArtifactStore::TryMakeLibraryRelative(library, outputPath.Get(), relative, diagnostic))
            return true;
        ArtifactManifest manifest;
        manifest.ObjectID = object;
        manifest.DatabaseRevision = record.DatabaseRevision;
        manifest.ProcessorID = record.ProcessorID;
        manifest.ProcessorImplementationVersion = 1;
        manifest.Target = request.Target;
        manifest.InputFingerprint = fingerprint;
        manifest.SourceHash = ContentHash::Compute("source", 6);
        manifest.SettingsHash = ContentHash::Compute("settings", 8);
        manifest.BuildID = Guid::New().ToString(Guid::FormatType::N);
        ArtifactManifestOutput output;
        output.Kind = request.OutputKind;
        output.Key = outputKey;
        output.RelativePath = relative;
        output.Content = ContentHash::Compute(bytes, ARRAY_COUNT(bytes));
        output.Size = ARRAY_COUNT(bytes);
        output.Compatibility = compatibility;
        manifest.Outputs.Add(output);
        StringAnsi json;
        if (manifest.ToJson(json, diagnostic))
            return true;
        ArtifactStoragePath manifestPath;
        if (ArtifactStore::TryGetManifestPath(library, request.Target, record.ID, manifestPath, diagnostic))
            return true;
        const String manifestDirectory = StringUtils::GetDirectoryName(manifestPath.Get());
        return (!FileSystem::DirectoryExists(manifestDirectory) && FileSystem::CreateDirectory(manifestDirectory)) ||
            File::WriteAllBytes(manifestPath.Get(), json.Get(), json.Length());
    }

    bool WaitForCount(const std::atomic<int32>& value, int32 expected)
    {
        for (int32 i = 0; i < 5000; i++)
        {
            if (value.load() == expected)
                return true;
            Platform::Sleep(1);
        }
        return false;
    }

    AssetObjectId MakeRuntimeGuidCollision(const Guid& runtimeId, int64 localId)
    {
        const uint64 local = static_cast<uint64>(localId);
        const uint32 low = static_cast<uint32>(local);
        const uint32 high = static_cast<uint32>(local >> 32);
        const Guid source(runtimeId.A ^ low, runtimeId.B ^ high,
            runtimeId.C ^ ((low << 13) | (low >> 19)),
            runtimeId.D ^ ((high << 7) | (high >> 25)) ^ 0x9e3779b9u);
        return AssetObjectId(AssetGuid(source), localId);
    }
}

#if COMPILE_WITH_MODEL_TOOL && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
TEST_CASE("Model hot swap retains authored package object identity")
{
    AssetRecord main;
    main.ID = Guid::New();
    main.SourceAssetID = main.ID;
    main.LocalId = 1;
    CHECK(ModelPipelineService::GetHotSwapStorageObjectForTesting(main) == AssetObjectId::Main(AssetGuid(main.ID)));

    AssetRecord child;
    child.ID = Guid::New();
    child.SourceAssetID = main.ID;
    child.LocalId = 771;
    const AssetObjectId storageObject = ModelPipelineService::GetHotSwapStorageObjectForTesting(child);
    CHECK(storageObject == AssetObjectId(AssetGuid(main.ID), 771));
    CHECK(storageObject != AssetObjectId::Main(AssetGuid(child.ID)));
}
#endif

TEST_CASE("ArtifactResolver enforces exact interactive and no-build policy without unsafe fallback")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactResolver-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String sourcePath = content / TEXT("resolver.source");
    REQUIRE_FALSE(File::WriteAllText(sourcePath, TEXT("source"), Encoding::ANSI));

    AssetDatabase database;
    AssetRecord input;
    input.ID = Guid::New();
    input.SourceAssetID = input.ID;
    input.TypeName = TEXT("FlaxEngine.RawDataAsset");
    input.CanonicalPath = CanonicalAssetPath(sourcePath);
    input.SourcePath = SourceFilePath(sourcePath);
    input.ProcessorID = TEXT("test.resolver");
    input.SourceKind = AssetSourceKind::ImportedSource;
    input.Status = AssetRecordStatus::Ready;
    Array<AssetRecord> records;
    records.Add(input);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    AssetRecord record;
    REQUIRE(database.TryGetRecord(input.ID, record));

    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));
    const ArtifactTarget target = ResolverTarget();
    ArtifactRequest baseRequest;
    baseRequest.Object = AssetObjectId(AssetGuid(record.SourceAssetID), record.LocalId);
    baseRequest.Target = target;
    baseRequest.OutputKind = "Runtime";
    baseRequest.RequiredCompatibility = "runtime-v1";
    const ArtifactKey oldFingerprint = ResolverKey("old-input");
    ArtifactKey currentFingerprint = ResolverKey("current-input");
    REQUIRE_FALSE(WriteResolvedManifest(library, record, baseRequest, oldFingerprint, StringAnsiView("runtime-v1"), diagnostic));

    std::atomic<int32> builds { 0 };
    std::atomic<int32> publications { 0 };
    std::atomic<int32> plans { 0 };
    ArtifactResolver resolver;
    ArtifactResolutionPlanProvider provider = [&](const AssetRecord& plannedRecord, const ArtifactRequest& request,
        ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& planDiagnostic)
    {
        plans++;
        plan.CurrentInputFingerprint = currentFingerprint;
        ArtifactKeyBuilder builder(StringAnsiView("resolver-job-v1"));
        builder.AddGuid(StringAnsiView("asset"), plannedRecord.ID);
        builder.AddUInt64(StringAnsiView("revision"), plannedRecord.DatabaseRevision);
        builder.AddKey(StringAnsiView("input"), currentFingerprint);
        plan.BuildRequest.Key.ExactPlan = builder.Finalize();
        plan.BuildRequest.AssetID = plannedRecord.ID;
        plan.BuildRequest.ProcessorClass = TEXT("resolver-test");
        plan.BuildRequest.MemoryBytes = 1;
        plan.BuildRequest.Build = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
        {
            builds++;
            return false;
        };
        plan.BuildRequest.Publish = [&, plannedRecord, request, fingerprint = currentFingerprint](const AssetCancellationToken&, AssetPipelineDiagnostic& publicationDiagnostic)
        {
            publications++;
            return WriteResolvedManifest(library, plannedRecord, request, fingerprint, StringAnsiView("runtime-v1"), publicationDiagnostic);
        };
        planDiagnostic = AssetPipelineDiagnostic();
        return false;
    };
    resolver.Configure(database, service, library, target, provider);

    ResolvedArtifact artifact;
    ArtifactRequest request = baseRequest;
    request.Policy = ArtifactResolvePolicy::PublishedOnly;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    CHECK_FALSE(artifact.IsExact);
    CHECK(plans.load() == 0);
    CHECK(builds.load() == 0);

    request.RequiredCompatibility.Clear();
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    request.RequiredCompatibility = "runtime-v1";

    request.Policy = ArtifactResolvePolicy::NoBuild;
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactRebuildRequired);
    CHECK(builds.load() == 0);

    request.RequiredCompatibility.Clear();
    request.Policy = ArtifactResolvePolicy::Interactive;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    CHECK_FALSE(artifact.IsExact);
    REQUIRE(WaitForCount(publications, 1));
    CHECK(builds.load() == 1);

    request.RequiredCompatibility = "runtime-v1";
    request.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsExact);
    CHECK_FALSE(artifact.IsLastGood);
    CHECK(builds.load() == 1);
    const int32 plansBeforeCurrentInspection = plans.load();
    CHECK(resolver.IsExactCurrent(request, currentFingerprint));
    Array<byte> exactBytes;
    REQUIRE_FALSE(File::ReadAllBytes(artifact.StoragePath.Get(), exactBytes));
    REQUIRE(exactBytes.HasItems());
    exactBytes[0] ^= 0xff;
    REQUIRE_FALSE(File::WriteAllBytes(artifact.StoragePath.Get(), exactBytes.Get(), exactBytes.Count()));
    CHECK_FALSE(resolver.IsExactCurrent(request, currentFingerprint));
    REQUIRE_FALSE(WriteResolvedManifest(library, record, request, currentFingerprint, StringAnsiView("runtime-v1"), diagnostic));
    CHECK(resolver.IsExactCurrent(request, currentFingerprint));
    REQUIRE_FALSE(FileSystem::DeleteFile(artifact.StoragePath.Get()));
    CHECK_FALSE(resolver.IsExactCurrent(request, currentFingerprint));
    REQUIRE_FALSE(WriteResolvedManifest(library, record, request, currentFingerprint, StringAnsiView("runtime-v1"), diagnostic));
    CHECK(resolver.IsExactCurrent(request, currentFingerprint));
    CHECK(plans.load() == plansBeforeCurrentInspection);
    AssetLoadLocation location;
    REQUIRE_FALSE(resolver.ResolveLoadLocation(request, location, diagnostic));
    CHECK(location.Info.Path == sourcePath);
    CHECK(location.Artifact.StoragePath.Get() != sourcePath);

    currentFingerprint = ResolverKey("newer-input");
    request.Policy = ArtifactResolvePolicy::NoBuild;
    request.RequiredCompatibility = "runtime-v2";
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactIncompatible);

    input.Status = AssetRecordStatus::MissingSource;
    records[0] = input;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    request.RequiredCompatibility.Clear();
    request.Policy = ArtifactResolvePolicy::Interactive;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    CHECK(publications.load() == 1);
    request.Policy = ArtifactResolvePolicy::Exact;
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceMissing);
}

TEST_CASE("ArtifactResolver isolates objects by persisted GUID despite legacy composite collisions")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactResolverCollision-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const AssetObjectId firstObject(AssetGuid(Guid(101, 202, 303, 404)), 2);
    const Guid runtimeId = LegacyCompositeGuidForTest(firstObject);
    const AssetObjectId secondObject = MakeRuntimeGuidCollision(runtimeId, 0x0000000200000003LL);
    REQUIRE(firstObject != secondObject);
    REQUIRE(LegacyCompositeGuidForTest(firstObject) == LegacyCompositeGuidForTest(secondObject));

    const String firstSource = content / TEXT("first.source");
    const String secondSource = content / TEXT("second.source");
    REQUIRE_FALSE(File::WriteAllText(firstSource, TEXT("first"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(secondSource, TEXT("second"), Encoding::ANSI));

    AssetRecord firstInput;
    firstInput.ID = Guid(501, 502, 503, 504);
    firstInput.SourceAssetID = firstObject.Asset.Value;
    firstInput.LocalId = firstObject.LocalId;
    firstInput.TypeName = TEXT("FlaxEngine.RawDataAsset");
    firstInput.CanonicalPath = CanonicalAssetPath(firstSource);
    firstInput.SourcePath = SourceFilePath(firstSource);
    firstInput.ProcessorID = TEXT("test.resolver.collision");
    firstInput.SourceKind = AssetSourceKind::ImportedSource;
    firstInput.Status = AssetRecordStatus::Ready;
    firstInput.SubAsset = SubAssetKey(TEXT("first-collision-subobject"));
    AssetRecord secondInput = firstInput;
    secondInput.ID = Guid(601, 602, 603, 604);
    secondInput.SourceAssetID = secondObject.Asset.Value;
    secondInput.LocalId = secondObject.LocalId;
    secondInput.CanonicalPath = CanonicalAssetPath(secondSource);
    secondInput.SourcePath = SourceFilePath(secondSource);
    secondInput.SubAsset = SubAssetKey(TEXT("collision-subobject"));

    AssetPipelineDiagnostic diagnostic;
    AssetDatabase database;
    Array<AssetRecord> records;
    records.Add(firstInput);
    records.Add(secondInput);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    AssetRecord firstRecord;
    AssetRecord secondRecord;
    REQUIRE(database.TryGetRecord(firstObject, firstRecord));
    REQUIRE(database.TryGetRecord(secondObject, secondRecord));

    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));
    const ArtifactTarget target = ResolverTarget();
    const ArtifactKey fingerprint = ResolverKey("collision-input");
    ArtifactResolutionPlanProvider provider = [fingerprint](const AssetRecord& record, const ArtifactRequest&,
        ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& planDiagnostic)
    {
        plan.CurrentInputFingerprint = fingerprint;
        ArtifactKeyBuilder builder(StringAnsiView("resolver-collision-job-v1"));
        builder.AddGuid(StringAnsiView("object-guid"), record.ID);
        plan.BuildRequest.Key.ExactPlan = builder.Finalize();
        planDiagnostic = AssetPipelineDiagnostic();
        return false;
    };

    ArtifactRequest firstRequest;
    firstRequest.Object = firstObject;
    firstRequest.Target = target;
    firstRequest.OutputKind = "Runtime";
    firstRequest.Policy = ArtifactResolvePolicy::NoBuild;
    ArtifactRequest secondRequest = firstRequest;
    secondRequest.Object = secondObject;
    REQUIRE_FALSE(WriteResolvedManifest(library, firstRecord, firstRequest, fingerprint, StringAnsiView(), diagnostic));
    REQUIRE_FALSE(WriteResolvedManifest(library, secondRecord, secondRequest, fingerprint, StringAnsiView(), diagnostic));

    ArtifactResolver resolver;
    resolver.Configure(database, service, library, target, provider);
    ResolvedArtifact firstArtifact;
    ResolvedArtifact secondArtifact;
    REQUIRE_FALSE(resolver.Resolve(firstRequest, firstArtifact, diagnostic));
    REQUIRE_FALSE(resolver.Resolve(secondRequest, secondArtifact, diagnostic));
    CHECK(firstArtifact.ObjectID == firstObject);
    CHECK(secondArtifact.ObjectID == secondObject);
    CHECK(firstArtifact.StoragePath.Get() != secondArtifact.StoragePath.Get());
}

TEST_CASE("ArtifactResolver exact wait observes caller cancellation")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactResolverCancellation-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String sourcePath = content / TEXT("cancel.source");
    REQUIRE_FALSE(File::WriteAllText(sourcePath, TEXT("source"), Encoding::ANSI));

    AssetRecord input;
    input.ID = Guid::New();
    input.SourceAssetID = input.ID;
    input.TypeName = TEXT("FlaxEngine.RawDataAsset");
    input.CanonicalPath = CanonicalAssetPath(sourcePath);
    input.SourcePath = SourceFilePath(sourcePath);
    input.ProcessorID = TEXT("test.resolver.cancel");
    input.SourceKind = AssetSourceKind::ImportedSource;
    input.Status = AssetRecordStatus::Ready;
    AssetPipelineDiagnostic diagnostic;
    AssetDatabase database;
    Array<AssetRecord> records;
    records.Add(input);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));

    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));
    std::atomic<bool> buildStarted { false };
    std::atomic<bool> buildCancelled { false };
    const ArtifactKey fingerprint = ResolverKey("cancel-input");
    ArtifactResolutionPlanProvider provider = [&](const AssetRecord& record, const ArtifactRequest&,
        ArtifactResolutionPlan& plan, AssetPipelineDiagnostic& planDiagnostic)
    {
        plan.CurrentInputFingerprint = fingerprint;
        plan.BuildRequest.Key.ExactPlan = ResolverKey("cancel-job");
        plan.BuildRequest.AssetID = record.ID;
        plan.BuildRequest.ProcessorClass = TEXT("resolver-cancel-test");
        plan.BuildRequest.MemoryBytes = 1;
        plan.BuildRequest.Build = [&](const AssetCancellationToken& token, AssetPipelineDiagnostic& buildDiagnostic)
        {
            buildStarted.store(true);
            while (!token.IsCancellationRequested())
                Platform::Sleep(1);
            buildCancelled.store(true);
            buildDiagnostic.Code = AssetPipelineDiagnosticCode::BuildCancelled;
            return true;
        };
        plan.BuildRequest.Publish = [](const AssetCancellationToken&, AssetPipelineDiagnostic&) { return false; };
        planDiagnostic = AssetPipelineDiagnostic();
        return false;
    };
    ArtifactResolver resolver;
    const ArtifactTarget target = ResolverTarget();
    resolver.Configure(database, service, library, target, provider);
    ArtifactRequest request;
    request.Object = AssetObjectId::Main(AssetGuid(input.SourceAssetID));
    request.Target = target;
    request.OutputKind = "Runtime";
    request.Policy = ArtifactResolvePolicy::Exact;
    request.IsCancellationRequested = [&] { return buildStarted.load(); };
    ResolvedArtifact artifact;
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCancelled);
    for (int32 i = 0; i < 5000 && !buildCancelled.load(); i++)
        Platform::Sleep(1);
    CHECK(buildCancelled.load());
}

#endif
