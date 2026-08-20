// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS

#include "Engine/Content/Artifacts/ArtifactLock.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <atomic>
#include <thread>
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    String LockLibrary(const Char* name)
    {
        const String root = Globals::TemporaryFolder / (String(name) + TEXT("-") + Guid::New().ToString(Guid::FormatType::N));
        FileSystem::CreateDirectory(root);
        const String library = root / TEXT("Library");
        FileSystem::CreateDirectory(library);
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(ArtifactStore::EnsureLayout(library, diagnostic));
        return library;
    }

    ArtifactKey LockKey(const char* text)
    {
        return ArtifactKey(ContentHash::Compute(text, StringUtils::Length(text)));
    }

    void WriteLockRecord(const String& library, const ArtifactLockRecord& record)
    {
        ArtifactStoragePath path;
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(ArtifactStore::TryGetLockPath(library, record.Key, path, diagnostic));
        StringAnsi json;
        REQUIRE_FALSE(record.ToJson(json, diagnostic));
        REQUIRE_FALSE(File::WriteAllBytes(path.Get(), json.Get(), json.Length()));
    }

    ArtifactLockRecord LocalRecord(const ArtifactKey& key, const Guid& jobId)
    {
        ArtifactLockRecord record;
        record.Key = key;
        record.ProcessID = Platform::GetCurrentProcessId();
        record.ProcessStartIdentity = ArtifactLock::GetCurrentProcessStartIdentity();
        record.HostIdentity = Platform::GetComputerName();
        record.CreatedUtcTicks = DateTime::NowUTC().Ticks;
        record.HeartbeatUtcTicks = record.CreatedUtcTicks;
        record.JobID = jobId;
        return record;
    }
}

TEST_CASE("ArtifactLock atomically excludes exact concurrent key owners")
{
    const String library = LockLibrary(TEXT("ArtifactLockAtomic"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const ArtifactKey key = LockKey("atomic-lock");
    AssetCancellationSource cancellation;
    AssetPipelineDiagnostic diagnostic;
    std::atomic<int32> acquired { 0 };
    std::atomic<bool> release { false };
    auto contender = [&]()
    {
        ArtifactLock lock;
        AssetPipelineDiagnostic localDiagnostic;
        if (!lock.Acquire(library, key, Guid::New(), cancellation.GetToken(), localDiagnostic, 0))
        {
            acquired++;
            while (!release.load())
                Platform::Sleep(1);
        }
    };
    std::thread first(contender);
    std::thread second(contender);
    for (int32 i = 0; i < 5000 && acquired.load() == 0; i++)
        Platform::Sleep(1);
    REQUIRE(acquired.load() == 1);
    release.store(true);
    first.join();
    second.join();
    CHECK(acquired.load() == 1);

    ArtifactLock next;
    REQUIRE_FALSE(next.Acquire(library, key, Guid::New(), cancellation.GetToken(), diagnostic, 0));
    CHECK(next.IsHeld());
}

TEST_CASE("ArtifactLock recovery proves process identity and removes only abandoned staging")
{
    const String library = LockLibrary(TEXT("ArtifactLockRecovery"));
    const String root = StringUtils::GetDirectoryName(library);
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    AssetCancellationSource cancellation;
    AssetPipelineDiagnostic diagnostic;

    const ArtifactKey reusedKey = LockKey("pid-reused");
    const Guid reusedJob = Guid::New();
    ArtifactLockRecord reused = LocalRecord(reusedKey, reusedJob);
    reused.ProcessStartIdentity++;
    WriteLockRecord(library, reused);
    ArtifactStoragePath reusedStaging;
    REQUIRE_FALSE(ArtifactStore::TryGetJobStagingPath(library, reusedJob, reusedStaging, diagnostic));
    REQUIRE_FALSE(FileSystem::CreateDirectory(reusedStaging.Get()));
    REQUIRE_FALSE(File::WriteAllText(reusedStaging.Get() / TEXT("partial.txt"), TEXT("partial"), Encoding::ANSI));

    ArtifactLock recovered;
    REQUIRE_FALSE(recovered.Acquire(library, reusedKey, Guid::New(), cancellation.GetToken(), diagnostic, 0));
    CHECK(recovered.IsHeld());
    CHECK_FALSE(FileSystem::DirectoryExists(reusedStaging.Get()));

    ArtifactLock live;
    const ArtifactKey liveKey = LockKey("live-owner");
    REQUIRE_FALSE(live.Acquire(library, liveKey, Guid::New(), cancellation.GetToken(), diagnostic, 0));
    ArtifactLock blocked;
    CHECK(blocked.Acquire(library, liveKey, Guid::New(), cancellation.GetToken(), diagnostic, 0));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactLockBusy);
    CHECK(live.IsHeld());

    const ArtifactKey deadKey = LockKey("dead-owner");
    const Guid deadJob = Guid::New();
    WriteLockRecord(library, LocalRecord(deadKey, deadJob));
    ArtifactStoragePath deadStaging;
    REQUIRE_FALSE(ArtifactStore::TryGetJobStagingPath(library, deadJob, deadStaging, diagnostic));
    REQUIRE_FALSE(FileSystem::CreateDirectory(deadStaging.Get()));
    int32 recoveredCount = 0;
    ArtifactLockLivenessProbe deadProbe = [deadKey](const ArtifactLockRecord& record)
    {
        return record.Key == deadKey ? ArtifactLockProcessState::RecordedProcessDead : ArtifactLockProcessState::SameProcessAlive;
    };
    REQUIRE_FALSE(ArtifactLock::RecoverAbandoned(library, deadProbe, recoveredCount, diagnostic));
    CHECK(recoveredCount == 1);
    CHECK_FALSE(FileSystem::DirectoryExists(deadStaging.Get()));
    ArtifactStoragePath deadLockPath;
    REQUIRE_FALSE(ArtifactStore::TryGetLockPath(library, deadKey, deadLockPath, diagnostic));
    CHECK_FALSE(FileSystem::FileExists(deadLockPath.Get()));
}

#endif
