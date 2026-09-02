// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Documents/SourceSaveTransaction.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestSourceSaveRevisionProvider final : public ISourceSaveRevisionProvider
    {
    public:
        SourceSaveRevision Tracked;
        bool HasTracked = true;
        bool Available = true;

        SourceSaveRevisionLookup LookupTrackedSource(const StringView& path, SourceSaveRevision& result,
            AssetPipelineDiagnostic& diagnostic) const override
        {
            if (!Available)
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.SourcePath = path;
                diagnostic.Message = TEXT("Injected unavailable durable database.");
                return SourceSaveRevisionLookup::Failed;
            }
            if (!HasTracked || !FileSystem::AreFilePathsEquivalent(path, Tracked.SourcePath))
            {
                diagnostic = AssetPipelineDiagnostic();
                return SourceSaveRevisionLookup::NotFound;
            }
            result = Tracked;
            diagnostic = AssetPipelineDiagnostic();
            return SourceSaveRevisionLookup::Found;
        }
    };

    class CountingSourceSaveCallback final : public ISourceSaveCallback
    {
    public:
        int32 Calls = 0;

        bool BeforeCommit(const SourceSaveRequest&, AssetPipelineDiagnostic& diagnostic) override
        {
            Calls++;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    class ReentrantSourceSaveCallback final : public ISourceSaveCallback
    {
    public:
        const SourceSaveTransaction& Transaction;
        String Path;
        StringAnsi ExternalBytes;
        bool CaptureFailed = true;

        ReentrantSourceSaveCallback(const SourceSaveTransaction& transaction, const StringView& path,
            const StringAnsiView& externalBytes)
            : Transaction(transaction)
            , Path(path)
            , ExternalBytes(externalBytes)
        {
        }

        bool BeforeCommit(const SourceSaveRequest&, AssetPipelineDiagnostic& diagnostic) override
        {
            SourceSaveRevision nested;
            CaptureFailed = Transaction.Capture(Path, SourceSaveRegistrationMode::RequireTracked, nested, diagnostic);
            if (CaptureFailed)
                return true;
            if (File::WriteAllBytes(Path, ExternalBytes.Get(), ExternalBytes.Length()))
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.SourcePath = Path;
                diagnostic.Message = TEXT("Cannot write the external test mutation.");
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    class SinglePointSourceSaveFailure final : public ISourceSaveFailureInjector
    {
    public:
        SourceSaveFailurePoint Point;

        explicit SinglePointSourceSaveFailure(SourceSaveFailurePoint point)
            : Point(point)
        {
        }

        bool ShouldFail(SourceSaveFailurePoint point) override
        {
            return point == Point;
        }
    };

    class MutateBeforeReplaceSourceSaveHook final : public ISourceSaveFailureInjector
    {
    public:
        String Path;
        StringAnsi ExternalBytes;
        bool MutationFailed = false;

        MutateBeforeReplaceSourceSaveHook(const StringView& path, const StringAnsiView& externalBytes)
            : Path(path)
            , ExternalBytes(externalBytes)
        {
        }

        bool ShouldFail(SourceSaveFailurePoint point) override
        {
            if (point == SourceSaveFailurePoint::BeforeReplace)
                MutationFailed = File::WriteAllBytes(Path, ExternalBytes.Get(), ExternalBytes.Length());
            return false;
        }
    };

    String MakeSourceSaveTestPath()
    {
        return Globals::TemporaryFolder / (Guid::New().ToString(Guid::FormatType::N) + TEXT(".json"));
    }

    void ConfigureTracked(TestSourceSaveRevisionProvider& provider, const StringView& path,
        const StringAnsiView& sourceBytes)
    {
        provider.Tracked.SourceAssetID = Guid(1, 2, 3, 4);
        provider.Tracked.SourcePath = path;
        provider.Tracked.SourceRevision = 17;
        provider.Tracked.DurableSourceHash = ContentHash::Compute(sourceBytes.Get(), sourceBytes.Length());
        provider.Tracked.IsTracked = true;
    }
}

TEST_CASE("Source save transactions commit exact tracked revisions and report no-ops")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi replacement("{\n  \"value\": 2\n}\n");
    const StringAnsi secondReplacement("{\n  \"value\": 3\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    SourceSaveTransaction transaction(&provider);
    SourceSaveRevision expected;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, SourceSaveRegistrationMode::RequireTracked, expected, diagnostic));
    CHECK(expected.SourceAssetID == provider.Tracked.SourceAssetID);
    CHECK(expected.SourceRevision == provider.Tracked.SourceRevision);
    CHECK(expected.SourceHash == ContentHash::Compute(initial.Get(), initial.Length()));
    CHECK(expected.DurableSourceHash == expected.SourceHash);

    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    request.Expected = expected;
    request.CanonicalBytes = replacement;
    SourceSaveResult result;
    REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Committed);
    CHECK(result.TransactionID.IsValid());
    CHECK(result.SelfWrite.TransactionID == result.TransactionID);
    CHECK(FileSystem::AreFilePathsEquivalent(result.SelfWrite.Path, path));
    CHECK(result.SelfWrite.Content == ContentHash::Compute(replacement.Get(), replacement.Length()));

    Array<byte> saved;
    REQUIRE_FALSE(File::ReadAllBytes(path, saved));
    REQUIRE(saved.Count() == replacement.Length());
    CHECK(Platform::MemoryCompare(saved.Get(), replacement.Get(), saved.Count()) == 0);

    provider.Tracked.DurableSourceHash = result.SelfWrite.Content;
    provider.Tracked.SourceRevision++;
    REQUIRE_FALSE(transaction.Capture(path, SourceSaveRegistrationMode::RequireTracked, request.Expected, diagnostic));
    request.CanonicalBytes = secondReplacement;
    result = SourceSaveResult();
    REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Committed);

    provider.Tracked.DurableSourceHash = result.SelfWrite.Content;
    provider.Tracked.SourceRevision++;
    REQUIRE_FALSE(transaction.Capture(path, SourceSaveRegistrationMode::RequireTracked, request.Expected, diagnostic));
    CountingSourceSaveCallback callback;
    result = SourceSaveResult();
    REQUIRE_FALSE(transaction.Commit(request, result, diagnostic, &callback));
    CHECK(result.Outcome == SourceSaveOutcome::Unchanged);
    CHECK(callback.Calls == 0);
    CHECK_FALSE(result.SelfWrite.TransactionID.IsValid());
}

