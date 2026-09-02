// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Documents/AssetSaveService.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestAssetSaveRevisionProvider final : public ISourceSaveRevisionProvider
    {
    public:
        SourceSaveRevision Tracked;

        SourceSaveRevisionLookup LookupTrackedSource(const StringView& path, SourceSaveRevision& result,
            AssetPipelineDiagnostic& diagnostic) const override
        {
            if (!FileSystem::AreFilePathsEquivalent(path, Tracked.SourcePath))
            {
                diagnostic = AssetPipelineDiagnostic();
                return SourceSaveRevisionLookup::NotFound;
            }
            result = Tracked;
            diagnostic = AssetPipelineDiagnostic();
            return SourceSaveRevisionLookup::Found;
        }
    };

    class TestAssetSavePipeline final : public IAssetSavePipeline
    {
    public:
        int32 RefreshCalls = 0;
        int32 ImportCalls = 0;
        int32 SelfWriteCalls = 0;
        bool FailRefresh = false;
        bool FailImport = false;
        bool LastForce = false;
        bool LastSynchronous = false;
        Guid LastImportedID = Guid::Empty;
        SourceSaveSelfWrite LastWrite;
        DirtySourceRegistry* DirtyRegistry = nullptr;
        String DirtyPath;
        ContentHash DirtyBaseHash;
        uint64 NewDirtyRevision = 0;

        bool RefreshSource(const StringView& path, AssetPipelineDiagnostic& diagnostic) override
        {
            RefreshCalls++;
            if (DirtyRegistry && DirtyPath.HasChars())
                NewDirtyRevision = DirtyRegistry->MarkDirty(DirtyPath, DirtyBaseHash, 7, TEXT("Edited during refresh"));
            diagnostic = AssetPipelineDiagnostic();
            if (!FailRefresh)
                return false;
            diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = path;
            diagnostic.Message = TEXT("Injected refresh failure.");
            return true;
        }

        bool ImportSource(const Guid& sourceID, bool force, bool synchronous,
            AssetPipelineDiagnostic& diagnostic) override
        {
            ImportCalls++;
            LastImportedID = sourceID;
            LastForce = force;
            LastSynchronous = synchronous;
            diagnostic = AssetPipelineDiagnostic();
            if (!FailImport)
                return false;
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.AssetGuid = sourceID;
            diagnostic.Message = TEXT("Injected import failure.");
            return true;
        }

        void RegisterSelfWrite(const SourceSaveSelfWrite& write) override
        {
            SelfWriteCalls++;
            LastWrite = write;
        }
    };

    class TestAssetSaveFailureInjector final : public ISourceSaveFailureInjector
    {
    public:
        SourceSaveFailurePoint Point;

        explicit TestAssetSaveFailureInjector(SourceSaveFailurePoint point)
            : Point(point)
        {
        }

        bool ShouldFail(SourceSaveFailurePoint point) override
        {
            return point == Point;
        }
    };

    String MakeAssetSaveTestPath()
    {
        return Globals::TemporaryFolder / (Guid::New().ToString(Guid::FormatType::N) + TEXT(".json"));
    }

    void ConfigureTracked(TestAssetSaveRevisionProvider& provider, const StringView& path,
        const StringAnsiView& durableBytes)
    {
        provider.Tracked.SourceAssetID = Guid(11, 12, 13, 14);
        provider.Tracked.SourcePath = path;
        provider.Tracked.SourceRevision = 41;
        provider.Tracked.DurableSourceHash = ContentHash::Compute(durableBytes.Get(), durableBytes.Length());
        provider.Tracked.IsTracked = true;
    }

    AssetSaveRequest MakeTrackedRequest(const StringView& path, const StringAnsiView& bytes)
    {
        AssetSaveRequest request;
        request.SourcePath = path;
        request.CanonicalBytes = bytes;
        request.RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
        return request;
    }
}

