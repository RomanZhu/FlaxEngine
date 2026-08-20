// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Content/Cache/AssetsCache.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Artifacts/ResolvedArtifact.h"
#include "Engine/Content/AssetPipeline/AssetPipelineSettings.h"
#include "Engine/Content/Factories/BinaryAssetFactory.h"
#include "Engine/Content/Loading/ContentLoadTask.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Upgraders/BinaryAssetUpgrader.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/ObjectsRemovalService.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    void WriteRawDataAsset(const String& path, const Guid& id, uint32 version, const byte* bytes, int32 length)
    {
        FlaxChunk chunk;
        chunk.Data.Copy(bytes, length);

        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = RawDataAsset::TypeName;
        data.Header.Chunks[0] = &chunk;
        data.SerializedVersion = version;
        REQUIRE(!FlaxStorage::Create(path, data, true));
    }

    void WriteRawDataAssetWithoutChunks(const String& path, const Guid& id)
    {
        AssetInitData data;
        data.Header.ID = id;
        data.Header.TypeName = RawDataAsset::TypeName;
        data.SerializedVersion = RawDataAsset::SerializedVersion;
        REQUIRE(!FlaxStorage::Create(path, data, true));
    }

    void CleanupRawDataAsset(const String& path, RawDataAsset*& asset)
    {
        if (asset)
        {
            Content::UnloadAsset(asset);
            asset = nullptr;
        }
        Content::GetRegistry()->DeleteAsset(path, nullptr);
        FileSystem::DeleteFile(path);
    }

    bool CopyUpgrade(AssetMigrationContext& context)
    {
        return BinaryAssetUpgrader::CopyChunks(context);
    }

    class CharacterizationUpgrader : public BinaryAssetUpgrader
    {
    public:
        CharacterizationUpgrader()
        {
            static const Upgrader Steps[] =
            {
                { 1, 2, CopyUpgrade },
                { 2, 3, CopyUpgrade },
            };
            setup(Steps, ARRAY_COUNT(Steps));
        }
    };
}

TEST_CASE("Binary asset factory load streaming and reload")
{
    const String path = Globals::ProjectContentFolder / TEXT("__BinaryAssetCharacterization.flax");
    const Guid id = Guid::New();
    const byte initialBytes[] = { 1, 3, 5, 7 };
    const byte replacementBytes[] = { 2, 4, 6, 8, 10 };
    RawDataAsset* asset = nullptr;
    CleanupRawDataAsset(path, asset);
    SCOPE_EXIT
    {
        CleanupRawDataAsset(path, asset);
    };

    WriteRawDataAsset(path, id, RawDataAsset::SerializedVersion, initialBytes, ARRAY_COUNT(initialBytes));
    Content::GetRegistry()->RegisterAsset(id, RawDataAsset::TypeName, path);

    asset = Content::Load<RawDataAsset>(path);
    REQUIRE(asset);
    REQUIRE_FALSE(asset->WaitForLoaded());
    REQUIRE(asset->Storage);
    CHECK(asset->Storage->GetPath() == path);
    CHECK(asset->Data.Count() == ARRAY_COUNT(initialBytes));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), initialBytes, ARRAY_COUNT(initialBytes)) == 0);
    CHECK(asset->HasChunk(0));
    CHECK(asset->HasChunkLoaded(0));

    asset->ReleaseChunk(0);
    CHECK_FALSE(asset->HasChunkLoaded(0));
    auto* chunkTask = asset->RequestChunkDataAsync(0);
    REQUIRE(chunkTask);
    chunkTask->Start();
    CHECK_FALSE(chunkTask->Wait());
    CHECK(asset->HasChunkLoaded(0));

    auto storage = ContentStorageManager::GetStorage(path);
    REQUIRE(storage);
    storage->CloseFileHandles();
    WriteRawDataAsset(path, id, RawDataAsset::SerializedVersion, replacementBytes, ARRAY_COUNT(replacementBytes));
    REQUIRE_FALSE(storage->Reload());
    REQUIRE_FALSE(asset->WaitForLoaded());
    REQUIRE(asset->Data.Count() == ARRAY_COUNT(replacementBytes));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), replacementBytes, ARRAY_COUNT(replacementBytes)) == 0);
}

