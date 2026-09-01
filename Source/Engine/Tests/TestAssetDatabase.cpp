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
#include "Engine/Platform/Platform.h"
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

TEST_CASE("Asset database publishes initial and targeted source file hashes durably")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseSourceHash-") +
        Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const String sourcePath = content / TEXT("Payload.bin");
    const byte initialBytes[] = { 1, 3, 5, 7 };
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, initialBytes, ARRAY_COUNT(initialBytes)));
    const Guid sourceId = Guid::New();
    Array<AssetRecord> records;
    records.Add(MakeDatabaseRecord(sourceId, sourceId, sourcePath));

    AssetDatabase database;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(database.Open(library, Guid::New(), diagnostic));
    SourceHashCache hashCache;
    ContentHash initialHash;
    SourceHashFileState state;
    REQUIRE_FALSE(hashCache.HashFile(sourcePath, initialHash, state, diagnostic));
    Array<SourceHashFileState> states;
    states.Add(state);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, states, diagnostic));

    SourceAssetRow source;
    REQUIRE(database.GetDurableSnapshot().TryGetSource(sourceId, source));
    CHECK(source.SourceHash == initialHash);
    CHECK(source.SourceSize == ARRAY_COUNT(initialBytes));
    CHECK(source.SourceMtimeHint == state.LastWriteTicks);

    const byte updatedBytes[] = { 2, 4, 6, 8, 10, 12 };
    REQUIRE_FALSE(File::WriteAllBytes(sourcePath, updatedBytes, ARRAY_COUNT(updatedBytes)));
    ContentHash updatedHash;
    REQUIRE_FALSE(hashCache.HashFile(sourcePath, updatedHash, state, diagnostic));
    states[0] = state;
    REQUIRE_FALSE(database.PublishFullSnapshot(records, states, diagnostic));
    REQUIRE(database.GetDurableSnapshot().TryGetSource(sourceId, source));
    CHECK(source.SourceHash == updatedHash);
    CHECK(source.SourceHash != initialHash);
    CHECK(source.SourceSize == ARRAY_COUNT(updatedBytes));
    CHECK(source.SourceMtimeHint == state.LastWriteTicks);
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
    const Guid publicationRefreshId = Guid::New();
    constexpr uint32 publicationPass = 5;
    REQUIRE_FALSE(database.RecordPublication(mainPublication, publicationDependencies, diagnostic,
        publicationRefreshId, publicationPass));
    SourceAssetPublicationRow subPublication = mainPublication;
    subPublication.LocalFileId = 2;
    subPublication.Artifact = ArtifactKey(ContentHash::Compute("sub-publication", 15));
    Array<SourceAssetDependencyRow> noDependencies;
    REQUIRE_FALSE(database.RecordPublication(subPublication, noDependencies, diagnostic,
        publicationRefreshId, publicationPass));

    const uint64 publicationRevision = database.GetRevision();
    REQUIRE_FALSE(database.ReadChangesAfter(stableRevision, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 2);
    for (const AssetChangeSet& change : changes)
    {
        CHECK(change.RefreshId == publicationRefreshId);
        CHECK(change.Pass == publicationPass);
        CHECK(change.Imported.Count() == 1);
    }
    changes.Clear();
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

TEST_CASE("Asset database scanner reconciles typed rows and retains unrelated durable state")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetDatabaseScannerRows-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(library));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const Guid sourceId = Guid::New();
    String sourcePath = content / TEXT("Payload.bin");
    REQUIRE_FALSE(File::WriteAllText(sourcePath, TEXT("initial"), Encoding::ANSI));
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(sourcePath + TEXT(".meta"), MakeDatabaseMeta(sourceId), diagnostic));

    AssetDatabase database;
    REQUIRE_FALSE(database.Open(library, Guid::New(), diagnostic));
    AssetDatabaseScanOptions options;
    AssetDatabaseScanResult scan;
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, database, scan));
    Array<AssetChangeSet> changes;
    bool requiresSnapshot = false;
    REQUIRE_FALSE(database.ReadChangesAfter(0, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    REQUIRE(changes[0].Added.Count() == 1);
    CHECK(changes[0].Added[0].AssetGuid == sourceId);

    SourceAssetPublicationRow publication;
    publication.AssetGuid = sourceId;
    publication.LocalFileId = 1;
    publication.TargetId = TEXT("Windows-x64");
    publication.Artifact = ArtifactKey(ContentHash::Compute("scanner-publication", 19));
    publication.IsLastKnownGood = true;
    Array<SourceAssetDependencyRow> publicationDependencies;
    REQUIRE_FALSE(database.RecordPublication(publication, publicationDependencies, diagnostic));
    REQUIRE_FALSE(database.RegisterCustomDependency(TEXT("scanner-environment"),
        ContentHash::Compute("stable", 6), TEXT("test"), diagnostic));

    uint64 baselineRevision = database.GetRevision();
    REQUIRE_FALSE(File::WriteAllText(sourcePath, TEXT("changed-payload"), Encoding::ANSI));
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, database, scan));
    changes.Clear();
    REQUIRE_FALSE(database.ReadChangesAfter(baselineRevision, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    CHECK(changes[0].SourceChanged.Count() == 1);
    CHECK(changes[0].Moved.IsEmpty());
    CHECK(changes[0].ObjectsChanged.IsEmpty());

    const String movedFolder = content / TEXT("Moved");
    REQUIRE_FALSE(FileSystem::CreateDirectory(movedFolder));
    const String movedPath = movedFolder / TEXT("Payload.bin");
    REQUIRE_FALSE(FileSystem::MoveFile(movedPath, sourcePath));
    REQUIRE_FALSE(FileSystem::MoveFile(movedPath + TEXT(".meta"), sourcePath + TEXT(".meta")));
    baselineRevision = database.GetRevision();
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, database, scan));
    changes.Clear();
    REQUIRE_FALSE(database.ReadChangesAfter(baselineRevision, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    REQUIRE(changes[0].Moved.Count() == 1);
    CHECK(changes[0].Moved[0].AssetGuid == sourceId);
    CHECK(changes[0].SourceChanged.IsEmpty());
    CHECK(changes[0].ObjectsChanged.IsEmpty());

    const AssetDatabaseReadSnapshot retainedSnapshot = database.GetDurableSnapshot();
    const SourceAssetDatabaseState& retained = retainedSnapshot.GetState();
    CHECK(retained.Publications.Count() == 1);
    CHECK(retained.CustomDependencies.Count() == 1);

    baselineRevision = database.GetRevision();
    REQUIRE_FALSE(FileSystem::DeleteFile(movedPath));
    REQUIRE_FALSE(FileSystem::DeleteFile(movedPath + TEXT(".meta")));
    REQUIRE_FALSE(AssetDatabaseScanner::Scan(root, content, library, options, database, scan));
    changes.Clear();
    REQUIRE_FALSE(database.ReadChangesAfter(baselineRevision, changes, requiresSnapshot, diagnostic));
    REQUIRE(changes.Count() == 1);
    REQUIRE(changes[0].Removed.Count() == 1);
    CHECK(changes[0].Removed[0].AssetGuid == sourceId);
    const AssetDatabaseReadSnapshot removedSnapshot = database.GetDurableSnapshot();
    CHECK(removedSnapshot.GetState().Publications.IsEmpty());
    CHECK(removedSnapshot.GetState().CustomDependencies.Count() == 1);
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

TEST_CASE("Default canonical metadata preserves typed authored document families")
{
    struct AuthoredCase
    {
        const Char* Extension;
        const Char* TypeName;
        const Char* ProcessorID;
    };
    const AuthoredCase cases[] =
    {
        { TEXT("material"), TEXT("FlaxEngine.Material"), TEXT("Flax.GraphDocument") },
        { TEXT("materialfunction"), TEXT("FlaxEngine.MaterialFunction"), TEXT("Flax.GraphDocument") },
        { TEXT("animgraph"), TEXT("FlaxEngine.AnimationGraph"), TEXT("Flax.GraphDocument") },
        { TEXT("animgraphfunction"), TEXT("FlaxEngine.AnimationGraphFunction"), TEXT("Flax.GraphDocument") },
        { TEXT("visualscript"), TEXT("FlaxEngine.VisualScript"), TEXT("Flax.GraphDocument") },
        { TEXT("behaviortree"), TEXT("FlaxEngine.BehaviorTree"), TEXT("Flax.GraphDocument") },
        { TEXT("particlefunction"), TEXT("FlaxEngine.ParticleEmitterFunction"), TEXT("Flax.GraphDocument") },
        { TEXT("particleemitter"), TEXT("FlaxEngine.ParticleEmitter"), TEXT("Flax.GraphDocument") },
        { TEXT("materialinstance"), TEXT("FlaxEngine.MaterialInstance"), TEXT("Flax.MaterialInstance") },
        { TEXT("skeletonmask"), TEXT("FlaxEngine.SkeletonMask"), TEXT("Flax.SkeletonMask") },
        { TEXT("sceneanimation"), TEXT("FlaxEngine.SceneAnimation"), TEXT("Flax.SceneAnimation") },
        { TEXT("particlesystem"), TEXT("FlaxEngine.ParticleSystem"), TEXT("Flax.ParticleSystem") },
        { TEXT("collisiondata"), TEXT("FlaxEngine.CollisionData"), TEXT("Flax.CollisionData") },
    };
    const String root = Globals::TemporaryFolder / (TEXT("AuthoredMetadataBatch-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    Array<String> sources;
    Array<String> stagingPaths;
    for (int32 i = 0; i < ARRAY_COUNT(cases); i++)
    {
        const String source = root / (String::Format(TEXT("Asset{0}."), i) + cases[i].Extension);
        String sourceText = TEXT("{\"type\":\"");
        sourceText += cases[i].TypeName;
        sourceText += TEXT("\"}\n");
        REQUIRE_FALSE(File::WriteAllText(source, sourceText, Encoding::ANSI));
        sources.Add(source);
        stagingPaths.Add(source + TEXT(".staged-meta"));
    }

    const Array<Guid> ids = AssetOperationService::StageDefaultMetadataBatch(sources, stagingPaths);
    REQUIRE(ids.Count() == ARRAY_COUNT(cases));
    for (int32 i = 0; i < ARRAY_COUNT(cases); i++)
    {
        REQUIRE(ids[i].IsValid());
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(AssetMeta::Load(stagingPaths[i], meta, diagnostic));
        CHECK(meta.AssetType == cases[i].TypeName);
        CHECK(meta.SourceKind == AssetSourceKind::TextDocument);
        CHECK(meta.Processor.ID == cases[i].ProcessorID);
        CHECK(meta.Processor.SettingsVersion == 1);
        CHECK(meta.Processor.SettingsJson == StringAnsiView("{}\n"));
    }

    const String mismatch = root / TEXT("Mismatch.particlesystem");
    REQUIRE_FALSE(File::WriteAllText(mismatch, TEXT("{\"type\":\"FlaxEngine.ParticleEmitter\"}\n"), Encoding::ANSI));
    sources.Clear();
    stagingPaths.Clear();
    sources.Add(mismatch);
    stagingPaths.Add(mismatch + TEXT(".staged-meta"));
    const Array<Guid> mismatchIds = AssetOperationService::StageDefaultMetadataBatch(sources, stagingPaths);
    REQUIRE(mismatchIds.Count() == 1);
    CHECK_FALSE(mismatchIds[0].IsValid());
    CHECK_FALSE(FileSystem::FileExists(stagingPaths[0]));
    const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
    REQUIRE(diagnostics.HasItems());
    CHECK(diagnostics[0].Code == AssetPipelineDiagnosticCode::InvalidMeta);
}

TEST_CASE("Canonical refresh narrowly repairs regression-damaged authored metadata")
{
    const String folder = Globals::ProjectContentFolder / (TEXT("__AuthoredMetadataRepair_") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(folder));
    Array<String> refresh;
    refresh.Add(folder);
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(folder, true);
        AssetPipelineService::RefreshSources(refresh);
    };

    const String source = folder / TEXT("System.particlesystem");
    REQUIRE_FALSE(File::WriteAllText(source,
        TEXT("{\"documentVersion\":1,\"type\":\"FlaxEngine.ParticleSystem\",\"framesPerSecond\":60.0,\"durationFrames\":0,\"tracks\":[],\"parameterOverrides\":[]}\n"),
        Encoding::ANSI));
    AssetMeta damaged;
    damaged.ID = Guid::New();
    damaged.AssetType = RawDataAsset::TypeName;
    damaged.SourceKind = AssetSourceKind::ImportedSource;
    damaged.Processor.ID = TEXT("Flax.Binary");
    damaged.Processor.SettingsVersion = 1;
    damaged.Processor.SettingsJson = "{}\n";
    damaged.Processor.UnknownFields.Add("processorExtension", "{\"enabled\":true}");
    damaged.MainObjectUnknownFields.Add("mainExtension", "17");
    damaged.Labels.Add(TEXT("preserved"));
    damaged.UserDataJson = "{\"owner\":\"test\"}";
    damaged.UnknownFields.Add("rootExtension", "[1,2,3]");
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(source + TEXT(".meta"), damaged, diagnostic));
    AssetMeta persisted;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), persisted, diagnostic));

    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    AssetMeta repaired;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), repaired, diagnostic));
    CHECK(repaired.ID == damaged.ID);
    CHECK(repaired.AssetType == TEXT("FlaxEngine.ParticleSystem"));
    CHECK(repaired.SourceKind == AssetSourceKind::TextDocument);
    CHECK(repaired.Processor.ID == TEXT("Flax.ParticleSystem"));
    CHECK(repaired.Labels.Contains(TEXT("preserved")));
    CHECK(repaired.UserDataJson == persisted.UserDataJson);
    REQUIRE(repaired.Processor.UnknownFields.ContainsKey("processorExtension"));
    CHECK(repaired.Processor.UnknownFields["processorExtension"] == persisted.Processor.UnknownFields["processorExtension"]);
    REQUIRE(repaired.MainObjectUnknownFields.ContainsKey("mainExtension"));
    CHECK(repaired.MainObjectUnknownFields["mainExtension"] == persisted.MainObjectUnknownFields["mainExtension"]);
    REQUIRE(repaired.UnknownFields.ContainsKey("rootExtension"));
    CHECK(repaired.UnknownFields["rootExtension"] == persisted.UnknownFields["rootExtension"]);
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
    records[0].ImporterSettingsVersion = 7;
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
    CHECK(found.ImporterSettingsVersion == 7);
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
    SourceAssetRow durableSource;
    REQUIRE(AssetDatabase::Get().GetDurableSnapshot().TryGetSource(firstId, durableSource));
    const ContentHash initialSourceHash = ContentHash::Compute("one", 3);
    CHECK(durableSource.SourceHash == initialSourceHash);
    CHECK(durableSource.SourceSize == 3);
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
    CHECK(AssetDatabase::Get().GetRevision() == revisionBeforeOverwrite + 1);
    REQUIRE(AssetDatabase::Get().TryGetRecord(firstId, found));
    CHECK(found.DatabaseRevision == firstRecordRevision);
    REQUIRE(AssetDatabase::Get().GetDurableSnapshot().TryGetSource(firstId, durableSource));
    CHECK(durableSource.SourceHash == ContentHash::Compute("overwrite", 9));
    CHECK(durableSource.SourceHash != initialSourceHash);
    CHECK(durableSource.SourceSize == 9);
    CHECK(AssetDatabaseQueryService::GetLastChange().Changed.Contains(firstId));

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

