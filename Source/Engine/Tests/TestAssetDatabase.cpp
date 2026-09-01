// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Content/Storage/FlaxStorage.h"
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
        result.LocalId = key.IsEmpty() ? 1 : 2;
        result.TypeName = key.IsEmpty() ? TEXT("FlaxEngine.Texture") : TEXT("FlaxEngine.Model");
        result.CanonicalPath = CanonicalAssetPath(path);
        result.SourcePath = SourceFilePath(path);
        result.MetaPath = MetaFilePath(String(path) + TEXT(".meta"));
        result.SubAsset = SubAssetKey(key);
        result.ProcessorID = TEXT("Flax.Test");
        result.PortabilityKey = String(path).ToLower();
        result.BuildInputDependencies.Add(AssetObjectId::Main(AssetGuid(Guid(91, 92, 93, 94))));
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
    records[0].Labels.Add(TEXT("Environment"));

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
    AssetRecordQuery composite;
    composite.Name = TEXT("test");
    composite.PathPrefix = Globals::ProjectContentFolder / TEXT("Database");
    composite.TypeName = TEXT("Texture");
    composite.ProcessorId = TEXT("Flax.Test");
    composite.Label = TEXT("environment");
    composite.MainAssetsOnly = true;
    database.QueryRecords(composite, query);
    REQUIRE(query.Count() == 1);
    CHECK(query[0].ID == rootId);

    const uint64 rootRecordRevision = found.DatabaseRevision;
    const uint64 unchangedSnapshotRevision = database.GetRevision();
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    CHECK(database.GetRevision() == unchangedSnapshotRevision);
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