TEST_CASE("Source save transactions reject any tracked identity revision or hash drift")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi replacement("{\n  \"value\": 2\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    request.CanonicalBytes = replacement;

    SECTION("GUID")
    {
        provider.Tracked.SourceAssetID = Guid(5, 6, 7, 8);
    }
    SECTION("path")
    {
        provider.Tracked.SourcePath += TEXT(".moved");
    }
    SECTION("revision")
    {
        provider.Tracked.SourceRevision++;
    }
    SECTION("hash")
    {
        const StringAnsi external("{\n  \"external\": true\n}\n");
        REQUIRE_FALSE(File::WriteAllBytes(path, external.Get(), external.Length()));
    }
    SECTION("durable row hash")
    {
        provider.Tracked.DurableSourceHash = ContentHash::Compute("stale", 5);
    }

    SourceSaveResult result;
    CHECK(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Conflict);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceBusy);
    Array<byte> saved;
    REQUIRE_FALSE(File::ReadAllBytes(path, saved));
    CHECK_FALSE((saved.Count() == replacement.Length() &&
        Platform::MemoryCompare(saved.Get(), replacement.Get(), saved.Count()) == 0));
}

TEST_CASE("Source save transactions require explicit adoption of external tracked bytes")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi external("{\n  \"external\": true\n}\n");
    const StringAnsi desired("{\n  \"forced\": true\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    SourceSaveTransaction transaction(&provider);
    REQUIRE_FALSE(File::WriteAllBytes(path, external.Get(), external.Length()));
    SourceSaveRevision expected;
    AssetPipelineDiagnostic diagnostic;
    CHECK(transaction.Capture(path, SourceSaveRegistrationMode::RequireTracked, expected, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceBusy);
    REQUIRE_FALSE(transaction.Capture(path, SourceSaveRegistrationMode::RequireTracked, expected, diagnostic,
        SourceSaveConflictPolicy::AdoptCurrent));
    CHECK(expected.SourceHash == ContentHash::Compute(external.Get(), external.Length()));
    CHECK(expected.DurableSourceHash == ContentHash::Compute(initial.Get(), initial.Length()));

    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    request.ConflictPolicy = SourceSaveConflictPolicy::AdoptCurrent;
    request.Expected = expected;
    SourceSaveResult result;
    SECTION("force replaces external bytes")
    {
        request.CanonicalBytes = desired;
        REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
        CHECK(result.Outcome == SourceSaveOutcome::Committed);
    }
    SECTION("adopting identical external bytes exposes the durable refresh requirement")
    {
        request.CanonicalBytes = external;
        REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
        CHECK(result.Outcome == SourceSaveOutcome::Unchanged);
        CHECK(result.Current.SourceHash != result.Current.DurableSourceHash);
        CHECK(FileSystem::AreFilePathsEquivalent(result.Current.SourcePath, path));
    }
}

TEST_CASE("Source save lookup failures never downgrade to unregistered")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    TestSourceSaveRevisionProvider provider;
    provider.Available = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRevision revision;
    AssetPipelineDiagnostic diagnostic;
    CHECK(transaction.Capture(path, SourceSaveRegistrationMode::AllowUnregistered, revision, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SnapshotInvalid);
    CHECK_FALSE(FileSystem::FileExists(path));
}

TEST_CASE("Untracked local saves bypass unavailable asset authority but reject tracked expectations")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    TestSourceSaveRevisionProvider provider;
    provider.Available = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::UntrackedLocal;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    request.CanonicalBytes = "{\n  \"local\": true\n}\n";
    SourceSaveResult result;
    REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Committed);

    request.Expected.IsTracked = true;
    request.Expected.SourceAssetID = Guid(1, 2, 3, 4);
    result = SourceSaveResult();
    CHECK(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Rejected);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
}

