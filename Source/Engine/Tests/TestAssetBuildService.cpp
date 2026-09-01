// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Build/AssetBuildService.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Importing/AssetImportScheduler.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <atomic>
#include <mutex>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetBuildJobKey JobKey(const char* text)
    {
        AssetBuildJobKey key;
        key.ExactPlan = ArtifactKey(ContentHash::Compute(text, StringUtils::Length(text)));
        return key;
    }

    String BuildServiceLibrary(const Char* name)
    {
        const String root = Globals::TemporaryFolder / (String(name) + TEXT("-") + Guid::New().ToString(Guid::FormatType::N));
        FileSystem::CreateDirectory(root);
        const String library = root / TEXT("Library");
        FileSystem::CreateDirectory(library);
        return library;
    }

    bool WaitUntil(const Function<bool()>& predicate, int32 timeoutMilliseconds = 5000)
    {
        for (int32 i = 0; i < timeoutMilliseconds; i++)
        {
            if (predicate())
                return true;
            Platform::Sleep(1);
        }
        return false;
    }

    AssetBuildJobRequest BasicRequest(const AssetBuildJobKey& key, const Guid& assetId)
    {
        AssetBuildJobRequest request;
        request.Key = key;
        request.AssetID = assetId;
        request.ProcessorClass = TEXT("test");
        request.MemoryBytes = 1;
        request.Build = [](const AssetCancellationToken&, AssetPipelineDiagnostic&) { return false; };
        request.Publish = [](const AssetCancellationToken&, AssetPipelineDiagnostic&) { return false; };
        return request;
    }
}

