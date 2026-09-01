// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetOperations.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseScanner.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Utilities/Crc.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    AssetMeta MakeOperationMeta()
    {
        AssetMeta meta;
        meta.ID = Guid::New();
        meta.AssetType = TEXT("FlaxEngine.Model");
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = TEXT("Flax.Model");
        meta.Processor.SettingsVersion = 3;
        meta.Processor.SettingsJson = "{\"scale\":2}";
        meta.Processor.UnknownFields.Add("processorExtension", "{\"enabled\":true}");
        meta.MainObjectUnknownFields.Add("mainExtension", "17");
        meta.Labels.Add(TEXT("model"));
        meta.UserDataJson = "{\"owner\":\"test\"}";
        meta.UnknownFields.Add("rootExtension", "[1,2,3]");
        SubAssetMeta mesh;
        mesh.ID = Guid::New();
        mesh.LocalId = 771;
        mesh.TypeName = TEXT("FlaxEngine.Model");
        mesh.DisplayName = TEXT("Body");
        meta.SubAssets.Add(TEXT("mesh:Body"), mesh);
        return meta;
    }

    class OperationProcessor : public IAssetModificationProcessor
    {
    public:
        int32 Calls = 0;
        bool Deny = false;
        AssetOperationKind LastKind = AssetOperationKind::Create;

        bool ValidateOperation(AssetOperationKind kind, const AssetOperationTarget& target,
            const StringView& destination, AssetPipelineDiagnostic& diagnostic) override
        {
            Calls++;
            LastKind = kind;
            if (!Deny)
                return false;
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
            diagnostic.Message = TEXT("Denied by test modification processor.");
            return true;
        }
    };

    class OperationDatabase : public IAssetOperationDatabaseCallbacks
    {
    public:
        int32 ClearCalls = 0;
        int32 RefreshCalls = 0;
        Guid ClearedSource;
        Guid ClearedCopy;
        int32 FailClearOnCall = 0;
        Array<AssetOperationCommit> LastCommits;
        int32 ImporterRevisionCalls = 0;
        bool FailRefresh = false;
        bool HasImporterRevision = false;
        AssetOperationTarget ImporterTarget;
        AssetImporterSettingsRevision ImporterRevision;

        bool ClearCopiedState(const Guid& sourceGuid, const Guid& copiedGuid,
            AssetPipelineDiagnostic& diagnostic) override
        {
            ClearCalls++;
            ClearedSource = sourceGuid;
            ClearedCopy = copiedGuid;
            if (FailClearOnCall == ClearCalls)
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                diagnostic.Message = TEXT("Injected copied-state failure.");
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool ValidateImporterSettingsRevision(const AssetOperationTarget& target,
            const AssetImporterSettingsRevision& expected, AssetPipelineDiagnostic& diagnostic) override
        {
            ImporterRevisionCalls++;
            if (!HasImporterRevision ||
                (target.ExpectedGuid == ImporterTarget.ExpectedGuid &&
                    FileSystem::AreFilePathsEquivalent(target.SourcePath, ImporterTarget.SourcePath) &&
                    expected.SourceRevision == ImporterRevision.SourceRevision &&
                    expected.MetaSemanticHash == ImporterRevision.MetaSemanticHash &&
                    expected.ImporterID == ImporterRevision.ImporterID &&
                    expected.StoredSettingsVersion == ImporterRevision.StoredSettingsVersion))
            {
                diagnostic = AssetPipelineDiagnostic();
                return false;
            }
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = target.ExpectedGuid;
            diagnostic.SourcePath = target.SourcePath;
            diagnostic.Message = TEXT("Importer settings revision is stale.");
            return true;
        }

        bool RefreshCommitted(const Array<AssetOperationCommit>& commits,
            AssetPipelineDiagnostic& diagnostic) override
        {
            RefreshCalls++;
            LastCommits = commits;
            if (FailRefresh)
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                diagnostic.Message = TEXT("Injected database publication failure.");
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    class ScanningOperationDatabase : public IAssetOperationDatabaseCallbacks
    {
    public:
        String ProjectRoot;
        String ContentRoot;
        String LibraryRoot;
        AssetDatabase Database;

        ScanningOperationDatabase(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot)
            : ProjectRoot(projectRoot)
            , ContentRoot(contentRoot)
            , LibraryRoot(libraryRoot)
        {
        }

        bool ClearCopiedState(const Guid&, const Guid&, AssetPipelineDiagnostic& diagnostic) override
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool ValidateImporterSettingsRevision(const AssetOperationTarget&, const AssetImporterSettingsRevision&,
            AssetPipelineDiagnostic& diagnostic) override
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool RefreshCommitted(const Array<AssetOperationCommit>&, AssetPipelineDiagnostic& diagnostic) override
        {
            AssetDatabaseScanOptions options;
            options.StrictMetadata = true;
            AssetDatabaseScanResult scan;
            if (AssetDatabaseScanner::Scan(ProjectRoot, ContentRoot, LibraryRoot, options, Database, scan))
                return true;
            if (scan.HasBlockingDiagnostics())
            {
                diagnostic = scan.Diagnostics[0];
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    uint64 GetOperationMetaSemanticHash(const AssetMeta& meta)
    {
        StringAnsi canonical;
        AssetPipelineDiagnostic diagnostic;
        if (meta.ToJson(canonical, diagnostic))
            return 0;
        return Crc::MemCrc32(canonical.Get(), canonical.Length());
    }

    bool EqualBytes(const BytesContainer& left, const BytesContainer& right)
    {
        return left.Length() == right.Length() &&
            Platform::MemoryCompare(left.Get(), right.Get(), left.Length()) == 0;
    }

    bool WriteOperationSceneFragments(const StringView& projectRoot, const Guid& sceneGuid,
        const Array<SceneFragmentWrite>& writes, String& error)
    {
        SceneFragmentSavePlan plan;
        if (SceneFragmentStore::PrepareSave(sceneGuid, writes, plan, error))
            return true;
        const String sceneDirectory = SceneFragmentStore::GetScenePath(projectRoot, sceneGuid);
        if (FileSystem::CreateDirectory(sceneDirectory) ||
            File::WriteAllBytes(sceneDirectory / TEXT("scene-fragments.index"), plan.IndexData.Get(), plan.IndexData.Count()))
            return true;
        for (const PreparedSceneFragment& fragment : plan.Fragments)
        {
            const String path = sceneDirectory / fragment.RelativePhysicalPath;
            const String parent = StringUtils::GetDirectoryName(path);
            if ((!FileSystem::DirectoryExists(parent) && FileSystem::CreateDirectory(parent)) ||
                File::WriteAllBytes(path, fragment.Data.Get(), fragment.Data.Count()))
                return true;
        }
        return false;
    }
}

TEST_CASE("Asset operations preserve exact identity and clone copy object mappings")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetOperations-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const AssetMeta sourceMeta = MakeOperationMeta();
    const String source = content / TEXT("Robot.gltf");
    const byte sourceBytes[] = { 1, 2, 3, 4, 5 };
    REQUIRE_FALSE(operations.CreateAsset(source, Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)),
        sourceMeta, diagnostic));
    CHECK(FileSystem::FileExists(source));
    CHECK(FileSystem::FileExists(source + TEXT(".meta")));

    AssetOperationTarget target;
    target.SourcePath = source;
    target.ExpectedGuid = sourceMeta.ID;
    const String copied = content / TEXT("Robot Copy.gltf");
    Guid copiedGuid;
    operations.StartAssetEditing();
    REQUIRE_FALSE(operations.CopyAsset(target, copied, copiedGuid, diagnostic));
    CHECK(copiedGuid.IsValid());
    CHECK(copiedGuid != sourceMeta.ID);
    CHECK(database.ClearCalls == 1);
    CHECK(database.ClearedSource == sourceMeta.ID);
    CHECK(database.ClearedCopy == copiedGuid);
    AssetMeta copiedMeta;
    REQUIRE_FALSE(AssetMeta::Load(copied + TEXT(".meta"), copiedMeta, diagnostic));
    AssetMeta persistedSourceMeta;
    REQUIRE_FALSE(AssetMeta::Load(source + TEXT(".meta"), persistedSourceMeta, diagnostic));
    CHECK(copiedMeta.ID == copiedGuid);
    CHECK(copiedMeta.Processor.SettingsJson == persistedSourceMeta.Processor.SettingsJson);
    REQUIRE(copiedMeta.SubAssets.ContainsKey(TEXT("mesh:Body")));
    CHECK(copiedMeta.SubAssets[TEXT("mesh:Body")].ID != persistedSourceMeta.SubAssets[TEXT("mesh:Body")].ID);
    CHECK(copiedMeta.SubAssets[TEXT("mesh:Body")].LocalId == 771);

    const String moved = content / TEXT("Moved/Robot.gltf");
    REQUIRE_FALSE(operations.MoveAsset(target, moved, diagnostic));
    CHECK_FALSE(FileSystem::FileExists(source));
    CHECK(FileSystem::FileExists(moved));
    CHECK(database.RefreshCalls == 1); // Create refreshed immediately; editing defers copy and move.
    REQUIRE_FALSE(operations.StopAssetEditing(diagnostic));
    CHECK(database.RefreshCalls == 2);
    REQUIRE(database.LastCommits.Count() == 2);
    CHECK(database.LastCommits[0].Kind == AssetOperationKind::Copy);
    CHECK(database.LastCommits[1].Kind == AssetOperationKind::Move);

    AssetOperationTarget stale;
    stale.SourcePath = moved;
    stale.ExpectedGuid = Guid::New();
    CHECK(operations.MoveAsset(stale, source, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
    CHECK(FileSystem::FileExists(moved));

    AssetOperationTarget movedTarget;
    movedTarget.SourcePath = moved;
    movedTarget.ExpectedGuid = sourceMeta.ID;
    AssetTrashRecord trash;
    REQUIRE_FALSE(operations.TrashAsset(movedTarget, trash, diagnostic));
    CHECK_FALSE(FileSystem::FileExists(moved));
    CHECK(FileSystem::FileExists(trash.TrashSourcePath));
    REQUIRE_FALSE(operations.RestoreAsset(trash, diagnostic));
    CHECK(FileSystem::FileExists(moved));
    CHECK_FALSE(FileSystem::FileExists(trash.TrashSourcePath));

    Array<AssetOperationSelfWrite> writes;
    operations.DrainSelfWrites(writes);
    CHECK(writes.Count() >= 8);
    for (const AssetOperationSelfWrite& write : writes)
    {
        CHECK(write.TransactionId.IsValid());
        CHECK_FALSE(write.Content.IsZero());
    }
    const ContentHash registeredHash = ContentHash::Compute("registered", 10);
    operations.RegisterSelfWrite(moved, registeredHash);
    operations.DrainSelfWrites(writes);
    REQUIRE(writes.Count() == 1);
    CHECK(FileSystem::AreFilePathsEquivalent(writes[0].Path, moved));
    CHECK(writes[0].Content == registeredHash);
    Array<AssetPipelineDiagnostic> recoveryDiagnostics;
    CHECK_FALSE(operations.RecoverIncompleteTransactions(recoveryDiagnostics));
    CHECK(recoveryDiagnostics.IsEmpty());
}

TEST_CASE("Asset operations enforce persistent GUID lifecycle through scanner publication")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetGuidLifecycle-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    ScanningOperationDatabase database(root, content, library);
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    AssetMeta sourceMeta = MakeOperationMeta();
    sourceMeta.SubAssets[TEXT("mesh:Body")].CollisionSalt = 3;
    sourceMeta.SubAssets[TEXT("mesh:Body")].PreviousKeys.Add(TEXT("mesh:OldBody"));
    const Guid sourceSubAssetGuid = sourceMeta.SubAssets[TEXT("mesh:Body")].ID;
    const String source = content / TEXT("Robot.gltf");
    const byte sourceBytes[] = { 1, 2, 3, 4, 5 };
    REQUIRE_FALSE(operations.CreateAsset(source, Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)),
        sourceMeta, diagnostic));
    AssetRecord record;
    REQUIRE(database.Database.TryGetRecord(sourceMeta.ID, record));
    REQUIRE(database.Database.TryGetRecord(sourceSubAssetGuid, record));
    REQUIRE(FileSystem::FileExists(source));
    REQUIRE(FileSystem::FileExists(source + TEXT(".meta")));

    AssetOperationTarget target;
    target.SourcePath = source;
    target.ExpectedGuid = sourceMeta.ID;
    const String moved = content / TEXT("Robot Moved.gltf");
    REQUIRE_FALSE(operations.MoveAsset(target, moved, diagnostic));
    AssetMeta movedMeta;
    REQUIRE_FALSE(AssetMeta::Load(moved + TEXT(".meta"), movedMeta, diagnostic));
    CHECK(movedMeta.ID == sourceMeta.ID);
    REQUIRE(movedMeta.SubAssets.ContainsKey(TEXT("mesh:Body")));
    CHECK(movedMeta.SubAssets[TEXT("mesh:Body")].ID == sourceSubAssetGuid);
    REQUIRE(database.Database.TryGetRecord(sourceMeta.ID, record));
    CHECK(FileSystem::AreFilePathsEquivalent(record.SourcePath.Get(), moved));
    REQUIRE(database.Database.TryGetRecord(sourceSubAssetGuid, record));
    CHECK(FileSystem::AreFilePathsEquivalent(record.SourcePath.Get(), moved));

    target.SourcePath = moved;
    const String copied = content / TEXT("Robot Copy.gltf");
    Guid copiedGuid;
    REQUIRE_FALSE(operations.CopyAsset(target, copied, copiedGuid, diagnostic));
    AssetMeta copiedMeta;
    REQUIRE_FALSE(AssetMeta::Load(copied + TEXT(".meta"), copiedMeta, diagnostic));
    REQUIRE(copiedMeta.SubAssets.ContainsKey(TEXT("mesh:Body")));
    const SubAssetMeta& copiedSubAsset = copiedMeta.SubAssets[TEXT("mesh:Body")];
    CHECK(copiedMeta.ID == copiedGuid);
    CHECK(copiedGuid != sourceMeta.ID);
    CHECK(copiedSubAsset.ID != sourceSubAssetGuid);
    CHECK(copiedSubAsset.LocalId == movedMeta.SubAssets[TEXT("mesh:Body")].LocalId);
    CHECK(copiedSubAsset.CollisionSalt == movedMeta.SubAssets[TEXT("mesh:Body")].CollisionSalt);
    CHECK(copiedSubAsset.PreviousKeys == movedMeta.SubAssets[TEXT("mesh:Body")].PreviousKeys);
    REQUIRE(database.Database.TryGetRecord(copiedGuid, record));
    REQUIRE(database.Database.TryGetRecord(copiedSubAsset.ID, record));

    AssetTrashRecord trash;
    REQUIRE_FALSE(operations.DeleteAsset(target, trash, diagnostic));
    CHECK_FALSE(database.Database.TryGetRecord(sourceMeta.ID, record));
    CHECK_FALSE(database.Database.TryGetRecord(sourceSubAssetGuid, record));
    REQUIRE(database.Database.TryGetRecord(copiedGuid, record));
    REQUIRE(database.Database.TryGetRecord(copiedSubAsset.ID, record));

    const AssetMeta recreatedMeta = MakeOperationMeta();
    CHECK(recreatedMeta.ID != sourceMeta.ID);
    CHECK(recreatedMeta.ID != copiedGuid);
    CHECK(recreatedMeta.SubAssets[TEXT("mesh:Body")].ID != sourceSubAssetGuid);
    CHECK(recreatedMeta.SubAssets[TEXT("mesh:Body")].ID != copiedSubAsset.ID);
    REQUIRE_FALSE(operations.CreateAsset(moved,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), recreatedMeta, diagnostic));
    REQUIRE(database.Database.TryGetRecord(recreatedMeta.ID, record));
    REQUIRE(database.Database.TryGetRecord(recreatedMeta.SubAssets[TEXT("mesh:Body")].ID, record));
    CHECK_FALSE(database.Database.TryGetRecord(sourceMeta.ID, record));
    CHECK_FALSE(database.Database.TryGetRecord(sourceSubAssetGuid, record));
    REQUIRE(database.Database.TryGetRecord(copiedGuid, record));
    REQUIRE(database.Database.TryGetRecord(copiedSubAsset.ID, record));
}