TEST_CASE("Binary asset factory separates canonical and storage paths")
{
    const String root = Globals::ProjectLibraryFolder / TEXT("Tests/DistinctPath");
    const String storagePath = root / TEXT("artifact.flax");
    const String canonicalPath = Globals::ProjectContentFolder / TEXT("DistinctPath.source");
    const Guid id = Guid::New();
    const byte payload[] = { 6, 2, 6, 4 };
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(storagePath);
        FileSystem::DeleteDirectory(root, true);
    };

    WriteRawDataAsset(storagePath, id, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    AssetLoadLocation location;
    location.Info = AssetInfo(id, RawDataAsset::TypeName, canonicalPath);
    location.Artifact.AssetID = id;
    location.Artifact.TypeName = RawDataAsset::TypeName;
    location.Artifact.StoragePath = ArtifactStoragePath(storagePath);
    location.Artifact.OutputKind = TEXT("runtime");
    location.Artifact.Key = TEXT("fixture-key");
    location.Artifact.StorageKind = ArtifactStorageKind::Generated;
    location.Artifact.IsExact = true;

    auto* factory = static_cast<BinaryAssetFactoryBase*>(Content::GetAssetFactory(location.Info));
    REQUIRE(factory);
    CHECK(ArtifactLease::GetCount(storagePath) == 0);
    auto* asset = static_cast<RawDataAsset*>(factory->New(location));
    REQUIRE(asset);
    CHECK(asset->GetPath() == canonicalPath);
    CHECK(asset->GetSourcePath() == canonicalPath);
    CHECK(asset->GetLogicalPath() == canonicalPath);
    CHECK(asset->GetStoragePath() == storagePath);
    CHECK(asset->GetArtifactKey() == TEXT("fixture-key"));
    CHECK(asset->IsUsingExactArtifact());
    CHECK(asset->IsUsingGeneratedArtifact());
    CHECK(asset->HasArtifactLease());
    CHECK(ArtifactLease::GetCount(storagePath) == 1);
    CHECK(ArtifactLease::HasLeaseWithin(root));
    REQUIRE_FALSE(asset->Storage->Load());
    CHECK_FALSE(factory->Init(asset));
    CHECK_FALSE(asset->LoadChunk(0));
    Delete(asset);
    CHECK(ArtifactLease::GetCount(storagePath) == 0);

    AssetLoadLocation mismatched = location;
    mismatched.Info.ID = Guid::New();
    mismatched.Artifact.AssetID = mismatched.Info.ID;
    auto* mismatchAsset = static_cast<RawDataAsset*>(factory->New(mismatched));
    REQUIRE(mismatchAsset);
    CHECK(factory->Init(mismatchAsset));
    Delete(mismatchAsset);

    mismatched = location;
    mismatched.Artifact.TypeName = TEXT("FlaxEngine.Texture");
    CHECK(factory->New(mismatched) == nullptr);
}

TEST_CASE("Artifact leases are reference counted")
{
    const String path = Globals::ProjectLibraryFolder / TEXT("Tests/lease.artifact");
    CHECK_FALSE(ArtifactLease::IsLeased(path));
    {
        ArtifactLease first = ArtifactLease::Acquire(path);
        CHECK(ArtifactLease::GetCount(path) == 1);
        {
            ArtifactLease second = first;
            CHECK(ArtifactLease::GetCount(path) == 2);
            ArtifactLease third = MoveTemp(second);
            CHECK_FALSE(second.IsValid());
            CHECK(third.IsValid());
            CHECK(ArtifactLease::GetCount(path) == 2);
        }
        CHECK(ArtifactLease::GetCount(path) == 1);
    }
    CHECK_FALSE(ArtifactLease::IsLeased(path));
}

