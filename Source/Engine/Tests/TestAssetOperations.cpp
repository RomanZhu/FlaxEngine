// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/AssetOperations.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/FileSystem.h"
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

        bool ValidateOperation(AssetOperationKind kind, const AssetOperationTarget& target,
            const StringView& destination, AssetPipelineDiagnostic& diagnostic) override
        {
            Calls++;
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

        bool ClearCopiedState(const Guid& sourceGuid, const Guid& copiedGuid,
            AssetPipelineDiagnostic& diagnostic) override
        {
            ClearCalls++;
            ClearedSource = sourceGuid;
            ClearedCopy = copiedGuid;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool RefreshCommitted(const Array<AssetOperationCommit>& commits,
            AssetPipelineDiagnostic& diagnostic) override
        {
            RefreshCalls++;
            LastCommits = commits;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };
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
    Array<AssetPipelineDiagnostic> recoveryDiagnostics;
    CHECK_FALSE(operations.RecoverIncompleteTransactions(recoveryDiagnostics));
    CHECK(recoveryDiagnostics.IsEmpty());
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
