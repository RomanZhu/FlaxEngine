// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetOperations.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Level/SceneFragments/SceneFragmentStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
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