TEST_CASE("Asset operations publish canonical copy batches once")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetCopyBatch-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const byte sourceBytes[] = { 1, 2, 3 };
    const AssetMeta firstMeta = MakeOperationMeta();
    const AssetMeta secondMeta = MakeOperationMeta();
    const String firstSource = content / TEXT("First.bin");
    const String secondSource = content / TEXT("Second.bin");
    REQUIRE_FALSE(operations.CreateAsset(firstSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), firstMeta, diagnostic));
    REQUIRE_FALSE(operations.CreateAsset(secondSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), secondMeta, diagnostic));
    const int32 baselineRefreshCalls = database.RefreshCalls;

    Array<AssetCopyEntryRequest> requests;
    requests.EnsureCapacity(2);
    AssetCopyEntryRequest& first = requests.AddOne();
    first.SourcePath = firstSource;
    first.DestinationPath = content / TEXT("First Copy.bin");
    first.ExpectedAssetGuid = firstMeta.ID;
    AssetCopyEntryRequest& second = requests.AddOne();
    second.SourcePath = secondSource;
    second.DestinationPath = content / TEXT("Second Copy.bin");
    second.ExpectedAssetGuid = secondMeta.ID;
    Array<Guid> copiedGuids;

    REQUIRE_FALSE(operations.CopyAssets(requests, copiedGuids, diagnostic));
    REQUIRE(copiedGuids.Count() == 2);
    CHECK(copiedGuids[0].IsValid());
    CHECK(copiedGuids[1].IsValid());
    CHECK(copiedGuids[0] != copiedGuids[1]);
    CHECK(FileSystem::FileExists(first.DestinationPath));
    CHECK(FileSystem::FileExists(second.DestinationPath));
    CHECK(database.RefreshCalls == baselineRefreshCalls + 1);
    REQUIRE(database.LastCommits.Count() == 2);
    CHECK(database.LastCommits[0].Kind == AssetOperationKind::Copy);
    CHECK(database.LastCommits[1].Kind == AssetOperationKind::Copy);
    CHECK(database.LastCommits[0].TransactionId.IsValid());
    CHECK(database.LastCommits[0].TransactionId == database.LastCommits[1].TransactionId);
}