TEST_CASE("AssetBuildService deduplicates exact work without coupling requester cancellation")
{
    const String library = BuildServiceLibrary(TEXT("AssetBuildServiceDedup"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 2;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));

    std::atomic<int32> executions { 0 };
    std::atomic<int32> publications { 0 };
    std::atomic<bool> release { false };
    AssetBuildJobRequest request = BasicRequest(JobKey("dedup"), Guid::New());
    request.RefreshId = Guid::New();
    request.Pass = 4;
    request.ProcessorID = TEXT("test.dedup");
    request.Target = TEXT("Windows-x64-Editor");
    request.OutputKinds.Add("Runtime");
    request.RebuildReason = TEXT("source content changed");
    ArtifactKeyComponent component;
    component.Name = "debug-path";
    component.Type = "string";
    component.Value = "C:\\Private\\Source.asset";
    request.KeyComponents.Add(component);
    request.Build = [&](const AssetCancellationToken& token, AssetPipelineDiagnostic&)
    {
        executions++;
        while (!release.load() && !token.IsCancellationRequested())
            Platform::Sleep(1);
        return false;
    };
    request.Publish = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        publications++;
        return false;
    };
    const AssetBuildRequestHandle first = service.Request(request);
    const AssetBuildRequestHandle second = service.Request(request);
    REQUIRE(WaitUntil([&]() { return executions.load() == 1; }));
    service.CancelRequester(first);
    release.store(true);
    REQUIRE(second.Wait(5000));
    CHECK(first.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(second.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(executions.load() == 1);
    CHECK(publications.load() == 1);

    AssetBuildJobResult result;
    REQUIRE(second.TryGetResult(result));
    CHECK(result.RefreshId == request.RefreshId);
    CHECK(result.Pass == request.Pass);
    ArtifactStoragePath logPath;
    REQUIRE_FALSE(ArtifactStore::TryGetJobLogPath(library, result.JobID.ToString(Guid::FormatType::N), logPath, diagnostic));
    StringAnsi log;
    REQUIRE_FALSE(File::ReadAllText(logPath.Get(), log));
    CHECK(log.Contains("\"stage\":\"queued\""));
    CHECK(log.Contains("\"stage\":\"succeeded\""));
    CHECK(log.Contains("\"processorId\":\"test.dedup\""));
    StringAnsi refreshField("\"refreshId\":\"");
    refreshField += StringAnsi(request.RefreshId.ToString(Guid::FormatType::N));
    refreshField += '"';
    CHECK(log.Contains(refreshField));
    CHECK(log.Contains("\"pass\":4"));
    CHECK(log.Contains("<absolute-path-redacted>"));
    CHECK_FALSE(log.Contains("C:\\Private"));
    const AssetBuildMetrics metrics = service.GetMetrics();
    CHECK(metrics.Requests == 2);
    CHECK(metrics.DeduplicationHits == 1);
    CHECK(metrics.BuildsStarted == 1);
    CHECK(metrics.PublicationsStarted == 1);
    CHECK(metrics.Succeeded == 1);
    Array<AssetBuildJobSummary> jobs;
    service.GetJobs(jobs);
    REQUIRE(jobs.Count() == 1);
    CHECK(jobs[0].ProcessorID == TEXT("test.dedup"));
    CHECK(jobs[0].RebuildReason == TEXT("source content changed"));
    CHECK(jobs[0].RefreshId == request.RefreshId);
    CHECK(jobs[0].Pass == request.Pass);
}

TEST_CASE("AssetImportScheduler preserves refresh context on asynchronous jobs")
{
    const String library = BuildServiceLibrary(TEXT("AssetImportSchedulerRefresh"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));

    AssetImportPlan plan;
    plan.Request.Asset = AssetGuid(Guid::New());
    plan.Request.RefreshId = Guid::New();
    plan.Request.Pass = 7;
    plan.StaticFingerprint = JobKey("refresh-scheduler").ExactPlan;
    plan.Importer.ID = TEXT("test.refresh-scheduler");
    plan.Importer.SupportsParallelImport = true;
    AssetImportScheduler scheduler(service);
    const AssetBuildRequestHandle handle = scheduler.Schedule(plan,
        [](const AssetImportPlan&, const AssetCancellationToken&, AssetPipelineDiagnostic&) { return false; });
    REQUIRE(handle.IsValid());
    REQUIRE(handle.Wait(5000));
    AssetBuildJobResult result;
    REQUIRE(handle.TryGetResult(result));
    CHECK(result.Status == AssetBuildJobStatus::Succeeded);
    CHECK(result.RefreshId == plan.Request.RefreshId);
    CHECK(result.Pass == plan.Request.Pass);
}

TEST_CASE("AssetBuildService replays terminal publication when requested")
{
    const String library = BuildServiceLibrary(TEXT("AssetBuildServiceReplayPublication"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));

    std::atomic<int32> executions { 0 };
    std::atomic<int32> publications { 0 };
    AssetBuildJobRequest request = BasicRequest(JobKey("replay-publication"), Guid::New());
    request.AllowTerminalDeduplication = false;
    request.Build = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        executions++;
        return false;
    };
    request.Publish = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        publications++;
        return false;
    };

    const AssetBuildRequestHandle first = service.Request(request);
    REQUIRE(first.Wait(5000));
    REQUIRE(first.GetStatus() == AssetBuildJobStatus::Succeeded);
    const AssetBuildRequestHandle second = service.Request(request);
    REQUIRE(second.Wait(5000));
    REQUIRE(second.GetStatus() == AssetBuildJobStatus::Succeeded);
    CHECK(executions.load() == 2);
    CHECK(publications.load() == 2);
    CHECK(service.GetMetrics().DeduplicationHits == 0);
}

