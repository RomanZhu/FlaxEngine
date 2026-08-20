// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    AssetRecord MakeDatabaseRecord(const Guid& id, const Guid& sourceId, const StringView& path, const StringView& key = StringView::Empty)
    {
        AssetRecord result;
        result.ID = id;
        result.SourceAssetID = sourceId;
        result.TypeName = key.IsEmpty() ? TEXT("FlaxEngine.Texture") : TEXT("FlaxEngine.Model");
        result.CanonicalPath = CanonicalAssetPath(path);
        result.SourcePath = SourceFilePath(path);
        result.MetaPath = MetaFilePath(String(path) + TEXT(".meta"));
        result.SubAsset = SubAssetKey(key);
        result.ProcessorID = TEXT("Flax.Test");
        result.PortabilityKey = String(path).ToLower();
        result.BuildInputDependencies.Add(Guid(91, 92, 93, 94));
        return result;
    }

    AssetMeta MakeDatabaseMeta(const Guid& id)
    {
        AssetMeta meta;
        meta.ID = id;
        meta.AssetType = TEXT("FlaxEngine.Texture");
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = TEXT("Flax.Texture");
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}";
        return meta;
    }
}

TEST_CASE("Asset database publishes coherent indexed immutable snapshots")
{
    AssetDatabase database;
    const Guid rootId(1, 2, 3, 4);
    const Guid subId(5, 6, 7, 8);
    const String path = Globals::ProjectContentFolder / TEXT("Database/Test.png");
    Array<AssetRecord> records;
    records.Add(MakeDatabaseRecord(rootId, rootId, path));
    records.Add(MakeDatabaseRecord(subId, rootId, path, TEXT("mesh:/Body")));

    int32 eventCount = 0;
    uint64 eventRevision = 0;
    database.Changed.Bind([&](const AssetDatabaseChangeBatch& change)
    {
        if (eventCount == 0)
            CHECK(change.Added.Count() == 2);
        eventCount++;
        eventRevision = change.Revision;
    });
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    CHECK(eventCount == 1);
    CHECK(eventRevision == database.GetRevision());

    AssetRecord found;
    CHECK(database.TryGetRecord(rootId, found));
    CHECK(found.DatabaseRevision == eventRevision);
    CHECK(database.TryGetMainRecordByPath(String(path).ToLower(), found));
    Array<AssetRecord> query;
    database.GetSubAssets(rootId, query);
    REQUIRE(query.Count() == 1);
    CHECK(query[0].ID == subId);
    database.GetByProcessor(TEXT("Flax.Test"), query);
    CHECK(query.Count() == 2);
    database.GetBuildDependants(Guid(91, 92, 93, 94), query);
    CHECK(query.Count() == 2);

    const uint64 rootRecordRevision = found.DatabaseRevision;
    const uint64 unchangedSnapshotRevision = database.GetRevision();
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    CHECK(database.GetRevision() == unchangedSnapshotRevision + 1);
    REQUIRE(database.TryGetRecord(rootId, found));
    CHECK(found.DatabaseRevision == rootRecordRevision);

    AssetDatabaseSnapshot snapshot = database.GetSnapshot();
    const uint64 oldRevision = snapshot.Revision;
    records[0].Status = AssetRecordStatus::Building;
    records.RemoveAt(1);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    CHECK(database.GetRevision() == oldRevision + 1);
    CHECK(snapshot.Records.Count() == 2);
    CHECK(snapshot.Records[0].DatabaseRevision == rootRecordRevision);
}

TEST_CASE("Asset database detects portable main path collisions")
{
    AssetDatabase database;
    Array<AssetRecord> records;
    records.Add(MakeDatabaseRecord(Guid(11, 12, 13, 14), Guid(11, 12, 13, 14), TEXT("C:/Project/Content/A.png")));
    records.Add(MakeDatabaseRecord(Guid(21, 22, 23, 24), Guid(21, 22, 23, 24), TEXT("C:/Project/Content/a.png")));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    Array<AssetRecord> collisions;
    database.GetByStatus(AssetRecordStatus::PathCollision, collisions);
    CHECK(collisions.Count() == 2);
}

