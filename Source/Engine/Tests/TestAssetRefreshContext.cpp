// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/Importing/AssetRefreshCoordinator.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Asset change sets round-trip framed refresh context")
{
    AssetChangeSet expected;
    expected.Revision = 17;
    expected.RefreshId = Guid::New();
    expected.Pass = 3;
    expected.Added.Add({ Guid::New(), TEXT("Content/Payload.bin") });
    expected.Moved.Add({ expected.Added[0].AssetGuid, TEXT("Content/Payload.bin"), TEXT("Content/Moved.bin") });

    Array<byte> bytes;
    expected.Serialize(bytes);
    AssetChangeSet actual;
    REQUIRE_FALSE(AssetChangeSet::Deserialize(bytes.Get(), bytes.Count(), actual));
    CHECK(actual.Revision == expected.Revision);
    CHECK(actual.RefreshId == expected.RefreshId);
    CHECK(actual.Pass == expected.Pass);
    CHECK(actual.Added.Count() == 1);
    CHECK(actual.Moved.Count() == 1);

    // Removing the explicit magic/version framing must never be interpreted as a legacy payload.
    REQUIRE(bytes.Count() > 8);
    CHECK(AssetChangeSet::Deserialize(bytes.Get() + 8, bytes.Count() - 8, actual));
}

TEST_CASE("Asset refresh keeps one ID across passes and persists a terminal session")
{
    const String library = Globals::TemporaryFolder / (TEXT("AssetRefreshContext-") +
        Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(library, true); };

    AssetPipelineDiagnostic diagnostic;
    AssetDatabase database;
    REQUIRE_FALSE(database.Open(library, Guid::New(), diagnostic));
    AssetImporterRegistry importers;
    AssetImportPlanner planner(importers);
    AssetPostprocessorRegistry postprocessors;
    AssetRefreshCoordinator coordinator(importers, planner, postprocessors, 4);
    Array<Guid> observedIds;
    Array<int32> observedPasses;

    AssetRefreshCallbacks callbacks;
    callbacks.Session = [&](const AssetRefreshResult& refresh, AssetRefreshRunState state,
        const AssetPipelineDiagnostic&, AssetPipelineDiagnostic& localDiagnostic)
    {
        SourceRefreshSessionRow session;
        if (state == AssetRefreshRunState::Started)
        {
            session.RefreshId = refresh.RefreshId;
            session.StartingRevision = database.GetRevision();
            session.Reason = TEXT("Test");
            session.Status = TEXT("Running");
            session.StartedUtcTicks = DateTime::NowUTC().Ticks;
        }
        else
        {
            const AssetDatabaseReadSnapshot snapshot = database.GetDurableSnapshot();
            if (!snapshot.TryGetRefreshSession(refresh.RefreshId, session))
                return true;
            session.EndingRevision = snapshot.GetRevision();
            session.IterationCount = refresh.Iterations;
            session.Status = state == AssetRefreshRunState::Succeeded ? TEXT("Completed") : TEXT("Failed");
            session.CompletedUtcTicks = DateTime::NowUTC().Ticks;
        }
        return database.RecordRefreshSession(session, refresh.Pass, localDiagnostic);
    };
    callbacks.Reconcile = [&](const AssetRefreshIterationContext& context, Array<AssetImportPlanRequest>&,
        bool& sourceChanged, AssetPipelineDiagnostic&)
    {
        observedIds.Add(context.RefreshId);
        observedPasses.Add(context.Pass);
        CHECK(context.Iteration == context.Pass);
        sourceChanged = context.Pass == 1;
        return false;
    };
    callbacks.Execute = [&](const AssetRefreshIterationContext& context, const Array<AssetImportPlan>&,
        Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)
    {
        observedIds.Add(context.RefreshId);
        observedPasses.Add(context.Pass);
        return false;
    };

    AssetRefreshResult result;
    REQUIRE_FALSE(coordinator.Refresh(AssetRefreshReason::Explicit, callbacks, result, diagnostic));
    REQUIRE(result.RefreshId.IsValid());
    CHECK(result.Pass == 2);
    CHECK(result.Iterations == 2);
    REQUIRE(observedIds.Count() == 3);
    CHECK(observedIds[0] == result.RefreshId);
    CHECK(observedIds[1] == result.RefreshId);
    CHECK(observedIds[2] == result.RefreshId);
    CHECK(observedPasses[0] == 1);
    CHECK(observedPasses[1] == 2);
    CHECK(observedPasses[2] == 2);

    const AssetDatabaseReadSnapshot snapshot = database.GetDurableSnapshot();
    SourceRefreshSessionRow session;
    REQUIRE(snapshot.TryGetRefreshSession(result.RefreshId, session));
    CHECK(session.Status == TEXT("Completed"));
    CHECK(session.IterationCount == 2);
    CHECK(session.StartedUtcTicks != 0);
    CHECK(session.CompletedUtcTicks >= session.StartedUtcTicks);
    CHECK(session.StartingRevision == 0);
    CHECK(session.EndingRevision == 1);

    Array<AssetChangeSet> changes;
    bool requiresSnapshot = false;
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 2);
    CHECK(changes[0].RefreshId == result.RefreshId);
    CHECK(changes[0].Pass == 0);
    CHECK(changes[1].RefreshId == result.RefreshId);
    CHECK(changes[1].Pass == 2);
    REQUIRE_FALSE(database.Close(&diagnostic));
}