TEST_CASE("AssetBuildService schedules dependencies and bounds independent fanout")
{
    const String library = BuildServiceLibrary(TEXT("AssetBuildServiceLimits"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 3;
    limits.MaximumMemoryBytes = 80;
    limits.MaximumExternalTools = 2;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));

    std::atomic<int32> active { 0 };
    std::atomic<int32> maximumActive { 0 };
    std::atomic<bool> release { false };
    Array<AssetBuildRequestHandle> handles;
    for (int32 i = 0; i < 3; i++)
    {
        AssetBuildJobRequest request = BasicRequest(JobKey(i == 0 ? "fanout-a" : i == 1 ? "fanout-b" : "fanout-c"), Guid::New());
        request.MemoryBytes = 40;
        request.ExternalToolSlots = 1;
        request.ProcessorConcurrencyLimit = 2;
        request.Build = [&](const AssetCancellationToken& token, AssetPipelineDiagnostic&)
        {
            const int32 current = ++active;
            int32 observed = maximumActive.load();
            while (current > observed && !maximumActive.compare_exchange_weak(observed, current))
            {
            }
            while (!release.load() && !token.IsCancellationRequested())
                Platform::Sleep(1);
            active--;
            return false;
        };
        handles.Add(service.Request(request));
    }
    REQUIRE(WaitUntil([&]() { return maximumActive.load() == 2; }));
    CHECK(active.load() == 2);
    release.store(true);
    for (const AssetBuildRequestHandle& handle : handles)
    {
        REQUIRE(handle.Wait(5000));
        CHECK(handle.GetStatus() == AssetBuildJobStatus::Succeeded);
    }
    CHECK(maximumActive.load() == 2);

    std::mutex orderMutex;
    Array<int32> order;
    std::atomic<bool> allowDependency { false };
    AssetBuildJobRequest dependency = BasicRequest(JobKey("topology-input"), Guid::New());
    dependency.Build = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        while (!allowDependency.load())
            Platform::Sleep(1);
        std::lock_guard<std::mutex> lock(orderMutex);
        order.Add(1);
        return false;
    };
    const AssetBuildRequestHandle dependencyHandle = service.Request(dependency);
    AssetBuildJobRequest dependant = BasicRequest(JobKey("topology-output"), Guid::New());
    dependant.Dependencies.Add(dependency.Key);
    dependant.Build = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        std::lock_guard<std::mutex> lock(orderMutex);
        order.Add(2);
        return false;
    };
    const AssetBuildRequestHandle dependantHandle = service.Request(dependant);
    allowDependency.store(true);
    REQUIRE(dependantHandle.Wait(5000));
    REQUIRE(dependencyHandle.Wait(5000));
    REQUIRE(order.Count() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
}

TEST_CASE("AssetBuildService cancels abandoned requests and closes publication before shutdown")
{
    const String library = BuildServiceLibrary(TEXT("AssetBuildServiceCancellation"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetBuildService service;
    AssetBuildServiceLimits limits;
    limits.MaximumWorkers = 1;
    limits.MaximumMemoryBytes = 64;
    limits.MaximumExternalTools = 1;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(service.Initialize(library, limits, diagnostic));

    std::atomic<bool> blockerStarted { false };
    std::atomic<bool> releaseBlocker { false };
    AssetBuildJobRequest blocker = BasicRequest(JobKey("cancel-blocker"), Guid::New());
    blocker.Build = [&](const AssetCancellationToken& token, AssetPipelineDiagnostic&)
    {
        blockerStarted.store(true);
        while (!releaseBlocker.load() && !token.IsCancellationRequested())
            Platform::Sleep(1);
        return false;
    };
    const AssetBuildRequestHandle blockerHandle = service.Request(blocker);
    REQUIRE(WaitUntil([&]() { return blockerStarted.load(); }));

    std::atomic<int32> abandonedExecutions { 0 };
    AssetBuildJobRequest abandoned = BasicRequest(JobKey("cancel-queued"), Guid::New());
    abandoned.Build = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        abandonedExecutions++;
        return false;
    };
    const AssetBuildRequestHandle abandonedHandle = service.Request(abandoned);
    service.CancelRequester(abandonedHandle);
    releaseBlocker.store(true);
    REQUIRE(blockerHandle.Wait(5000));
    REQUIRE(abandonedHandle.Wait(5000));
    CHECK(abandonedHandle.GetStatus() == AssetBuildJobStatus::Cancelled);
    CHECK(abandonedExecutions.load() == 0);

    std::atomic<bool> shutdownBuildStarted { false };
    std::atomic<int32> shutdownPublications { 0 };
    AssetBuildJobRequest shutdownRequest = BasicRequest(JobKey("shutdown"), Guid::New());
    shutdownRequest.Build = [&](const AssetCancellationToken& token, AssetPipelineDiagnostic&)
    {
        shutdownBuildStarted.store(true);
        while (!token.IsCancellationRequested())
            Platform::Sleep(1);
        return false;
    };
    shutdownRequest.Publish = [&](const AssetCancellationToken&, AssetPipelineDiagnostic&)
    {
        shutdownPublications++;
        return false;
    };
    const AssetBuildRequestHandle shutdownHandle = service.Request(shutdownRequest);
    REQUIRE(WaitUntil([&]() { return shutdownBuildStarted.load(); }));
    service.Shutdown();
    REQUIRE(shutdownHandle.Wait(5000));
    CHECK(shutdownHandle.GetStatus() == AssetBuildJobStatus::Cancelled);
    CHECK(shutdownPublications.load() == 0);
    CHECK_FALSE(service.IsRunning());
}

#endif
