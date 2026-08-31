// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <atomic>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
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
        manifest.AssetID = record.ID;
        manifest.DatabaseRevision = record.DatabaseRevision;
        manifest.ProcessorID = record.ProcessorID;
        manifest.ProcessorImplementationVersion = 1;
        manifest.Target = request.Target;
        manifest.InputFingerprint = fingerprint;
        manifest.SourceHash = ContentHash::Compute("source", 6);
        manifest.SettingsHash = ContentHash::Compute("settings", 8);
        manifest.BuildID = Guid::New().ToString(Guid::FormatType::N);
        ArtifactManifestObject object;
        object.ObjectID = record.GetObjectId();
        object.BackingAssetID = record.ID;
        object.TypeName = record.TypeName;
        object.Name = TEXT("ResolverAsset");
        object.IsMainObject = record.IsMainAsset();
        manifest.Objects.Add(object);
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
}

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
    baseRequest.AssetID = record.ID;
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

    request.Policy = ArtifactResolvePolicy::NoBuild;
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactRebuildRequired);
    CHECK(builds.load() == 0);

    request.Policy = ArtifactResolvePolicy::Interactive;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    CHECK_FALSE(artifact.IsExact);
    REQUIRE(WaitForCount(publications, 1));
    CHECK(builds.load() == 1);

    request.Policy = ArtifactResolvePolicy::Exact;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsExact);
    CHECK_FALSE(artifact.IsLastGood);
    CHECK(builds.load() == 1);
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
    request.RequiredCompatibility = "runtime-v1";
    request.Policy = ArtifactResolvePolicy::Interactive;
    REQUIRE_FALSE(resolver.Resolve(request, artifact, diagnostic));
    CHECK(artifact.IsLastGood);
    CHECK(publications.load() == 1);
    request.Policy = ArtifactResolvePolicy::Exact;
    CHECK(resolver.Resolve(request, artifact, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceMissing);
}

#endif