TEST_CASE("Asset save service reports commit refresh and import independently")
{
    const String path = MakeAssetSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi replacement("{\n  \"value\": 2\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestAssetSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    TestAssetSavePipeline pipeline;
    DirtySourceRegistry dirty;
    const uint64 editRevision = dirty.MarkDirty(path, provider.Tracked.DurableSourceHash, 3, TEXT("Graph edit"));
    AssetSaveService service(&provider, &pipeline, &dirty);
    AssetSaveRequest request = MakeTrackedRequest(path, replacement);
    request.EditRevision = editRevision;
    request.ImportMode = AssetSaveImportMode::Synchronous;
    request.ForceImport = true;
    AssetSaveResult result;
    AssetPipelineDiagnostic diagnostic;

    REQUIRE_FALSE(service.Save(request, result, diagnostic));
    CHECK(result.Source.Outcome == SourceSaveOutcome::Committed);
    CHECK(result.Refresh == AssetSaveRefreshOutcome::Succeeded);
    CHECK(result.Import == AssetSaveImportOutcome::Succeeded);
    CHECK(result.IsSourceCommitted());
    CHECK(result.IsSuccessful());
    CHECK(result.CommittedEditRevision == editRevision);
    CHECK(result.DirtyCleared);
    CHECK(dirty.Count() == 0);
    CHECK(pipeline.RefreshCalls == 1);
    CHECK(pipeline.ImportCalls == 1);
    CHECK(pipeline.SelfWriteCalls == 1);
    CHECK(pipeline.LastImportedID == provider.Tracked.SourceAssetID);
    CHECK(pipeline.LastForce);
    CHECK(pipeline.LastSynchronous);
    CHECK(FileSystem::AreFilePathsEquivalent(pipeline.LastWrite.Path, path));
}

TEST_CASE("Asset save service preserves committed source across follow-up failures")
{
    SECTION("Refresh failure")
    {
        const String path = MakeAssetSaveTestPath();
        SCOPE_EXIT { FileSystem::DeleteFile(path); };
        const StringAnsi initial("{\"value\":1}\n");
        const StringAnsi replacement("{\"value\":2}\n");
        REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));
        TestAssetSaveRevisionProvider provider;
        ConfigureTracked(provider, path, initial);
        TestAssetSavePipeline pipeline;
        pipeline.FailRefresh = true;
        DirtySourceRegistry dirty;
        const uint64 revision = dirty.MarkDirty(path, provider.Tracked.DurableSourceHash);
        AssetSaveService service(&provider, &pipeline, &dirty);
        AssetSaveRequest request = MakeTrackedRequest(path, replacement);
        request.EditRevision = revision;
        request.ImportMode = AssetSaveImportMode::Synchronous;
        AssetSaveResult result;
        AssetPipelineDiagnostic diagnostic;

        CHECK(service.Save(request, result, diagnostic));
        CHECK(result.Source.Outcome == SourceSaveOutcome::Committed);
        CHECK(result.Refresh == AssetSaveRefreshOutcome::Failed);
        CHECK(result.Import == AssetSaveImportOutcome::Blocked);
        CHECK(result.IsSourceCommitted());
        CHECK_FALSE(result.IsSuccessful());
        CHECK(result.DirtyCleared);
        CHECK(dirty.Count() == 0);
        CHECK(pipeline.ImportCalls == 0);
        CHECK(pipeline.SelfWriteCalls == 0);
    }

    SECTION("Import failure")
    {
        const String path = MakeAssetSaveTestPath();
        SCOPE_EXIT { FileSystem::DeleteFile(path); };
        const StringAnsi initial("{\"value\":1}\n");
        const StringAnsi replacement("{\"value\":2}\n");
        REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));
        TestAssetSaveRevisionProvider provider;
        ConfigureTracked(provider, path, initial);
        TestAssetSavePipeline pipeline;
        pipeline.FailImport = true;
        DirtySourceRegistry dirty;
        const uint64 revision = dirty.MarkDirty(path, provider.Tracked.DurableSourceHash);
        AssetSaveService service(&provider, &pipeline, &dirty);
        AssetSaveRequest request = MakeTrackedRequest(path, replacement);
        request.EditRevision = revision;
        request.ImportMode = AssetSaveImportMode::Asynchronous;
        AssetSaveResult result;
        AssetPipelineDiagnostic diagnostic;

        CHECK(service.Save(request, result, diagnostic));
        CHECK(result.Source.Outcome == SourceSaveOutcome::Committed);
        CHECK(result.Refresh == AssetSaveRefreshOutcome::Succeeded);
        CHECK(result.Import == AssetSaveImportOutcome::Failed);
        CHECK(result.IsSourceCommitted());
        CHECK_FALSE(result.IsSuccessful());
        CHECK(result.DirtyCleared);
        CHECK(pipeline.RefreshCalls == 1);
        CHECK(pipeline.ImportCalls == 1);
        CHECK_FALSE(pipeline.LastSynchronous);
        CHECK(pipeline.SelfWriteCalls == 1);
    }
}