TEST_CASE("Source save callbacks run outside reservation and are exactly revalidated")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi desired("{\n  \"value\": 2\n}\n");
    const StringAnsi external("{\n  \"external\": true\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    ConfigureTracked(provider, path, initial);
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::RequireTracked;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    request.CanonicalBytes = desired;
    ReentrantSourceSaveCallback callback(transaction, path, external);
    SourceSaveResult result;
    CHECK(transaction.Commit(request, result, diagnostic, &callback));
    CHECK_FALSE(callback.CaptureFailed);
    CHECK(result.Outcome == SourceSaveOutcome::Conflict);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceBusy);

    Array<byte> saved;
    REQUIRE_FALSE(File::ReadAllBytes(path, saved));
    REQUIRE(saved.Count() == external.Length());
    CHECK(Platform::MemoryCompare(saved.Get(), external.Get(), saved.Count()) == 0);
}

TEST_CASE("Source save transactions revalidate destination after durable staging")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi desired("{\n  \"value\": 2\n}\n");
    const StringAnsi external("{\n  \"external\": true\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    provider.HasTracked = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::AllowUnregistered;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    request.CanonicalBytes = desired;
    MutateBeforeReplaceSourceSaveHook hook(path, external);
    SourceSaveResult result;
    CHECK(transaction.Commit(request, result, diagnostic, nullptr, &hook));
    CHECK_FALSE(hook.MutationFailed);
    CHECK(result.Outcome == SourceSaveOutcome::Conflict);
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SourceBusy);

    Array<byte> saved;
    REQUIRE_FALSE(File::ReadAllBytes(path, saved));
    REQUIRE(saved.Count() == external.Length());
    CHECK(Platform::MemoryCompare(saved.Get(), external.Get(), saved.Count()) == 0);
    const String staging = path + TEXT(".stage-") + result.TransactionID.ToString(Guid::FormatType::N);
    CHECK_FALSE(FileSystem::FileExists(staging));
}