TEST_CASE("Asset operations atomically copy mixed flattened Content batches")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetMixedCopyBatch-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const byte assetBytes[] = { 1, 3, 5 };
    const AssetMeta assetMeta = MakeOperationMeta();
    const String assetSource = content / TEXT("Asset.bin");
    REQUIRE_FALSE(operations.CreateAsset(assetSource,
        Span<byte>(const_cast<byte*>(assetBytes), ARRAY_COUNT(assetBytes)), assetMeta, diagnostic));
    const String sourceFolder = content / TEXT("Source Folder");
    REQUIRE_FALSE(FileSystem::CreateDirectory(sourceFolder));
    const byte noteBytes[] = { 2, 4, 6, 8 };
    const String noteSource = sourceFolder / TEXT("Note.txt");
    REQUIRE_FALSE(File::WriteAllBytes(noteSource, noteBytes, ARRAY_COUNT(noteBytes)));
    const AssetMeta folderMeta = MakeOperationMeta();
    REQUIRE_FALSE(AssetMeta::SaveAtomic(sourceFolder + TEXT(".meta"), folderMeta, diagnostic));
    const int32 baselineRefreshCalls = database.RefreshCalls;

    const String destinationFolder = content / TEXT("Folder Copy");
    Array<AssetCopyEntryRequest> requests;
    AssetCopyEntryRequest& asset = requests.AddOne();
    asset.SourcePath = assetSource;
    asset.DestinationPath = content / TEXT("Asset Copy.bin");
    asset.ExpectedAssetGuid = assetMeta.ID;
    AssetCopyEntryRequest& directory = requests.AddOne();
    directory.SourcePath = sourceFolder;
    directory.DestinationPath = destinationFolder;
    directory.Kind = AssetCopyEntryKind::Directory;
    AssetCopyEntryRequest& metadata = requests.AddOne();
    metadata.SourcePath = sourceFolder + TEXT(".meta");
    metadata.DestinationPath = destinationFolder + TEXT(".meta");
    metadata.Kind = AssetCopyEntryKind::MetadataSidecar;
    AssetCopyEntryRequest& note = requests.AddOne();
    note.SourcePath = noteSource;
    note.DestinationPath = destinationFolder / TEXT("Note.txt");
    note.Kind = AssetCopyEntryKind::File;
    Array<Guid> copiedGuids;

    REQUIRE_FALSE(operations.CopyAssets(requests, copiedGuids, diagnostic));
    REQUIRE(copiedGuids.Count() == 4);
    CHECK(copiedGuids[0].IsValid());
    CHECK_FALSE(copiedGuids[1].IsValid());
    CHECK(copiedGuids[2].IsValid());
    CHECK_FALSE(copiedGuids[3].IsValid());
    CHECK(copiedGuids[0] != assetMeta.ID);
    CHECK(copiedGuids[2] != folderMeta.ID);
    CHECK(FileSystem::FileExists(asset.DestinationPath));
    CHECK(FileSystem::DirectoryExists(destinationFolder));
    CHECK(FileSystem::FileExists(metadata.DestinationPath));
    CHECK(FileSystem::FileExists(note.DestinationPath));
    AssetMeta copiedFolderMeta;
    REQUIRE_FALSE(AssetMeta::Load(metadata.DestinationPath, copiedFolderMeta, diagnostic));
    CHECK(copiedFolderMeta.ID == copiedGuids[2]);
    BytesContainer copiedNote;
    REQUIRE_FALSE(File::ReadAllBytes(note.DestinationPath, copiedNote));
    CHECK(copiedNote.Length() == ARRAY_COUNT(noteBytes));
    CHECK(Platform::MemoryCompare(copiedNote.Get(), noteBytes, ARRAY_COUNT(noteBytes)) == 0);
    CHECK(database.RefreshCalls == baselineRefreshCalls + 1);
    REQUIRE(database.LastCommits.Count() == 4);
    for (const AssetOperationCommit& commit : database.LastCommits)
        CHECK(commit.TransactionId == database.LastCommits[0].TransactionId);
    CHECK(database.LastCommits[1].SelfWrites.IsEmpty());
    CHECK(database.LastCommits[2].SelfWrites.Count() == 1);
    CHECK(database.LastCommits[3].SelfWrites.Count() == 1);
}

