// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Content/AssetObjectRegistry.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Artifacts/ResolvedArtifact.h"
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/Factories/BinaryAssetFactory.h"
#include "Engine/Content/Loading/ContentLoadTask.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Core/Log.h"
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
        Content::GetObjectRegistry()->RemoveTransientPackage(path, nullptr);
        FileSystem::DeleteFile(path);
    }

}

TEST_CASE("Binary asset factory load streaming and reload")
{
    const String path = Globals::ProjectCacheFolder / TEXT("__BinaryAssetCharacterization.flax");
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
    Content::GetObjectRegistry()->RegisterTransientObject(id, RawDataAsset::TypeName, path);

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
    location.Artifact.ObjectID = AssetObjectId::Main(AssetGuid(location.Info.ObjectID));
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
    const String root = Globals::ProjectCacheFolder / TEXT("Tests/StorageSwitch");
    const String canonicalPath = root / TEXT("initial.flax");
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
    Content::GetObjectRegistry()->RegisterTransientObject(id, RawDataAsset::TypeName, canonicalPath);
    asset = Content::Load<RawDataAsset>(canonicalPath);
    REQUIRE(asset);
    REQUIRE_FALSE(asset->WaitForLoaded());

    ResolvedArtifact replacement;
    replacement.ObjectID = AssetObjectId::Main(AssetGuid(asset->GetPersistentObjectId()));
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

#if COMPILE_WITH_MODEL_TOOL && COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
TEST_CASE("Model child storage switch uses source local package identity")
{
    const String root = Globals::ProjectCacheFolder / TEXT("Tests/ModelChildStorageSwitch");
    const String initialPath = root / TEXT("initial.flaxpac");
    const String replacementPath = root / TEXT("replacement.flaxpac");
    const Guid sourceId = Guid::New();
    const Guid childId = Guid::New();
    AssetObjectId storageObject(AssetGuid(sourceId), 771);
    RawDataAsset* asset = nullptr;
    FileSystem::DeleteDirectory(root, true);
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT
    {
        if (asset)
        {
            Content::UnloadAsset(asset);
            asset = nullptr;
            ObjectsRemovalService::Flush();
        }
        ContentStorageManager::EnsureAccess(initialPath);
        ContentStorageManager::EnsureAccess(replacementPath);
        FileSystem::DeleteDirectory(root, true);
    };

    auto writePackage = [&](const String& path, byte value)
    {
        FlaxChunk chunk;
        chunk.Data.Copy(&value, 1);
        AssetInitData data;
        data.Header.ID = childId;
        data.Header.TypeName = RawDataAsset::TypeName;
        data.Header.Chunks[0] = &chunk;
        data.SerializedVersion = RawDataAsset::SerializedVersion;
        return FlaxStorage::CreateRuntimePackage(path, Span<AssetInitData>(&data, 1), Span<AssetObjectId>(&storageObject, 1), true);
    };
    REQUIRE_FALSE(writePackage(initialPath, 1));
    REQUIRE_FALSE(writePackage(replacementPath, 2));

    AssetLoadLocation location;
    location.Info = AssetInfo(childId, RawDataAsset::TypeName, TEXT("model.glb.subasset"));
    location.Artifact.ObjectID = storageObject;
    location.Artifact.AssetID = childId;
    location.Artifact.TypeName = RawDataAsset::TypeName;
    location.Artifact.StoragePath = ArtifactStoragePath(initialPath);
    location.Artifact.OutputKind = TEXT("runtime");
    location.Artifact.Key = TEXT("initial");
    location.Artifact.StorageKind = ArtifactStorageKind::Generated;
    auto* factory = static_cast<BinaryAssetFactoryBase*>(Content::GetAssetFactory(location.Info));
    REQUIRE(factory);
    asset = static_cast<RawDataAsset*>(factory->New(location));
    REQUIRE(asset);
    REQUIRE_FALSE(asset->Storage->Load());
    REQUIRE_FALSE(factory->Init(asset));
    REQUIRE_FALSE(asset->LoadChunk(0));

    AssetRecord child;
    child.ID = childId;
    child.SourceAssetID = sourceId;
    child.LocalId = storageObject.LocalId;
    ResolvedArtifact replacement = location.Artifact;
    replacement.ObjectID = ModelPipelineService::GetHotSwapStorageObjectForTesting(child);
    replacement.StoragePath = ArtifactStoragePath(replacementPath);
    replacement.Key = TEXT("replacement");
    REQUIRE(asset->SwitchStorage(replacement) == BinaryAssetStorageSwitchResult::Success);
    REQUIRE(asset->Data.Count() == 1);
    CHECK(asset->Data[0] == 2);
}
#endif

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
    Content::GetObjectRegistry()->RegisterTransientObject(corruptId, RawDataAsset::TypeName, corruptPath);
    corruptAsset = Content::Load<RawDataAsset>(corruptPath);
    CHECK_FALSE(corruptAsset);

    const Guid truncatedId = Guid::New();
    WriteRawDataAsset(truncatedPath, truncatedId, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    REQUIRE(!File::ReadAllBytes(truncatedPath, bytes));
    REQUIRE(bytes.Length() > 2);
    REQUIRE(!File::WriteAllBytes(truncatedPath, bytes.Get(), bytes.Length() / 2));
    Content::GetObjectRegistry()->RegisterTransientObject(truncatedId, RawDataAsset::TypeName, truncatedPath);
    truncatedAsset = Content::Load<RawDataAsset>(truncatedPath);
    CHECK_FALSE(truncatedAsset);

    const Guid versionId = Guid::New();
    WriteRawDataAsset(versionPath, versionId, RawDataAsset::SerializedVersion + 1, payload, ARRAY_COUNT(payload));
    Content::GetObjectRegistry()->RegisterTransientObject(versionId, RawDataAsset::TypeName, versionPath);
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
    location.Artifact.ObjectID = AssetObjectId::Main(AssetGuid(location.Info.ObjectID));
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

TEST_CASE("Content reads editor-private transient binary assets")
{
    const String legacyPath = Globals::ProjectCacheFolder / TEXT("__FoundationTransient.flax");
    const Guid legacyId = Guid::New();
    const byte payload[] = { 10, 20, 30, 40 };
    RawDataAsset* legacyAsset = nullptr;
    CleanupRawDataAsset(legacyPath, legacyAsset);
    SCOPE_EXIT
    {
        CleanupRawDataAsset(legacyPath, legacyAsset);
    };

    WriteRawDataAsset(legacyPath, legacyId, RawDataAsset::SerializedVersion, payload, ARRAY_COUNT(payload));
    Content::GetObjectRegistry()->RegisterTransientObject(legacyId, RawDataAsset::TypeName, legacyPath);
    legacyAsset = Content::Load<RawDataAsset>(legacyPath);
    REQUIRE(legacyAsset);
    CHECK(legacyAsset->GetPath() == legacyPath);
    CHECK(legacyAsset->GetStoragePath() == legacyPath);
    CHECK_FALSE(legacyAsset->IsUsingGeneratedArtifact());
}

TEST_CASE("Canonical missing source path reaches exact asset object loader diagnostics")
{
    AssetDatabase& database = AssetDatabase::Get();
    const AssetDatabaseSnapshot savedDatabase = database.GetSnapshot();
    AssetPipelineDiagnostic diagnostic;
    const String missingPath = Globals::ProjectContentFolder / (TEXT("__CanonicalMissingSource-") + Guid::New().ToString(Guid::FormatType::N) + TEXT(".source"));
    REQUIRE_FALSE(FileSystem::FileExists(missingPath));

    AssetPathPolicy::ProjectPath projectPath;
    REQUIRE_FALSE(AssetPathPolicy::TryNormalizeProjectPath(Globals::ProjectFolder, Globals::ProjectContentFolder,
        Globals::ProjectLibraryFolder, missingPath, projectPath, diagnostic));
    const Guid id = Guid::New();
    AssetRecord record;
    record.ID = id;
    record.SourceAssetID = id;
    record.TypeName = RawDataAsset::TypeName;
    record.CanonicalPath = CanonicalAssetPath(projectPath.AbsolutePath);
    record.SourcePath = SourceFilePath(projectPath.AbsolutePath);
    record.PortabilityKey = projectPath.PortabilityKey;
    record.ProcessorID = TEXT("tests.missing-source");
    record.Status = AssetRecordStatus::MissingSource;
    Array<AssetRecord> records;
    records.Add(record);
    REQUIRE_FALSE(database.PublishFullSnapshot(records, diagnostic));

    AssetBuildService buildService;
    ArtifactTarget target;
    bool planRequested = false;
    ArtifactResolutionPlanProvider provider = [&planRequested](const AssetRecord&, const ArtifactRequest&,
        ArtifactResolutionPlan&, AssetPipelineDiagnostic&)
    {
        planRequested = true;
        return true;
    };
    ArtifactResolver::ScopedConfiguration resolverConfiguration(ArtifactResolver::Get());
    ArtifactResolver::Get().Configure(database, buildService, Globals::ProjectLibraryFolder, target, provider);
    SCOPE_EXIT
    {
        database.PublishFullSnapshot(savedDatabase.Records, diagnostic);
    };

    String loaderDiagnostic;
    bool sawFilesystemGate = false;
    Delegate<LogType, const StringView&>::FunctionType logHandler = [&loaderDiagnostic, &sawFilesystemGate](LogType type, const StringView& message)
    {
        if (type != LogType::Error)
            return;
        if (message.Contains(TEXT("ASSET_SOURCE_MISSING")))
            loaderDiagnostic = message;
        if (message.Contains(TEXT("Missing file")))
            sawFilesystemGate = true;
    };
    Log::Logger::OnMessage.Bind(logHandler);
    SCOPE_EXIT { Log::Logger::OnMessage.Unbind(logHandler); };

    CHECK(Content::LoadAsync<RawDataAsset>(missingPath) == nullptr);
    CHECK_FALSE(planRequested);
    CHECK_FALSE(sawFilesystemGate);
    CHECK(loaderDiagnostic.Contains(TEXT("ASSET_SOURCE_MISSING")));
    CHECK(loaderDiagnostic.Contains(record.SourcePath.Get()));
}

TEST_CASE("Runtime packages persist and resolve persistent object GUID entries")
{
    const String path = Globals::ProjectLibraryFolder / TEXT("__PersistentRuntimePackage.flaxpac");
    const AssetObjectId firstObject = AssetObjectId::Main(AssetGuid(Guid::New()));
    const AssetObjectId secondObject = AssetObjectId::Main(AssetGuid(Guid::New()));
    const Guid firstInstance = Guid::New();
    const Guid secondInstance = Guid::New();
    const byte firstBytes[] = { 1, 2, 3 };
    const byte secondBytes[] = { 4, 5, 6, 7 };
    FlaxChunk firstChunk;
    firstChunk.Data.Copy(firstBytes, ARRAY_COUNT(firstBytes));
    FlaxChunk secondChunk;
    secondChunk.Data.Copy(secondBytes, ARRAY_COUNT(secondBytes));
    Array<AssetInitData> assets;
    assets.Resize(2);
    assets[0].Header.ID = firstInstance;
    assets[0].Header.TypeName = RawDataAsset::TypeName;
    assets[0].Header.Chunks[0] = &firstChunk;
    assets[0].SerializedVersion = RawDataAsset::SerializedVersion;
    assets[1].Header.ID = secondInstance;
    assets[1].Header.TypeName = RawDataAsset::TypeName;
    assets[1].Header.Chunks[0] = &secondChunk;
    assets[1].SerializedVersion = RawDataAsset::SerializedVersion;
    Array<AssetObjectId> objects;
    objects.Add(firstObject);
    objects.Add(secondObject);

    FileSystem::DeleteFile(path);
    REQUIRE_FALSE(FlaxStorage::CreateRuntimePackage(path, ToSpan(assets), ToSpan(objects), true));
    FlaxStorageReference storage = ContentStorageManager::GetStorage(path);
    REQUIRE(storage);
    SCOPE_EXIT
    {
        storage = nullptr;
        ContentStorageManager::EnsureAccess(path);
        FileSystem::DeleteFile(path);
    };
    CHECK(storage->UsesAssetObjectIds());
    CHECK_FALSE(storage->HasAsset(firstInstance));

    AssetInitData firstData;
    REQUIRE_FALSE(storage->LoadAssetHeader(firstObject, firstData));
    CHECK(firstData.Header.ID == firstInstance);
    CHECK(firstData.Header.TypeName == RawDataAsset::TypeName);
    AssetInitData secondData;
    REQUIRE_FALSE(storage->LoadAssetHeader(secondObject, secondData));
    CHECK(secondData.Header.ID == secondInstance);
}

#endif