TEST_CASE("Asset database durable republish is a no-op and preserves publications")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseIncrementalPublish-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid projectId = Guid::New();
    const Guid sourceId = Guid::New();
    const Guid subObjectId = Guid::New();
    AssetDatabase database;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.Open(library, projectId, diagnostic));
    Array<AssetRecord> records;
    records.Add(MakeDatabaseRecord(sourceId, sourceId, root / TEXT("Body.fbx")));
    records.Add(MakeDatabaseRecord(subObjectId, sourceId, root / TEXT("Body.fbx"), TEXT("mesh:/Body")));
    Array<AssetPipelineDiagnostic> diagnostics;
    AssetPipelineDiagnostic scanDiagnostic;
    scanDiagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
    scanDiagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
    scanDiagnostic.AssetGuid = sourceId;
    scanDiagnostic.SourcePath = root / TEXT("Body.fbx");
    scanDiagnostic.Message = TEXT("Stable test diagnostic.");
    diagnostics.Add(scanDiagnostic);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostics, diagnostic));

    const uint64 stableRevision = database.GetRevision();
    const String walPath = library / TEXT("AssetDatabase/normalized-store.wal");
    const uint64 stableWalSize = FileSystem::GetFileSize(walPath);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostics, diagnostic));
    CHECK(database.GetRevision() == stableRevision);
    CHECK(FileSystem::GetFileSize(walPath) == stableWalSize);
    Array<AssetChangeSet> changes;
    bool requiresSnapshot = false;
    REQUIRE_FALSE(database.ReadChangesAfter(stableRevision, changes, requiresSnapshot, diagnostic));
    CHECK_FALSE(requiresSnapshot);
    CHECK(changes.IsEmpty());

    SourceAssetPublicationRow mainPublication;
    mainPublication.AssetGuid = sourceId;
    mainPublication.LocalFileId = 1;
    mainPublication.TargetId = TEXT("Windows-x64");
    mainPublication.Artifact = ArtifactKey(ContentHash::Compute("main-publication", 16));
    mainPublication.IsLastKnownGood = true;
    SourceAssetDependencyRow dependency;
    dependency.OwnerAssetGuid = sourceId;
    dependency.OwnerLocalFileId = 1;
    dependency.TargetId = mainPublication.TargetId;
    dependency.Kind = AssetDependencyKind::RuntimeReference;
    dependency.TargetAssetGuid = sourceId;
    dependency.TargetLocalFileId = 2;
    dependency.CustomDependency = AssetObjectId(AssetGuid(sourceId), 2).ToString();
    Array<SourceAssetDependencyRow> publicationDependencies;
    publicationDependencies.Add(dependency);
    REQUIRE_FALSE(database.RecordPublication(mainPublication, publicationDependencies, diagnostic));
    SourceAssetPublicationRow subPublication = mainPublication;
    subPublication.LocalFileId = 2;
    subPublication.Artifact = ArtifactKey(ContentHash::Compute("sub-publication", 15));
    Array<SourceAssetDependencyRow> noDependencies;
    REQUIRE_FALSE(database.RecordPublication(subPublication, noDependencies, diagnostic));

    const uint64 publicationRevision = database.GetRevision();
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostics, diagnostic));
    CHECK(database.GetRevision() == publicationRevision);
    CHECK(database.GetDurableSnapshot().GetState().Publications.Count() == 2);

    records.RemoveAt(1);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostics, diagnostic));
    const AssetDatabaseReadSnapshot durableSnapshot = database.GetDurableSnapshot();
    const SourceAssetDatabaseState& state = durableSnapshot.GetState();
    REQUIRE(state.Publications.Count() == 1);
    CHECK(state.Publications[0].LocalFileId == 1);
    CHECK(state.Dependencies.IsEmpty());
    changes.Clear();
    REQUIRE_FALSE(database.ReadChangesAfter(publicationRevision, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    CHECK(changes[0].ObjectsChanged.Count() == 1);
    CHECK(changes[0].DiagnosticsChanged.IsEmpty());
    REQUIRE_FALSE(database.Close(&diagnostic));
}

TEST_CASE("Asset database incremental publication keeps WAL growth bounded")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseBoundedWal-") + Guid::New().ToString(Guid::FormatType::N));
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    AssetDatabase database;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.Open(library, Guid::New(), diagnostic));
    const String walPath = library / TEXT("AssetDatabase/normalized-store.wal");
    const uint64 emptyWalSize = FileSystem::GetFileSize(walPath);
    Array<AssetRecord> records;
    for (int32 i = 0; i < 80; i++)
    {
        const Guid id(i + 1, i + 101, i + 201, i + 301);
        records.Add(MakeDatabaseRecord(id, id, root / (StringUtils::ToString(i) + TEXT(".png"))));
    }
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    const uint64 fullGrowth = FileSystem::GetFileSize(walPath) - emptyWalSize;
    records[0].Labels.Add(TEXT("Changed"));
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    const uint64 incrementalGrowth = FileSystem::GetFileSize(walPath) - emptyWalSize - fullGrowth;
    CHECK(incrementalGrowth < 4096);
    CHECK(incrementalGrowth * 8 < fullGrowth);
    REQUIRE_FALSE(database.Close(&diagnostic));
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
    const String missingText = content / TEXT("Notes.txt");
    const String orphanMeta = content / TEXT("Orphan.png.meta");
    REQUIRE_FALSE(File::WriteAllText(first, TEXT("one"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(second, TEXT("two"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(missing, TEXT("missing"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(missingText, TEXT("notes"), Encoding::ANSI));
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
    bool missingTextMeta = false;
    bool orphan = false;
    for (const AssetPipelineDiagnostic& item : scan.Diagnostics)
    {
        duplicate |= item.Code == AssetPipelineDiagnosticCode::DuplicateGuid;
        missingMeta |= item.Code == AssetPipelineDiagnosticCode::MissingMeta;
        missingTextMeta |= item.Code == AssetPipelineDiagnosticCode::MissingMeta && item.SourcePath.EndsWith(TEXT("Notes.txt"));
        orphan |= item.Code == AssetPipelineDiagnosticCode::SourceMissing && item.SourcePath.EndsWith(TEXT("Orphan.png"));
    }
    CHECK(duplicate);
    CHECK(missingMeta);
    CHECK(missingTextMeta);
    CHECK(orphan);
    AssetRecord record;
    REQUIRE(database.TryGetRecord(sharedId, record));
    CHECK(record.Status == AssetRecordStatus::DuplicateGuid);
    REQUIRE(database.TryGetRecord(Guid(41, 42, 43, 44), record));
    CHECK(record.Status == AssetRecordStatus::OrphanMeta);
}

TEST_CASE("Default canonical metadata supports text sources")
{
    const String root = Globals::TemporaryFolder / (TEXT("TextMetadataBatch-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String source = root / TEXT("Notes.txt");
    const String staging = root / TEXT("Notes.txt.staged-meta");
    REQUIRE_FALSE(File::WriteAllText(source, TEXT("notes"), Encoding::ANSI));
    Array<String> sources;
    sources.Add(source);
    Array<String> stagingPaths;
    stagingPaths.Add(staging);

    const Array<Guid> ids = AssetOperationService::StageDefaultMetadataBatch(sources, stagingPaths);
    REQUIRE(ids.Count() == 1);
    REQUIRE(ids[0].IsValid());
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::Load(staging, meta, diagnostic));
    CHECK(meta.ID == ids[0]);
    CHECK(meta.AssetType == RawDataAsset::TypeName);
    CHECK(meta.SourceKind == AssetSourceKind::TextDocument);
    CHECK(meta.Processor.ID == TEXT("Flax.Text"));
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

TEST_CASE("Asset database CollectFromFiles indexes an explicit file list")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseCollectFromFiles-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid firstId = Guid::New();
    const Guid secondId = Guid::New();
    const String first = content / TEXT("First.png");
    const String second = content / TEXT("Second.png");
    REQUIRE_FALSE(File::WriteAllText(first, TEXT("one"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(second, TEXT("two"), Encoding::ANSI));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(first + TEXT(".meta"), MakeDatabaseMeta(firstId), diagnostic));
    REQUIRE_FALSE(AssetMeta::SaveAtomic(second + TEXT(".meta"), MakeDatabaseMeta(secondId), diagnostic));

    AssetDatabaseScanOptions options;
    AssetDatabaseSnapshot previous;
    Array<AssetRecord> allRecords;
    AssetDatabaseScanResult allScan;
    REQUIRE_FALSE(AssetDatabaseScanner::Collect(root, content, library, options, previous, allRecords, allScan));
    REQUIRE(allRecords.Count() == 2);

    Array<String> subset;
    subset.Add(first);
    subset.Add(first + TEXT(".meta"));
    Array<AssetRecord> subsetRecords;
    AssetDatabaseScanResult subsetScan;
    REQUIRE_FALSE(AssetDatabaseScanner::CollectFromFiles(root, content, library, subset, options, previous, subsetRecords, subsetScan));
    REQUIRE(subsetRecords.Count() == 1);
    CHECK(subsetRecords[0].ID == firstId);

    bool matched = false;
    for (const AssetRecord& record : allRecords)
    {
        if (record.ID != firstId)
            continue;
        CHECK(record.MetaSemanticHash == subsetRecords[0].MetaSemanticHash);
        CHECK(record.Status == subsetRecords[0].Status);
        CHECK(FileSystem::AreFilePathsEquivalent(record.SourcePath.Get(), subsetRecords[0].SourcePath.Get()));
        matched = true;
    }
    CHECK(matched);
}

TEST_CASE("Asset database RefreshSources patches known writes without a full scan")
{
    const String folder = Globals::ProjectContentFolder / (TEXT("__RefreshSources_") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(folder));
    Array<String> cleanup;
    cleanup.Add(folder);
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(folder, true);
        AssetPipelineService::RefreshSources(cleanup);
    };

    const Guid firstId = Guid::New();
    const Guid secondId = Guid::New();
    const String first = folder / TEXT("First.png");
    const String second = folder / TEXT("Second.png");
    REQUIRE_FALSE(File::WriteAllText(first, TEXT("one"), Encoding::ANSI));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(first + TEXT(".meta"), MakeDatabaseMeta(firstId), diagnostic));

    Array<String> refresh;
    refresh.Add(first);
    const uint64 revisionBeforeAdd = AssetDatabase::Get().GetRevision();
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    CHECK(AssetDatabase::Get().GetRevision() == revisionBeforeAdd + 1);
    AssetRecord found;
    REQUIRE(AssetDatabase::Get().TryGetRecord(firstId, found));
    CHECK(FileSystem::AreFilePathsEquivalent(found.SourcePath.Get(), first));
    const uint64 firstRecordRevision = found.DatabaseRevision;
    CHECK(AssetDatabaseQueryService::GetLastChange().Added.Contains(firstId));
    CHECK(AssetDatabaseQueryService::GetLastChange().Added.Count() == 1);

    Array<String> subset;
    subset.Add(first);
    subset.Add(first + TEXT(".meta"));
    Array<AssetRecord> collected;
    AssetDatabaseScanResult collectedScan;
    AssetDatabaseScanOptions options;
    REQUIRE_FALSE(AssetDatabaseScanner::CollectFromFiles(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder,
        subset, options, AssetDatabase::Get().GetSnapshot(), collected, collectedScan));
    REQUIRE(collected.Count() == 1);
    CHECK(collected[0].ID == firstId);
    CHECK(collected[0].MetaSemanticHash == found.MetaSemanticHash);

    REQUIRE_FALSE(File::WriteAllText(first, TEXT("overwrite"), Encoding::ANSI));
    const uint64 revisionBeforeOverwrite = AssetDatabase::Get().GetRevision();
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    CHECK(AssetDatabase::Get().GetRevision() == revisionBeforeOverwrite);
    REQUIRE(AssetDatabase::Get().TryGetRecord(firstId, found));
    CHECK(found.DatabaseRevision == firstRecordRevision);

    REQUIRE_FALSE(File::WriteAllText(second, TEXT("two"), Encoding::ANSI));
    REQUIRE_FALSE(AssetMeta::SaveAtomic(second + TEXT(".meta"), MakeDatabaseMeta(secondId), diagnostic));
    refresh.Clear();
    refresh.Add(second);
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    CHECK(AssetDatabaseQueryService::GetLastChange().Added.Contains(secondId));
    CHECK(AssetDatabaseQueryService::GetLastChange().Added.Count() == 1);
    REQUIRE(AssetDatabase::Get().TryGetRecord(secondId, found));

    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Load(first + TEXT(".meta"), meta, diagnostic));
    REQUIRE(AssetDatabase::Get().TryGetRecord(firstId, found));
    const uint64 previousHash = found.MetaSemanticHash;
    meta.Processor.SettingsJson = "{\"refresh\":1}\n";
    REQUIRE_FALSE(AssetMeta::SaveAtomic(first + TEXT(".meta"), meta, diagnostic));
    refresh.Clear();
    refresh.Add(first);
    const uint64 revisionBeforeMeta = AssetDatabase::Get().GetRevision();
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    CHECK(AssetDatabase::Get().GetRevision() == revisionBeforeMeta + 1);
    CHECK(AssetDatabaseQueryService::GetLastChange().Changed.Contains(firstId));
    CHECK(AssetDatabaseQueryService::GetLastChange().Changed.Count() == 1);
    REQUIRE(AssetDatabase::Get().TryGetRecord(firstId, found));
    CHECK(found.MetaSemanticHash != previousHash);
    CHECK(found.DatabaseRevision != firstRecordRevision);

    FileSystem::DeleteFile(first);
    FileSystem::DeleteFile(first + TEXT(".meta"));
    FileSystem::DeleteFile(second);
    FileSystem::DeleteFile(second + TEXT(".meta"));
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(cleanup));
    CHECK_FALSE(AssetDatabase::Get().TryGetRecord(firstId, found));
    CHECK_FALSE(AssetDatabase::Get().TryGetRecord(secondId, found));
}

TEST_CASE("Asset database RefreshSources creates canonical sidecars and keeps unaffected records")
{
    const String folder = Globals::ProjectContentFolder / (TEXT("__RefreshDiagnostics_") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(folder));
    Array<String> cleanup;
    cleanup.Add(folder);
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(folder, true);
        AssetPipelineService::RefreshSources(cleanup);
    };

    const String orphan = folder / TEXT("Orphan.txt");
    const String tracked = folder / TEXT("Tracked.txt");
    REQUIRE_FALSE(File::WriteAllText(orphan, TEXT("orphan"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(tracked, TEXT("tracked"), Encoding::ANSI));

    // Filesystem additions are registered through the native metadata boundary before collection.
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(cleanup));
    REQUIRE(FileSystem::FileExists(orphan + TEXT(".meta")));
    REQUIRE(FileSystem::FileExists(tracked + TEXT(".meta")));
    AssetPipelineDiagnostic diagnostic;
    AssetMeta orphanMeta;
    REQUIRE_FALSE(AssetMeta::Load(orphan + TEXT(".meta"), orphanMeta, diagnostic));
    AssetRecord found;
    REQUIRE(AssetDatabase::Get().TryGetRecord(orphanMeta.ID, found));

    // Refreshing one source must not discard every other source in the committed snapshot.
    Array<String> trackedOnly;
    trackedOnly.Add(tracked);
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(trackedOnly));
    REQUIRE(AssetDatabase::Get().TryGetRecord(orphanMeta.ID, found));
}

TEST_CASE("Asset database clears a path collision once the conflict is resolved")
{
    AssetDatabase database;
    const Guid firstId(11, 12, 13, 15);
    const Guid secondId(21, 22, 23, 25);
    Array<AssetRecord> records;
    records.Add(MakeDatabaseRecord(firstId, firstId, TEXT("C:/Project/Content/Clash.png")));
    records.Add(MakeDatabaseRecord(secondId, secondId, TEXT("C:/Project/Content/clash.png")));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));
    Array<AssetRecord> collisions;
    database.GetByStatus(AssetRecordStatus::PathCollision, collisions);
    REQUIRE(collisions.Count() == 2);

    // Republish what the database now reports, with one of the two renamed away.
    Array<AssetRecord> resolved;
    AssetRecord carried;
    REQUIRE(database.TryGetRecord(firstId, carried));
    resolved.Add(carried);
    REQUIRE(database.TryGetRecord(secondId, carried));
    carried.CanonicalPath = CanonicalAssetPath(TEXT("C:/Project/Content/Distinct.png"));
    carried.SourcePath = SourceFilePath(TEXT("C:/Project/Content/Distinct.png"));
    carried.PortabilityKey = TEXT("c:/project/content/distinct.png");
    resolved.Add(carried);
    REQUIRE_FALSE(database.PublishFullSnapshot(resolved, diagnostic));

    collisions.Clear();
    database.GetByStatus(AssetRecordStatus::PathCollision, collisions);
    CHECK(collisions.Count() == 0);
    AssetRecord found;
    REQUIRE(database.TryGetRecord(firstId, found));
    CHECK(found.Status == AssetRecordStatus::Ready);
    REQUIRE(database.TryGetRecord(secondId, found));
    CHECK(found.Status == AssetRecordStatus::Ready);
}

TEST_CASE("Asset database RefreshSources prunes records under a deleted directory")
{
    const String folder = Globals::ProjectContentFolder / (TEXT("__RefreshFileStates_") + Guid::New().ToString(Guid::FormatType::N));
    const String nested = folder / TEXT("Nested");
    REQUIRE_FALSE(FileSystem::CreateDirectory(nested));
    Array<String> cleanup;
    cleanup.Add(folder);
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(folder, true);
        AssetPipelineService::RefreshSources(cleanup);
    };

    const Guid trackedId = Guid::New();
    const String tracked = nested / TEXT("Tracked.png");
    REQUIRE_FALSE(File::WriteAllText(tracked, TEXT("tracked"), Encoding::ANSI));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(tracked + TEXT(".meta"), MakeDatabaseMeta(trackedId), diagnostic));
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(cleanup));

    AssetRecord found;
    REQUIRE(AssetDatabase::Get().TryGetRecord(trackedId, found));

    REQUIRE_FALSE(FileSystem::DeleteDirectory(folder, true));
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(cleanup));
    CHECK_FALSE(AssetDatabase::Get().TryGetRecord(trackedId, found));
}

#endif