TEST_CASE("Asset importer settings facade preserves conflicts and no-op revisions")
{
    const String folder = Globals::ProjectContentFolder / (TEXT("__ImporterSettings_") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(folder));
    Array<String> refresh;
    refresh.Add(folder);
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(folder, true);
        AssetPipelineService::RefreshSources(refresh);
    };

    const Guid id = Guid::New();
    const String source = folder / TEXT("Payload.bin");
    const byte sourceBytes[] = { 4, 8, 15, 16, 23, 42 };
    REQUIRE_FALSE(File::WriteAllBytes(source, sourceBytes, ARRAY_COUNT(sourceBytes)));
    AssetMeta meta;
    meta.ID = id;
    meta.AssetType = RawDataAsset::TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TEXT("Flax.Binary");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}";
    meta.Labels.Add(TEXT("facade-test"));
    meta.UserDataJson = "{\"preserved\":true}";
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::SaveAtomic(source + TEXT(".meta"), meta, diagnostic));
    const Guid textID = Guid::New();
    const String textSource = folder / TEXT("Notes.txt");
    REQUIRE_FALSE(File::WriteAllText(textSource, TEXT("not generic importer settings"), Encoding::ANSI));
    AssetMeta textMeta;
    textMeta.ID = textID;
    textMeta.AssetType = RawDataAsset::TypeName;
    textMeta.SourceKind = AssetSourceKind::TextDocument;
    textMeta.Processor.ID = TEXT("Flax.Text");
    textMeta.Processor.SettingsVersion = 1;
    textMeta.Processor.SettingsJson = "{}";
    REQUIRE_FALSE(AssetMeta::SaveAtomic(textSource + TEXT(".meta"), textMeta, diagnostic));
    REQUIRE_FALSE(AssetPipelineService::RefreshSources(refresh));
    AssetMeta persistedMeta;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), persistedMeta, diagnostic));

    AssetImporterSettingsSnapshot ineligible;
    CHECK(AssetOperationService::GetImporterSettings(textID, ineligible));
    CHECK(ineligible.SourceAssetID == Guid::Empty);
    const Array<AssetPipelineDiagnostic> ineligibleDiagnostics = AssetDatabaseQueryService::GetDiagnostics();
    REQUIRE(ineligibleDiagnostics.HasItems());
    CHECK(ineligibleDiagnostics[0].Code == AssetPipelineDiagnosticCode::SourceMissing);
    AssetImporterSettingsSnapshot captured;
    REQUIRE_FALSE(AssetOperationService::GetImporterSettings(id, captured));
    CHECK(captured.SourceAssetID == id);
    CHECK(captured.SourceRevision != 0);
    CHECK(captured.ImporterID == TEXT("Flax.Binary"));
    CHECK(captured.StoredSettingsVersion == 1);
    CHECK(captured.SettingsSchemaVersion == 1);
    CHECK(captured.SettingsJson == TEXT("{}\n"));

    AssetOperationService::StartEditing();
    const AssetImporterSettingsSaveResult blocked = AssetOperationService::SaveImporterSettingsAndReimportDetailed(
        captured, TEXT("{\"marker\":0}"));
    CHECK(blocked.WriteOutcome == AssetImporterSettingsWriteOutcome::Failed);
    CHECK(blocked.ReimportOutcome == AssetImporterSettingsReimportOutcome::NotRequired);
    CHECK(blocked.Diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
    REQUIRE_FALSE(AssetOperationService::StopEditing());
    AssetMeta unchanged;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), unchanged, diagnostic));
    CHECK(unchanged.Processor.SettingsJson == "{}\n");

    const AssetImporterSettingsSaveResult invalid = AssetOperationService::SaveImporterSettingsAndReimportDetailed(
        captured, TEXT("[1, 2]"));
    CHECK(invalid.WriteOutcome == AssetImporterSettingsWriteOutcome::Failed);
    CHECK(invalid.Diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);

    const AssetImporterSettingsSaveResult saved = AssetOperationService::SaveImporterSettingsAndReimportDetailed(
        captured, TEXT(" { \"marker\" : 1 } "));
    CHECK(saved.WriteOutcome == AssetImporterSettingsWriteOutcome::Committed);
    CHECK(saved.ReimportOutcome == AssetImporterSettingsReimportOutcome::Queued);
    CHECK(saved.Diagnostic.Code == AssetPipelineDiagnosticCode::None);
    const AssetImporterSettingsSnapshot& current = saved.Current;
    CHECK(current.SourceAssetID == id);
    CHECK(current.SourceRevision > captured.SourceRevision);
    CHECK(current.MetaSemanticHash != captured.MetaSemanticHash);
    CHECK(current.SettingsJson == TEXT("{\n  \"marker\": 1\n}\n"));
    AssetMeta updated;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), updated, diagnostic));
    CHECK(updated.Labels == persistedMeta.Labels);
    CHECK(updated.UserDataJson == persistedMeta.UserDataJson);

    String status;
    for (int32 i = 0; i < 10000; i++)
    {
        status = AssetPipelineService::GetBuildStatus(id);
        if (status == TEXT("ReadyExact") || status == TEXT("Failed") || status == TEXT("Cancelled"))
            break;
        Platform::Sleep(1);
    }
    CHECK(status == TEXT("ReadyExact"));

    BytesContainer beforeNoOp;
    REQUIRE_FALSE(File::ReadAllBytes(source + TEXT(".meta"), beforeNoOp));
    const uint64 beforeNoOpRevision = current.SourceRevision;
    const AssetImporterSettingsSaveResult noOp = AssetOperationService::SaveImporterSettingsAndReimportDetailed(
        current, TEXT("{\"marker\":1}"));
    CHECK(noOp.WriteOutcome == AssetImporterSettingsWriteOutcome::Unchanged);
    CHECK(noOp.ReimportOutcome == AssetImporterSettingsReimportOutcome::NotRequired);
    const AssetImporterSettingsSnapshot& afterNoOp = noOp.Current;
    CHECK(afterNoOp.SourceRevision == beforeNoOpRevision);
    BytesContainer afterNoOpBytes;
    REQUIRE_FALSE(File::ReadAllBytes(source + TEXT(".meta"), afterNoOpBytes));
    REQUIRE(afterNoOpBytes.Length() == beforeNoOp.Length());
    CHECK(Platform::MemoryCompare(afterNoOpBytes.Get(), beforeNoOp.Get(), beforeNoOp.Length()) == 0);

    const AssetImporterSettingsSaveResult conflict = AssetOperationService::SaveImporterSettingsAndReimportDetailed(
        captured, TEXT("{\"marker\":2}"));
    CHECK(conflict.WriteOutcome == AssetImporterSettingsWriteOutcome::Conflict);
    CHECK(conflict.ReimportOutcome == AssetImporterSettingsReimportOutcome::NotRequired);
    CHECK(conflict.Diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
    const AssetImporterSettingsSnapshot& conflictCurrent = conflict.Current;
    CHECK(conflictCurrent.SourceRevision == current.SourceRevision);
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), updated, diagnostic));
    CHECK(updated.Processor.SettingsJson == "{\n  \"marker\": 1\n}\n");
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