TEST_CASE("Binary asset storage switch succeeds or restores old storage")
{
    const String root = Globals::ProjectLibraryFolder / TEXT("Tests/StorageSwitch");
    const String canonicalPath = Globals::ProjectContentFolder / TEXT("__BinaryAssetStorageSwitch.flax");
    const String replacementPath = root / TEXT("replacement.flax");
    const String invalidPath = root / TEXT("invalid.flax");
    const String unloadablePath = root / TEXT("unloadable.flax");
    const String concurrentPath = root / TEXT("concurrent.flax");
    const Guid id = Guid::New();
    const byte initialBytes[] = { 1, 2, 3 };
    const byte replacementBytes[] = { 8, 5, 3, 1 };
    const byte concurrentBytes[] = { 9, 9, 7, 7, 5 };
    RawDataAsset* asset = nullptr;
    CleanupRawDataAsset(canonicalPath, asset);
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        CleanupRawDataAsset(canonicalPath, asset);
        ContentStorageManager::EnsureAccess(replacementPath);
        ContentStorageManager::EnsureAccess(invalidPath);
        ContentStorageManager::EnsureAccess(unloadablePath);
        ContentStorageManager::EnsureAccess(concurrentPath);
        FileSystem::DeleteDirectory(root, true);
    };

    WriteRawDataAsset(canonicalPath, id, RawDataAsset::SerializedVersion, initialBytes, ARRAY_COUNT(initialBytes));
    WriteRawDataAsset(replacementPath, id, RawDataAsset::SerializedVersion, replacementBytes, ARRAY_COUNT(replacementBytes));
    WriteRawDataAsset(invalidPath, Guid::New(), RawDataAsset::SerializedVersion, replacementBytes, ARRAY_COUNT(replacementBytes));
    WriteRawDataAssetWithoutChunks(unloadablePath, id);
    WriteRawDataAsset(concurrentPath, id, RawDataAsset::SerializedVersion, concurrentBytes, ARRAY_COUNT(concurrentBytes));
    Content::GetRegistry()->RegisterAsset(id, RawDataAsset::TypeName, canonicalPath);
    asset = Content::Load<RawDataAsset>(canonicalPath);
    REQUIRE(asset);
    REQUIRE_FALSE(asset->WaitForLoaded());

    ResolvedArtifact replacement;
    replacement.AssetID = id;
    replacement.TypeName = RawDataAsset::TypeName;
    replacement.StoragePath = ArtifactStoragePath(replacementPath);
    replacement.OutputKind = TEXT("runtime");
    replacement.Key = TEXT("replacement-key");
    replacement.StorageKind = ArtifactStorageKind::Generated;
    REQUIRE(asset->SwitchStorage(replacement) == BinaryAssetStorageSwitchResult::Success);
    CHECK(asset->GetPath() == canonicalPath);
    CHECK(asset->GetStoragePath() == replacementPath);
    CHECK(asset->GetArtifactKey() == TEXT("replacement-key"));
    REQUIRE(asset->Data.Count() == ARRAY_COUNT(replacementBytes));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), replacementBytes, ARRAY_COUNT(replacementBytes)) == 0);
    CHECK(ArtifactLease::GetCount(replacementPath) == 1);

    ResolvedArtifact invalid = replacement;
    invalid.StoragePath = ArtifactStoragePath(invalidPath);
    CHECK(asset->SwitchStorage(invalid) == BinaryAssetStorageSwitchResult::InvalidArtifact);
    CHECK(asset->GetStoragePath() == replacementPath);
    CHECK(ArtifactLease::GetCount(invalidPath) == 0);

    ResolvedArtifact unloadable = replacement;
    unloadable.StoragePath = ArtifactStoragePath(unloadablePath);
    unloadable.Key = TEXT("unloadable-key");
    CHECK(asset->SwitchStorage(unloadable) == BinaryAssetStorageSwitchResult::LoadFailed);
    CHECK(asset->GetStoragePath() == replacementPath);
    CHECK(asset->GetArtifactKey() == TEXT("replacement-key"));
    REQUIRE(asset->Data.Count() == ARRAY_COUNT(replacementBytes));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), replacementBytes, ARRAY_COUNT(replacementBytes)) == 0);
    CHECK(ArtifactLease::GetCount(unloadablePath) == 0);
    CHECK(ArtifactLease::GetCount(replacementPath) == 1);

    asset->ReleaseChunk(0);
    auto* oldReader = asset->RequestChunkDataAsync(0);
    REQUIRE(oldReader);
    CHECK(ArtifactLease::GetCount(replacementPath) == 2);
    ResolvedArtifact concurrent = replacement;
    concurrent.StoragePath = ArtifactStoragePath(concurrentPath);
    concurrent.Key = TEXT("concurrent-key");
    REQUIRE(asset->SwitchStorage(concurrent) == BinaryAssetStorageSwitchResult::Success);
    CHECK(ArtifactLease::GetCount(replacementPath) == 1);
    oldReader->Start();
    CHECK_FALSE(oldReader->Wait());
    CHECK(ArtifactLease::GetCount(replacementPath) == 0);
    REQUIRE(asset->Data.Count() == ARRAY_COUNT(concurrentBytes));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), concurrentBytes, ARRAY_COUNT(concurrentBytes)) == 0);

    CleanupRawDataAsset(canonicalPath, asset);
    ObjectsRemovalService::Flush();
    CHECK(ArtifactLease::GetCount(concurrentPath) == 0);
}

