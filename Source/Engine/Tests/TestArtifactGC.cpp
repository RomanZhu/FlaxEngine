// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Artifacts/ArtifactGC.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    ArtifactTarget GCTarget()
    {
        ArtifactTarget target;
        target.Platform = "Windows";
        target.Architecture = "x64";
        target.Role = "Editor";
        return target;
    }

    ArtifactKey GCKey(const char* value)
    {
        return ArtifactKey(ContentHash::Compute(value, StringUtils::Length(value)));
    }

    String WriteGCArtifact(const String& library, const ArtifactTarget& target, const Guid& assetId, const char* keyText)
    {
        AssetPipelineDiagnostic diagnostic;
        ArtifactStoragePath path;
        REQUIRE_FALSE(ArtifactStore::TryGetArtifactPath(library, target, ArtifactTargetDimension::All, assetId,
            StringAnsiView("Runtime"), GCKey(keyText), StringAnsiView(".bin"), path, diagnostic));
        const String directory = StringUtils::GetDirectoryName(path.Get());
        if (!FileSystem::DirectoryExists(directory))
            REQUIRE_FALSE(FileSystem::CreateDirectory(directory));
        REQUIRE_FALSE(File::WriteAllBytes(path.Get(), keyText, StringUtils::Length(keyText)));
        return path.Get();
    }

    void WriteGCManifest(const String& library, const ArtifactTarget& target, const Guid& assetId, const String& artifactPath, const char* bytes)
    {
        AssetPipelineDiagnostic diagnostic;
        ArtifactManifest manifest;
        manifest.ObjectID = AssetObjectId::Main(AssetGuid(assetId));
        manifest.DatabaseRevision = 1;
        manifest.ProcessorID = TEXT("test.gc");
        manifest.ProcessorImplementationVersion = 1;
        manifest.Target = target;
        manifest.InputFingerprint = GCKey("gc-input");
        manifest.SourceHash = ContentHash::Compute("source", 6);
        manifest.SettingsHash = ContentHash::Compute("settings", 8);
        manifest.BuildID = Guid::New().ToString(Guid::FormatType::N);
        ArtifactManifestOutput output;
        output.Kind = "Runtime";
        output.Key = GCKey(bytes);
        REQUIRE_FALSE(ArtifactStore::TryMakeLibraryRelative(library, artifactPath, output.RelativePath, diagnostic));
        output.Content = ContentHash::Compute(bytes, StringUtils::Length(bytes));
        output.Size = StringUtils::Length(bytes);
        output.Compatibility = "runtime-v1";
        manifest.Outputs.Add(output);
        StringAnsi json;
        REQUIRE_FALSE(manifest.ToJson(json, diagnostic));
        ArtifactStoragePath manifestPath;
        REQUIRE_FALSE(ArtifactStore::TryGetManifestPath(library, target, manifest.ObjectID.Asset.Value, manifestPath, diagnostic));
        const String directory = StringUtils::GetDirectoryName(manifestPath.Get());
        if (!FileSystem::DirectoryExists(directory))
            REQUIRE_FALSE(FileSystem::CreateDirectory(directory));
        REQUIRE_FALSE(File::WriteAllBytes(manifestPath.Get(), json.Get(), json.Length()));
    }
}

TEST_CASE("ArtifactGC preserves manifests leases and staging while reclaiming only eligible orphans")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactGC-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
    const ArtifactTarget target = GCTarget();
    const Guid currentId = Guid::New();
    const String current = WriteGCArtifact(library, target, currentId, "current");
    WriteGCManifest(library, target, currentId, current, "current");
    const String orphan = WriteGCArtifact(library, target, Guid::New(), "orphan");
    const String leasedPath = WriteGCArtifact(library, target, Guid::New(), "leased");
    ArtifactLease loadedLease = ArtifactLease::Acquire(leasedPath);
    const Guid stagingJob = Guid::New();
    ArtifactStoragePath stagingPath;
    REQUIRE_FALSE(ArtifactStore::TryGetJobStagingPath(library, stagingJob, stagingPath, diagnostic));
    REQUIRE_FALSE(FileSystem::CreateDirectory(stagingPath.Get()));
    const String stagedFile = stagingPath.Get() / TEXT("active.partial");
    REQUIRE_FALSE(File::WriteAllText(stagedFile, TEXT("active"), Encoding::ANSI));

    ArtifactGCOptions options;
    options.GracePeriod = TimeSpan::Zero();
    options.MaximumDeletes = 16;
    options.MaximumDeleteBytes = 1024;
    options.QueryFreeBytes = [](const StringView&, uint64& bytes)
    {
        bytes = 1024ull * 1024ull;
        return false;
    };
    ArtifactGCResult result;
    REQUIRE_FALSE(ArtifactGC::Run(library, options, result, diagnostic));
    CHECK(FileSystem::FileExists(current));
    CHECK(FileSystem::FileExists(leasedPath));
    CHECK_FALSE(FileSystem::FileExists(orphan));
    CHECK(FileSystem::FileExists(stagedFile));
    CHECK(result.ReachableFiles == 1);
    CHECK(result.LeasedFiles == 1);
    CHECK(result.DeletedFiles == 1);

    loadedLease.Reset();
    REQUIRE_FALSE(ArtifactGC::Run(library, options, result, diagnostic));
    CHECK_FALSE(FileSystem::FileExists(leasedPath));
    CHECK(FileSystem::FileExists(current));
}

TEST_CASE("ArtifactGC blocks on corrupt selection and reports safe disk pressure")
{
    const String root = Globals::TemporaryFolder / (TEXT("ArtifactGCSafety-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
    const String orphan = WriteGCArtifact(library, GCTarget(), Guid::New(), "blocked-orphan");
    const String corruptDirectory = ArtifactStore::GetManifestsPath(library) / TEXT("corrupt");
    REQUIRE_FALSE(FileSystem::CreateDirectory(corruptDirectory));
    REQUIRE_FALSE(File::WriteAllText(corruptDirectory / TEXT("selection.json"), TEXT("{bad"), Encoding::ANSI));

    ArtifactGCOptions options;
    options.GracePeriod = TimeSpan::Zero();
    options.DiskQuotaBytes = 1;
    options.MinimumFreeBytes = 1024;
    options.QueryFreeBytes = [](const StringView&, uint64& bytes)
    {
        bytes = 0;
        return false;
    };
    ArtifactGCResult result;
    REQUIRE_FALSE(ArtifactGC::Run(library, options, result, diagnostic));
    CHECK(result.BlockedByInvalidManifest);
    CHECK(result.PressureDetected);
    CHECK(result.DeletedFiles == 0);
    CHECK(FileSystem::FileExists(orphan));
    CHECK(ArtifactGC::CheckBuildCapacity(library, 1, options, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);

    FileSystem::DeleteFile(corruptDirectory / TEXT("selection.json"));
    AssetCancellationSource cancellation;
    cancellation.Cancel();
    options.Cancellation = cancellation.GetToken();
    REQUIRE_FALSE(ArtifactGC::Run(library, options, result, diagnostic));
    CHECK(result.WasCancelled);
    CHECK(FileSystem::FileExists(orphan));
}

#endif
