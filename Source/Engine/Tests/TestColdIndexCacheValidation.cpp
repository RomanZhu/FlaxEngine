// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && USE_EDITOR

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    struct ColdIndexSource
    {
        String Path;
        Guid MainId;
        Guid ChildId;
        String Processor;
        String Type;
    };

    void WriteColdIndexSource(const ColdIndexSource& source)
    {
        const char payload[] = "reduced production validation source";
        REQUIRE_FALSE(File::WriteAllBytes(source.Path, payload, ARRAY_COUNT(payload) - 1));

        AssetMeta meta;
        meta.ID = source.MainId;
        meta.AssetType = source.Type;
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = source.Processor;
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}";
        if (source.ChildId.IsValid())
        {
            SubAssetMeta child;
            child.ID = source.ChildId;
            child.LocalId = 2;
            child.TypeName = TEXT("FlaxEngine.Model");
            child.DisplayName = TEXT("FixtureTriangle");
            meta.SubAssets.Add(TEXT("mesh:/FixtureTriangle"), child);
        }
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(AssetMeta::SaveAtomic(source.Path + TEXT(".meta"), meta, diagnostic));
    }

    void RequireCohortIdentity(AssetDatabase& database, const Array<ColdIndexSource>& sources)
    {
        for (const ColdIndexSource& source : sources)
        {
            AssetRecord record;
            REQUIRE(database.TryGetRecord(source.MainId, record));
            CHECK(record.ID == source.MainId);
            CHECK(record.SourceAssetID == source.MainId);
            CHECK(record.ProcessorID == source.Processor);
            CHECK(record.TypeName == source.Type);
            CHECK(record.Status == AssetRecordStatus::Ready);
            if (source.ChildId.IsValid())
            {
                REQUIRE(database.TryGetRecord(source.ChildId, record));
                CHECK(record.SourceAssetID == source.MainId);
                CHECK(record.SubAsset.Get() == TEXT("mesh:/FixtureTriangle"));
                CHECK(record.Status == AssetRecordStatus::Ready);
            }
        }
    }
}

TEST_CASE("Cold Library rebuild retains deterministic identity and reuses unchanged import cache")
{
    const double started = Platform::GetTimeSeconds();
    const String root = Globals::TemporaryFolder / (Guid::New().ToString(Guid::FormatType::N) + TEXT("-Asset81"));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    Array<ColdIndexSource> sources;
    sources.Add({ content / TEXT("Fixture.png"), Guid(8101, 1, 1, 1), Guid::Empty,
        TEXT("Flax.Texture"), TEXT("FlaxEngine.Texture") });
    sources.Add({ content / TEXT("Fixture.glb"), Guid(8102, 2, 2, 2), Guid(8103, 3, 3, 3),
        TEXT("Flax.Model"), TEXT("FlaxEngine.Model") });
    sources.Add({ content / TEXT("Fixture.prefab"), Guid(8104, 4, 4, 4), Guid::Empty,
        TEXT("Flax.JsonDocument"), TEXT("FlaxEngine.Prefab") });
    for (const ColdIndexSource& source : sources)
        WriteColdIndexSource(source);

    Array<String> warningOrErrorOutput;
    Delegate<LogType, const StringView&>::FunctionType logHandler = [&warningOrErrorOutput](LogType type, const StringView& message)
    {
        if (type == LogType::Warning || type == LogType::Error || type == LogType::Fatal)
            warningOrErrorOutput.Add(String(message));
    };
    Log::Logger::OnMessage.Bind(logHandler);
    SCOPE_EXIT { Log::Logger::OnMessage.Unbind(logHandler); };

    const Guid projectId(8181, 81, 81, 81);
    AssetPipelineDiagnostic diagnostic;
    AssetDatabaseScanOptions options;
    options.StrictMetadata = true;
    AssetDatabaseScanResult scan;

    // The first scan starts with no Library and establishes the canonical identity cohort.
    REQUIRE_FALSE(FileSystem::DirectoryExists(library));
    AssetDatabase cold;
    REQUIRE_FALSE(cold.Open(library, projectId, diagnostic));
    SourceHashCache coldHashes;
    options.HashCache = &coldHashes;
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, cold, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    RequireCohortIdentity(cold, sources);
    const Array<AssetDatabaseFileState> coldFileStates = scan.FileStates;
    REQUIRE_FALSE(cold.Close(&diagnostic));

    // Removing only Library forces a complete index rebuild from unchanged canonical sources.
    REQUIRE_FALSE(FileSystem::DeleteDirectory(library, true));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    AssetDatabase rebuilt;
    REQUIRE_FALSE(rebuilt.Open(library, projectId, diagnostic));
    SourceHashCache rebuildHashes;
    options.HashCache = &rebuildHashes;
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, rebuilt, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    RequireCohortIdentity(rebuilt, sources);
    const Array<AssetDatabaseFileState> rebuiltFileStates = scan.FileStates;

    // Model successful imports with exact durable publications for the reduced root cohort.
    for (int32 i = 0; i < sources.Count(); i++)
    {
        SourceAssetPublicationRow publication;
        publication.AssetGuid = sources[i].MainId;
        publication.ObjectGuid = sources[i].MainId;
        publication.LocalFileId = 1;
        publication.TargetId = TEXT("Windows-x64-ASSET81");
        publication.Artifact = ArtifactKey(ContentHash::Compute(&i, sizeof(i)));
        publication.IsLastKnownGood = true;
        Array<SourceAssetDependencyRow> dependencies;
        REQUIRE_FALSE(rebuilt.RecordPublication(publication, dependencies, diagnostic));
    }
    const uint64 cachedRevision = rebuilt.GetRevision();
    REQUIRE(rebuilt.GetDurableSnapshot().GetState().Publications.Count() == sources.Count());
    REQUIRE_FALSE(rebuilt.Close(&diagnostic));

    // Restart and re-add the same root. Stable file identity avoids source hashing and import work.
    AssetDatabase restarted;
    REQUIRE_FALSE(restarted.Open(library, projectId, diagnostic));
    CHECK(restarted.GetRevision() == cachedRevision);
    SourceHashCache restartedHashes;
    restartedHashes.Seed(rebuiltFileStates);
    options.HashCache = &restartedHashes;
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, restarted, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, restarted, scan));
    CHECK(scan.Diagnostics.IsEmpty());
    CHECK(restarted.GetRevision() == cachedRevision);
    RequireCohortIdentity(restarted, sources);
    CHECK(restarted.GetDurableSnapshot().GetState().Publications.Count() == sources.Count());
    const SourceHashMetrics restartMetrics = restartedHashes.GetMetrics();
    CHECK(restartMetrics.CacheHits >= rebuiltFileStates.Count());
    CHECK(restartMetrics.CacheMisses == 0);
    CHECK(restartMetrics.BytesHashed == 0);
    Array<AssetChangeSet> changes;
    bool requiresSnapshot = false;
    REQUIRE_FALSE(restarted.ReadChangesAfter(cachedRevision, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    CHECK(changes.IsEmpty());
    REQUIRE_FALSE(restarted.Close(&diagnostic));

    CHECK(coldFileStates.Count() == rebuiltFileStates.Count());
    CHECK(Platform::GetTimeSeconds() - started < 5.0 * 60.0);
    CHECK(warningOrErrorOutput.IsEmpty());
}

#endif