TEST_CASE("Asset operations roll back mixed flattened copy failures")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetMixedCopyRollback-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const byte sourceBytes[] = { 9, 7, 5 };
    const AssetMeta assetMeta = MakeOperationMeta();
    const String assetSource = content / TEXT("Asset.bin");
    REQUIRE_FALSE(operations.CreateAsset(assetSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), assetMeta, diagnostic));
    const String sourceFolder = content / TEXT("Source Folder");
    REQUIRE_FALSE(FileSystem::CreateDirectory(sourceFolder));
    const String noteSource = sourceFolder / TEXT("Note.txt");
    REQUIRE_FALSE(File::WriteAllBytes(noteSource, sourceBytes, ARRAY_COUNT(sourceBytes)));
    const int32 baselineRefreshCalls = database.RefreshCalls;
    database.FailClearOnCall = database.ClearCalls + 1;

    const String destinationFolder = content / TEXT("Folder Copy");
    Array<AssetCopyEntryRequest> requests;
    AssetCopyEntryRequest& directory = requests.AddOne();
    directory.SourcePath = sourceFolder;
    directory.DestinationPath = destinationFolder;
    directory.Kind = AssetCopyEntryKind::Directory;
    AssetCopyEntryRequest& note = requests.AddOne();
    note.SourcePath = noteSource;
    note.DestinationPath = destinationFolder / TEXT("Note.txt");
    note.Kind = AssetCopyEntryKind::File;
    AssetCopyEntryRequest& asset = requests.AddOne();
    asset.SourcePath = assetSource;
    asset.DestinationPath = content / TEXT("Asset Copy.bin");
    asset.ExpectedAssetGuid = assetMeta.ID;
    Array<Guid> copiedGuids;

    CHECK(operations.CopyAssets(requests, copiedGuids, diagnostic));
    CHECK(copiedGuids.IsEmpty());
    CHECK_FALSE(FileSystem::DirectoryExists(destinationFolder));
    CHECK_FALSE(FileSystem::FileExists(note.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(asset.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(asset.DestinationPath + TEXT(".meta")));
    CHECK(FileSystem::FileExists(noteSource));
    CHECK(FileSystem::FileExists(assetSource));
    CHECK(database.RefreshCalls == baselineRefreshCalls);
}

TEST_CASE("Asset operations roll back canonical copy batch failures")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetCopyBatchRollback-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const byte sourceBytes[] = { 4, 5, 6 };
    const AssetMeta firstMeta = MakeOperationMeta();
    const AssetMeta secondMeta = MakeOperationMeta();
    const String firstSource = content / TEXT("First.bin");
    const String secondSource = content / TEXT("Second.bin");
    REQUIRE_FALSE(operations.CreateAsset(firstSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), firstMeta, diagnostic));
    REQUIRE_FALSE(operations.CreateAsset(secondSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), secondMeta, diagnostic));
    const int32 baselineRefreshCalls = database.RefreshCalls;
    database.FailClearOnCall = database.ClearCalls + 2;

    Array<AssetCopyEntryRequest> requests;
    requests.EnsureCapacity(2);
    AssetCopyEntryRequest& first = requests.AddOne();
    first.SourcePath = firstSource;
    first.DestinationPath = content / TEXT("First Copy.bin");
    first.ExpectedAssetGuid = firstMeta.ID;
    AssetCopyEntryRequest& second = requests.AddOne();
    second.SourcePath = secondSource;
    second.DestinationPath = content / TEXT("Second Copy.bin");
    second.ExpectedAssetGuid = secondMeta.ID;
    Array<Guid> copiedGuids;

    CHECK(operations.CopyAssets(requests, copiedGuids, diagnostic));
    CHECK(copiedGuids.IsEmpty());
    CHECK_FALSE(FileSystem::FileExists(first.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(first.DestinationPath + TEXT(".meta")));
    CHECK_FALSE(FileSystem::FileExists(second.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(second.DestinationPath + TEXT(".meta")));
    CHECK(FileSystem::FileExists(firstSource));
    CHECK(FileSystem::FileExists(secondSource));
    CHECK(database.RefreshCalls == baselineRefreshCalls);

    database.FailClearOnCall = 0;
    database.FailRefresh = true;
    const int32 publicationFailureBaseline = database.RefreshCalls;
    CHECK(operations.CopyAssets(requests, copiedGuids, diagnostic));
    CHECK(copiedGuids.IsEmpty());
    CHECK_FALSE(FileSystem::FileExists(first.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(first.DestinationPath + TEXT(".meta")));
    CHECK_FALSE(FileSystem::FileExists(second.DestinationPath));
    CHECK_FALSE(FileSystem::FileExists(second.DestinationPath + TEXT(".meta")));
    CHECK(database.RefreshCalls == publicationFailureBaseline + 2);
}

TEST_CASE("Project panel routes canonical multi-copy through native batch")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestEditorUtils");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunMultiCopyRoutesCanonicalSourcesThroughNativeBatch", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestEditorUtils"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

TEST_CASE("Project panel routes mixed folder copy through native batch")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestEditorUtils");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunMixedFolderCopyUsesOneOrderedNativeBatch", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestEditorUtils"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

TEST_CASE("Content importer overlaps independent canonical preparation")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestEditorUtils");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunConcurrentCanonicalImportCoordinatorTests", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestEditorUtils"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

TEST_CASE("Project panel preserves authored particle and collision text lifecycle")
{
#if USE_CSHARP && USE_NETCORE
    MClass* testClass = Scripting::FindClass("FlaxEngine.Tests.TestEditorUtils");
    REQUIRE(testClass);
    MMethod* testMethod = testClass->GetMethod("RunParticleAndCollisionAuthoredTextLifecycle", 0);
    REQUIRE(testMethod);
    MObject* exception = nullptr;
    MObject* result = testMethod->Invoke(nullptr, nullptr, &exception);
    if (exception)
        MException(exception).Log(LogType::Error, TEXT("TestEditorUtils"));
    CHECK_FALSE(exception);
    REQUIRE(result);
    CHECK(MUtils::Unbox<int32>(result) == 0);
#endif
}

TEST_CASE("Asset operations atomically remap external-actors scene copies")
{
    const String root = Globals::TemporaryFolder / (TEXT("ExternalActorCopy-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    AssetMeta sourceMeta = MakeOperationMeta();
    sourceMeta.AssetType = TEXT("FlaxEngine.SceneAsset");
    sourceMeta.SourceKind = AssetSourceKind::TextDocument;
    sourceMeta.Processor.ID = TEXT("Flax.JsonDocument");
    const Guid foreignGuid = Guid::New();
    const String sourceGuidText = sourceMeta.ID.ToString(Guid::FormatType::N).ToLower();
    const String foreignGuidText = foreignGuid.ToString(Guid::FormatType::N).ToLower();
    const StringAnsi sourceGuidAnsi(sourceGuidText);
    const StringAnsi foreignGuidAnsi(foreignGuidText);
    rapidjson_flax::StringBuffer sourceBuffer;
    PrettyJsonWriter sourceWriter(sourceBuffer);
    sourceWriter.StartObject();
    sourceWriter.JKEY("sceneVersion");
    sourceWriter.Int(4);
    sourceWriter.JKEY("externalActors");
    sourceWriter.Bool(true);
    sourceWriter.JKEY("selfSceneGuid");
    sourceWriter.String(sourceGuidAnsi.Get(), sourceGuidAnsi.Length());
    sourceWriter.JKEY("foreignGuid");
    sourceWriter.String(foreignGuidAnsi.Get(), foreignGuidAnsi.Length());
    sourceWriter.JKEY("rootActorLocalId");
    sourceWriter.Int64(4201);
    sourceWriter.JKEY("objects");
    sourceWriter.StartArray();
    sourceWriter.EndArray(0);
    sourceWriter.EndObject();

    const String source = content / TEXT("Source.scene");
    REQUIRE_FALSE(operations.CreateAsset(source,
        Span<byte>(reinterpret_cast<byte*>(const_cast<char*>(sourceBuffer.GetString())),
            static_cast<int32>(sourceBuffer.GetSize())), sourceMeta, diagnostic));
    rapidjson_flax::StringBuffer payloadBuffer;
    PrettyJsonWriter payloadWriter(payloadBuffer);
    payloadWriter.StartArray();
    payloadWriter.StartObject();
    payloadWriter.JKEY("ID");
    payloadWriter.String(sourceGuidAnsi.Get(), sourceGuidAnsi.Length());
    payloadWriter.JKEY("sceneReference");
    payloadWriter.String(sourceGuidAnsi.Get(), sourceGuidAnsi.Length());
    payloadWriter.JKEY("foreignReference");
    payloadWriter.String(foreignGuidAnsi.Get(), foreignGuidAnsi.Length());
    payloadWriter.JKEY("localId");
    payloadWriter.Int64(4201);
    payloadWriter.EndObject();
    payloadWriter.EndArray(1);
    SceneFragmentWrite fragmentWrite;
    fragmentWrite.RootActorLocalId = 4201;
    fragmentWrite.ContainedLocalIds.Add(4201);
    fragmentWrite.Payload.Set(reinterpret_cast<const byte*>(payloadBuffer.GetString()),
        static_cast<int32>(payloadBuffer.GetSize()));
    Array<SceneFragmentWrite> fragmentWrites;
    fragmentWrites.Add(MoveTemp(fragmentWrite));
    String fragmentError;
    REQUIRE_FALSE(WriteOperationSceneFragments(root, sourceMeta.ID, fragmentWrites, fragmentError));

    const String sourceFragment = SceneFragmentStore::GetScenePath(root, sourceMeta.ID) /
        SceneFragmentStore::GetRelativeFragmentPath(4201);
    BytesContainer originalSourceBytes;
    BytesContainer originalFragmentBytes;
    REQUIRE_FALSE(File::ReadAllBytes(source, originalSourceBytes));
    REQUIRE_FALSE(File::ReadAllBytes(sourceFragment, originalFragmentBytes));

    AssetOperationTarget target;
    target.SourcePath = source;
    target.ExpectedGuid = sourceMeta.ID;
    const String copied = content / TEXT("Copied.scene");
    Guid copiedGuid;
    REQUIRE_FALSE(operations.CopyAsset(target, copied, copiedGuid, diagnostic));
    REQUIRE(copiedGuid.IsValid());
    CHECK(copiedGuid != sourceMeta.ID);

    BytesContainer currentSourceBytes;
    BytesContainer currentFragmentBytes;
    REQUIRE_FALSE(File::ReadAllBytes(source, currentSourceBytes));
    REQUIRE_FALSE(File::ReadAllBytes(sourceFragment, currentFragmentBytes));
    CHECK(EqualBytes(currentSourceBytes, originalSourceBytes));
    CHECK(EqualBytes(currentFragmentBytes, originalFragmentBytes));

    BytesContainer copiedSourceBytes;
    REQUIRE_FALSE(File::ReadAllBytes(copied, copiedSourceBytes));
    rapidjson_flax::Document copiedDocument;
    copiedDocument.Parse(copiedSourceBytes.Get<char>(), copiedSourceBytes.Length());
    REQUIRE_FALSE(copiedDocument.HasParseError());
    CHECK(JsonTools::GetGuid(copiedDocument, "selfSceneGuid") == copiedGuid);
    CHECK(JsonTools::GetGuid(copiedDocument, "foreignGuid") == foreignGuid);
    REQUIRE(copiedDocument.HasMember("rootActorLocalId"));
    CHECK(copiedDocument["rootActorLocalId"].GetInt64() == 4201);

    const String copiedFragments = SceneFragmentStore::GetScenePath(root, copiedGuid);
    BytesContainer copiedFragmentBytes;
    REQUIRE_FALSE(File::ReadAllBytes(copiedFragments / SceneFragmentStore::GetRelativeFragmentPath(4201),
        copiedFragmentBytes));
    rapidjson_flax::Document copiedFragment;
    copiedFragment.Parse(copiedFragmentBytes.Get<char>(), copiedFragmentBytes.Length());
    REQUIRE_FALSE(copiedFragment.HasParseError());
    CHECK(JsonTools::GetGuid(copiedFragment, "ownerSceneGuid") == copiedGuid);
    REQUIRE(copiedFragment.HasMember("rootActorLocalId"));
    CHECK(copiedFragment["rootActorLocalId"].GetInt64() == 4201);
    const auto payload = copiedFragment.FindMember("payload");
    REQUIRE(payload != copiedFragment.MemberEnd());
    REQUIRE(payload->value.IsArray());
    REQUIRE(payload->value.Size() == 1);
    CHECK(JsonTools::GetGuid(payload->value[0], "ID") == copiedGuid);
    CHECK(JsonTools::GetGuid(payload->value[0], "sceneReference") == copiedGuid);
    CHECK(JsonTools::GetGuid(payload->value[0], "foreignReference") == foreignGuid);
    REQUIRE(payload->value[0].HasMember("localId"));
    CHECK(payload->value[0]["localId"].GetInt64() == 4201);

    AssetMeta copiedMeta;
    REQUIRE_FALSE(AssetMeta::Load(copied + TEXT(".meta"), copiedMeta, diagnostic));
    CHECK(copiedMeta.ID == copiedGuid);
    CHECK(FileSystem::FileExists(copiedFragments / TEXT("scene-fragments.index")));

    const String fragmentsRoot = SceneFragmentStore::GetRootPath(root);
    Array<String> expectedFragmentDirectories;
    REQUIRE_FALSE(FileSystem::GetChildDirectories(expectedFragmentDirectories, fragmentsRoot));
    REQUIRE(expectedFragmentDirectories.Count() == 2);

    const String collision = content / TEXT("Collision.scene");
    const byte collisionBytes[] = { 7, 8, 9, 10 };
    REQUIRE_FALSE(File::WriteAllBytes(collision, collisionBytes, ARRAY_COUNT(collisionBytes)));
    Guid rejectedGuid;
    CHECK(operations.CopyAsset(target, collision, rejectedGuid, diagnostic));
    CHECK_FALSE(rejectedGuid.IsValid());
    CHECK_FALSE(FileSystem::FileExists(collision + TEXT(".meta")));
    BytesContainer preservedCollisionBytes;
    REQUIRE_FALSE(File::ReadAllBytes(collision, preservedCollisionBytes));
    REQUIRE(preservedCollisionBytes.Length() == ARRAY_COUNT(collisionBytes));
    CHECK(Platform::MemoryCompare(preservedCollisionBytes.Get(), collisionBytes,
        ARRAY_COUNT(collisionBytes)) == 0);
    Array<String> collisionFragmentDirectories;
    REQUIRE_FALSE(FileSystem::GetChildDirectories(collisionFragmentDirectories, fragmentsRoot));
    CHECK(collisionFragmentDirectories.Count() == expectedFragmentDirectories.Count());
    for (const String& directory : expectedFragmentDirectories)
        CHECK(collisionFragmentDirectories.Contains(directory));

    const byte malformedSource[] = { '{', '"', 'e', 'x', 't', 'e', 'r', 'n', 'a', 'l', 'A', 'c', 't', 'o', 'r', 's', '"', ':', 't' };
    REQUIRE_FALSE(File::WriteAllBytes(source, malformedSource, ARRAY_COUNT(malformedSource)));
    const String failedCopy = content / TEXT("Malformed Copy.scene");
    Guid failedGuid;
    CHECK(operations.CopyAsset(target, failedCopy, failedGuid, diagnostic));
    CHECK_FALSE(failedGuid.IsValid());
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::LibraryCreationFailed);
    CHECK_FALSE(FileSystem::FileExists(failedCopy));
    CHECK_FALSE(FileSystem::FileExists(failedCopy + TEXT(".meta")));
    BytesContainer preservedMalformedSource;
    REQUIRE_FALSE(File::ReadAllBytes(source, preservedMalformedSource));
    REQUIRE(preservedMalformedSource.Length() == ARRAY_COUNT(malformedSource));
    CHECK(Platform::MemoryCompare(preservedMalformedSource.Get(), malformedSource,
        ARRAY_COUNT(malformedSource)) == 0);
    REQUIRE_FALSE(File::ReadAllBytes(sourceFragment, currentFragmentBytes));
    CHECK(EqualBytes(currentFragmentBytes, originalFragmentBytes));
    Array<String> failedFragmentDirectories;
    REQUIRE_FALSE(FileSystem::GetChildDirectories(failedFragmentDirectories, fragmentsRoot));
    CHECK(failedFragmentDirectories.Count() == expectedFragmentDirectories.Count());
    for (const String& directory : expectedFragmentDirectories)
        CHECK(failedFragmentDirectories.Contains(directory));
}

TEST_CASE("Default metadata batches roll back and recover native staged publication")
{
    const Guid testId = Guid::New();
    const String contentRoot = Globals::ProjectContentFolder /
        (TEXT("__MetadataBatchOperations-") + testId.ToString(Guid::FormatType::N));
    const String stagingRoot = Globals::ProjectLibraryFolder / TEXT("Temp/MetadataBatches") /
        testId.ToString(Guid::FormatType::N);
    const String first = contentRoot / TEXT("First.txt");
    const String second = contentRoot / TEXT("Second.txt");
    const String firstMeta = first + TEXT(".meta");
    REQUIRE_FALSE(FileSystem::CreateDirectory(contentRoot));
    REQUIRE_FALSE(FileSystem::CreateDirectory(stagingRoot));
    REQUIRE_FALSE(File::WriteAllText(first, TEXT("first"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(second, TEXT("second"), Encoding::ANSI));
    REQUIRE_FALSE(File::WriteAllText(firstMeta,
        TEXT("fileFormatVersion: 2\nguid: 00112233445566778899aabbccddeeff\n"), Encoding::ANSI));
    BytesContainer foreignMetadata;
    REQUIRE_FALSE(File::ReadAllBytes(firstMeta, foreignMetadata));
    SCOPE_EXIT
    {
        FileSystem::DeleteDirectory(contentRoot, true);
        FileSystem::DeleteDirectory(stagingRoot, true);
        Array<String> refresh;
        refresh.Add(contentRoot);
        AssetPipelineService::RefreshSources(refresh);
    };

    Array<String> sources;
    sources.Add(first);
    sources.Add(second);
    Array<String> staging;
    staging.Add(stagingRoot / TEXT("0.meta"));
    staging.Add(stagingRoot / TEXT("1.meta"));
    auto Stage = [&]()
    {
        const Array<Guid> ids = AssetOperationService::StageDefaultMetadataBatch(sources, staging);
        REQUIRE(ids.Count() == 2);
        REQUIRE(ids[0].IsValid());
        REQUIRE(ids[1].IsValid());
        Array<AssetDefaultMetadataBatchEntry> entries;
        entries.Resize(2);
        for (int32 i = 0; i < entries.Count(); i++)
        {
            entries[i].AssetID = ids[i];
            entries[i].SourcePath = sources[i];
            entries[i].StagingPath = staging[i];
        }
        entries[0].ReplaceExistingMetadata = true;
        return entries;
    };

    Array<AssetDefaultMetadataBatchEntry> entries = Stage();
    CHECK(AssetOperationService::PublishDefaultMetadataBatch(entries,
        AssetDefaultMetadataBatchFailurePoint::AfterFirstMetadata));
    BytesContainer restoredMetadata;
    REQUIRE_FALSE(File::ReadAllBytes(firstMeta, restoredMetadata));
    CHECK(EqualBytes(foreignMetadata, restoredMetadata));
    CHECK_FALSE(FileSystem::FileExists(second + TEXT(".meta")));
    CHECK_FALSE(FileSystem::FileExists(staging[0]));
    CHECK_FALSE(FileSystem::FileExists(staging[1]));

    entries = Stage();
    CHECK(AssetOperationService::PublishDefaultMetadataBatch(entries,
        AssetDefaultMetadataBatchFailurePoint::AfterFirstMetadataWithoutRollback));
    AssetMeta active;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetMeta::Load(firstMeta, active, diagnostic));
    CHECK(active.ID == entries[0].AssetID);

    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations recovery(Globals::ProjectFolder, Globals::ProjectContentFolder,
        Globals::ProjectLibraryFolder, processor, database);
    REQUIRE_FALSE(recovery.Initialize(diagnostic));
    Array<AssetPipelineDiagnostic> recoveryDiagnostics;
    REQUIRE_FALSE(recovery.RecoverIncompleteTransactions(recoveryDiagnostics));
    CHECK(recoveryDiagnostics.IsEmpty());
    REQUIRE_FALSE(File::ReadAllBytes(firstMeta, restoredMetadata));
    CHECK(EqualBytes(foreignMetadata, restoredMetadata));
    CHECK_FALSE(FileSystem::FileExists(second + TEXT(".meta")));
    CHECK_FALSE(FileSystem::FileExists(staging[0]));
    CHECK_FALSE(FileSystem::FileExists(staging[1]));

    entries = Stage();
    REQUIRE_FALSE(AssetOperationService::PublishDefaultMetadataBatch(entries));
    REQUIRE_FALSE(AssetMeta::Load(firstMeta, active, diagnostic));
    CHECK(active.ID == entries[0].AssetID);
    REQUIRE_FALSE(AssetMeta::Load(second + TEXT(".meta"), active, diagnostic));
    CHECK(active.ID == entries[1].AssetID);
    AssetRecord record;
    REQUIRE(AssetDatabase::Get().TryGetRecord(entries[0].AssetID, record));
    CHECK(FileSystem::AreFilePathsEquivalent(record.SourcePath.Get(), first));
    REQUIRE(AssetDatabase::Get().TryGetRecord(entries[1].AssetID, record));
    CHECK(FileSystem::AreFilePathsEquivalent(record.SourcePath.Get(), second));
}

TEST_CASE("Asset operations batch trash restores folders and private scene fragments atomically")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetTrashBatch-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    const String folder = content / TEXT("Maps");
    const String scene = folder / TEXT("Level.scene");
    const String note = content / TEXT("Notes.txt");
    REQUIRE_FALSE(FileSystem::CreateDirectory(folder));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    AssetMeta folderMeta;
    folderMeta.ID = Guid::New();
    folderMeta.FolderAsset = true;
    folderMeta.AssetType = TEXT("FlaxEngine.Folder");
    folderMeta.SourceKind = AssetSourceKind::Folder;
    folderMeta.Processor.ID = TEXT("Flax.Folder");
    folderMeta.Processor.SettingsVersion = 1;
    folderMeta.Processor.SettingsJson = "{}";
    REQUIRE_FALSE(AssetMeta::SaveAtomic(folder + TEXT(".meta"), folderMeta, diagnostic));
    const byte sceneBytes[] = { 1, 2, 3, 4 };
    REQUIRE_FALSE(File::WriteAllBytes(scene, sceneBytes, ARRAY_COUNT(sceneBytes)));
    AssetMeta sceneMeta = MakeOperationMeta();
    sceneMeta.AssetType = TEXT("FlaxEngine.SceneAsset");
    REQUIRE_FALSE(AssetMeta::SaveAtomic(scene + TEXT(".meta"), sceneMeta, diagnostic));
    const byte noteBytes[] = { 7, 8, 9 };
    REQUIRE_FALSE(File::WriteAllBytes(note, noteBytes, ARRAY_COUNT(noteBytes)));
    const String fragments = SceneFragmentStore::GetScenePath(root, sceneMeta.ID);
    REQUIRE_FALSE(FileSystem::CreateDirectory(fragments));
    REQUIRE_FALSE(File::WriteAllBytes(fragments / TEXT("fragment.bin"), sceneBytes, ARRAY_COUNT(sceneBytes)));

    Array<AssetTrashEntryRequest> nested;
    AssetTrashEntryRequest folderRequest;
    folderRequest.SourcePath = folder;
    folderRequest.IsFolder = true;
    nested.Add(folderRequest);
    AssetTrashEntryRequest sceneRequest;
    sceneRequest.SourcePath = scene;
    sceneRequest.ExpectedAssetGuid = sceneMeta.ID;
    nested.Add(sceneRequest);
    AssetTrashBatch rejected;
    CHECK(operations.TrashEntries(nested, rejected, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PathCollision);
    CHECK(FileSystem::DirectoryExists(folder));
    CHECK(FileSystem::DirectoryExists(fragments));

    Array<AssetTrashEntryRequest> requests;
    requests.Add(folderRequest);
    AssetTrashEntryRequest noteRequest;
    noteRequest.SourcePath = note;
    requests.Add(noteRequest);
    AssetTrashBatch trash;
    REQUIRE_FALSE(operations.TrashEntries(requests, trash, diagnostic));
    REQUIRE(trash.Entries.Count() == 2);
    CHECK_FALSE(FileSystem::DirectoryExists(folder));
    CHECK_FALSE(FileSystem::FileExists(folder + TEXT(".meta")));
    CHECK_FALSE(FileSystem::FileExists(note));
    CHECK_FALSE(FileSystem::DirectoryExists(fragments));
    CHECK(FileSystem::DirectoryExists(trash.Entries[0].TrashPath));
    REQUIRE(trash.Entries[0].Fragments.Count() == 1);
    CHECK(FileSystem::DirectoryExists(trash.Entries[0].Fragments[0].TrashPath));
    const String restoredTrashRoot = library / TEXT("AssetOperations/Trash") /
        trash.TransactionId.ToString(Guid::FormatType::N);
    REQUIRE(database.LastCommits.Count() == 2);
    CHECK(database.LastCommits[0].Kind == AssetOperationKind::Trash);

    REQUIRE_FALSE(operations.RestoreEntries(trash, diagnostic));
    CHECK(FileSystem::DirectoryExists(folder));
    CHECK(FileSystem::FileExists(folder + TEXT(".meta")));
    CHECK(FileSystem::FileExists(note));
    CHECK(FileSystem::DirectoryExists(fragments));
    CHECK_FALSE(FileSystem::DirectoryExists(restoredTrashRoot));
    REQUIRE(database.LastCommits.Count() == 2);
    CHECK(database.LastCommits[0].Kind == AssetOperationKind::Restore);

    database.FailRefresh = true;
    AssetTrashBatch failedTrash;
    CHECK(operations.TrashEntries(requests, failedTrash, diagnostic));
    CHECK(FileSystem::DirectoryExists(folder));
    CHECK(FileSystem::FileExists(note));
    CHECK(FileSystem::DirectoryExists(fragments));
    database.FailRefresh = false;

    REQUIRE_FALSE(operations.TrashEntries(requests, trash, diagnostic));
    const String nativeTrashRoot = library / TEXT("AssetOperations/Trash") /
        trash.TransactionId.ToString(Guid::FormatType::N);
    REQUIRE(FileSystem::DirectoryExists(nativeTrashRoot));
    REQUIRE_FALSE(File::WriteAllBytes(note, noteBytes, ARRAY_COUNT(noteBytes)));
    CHECK(operations.DiscardTrash(trash, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
    CHECK(FileSystem::DirectoryExists(nativeTrashRoot));
    REQUIRE_FALSE(FileSystem::DeleteFile(note));
    REQUIRE_FALSE(operations.DiscardTrash(trash, diagnostic));
    CHECK_FALSE(FileSystem::DirectoryExists(nativeTrashRoot));
}

TEST_CASE("Asset operations importer settings are revision-bound and atomic")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetImporterSettings-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    AssetMeta sourceMeta = MakeOperationMeta();
    sourceMeta.Processor.SettingsVersion = 2;
    const String source = content / TEXT("Robot.gltf");
    const String metaPath = source + TEXT(".meta");
    const byte sourceBytes[] = { 11, 22, 33, 44 };
    REQUIRE_FALSE(operations.CreateAsset(source,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), sourceMeta, diagnostic));
    AssetMeta persisted;
    REQUIRE_FALSE(AssetMeta::Load(metaPath, persisted, diagnostic));
    BytesContainer originalMetaBytes;
    BytesContainer originalSourceBytes;
    REQUIRE_FALSE(File::ReadAllBytes(metaPath, originalMetaBytes));
    REQUIRE_FALSE(File::ReadAllBytes(source, originalSourceBytes));

    AssetOperationTarget target;
    target.SourcePath = source;
    target.ExpectedGuid = persisted.ID;
    AssetImporterSettingsRevision expected;
    expected.SourceRevision = 17;
    expected.MetaSemanticHash = GetOperationMetaSemanticHash(persisted);
    expected.ImporterID = persisted.Processor.ID;
    expected.StoredSettingsVersion = persisted.Processor.SettingsVersion;
    database.HasImporterRevision = true;
    database.ImporterTarget = target;
    database.ImporterRevision = expected;
    processor.Calls = 0;
    database.RefreshCalls = 0;
    database.LastCommits.Clear();
    Array<AssetOperationSelfWrite> discardedWrites;
    operations.DrainSelfWrites(discardedWrites);

    SECTION("preserves unrelated metadata and source bytes")
    {
        REQUIRE_FALSE(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"z\":1,\"a\":2}"), diagnostic));
        AssetMeta updated;
        REQUIRE_FALSE(AssetMeta::Load(metaPath, updated, diagnostic));
        CHECK(updated.ID == persisted.ID);
        CHECK(updated.AssetType == persisted.AssetType);
        CHECK(updated.Processor.ID == persisted.Processor.ID);
        CHECK(updated.Processor.SettingsVersion == 3);
        CHECK(updated.Processor.SettingsJson == "{\n  \"a\": 2,\n  \"z\": 1\n}\n");
        REQUIRE(updated.Processor.UnknownFields.ContainsKey("processorExtension"));
        CHECK(updated.Processor.UnknownFields["processorExtension"] == persisted.Processor.UnknownFields["processorExtension"]);
        REQUIRE(updated.MainObjectUnknownFields.ContainsKey("mainExtension"));
        CHECK(updated.MainObjectUnknownFields["mainExtension"] == persisted.MainObjectUnknownFields["mainExtension"]);
        REQUIRE(updated.SubAssets.ContainsKey(TEXT("mesh:Body")));
        CHECK(updated.SubAssets[TEXT("mesh:Body")].ID == persisted.SubAssets[TEXT("mesh:Body")].ID);
        CHECK(updated.SubAssets[TEXT("mesh:Body")].LocalId == persisted.SubAssets[TEXT("mesh:Body")].LocalId);
        CHECK(updated.SubAssets[TEXT("mesh:Body")].TypeName == persisted.SubAssets[TEXT("mesh:Body")].TypeName);
        CHECK(updated.SubAssets[TEXT("mesh:Body")].DisplayName == persisted.SubAssets[TEXT("mesh:Body")].DisplayName);
        CHECK(updated.Labels == persisted.Labels);
        CHECK(updated.UserDataJson == persisted.UserDataJson);
        REQUIRE(updated.UnknownFields.ContainsKey("rootExtension"));
        CHECK(updated.UnknownFields["rootExtension"] == persisted.UnknownFields["rootExtension"]);
        BytesContainer currentSourceBytes;
        REQUIRE_FALSE(File::ReadAllBytes(source, currentSourceBytes));
        CHECK(EqualBytes(currentSourceBytes, originalSourceBytes));
        CHECK(processor.Calls == 1);
        CHECK(processor.LastKind == AssetOperationKind::ImporterSettings);
        CHECK(database.ImporterRevisionCalls == 2);
        CHECK(database.RefreshCalls == 1);
        REQUIRE(database.LastCommits.Count() == 1);
        CHECK(database.LastCommits[0].Kind == AssetOperationKind::ImporterSettings);
        CHECK(database.LastCommits[0].AssetGuid == persisted.ID);
        CHECK(FileSystem::AreFilePathsEquivalent(database.LastCommits[0].SourcePath, source));
        Array<AssetOperationSelfWrite> writes;
        operations.DrainSelfWrites(writes);
        REQUIRE(writes.Count() == 1);
        CHECK(FileSystem::AreFilePathsEquivalent(writes[0].Path, metaPath));
    }

    SECTION("canonical no-op does not authorize or publish a mutation")
    {
        REQUIRE_FALSE(operations.WriteImporterSettings(target, expected, 2,
            StringAnsiView(" { \"scale\" : 2 } "), diagnostic));
        BytesContainer currentMetaBytes;
        REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
        CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        CHECK(database.ImporterRevisionCalls == 1);
        CHECK(processor.Calls == 0);
        CHECK(database.RefreshCalls == 0);
        Array<AssetOperationSelfWrite> writes;
        operations.DrainSelfWrites(writes);
        CHECK(writes.IsEmpty());
    }

    SECTION("stale durable revision is rejected")
    {
        database.ImporterRevision.SourceRevision++;
        CHECK(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
        CHECK(processor.Calls == 0);
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("stale disk metadata is rejected")
    {
        expected.MetaSemanticHash ^= 0x5a5a5a5aU;
        database.ImporterRevision = expected;
        CHECK(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
        CHECK(processor.Calls == 0);
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("stale importer identity and settings version are rejected")
    {
        AssetImporterSettingsRevision stale = expected;
        stale.ImporterID = TEXT("Flax.Texture");
        database.ImporterRevision = stale;
        CHECK(operations.WriteImporterSettings(target, stale, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
        stale = expected;
        stale.StoredSettingsVersion--;
        database.ImporterRevision = stale;
        CHECK(operations.WriteImporterSettings(target, stale, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
        CHECK(processor.Calls == 0);
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("malformed and non-object settings are rejected")
    {
        CHECK(operations.WriteImporterSettings(target, expected, 3, StringAnsiView("{"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
        CHECK(operations.WriteImporterSettings(target, expected, 3, StringAnsiView("[1,2]"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
        BytesContainer currentMetaBytes;
        REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
        CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        CHECK(processor.Calls == 0);
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("modification denial leaves metadata unchanged")
    {
        processor.Deny = true;
        CHECK(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        BytesContainer currentMetaBytes;
        REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
        CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        CHECK(processor.Calls == 1);
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("injected publication failures preserve the complete old sidecar")
    {
        const AssetMetaWriteFailurePoint failures[] =
        {
            AssetMetaWriteFailurePoint::BeforeWrite,
            AssetMetaWriteFailurePoint::AfterWrite,
            AssetMetaWriteFailurePoint::AfterValidate,
            AssetMetaWriteFailurePoint::BeforeReplace,
        };
        for (const AssetMetaWriteFailurePoint failure : failures)
        {
            CHECK(operations.WriteImporterSettings(target, expected, 3,
                StringAnsiView("{\"scale\":4}"), diagnostic, failure));
            BytesContainer currentMetaBytes;
            REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
            CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        }
        CHECK(database.RefreshCalls == 0);
    }

    SECTION("database publication failure restores the prior sidecar")
    {
        database.FailRefresh = true;
        CHECK(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildFailed);
        BytesContainer currentMetaBytes;
        REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
        CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        CHECK(database.RefreshCalls == 1);

        database.FailRefresh = false;
        operations.StartAssetEditing();
        REQUIRE_FALSE(operations.StopAssetEditing(diagnostic));
        CHECK(database.RefreshCalls == 1);
    }

    SECTION("editing batches reject importer settings without changing metadata")
    {
        operations.StartAssetEditing();
        CHECK(operations.WriteImporterSettings(target, expected, 3,
            StringAnsiView("{\"scale\":4}"), diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated);
        BytesContainer currentMetaBytes;
        REQUIRE_FALSE(File::ReadAllBytes(metaPath, currentMetaBytes));
        CHECK(EqualBytes(currentMetaBytes, originalMetaBytes));
        REQUIRE_FALSE(operations.StopAssetEditing(diagnostic));
        CHECK(database.RefreshCalls == 0);
    }
}

TEST_CASE("Asset operations reject private and unregistered source roots")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetOperationRoots-") + Guid::New().ToString(Guid::FormatType::N));
    const String content = root / TEXT("Content");
    const String library = root / TEXT("Library");
    const String externalActors = root / TEXT("ExternalActors");
    const String readOnly = root / TEXT("SharedContent");
    REQUIRE_FALSE(FileSystem::CreateDirectory(content));
    REQUIRE_FALSE(FileSystem::CreateDirectory(externalActors));
    REQUIRE_FALSE(FileSystem::CreateDirectory(readOnly));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    OperationProcessor processor;
    OperationDatabase database;
    AssetOperations operations(root, content, library, processor, database);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(operations.Initialize(diagnostic));

    const byte sourceBytes[] = { 1, 2, 3 };
    const AssetMeta meta = MakeOperationMeta();
    const String privateSource = externalActors / TEXT("Scene/Actor.sceneactor");
    CHECK(operations.CreateAsset(privateSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), meta, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    CHECK_FALSE(FileSystem::FileExists(privateSource));
    CHECK(processor.Calls == 0);

    const String unregisteredSource = readOnly / TEXT("Material.json");
    CHECK(operations.CreateAsset(unregisteredSource,
        Span<byte>(const_cast<byte*>(sourceBytes), ARRAY_COUNT(sourceBytes)), meta, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    CHECK_FALSE(FileSystem::FileExists(unregisteredSource));
    CHECK(processor.Calls == 0);
}

#endif