TEST_CASE("Asset save service never adopts another session dirty revision")
{
    const String path = MakeAssetSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\"value\":1}\n");
    const StringAnsi replacement("{\"value\":2}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));
    TestAssetSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    TestAssetSavePipeline pipeline;
    DirtySourceRegistry dirty;
    const uint64 otherRevision = dirty.MarkDirty(path, provider.Tracked.DurableSourceHash);
    AssetSaveService service(&provider, &pipeline, &dirty);
    AssetSaveRequest request = MakeTrackedRequest(path, replacement);
    AssetSaveResult result;
    AssetPipelineDiagnostic diagnostic;

    REQUIRE_FALSE(service.Save(request, result, diagnostic));
    CHECK(result.Source.Outcome == SourceSaveOutcome::Committed);
    CHECK(result.CommittedEditRevision == 0);
    CHECK_FALSE(result.DirtyCleared);
    DirtySourceRecord remaining;
    REQUIRE(dirty.TryGet(path, remaining));
    CHECK(remaining.EditRevision == otherRevision);
}

TEST_CASE("Asset save service clears only the committed dirty revision")
{
    const String path = MakeAssetSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\"value\":1}\n");
    const StringAnsi replacement("{\"value\":2}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));
    TestAssetSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    DirtySourceRegistry dirty;
    const uint64 committedRevision = dirty.MarkDirty(path, provider.Tracked.DurableSourceHash);
    TestAssetSavePipeline pipeline;
    pipeline.DirtyRegistry = &dirty;
    pipeline.DirtyPath = path;
    pipeline.DirtyBaseHash = ContentHash::Compute(replacement.Get(), replacement.Length());
    AssetSaveService service(&provider, &pipeline, &dirty);
    AssetSaveRequest request = MakeTrackedRequest(path, replacement);
    request.EditRevision = committedRevision;
    AssetSaveResult result;
    AssetPipelineDiagnostic diagnostic;

    REQUIRE_FALSE(service.Save(request, result, diagnostic));
    REQUIRE(pipeline.NewDirtyRevision > committedRevision);
    DirtySourceRecord current;
    REQUIRE(dirty.TryGet(path, current));
    CHECK(current.EditRevision == pipeline.NewDirtyRevision);
    CHECK(current.DirtyLocalIDs.Contains(7));
}

TEST_CASE("Asset save service refreshes adopted current bytes without rewriting")
{
    const String path = MakeAssetSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi durable("{\"value\":1}\n");
    const StringAnsi external("{\"value\":2}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, external.Get(), external.Length()));
    TestAssetSaveRevisionProvider provider;
    ConfigureTracked(provider, path, durable);
    TestAssetSavePipeline pipeline;
    DirtySourceRegistry dirty;
    AssetSaveService service(&provider, &pipeline, &dirty);
    AssetSaveRequest request = MakeTrackedRequest(path, external);
    request.ConflictPolicy = SourceSaveConflictPolicy::AdoptCurrent;
    AssetSaveResult result;
    AssetPipelineDiagnostic diagnostic;

    REQUIRE_FALSE(service.Save(request, result, diagnostic));
    CHECK(result.Source.Outcome == SourceSaveOutcome::Unchanged);
    CHECK(result.Refresh == AssetSaveRefreshOutcome::Succeeded);
    CHECK(result.Import == AssetSaveImportOutcome::NotRequested);
    CHECK(pipeline.RefreshCalls == 1);
    CHECK(pipeline.SelfWriteCalls == 0);
}