TEST_CASE("Asset database scan pairs exact sidecars and diagnoses duplicates orphans and missing metadata")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseScan-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid sharedId(31, 32, 33, 34);
    const String first = content / TEXT("First.png");
    const String second = content / TEXT("Second.png");
    const String missing = content / TEXT("Missing.png");
    const String orphanMeta = content / TEXT("Orphan.png.meta");
    REQUIRE_FALSE(File::WriteAllText(first, TEXT("one"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(second, TEXT("two"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(missing, TEXT("missing"), Encoding::ANSI));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(first + TEXT(".meta"), MakeDatabaseMeta(sharedId), diagnostic));
    REQUIRE_FALSE(AssetMeta::SaveAtomic(second + TEXT(".meta"), MakeDatabaseMeta(sharedId), diagnostic));
    REQUIRE_FALSE(AssetMeta::SaveAtomic(orphanMeta, MakeDatabaseMeta(Guid(41, 42, 43, 44)), diagnostic));

    AssetDatabase database;
    AssetDatabaseScanOptions options;
    options.StrictMetadata = true;
    AssetDatabaseScanResult scan;
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, database, scan));
    CHECK(scan.Revision == 1);
    bool duplicate = false;
    bool missingMeta = false;
    bool orphan = false;
    for (const AssetPipelineDiagnostic& item : scan.Diagnostics)
    {
        duplicate |= item.Code == AssetPipelineDiagnosticCode::DuplicateGuid;
        missingMeta |= item.Code == AssetPipelineDiagnosticCode::MissingMeta;
        orphan |= item.Code == AssetPipelineDiagnosticCode::SourceMissing && item.SourcePath.EndsWith(TEXT("Orphan.png"));
    }
    CHECK(duplicate);
    CHECK(missingMeta);
    CHECK(orphan);
    AssetRecord record;
    REQUIRE(database.TryGetRecord(sharedId, record));
    CHECK(record.Status == AssetRecordStatus::DuplicateGuid);
    REQUIRE(database.TryGetRecord(Guid(41, 42, 43, 44), record));
    CHECK(record.Status == AssetRecordStatus::OrphanMeta);
}

TEST_CASE("Asset database snapshot is disposable checksummed and project scoped")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseSnapshot-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetDatabase source;
    Array<AssetRecord> records;
    const Guid id(51, 52, 53, 54);
    records.Add(MakeDatabaseRecord(id, id, content / TEXT("Warm.png")));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(source.PublishFullSnapshot(records, diagnostic));
    Array<AssetDatabaseFileState> states;
    AssetDatabaseFileState state;
    state.Path = content / TEXT("Warm.png");
    state.Size = 123;
    state.LastWriteTicks = 456;
    state.VolumeIdentity = 12;
    state.FileIdentity = 34;
    state.ChangeTicks = 567;
    state.IdentityReliable = true;
    state.CachedContentHash = ContentHash::Compute("warm", 4);
    state.CacheChecksum = 789;
    states.Add(state);
    const String path = library / TEXT("index.bin");
    REQUIRE_FALSE(AssetDatabaseSnapshotStore::SaveAtomic(path, root, content, source.GetSnapshot(), states, diagnostic));

    AssetDatabase loaded;
    Array<AssetDatabaseFileState> loadedStates;
    REQUIRE_FALSE(AssetDatabaseSnapshotStore::Load(path, root, content, loaded, loadedStates, diagnostic));
    AssetRecord found;
    CHECK(loaded.TryGetRecord(id, found));
    REQUIRE(loadedStates.Count() == 1);
    CHECK(loadedStates[0].CachedContentHash == state.CachedContentHash);
    CHECK(loadedStates[0].CacheChecksum == 789);
    CHECK(AssetDatabaseSnapshotStore::Load(path, root + TEXT("-other"), content, loaded, loadedStates, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SnapshotInvalid);

    BytesContainer bytes;
    REQUIRE_FALSE(File::ReadAllBytes(path, bytes));
    bytes.Get()[bytes.Length() - 1] ^= 0xff;
    REQUIRE_FALSE(File::WriteAllBytes(path, bytes.Get(), bytes.Length()));
    CHECK(AssetDatabaseSnapshotStore::Load(path, root, content, loaded, loadedStates, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SnapshotInvalid);
}

#endif