TEST_CASE("Binary asset storage rejects malformed and unsupported data")
{
    const String corruptPath = Globals::ProjectContentFolder / TEXT("__BinaryAssetCorrupt.flax");
    const String truncatedPath = Globals::ProjectContentFolder / TEXT("__BinaryAssetTruncated.flax");
    const String versionPath = Globals::ProjectContentFolder / TEXT("__BinaryAssetUnsupportedVersion.flax");
    const byte payload[] = { 11, 22, 33, 44, 55 };
    RawDataAsset* corruptAsset = nullptr;
    RawDataAsset* truncatedAsset = nullptr;
    RawDataAsset* versionAsset = nullptr;
    CleanupRawDataAsset(corruptPath, corruptAsset);
    CleanupRawDataAsset(truncatedPath, truncatedAsset);
    CleanupRawDataAsset(versionPath, versionAsset);
    SCOPE_EXIT
    {
        CleanupRawDataAsset(corruptPath, corruptAsset);
        CleanupRawDataAsset(truncatedPath, truncatedAsset);
        CleanupRawDataAsset(versionPath, versionAsset);
    };

    const Guid corruptId = Guid::New();
    WriteRawDataAsset(corruptPath, corruptId, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    BytesContainer bytes;
    REQUIRE(!File::ReadAllBytes(corruptPath, bytes));
    REQUIRE(bytes.Length() > 16);
    bytes[0] ^= 0xff;
    REQUIRE(!File::WriteAllBytes(corruptPath, bytes.Get(), bytes.Length()));
    Content::GetRegistry()->RegisterAsset(corruptId, RawDataAsset::TypeName, corruptPath);
    corruptAsset = Content::Load<RawDataAsset>(corruptPath);
    CHECK_FALSE(corruptAsset);

    const Guid truncatedId = Guid::New();
    WriteRawDataAsset(truncatedPath, truncatedId, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    REQUIRE(!File::ReadAllBytes(truncatedPath, bytes));
    REQUIRE(bytes.Length() > 2);
    REQUIRE(!File::WriteAllBytes(truncatedPath, bytes.Get(), bytes.Length() / 2));
    Content::GetRegistry()->RegisterAsset(truncatedId, RawDataAsset::TypeName, truncatedPath);
    truncatedAsset = Content::Load<RawDataAsset>(truncatedPath);
    CHECK_FALSE(truncatedAsset);

    const Guid versionId = Guid::New();
    WriteRawDataAsset(versionPath, versionId, RawDataAsset::SerializedVersion + 1, payload, ARRAY_COUNT(payload));
    Content::GetRegistry()->RegisterAsset(versionId, RawDataAsset::TypeName, versionPath);
    versionAsset = Content::Load<RawDataAsset>(versionPath);
    CHECK_FALSE(versionAsset);
}

TEST_CASE("Generated unsupported binary requests rebuild without mutation")
{
    const String root = Globals::ProjectLibraryFolder / TEXT("Tests/GeneratedUpgradePolicy");
    const String path = root / TEXT("old-version.flax");
    const String canonicalPath = Globals::ProjectContentFolder / TEXT("GeneratedUpgradePolicy.source");
    const Guid id = Guid::New();
    const byte payload[] = { 4, 8, 15, 16, 23, 42 };
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(path);
        FileSystem::DeleteDirectory(root, true);
    };

    WriteRawDataAsset(path, id, RawDataAsset::SerializedVersion + 1, payload, ARRAY_COUNT(payload));
    BytesContainer before;
    REQUIRE_FALSE(File::ReadAllBytes(path, before));

    AssetLoadLocation location;
    location.Info = AssetInfo(id, RawDataAsset::TypeName, canonicalPath);
    location.Artifact.AssetID = id;
    location.Artifact.TypeName = RawDataAsset::TypeName;
    location.Artifact.StoragePath = ArtifactStoragePath(path);
    location.Artifact.OutputKind = TEXT("runtime");
    location.Artifact.Key = TEXT("old-key");
    location.Artifact.StorageKind = ArtifactStorageKind::Generated;
    auto* factory = static_cast<BinaryAssetFactoryBase*>(Content::GetAssetFactory(location.Info));
    REQUIRE(factory);
    auto* asset = static_cast<RawDataAsset*>(factory->New(location));
    REQUIRE(asset);
    REQUIRE_FALSE(asset->Storage->Load());
    CHECK(factory->Init(asset));
    CHECK(asset->NeedsArtifactRebuild());

    asset->Storage->CloseFileHandles();
    BytesContainer after;
    REQUIRE_FALSE(File::ReadAllBytes(path, after));
    REQUIRE(after.Length() == before.Length());
    CHECK(Platform::MemoryCompare(after.Get(), before.Get(), before.Length()) == 0);
    Delete(asset);
}

TEST_CASE("Content resolves an explicit distinct Library load location behind rollout flags")
{
    const String root = Globals::ProjectLibraryFolder / TEXT("Artifacts/FoundationFixture");
    const String canonicalPath = Globals::ProjectContentFolder / TEXT("__FoundationFixture.source");
    const String legacyPath = Globals::ProjectContentFolder / TEXT("__FoundationLegacy.flax");
    const String storagePath = root / TEXT("runtime.flax");
    const Guid id = Guid::New();
    const Guid legacyId = Guid::New();
    const byte sourceBytes[] = { 's', 'o', 'u', 'r', 'c', 'e' };
    const byte payload[] = { 10, 20, 30, 40 };
    RawDataAsset* asset = nullptr;
    RawDataAsset* legacyAsset = nullptr;
    AssetPipelineSettings* settings = AssetPipelineSettings::Get();
    const bool oldDatabaseFlag = settings->UseNewAssetDatabase;
    const bool oldLibraryFlag = settings->UseLibraryArtifacts;
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    REQUIRE_FALSE(File::WriteAllBytes(canonicalPath, sourceBytes, ARRAY_COUNT(sourceBytes)));
    WriteRawDataAsset(storagePath, id, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    SCOPE_EXIT
    {
        if (asset)
            Content::UnloadAsset(asset);
        CleanupRawDataAsset(legacyPath, legacyAsset);
        Content::UnregisterAssetLoadLocation(id);
        Content::GetRegistry()->DeleteAsset(canonicalPath, nullptr);
        ContentStorageManager::EnsureAccess(storagePath);
        FileSystem::DeleteDirectory(root, true);
        FileSystem::DeleteFile(canonicalPath);
        settings->UseNewAssetDatabase = oldDatabaseFlag;
        settings->UseLibraryArtifacts = oldLibraryFlag;
    };

    AssetLoadLocation location;
    location.Info = AssetInfo(id, RawDataAsset::TypeName, canonicalPath);
    location.Artifact.AssetID = id;
    location.Artifact.TypeName = RawDataAsset::TypeName;
    location.Artifact.StoragePath = ArtifactStoragePath(storagePath);
    location.Artifact.OutputKind = TEXT("runtime");
    location.Artifact.Key = TEXT("foundation-key");
    location.Artifact.StorageKind = ArtifactStorageKind::Generated;
    location.Artifact.IsExact = true;

    AssetPipelineDiagnostic diagnostic;
    settings->UseNewAssetDatabase = false;
    settings->UseLibraryArtifacts = false;
    CHECK(Content::RegisterAssetLoadLocation(location, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);

    settings->UseNewAssetDatabase = true;
    settings->UseLibraryArtifacts = true;
    REQUIRE_FALSE(Content::RegisterAssetLoadLocation(location, diagnostic));
    asset = Content::Load<RawDataAsset>(id);
    REQUIRE(asset);
    CHECK(asset->GetPath() == canonicalPath);
    CHECK(asset->GetStoragePath() == storagePath);
    CHECK(asset->GetArtifactKey() == TEXT("foundation-key"));
    CHECK(asset->HasArtifactLease());
    REQUIRE(asset->Data.Count() == ARRAY_COUNT(payload));
    CHECK(Platform::MemoryCompare(asset->Data.Get(), payload, ARRAY_COUNT(payload)) == 0);
    CHECK(ArtifactStore::CleanEntireLibrary(diagnostic));
    CHECK(FileSystem::FileExists(storagePath));
    CHECK(FileSystem::FileExists(canonicalPath));

    Content::UnloadAsset(asset);
    asset = nullptr;
    ObjectsRemovalService::Flush();
    CHECK_FALSE(ArtifactLease::IsLeased(storagePath));
    Content::UnregisterAssetLoadLocation(id);
    CHECK_FALSE(ArtifactStore::CleanEntireLibrary(diagnostic));
    CHECK_FALSE(FileSystem::FileExists(storagePath));
    CHECK(FileSystem::FileExists(canonicalPath));

    WriteRawDataAsset(legacyPath, legacyId, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    Content::GetRegistry()->RegisterAsset(legacyId, RawDataAsset::TypeName, legacyPath);
    legacyAsset = Content::Load<RawDataAsset>(legacyId);
    REQUIRE(legacyAsset);
    CHECK(legacyAsset->GetPath() == legacyPath);
    CHECK(legacyAsset->GetStoragePath() == legacyPath);
    CHECK_FALSE(legacyAsset->IsUsingGeneratedArtifact());
}

TEST_CASE("Binary asset upgrader chains preserve chunks")
{
    CharacterizationUpgrader upgrader;
    CHECK(upgrader.ShouldUpgrade(1));
    CHECK(upgrader.ShouldUpgrade(2));
    CHECK_FALSE(upgrader.ShouldUpgrade(3));

    const byte payload[] = { 9, 7, 5, 3, 1 };
    FlaxChunk inputChunk;
    inputChunk.Data.Copy(payload, ARRAY_COUNT(payload));
    AssetMigrationContext context;
    context.Input.SerializedVersion = 1;
    context.Input.Header.Chunks[0] = &inputChunk;

    REQUIRE_FALSE(upgrader.Upgrade(context.Input.SerializedVersion, context));
    CHECK(context.Output.SerializedVersion == 2);
    REQUIRE(context.Output.Header.Chunks[0]);
    REQUIRE(context.Output.Header.Chunks[0]->Data.Length() == ARRAY_COUNT(payload));
    CHECK(Platform::MemoryCompare(context.Output.Header.Chunks[0]->Data.Get(), payload, ARRAY_COUNT(payload)) == 0);
    context.Output.Header.DeleteChunks();
}

#endif