TEST_CASE("Source save failure injection preserves old bytes and removes staging")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    const StringAnsi initial("{\n  \"value\": 1\n}\n");
    const StringAnsi desired("{\n  \"value\": 2\n}\n");
    REQUIRE_FALSE(File::WriteAllBytes(path, initial.Get(), initial.Length()));

    TestSourceSaveRevisionProvider provider;
    provider.HasTracked = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::AllowUnregistered;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    request.CanonicalBytes = desired;

    SourceSaveFailurePoint point = SourceSaveFailurePoint::AfterStagingWrite;
    SECTION("before staging")
    {
        point = SourceSaveFailurePoint::BeforeStagingWrite;
    }
    SECTION("after staging")
    {
        point = SourceSaveFailurePoint::AfterStagingWrite;
    }
    SECTION("before replace")
    {
        point = SourceSaveFailurePoint::BeforeReplace;
    }
    SinglePointSourceSaveFailure injector(point);
    SourceSaveResult result;
    CHECK(transaction.Commit(request, result, diagnostic, nullptr, &injector));
    CHECK(result.Outcome == SourceSaveOutcome::Failed);

    Array<byte> saved;
    REQUIRE_FALSE(File::ReadAllBytes(path, saved));
    REQUIRE(saved.Count() == initial.Length());
    CHECK(Platform::MemoryCompare(saved.Get(), initial.Get(), saved.Count()) == 0);
    const String staging = path + TEXT(".stage-") + result.TransactionID.ToString(Guid::FormatType::N);
    CHECK_FALSE(FileSystem::FileExists(staging));
}

TEST_CASE("Source save transactions explicitly create unregistered sources")
{
    const String path = MakeSourceSaveTestPath();
    SCOPE_EXIT { FileSystem::DeleteFile(path); };
    FileSystem::DeleteFile(path);
    TestSourceSaveRevisionProvider provider;
    provider.HasTracked = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRequest request;
    request.RegistrationMode = SourceSaveRegistrationMode::AllowUnregistered;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(transaction.Capture(path, request.RegistrationMode, request.Expected, diagnostic));
    CHECK_FALSE(request.Expected.Exists);
    CHECK_FALSE(request.Expected.IsTracked);
    request.CanonicalBytes = "{\n  \"created\": true\n}\n";
    SourceSaveResult result;
    REQUIRE_FALSE(transaction.Commit(request, result, diagnostic));
    CHECK(result.Outcome == SourceSaveOutcome::Committed);
    CHECK(FileSystem::FileExists(path));
}

TEST_CASE("Untracked local saves cannot address authored source roots")
{
    TestSourceSaveRevisionProvider provider;
    provider.Available = false;
    SourceSaveTransaction transaction(&provider);
    SourceSaveRevision revision;
    AssetPipelineDiagnostic diagnostic;
    if (Globals::ProjectContentFolder.HasChars())
    {
        String contentPath = Globals::ProjectContentFolder / TEXT("LocalSaveMustReject.json");
        contentPath.Replace('\\', '/');
        CHECK(transaction.Capture(contentPath, SourceSaveRegistrationMode::UntrackedLocal, revision, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    }
    if (Globals::ProjectFolder.HasChars())
    {
        String actorPath = Globals::ProjectFolder / TEXT("ExternalActors/LocalSaveMustReject.json");
        actorPath.Replace('\\', '/');
        CHECK(transaction.Capture(actorPath, SourceSaveRegistrationMode::UntrackedLocal, revision, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    }
}
