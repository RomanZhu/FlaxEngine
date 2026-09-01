// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/AssetBuildSnapshot.h"
#include "Engine/Content/Build/RuntimeAssetCatalog.h"
#include "Engine/Content/Build/RuntimeDependencyClosure.h"
#include "Engine/Content/Loading/LoadedAssetRuntimeIdIndex.h"
#include <ThirdParty/catch2/catch.hpp>
#include <utility>

namespace
{
    ContentHash TestHash(const char* value)
    {
        return ContentHash::Compute(value, StringUtils::Length(value));
    }

    RuntimeAssetCatalogEntry CatalogEntry(const Guid& object, const char* type, const char* package, const char* content)
    {
        RuntimeAssetCatalogEntry result;
        result.Object = object;
        result.TypeName = type;
        result.PackageName = package;
        result.Offset = static_cast<uint64>(object.A) * 16;
        result.Size = 16;
        result.Content = TestHash(content);
        return result;
    }
}

TEST_CASE("Runtime asset catalog is deterministic object-level binary data")
{
    const Guid model(1, 0, 0, 0);
    const Guid texture(2, 0, 0, 0);
    RuntimeAssetCatalogEntry modelEntry = CatalogEntry(model, "FlaxEngine.Model", "base/objects.pak", "model");
    modelEntry.Dependencies.Add(texture);
    const RuntimeAssetCatalogEntry textureEntry = CatalogEntry(texture, "FlaxEngine.Texture", "base/objects.pak", "texture");

    Array<RuntimeAssetCatalogEntry> unordered;
    unordered.Add(textureEntry);
    unordered.Add(modelEntry);
    Array<RuntimeAssetCatalogEntry> ordered;
    ordered.Add(modelEntry);
    ordered.Add(textureEntry);
    RuntimeAssetCatalogAlias modelAlias;
    REQUIRE_FALSE(RuntimeAssetCatalog::HashPathAlias(TEXT("Content/Models/Hero.flax"), modelAlias.PathHash));
    modelAlias.Object = model;
    RuntimeAssetCatalogAlias textureAlias;
    REQUIRE_FALSE(RuntimeAssetCatalog::HashPathAlias(TEXT("Content/Textures/Hero.png"), textureAlias.PathHash));
    textureAlias.Object = texture;
    Array<RuntimeAssetCatalogAlias> unorderedAliases;
    unorderedAliases.Add(textureAlias);
    unorderedAliases.Add(modelAlias);
    Array<RuntimeAssetCatalogAlias> orderedAliases;
    orderedAliases.Add(modelAlias);
    orderedAliases.Add(textureAlias);

    AssetPipelineDiagnostic diagnostic;
    RuntimeAssetCatalog first;
    RuntimeAssetCatalog second;
    REQUIRE_FALSE(first.Set(StringAnsiView("win64-development-1"), TestHash("target"), unordered, unorderedAliases, diagnostic));
    REQUIRE_FALSE(second.Set(StringAnsiView("win64-development-1"), TestHash("target"), ordered, orderedAliases, diagnostic));
    Array<byte> firstBytes;
    Array<byte> secondBytes;
    REQUIRE_FALSE(first.ToBytes(firstBytes, diagnostic));
    REQUIRE_FALSE(second.ToBytes(secondBytes, diagnostic));
    CHECK(firstBytes == secondBytes);
    const char* sourcePath = "content/models/hero.flax";
    bool containsSourcePath = false;
    for (int32 i = 0; i + StringUtils::Length(sourcePath) <= firstBytes.Count(); i++)
    {
        if (Platform::MemoryCompare(firstBytes.Get() + i, sourcePath, StringUtils::Length(sourcePath)) == 0)
        {
            containsSourcePath = true;
            break;
        }
    }
    CHECK_FALSE(containsSourcePath);

    RuntimeAssetCatalog loaded;
    REQUIRE_FALSE(RuntimeAssetCatalog::FromBytes(Span<byte>(firstBytes.Get(), firstBytes.Count()), loaded, diagnostic));
    RuntimeAssetCatalogEntry found;
    REQUIRE(loaded.TryGet(model, found));
    CHECK(found.Object == model);
    REQUIRE(found.Dependencies.Count() == 1);
    CHECK(found.Dependencies[0] == texture);
    CHECK(loaded.TryGet(texture, found));
    CHECK_FALSE(loaded.TryGet(Guid(9, 0, 0, 0), found));
    Guid aliasObject;
    REQUIRE(loaded.TryGetByPathHash(modelAlias.PathHash, aliasObject));
    CHECK(aliasObject == model);
    CHECK_FALSE(loaded.TryGetByPathHash(TestHash("missing-path"), aliasObject));

    firstBytes[firstBytes.Count() - 1] ^= 1;
    CHECK(RuntimeAssetCatalog::FromBytes(Span<byte>(firstBytes.Get(), firstBytes.Count()), loaded, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
}

TEST_CASE("Runtime asset catalog rejects source and Library paths")
{
    const Guid object(3, 0, 0, 0);
    AssetPipelineDiagnostic diagnostic;
    RuntimeAssetCatalog catalog;
    Array<RuntimeAssetCatalogEntry> entries;
    entries.Add(CatalogEntry(object, "FlaxEngine.Texture", "Content/Texture.png", "content"));
    CHECK(catalog.Set(StringAnsiView("build"), TestHash("target"), entries, diagnostic));
    CHECK_FALSE(RuntimeAssetCatalog::IsPackageNameValid(StringAnsiView("Content/Texture.png")));
    CHECK_FALSE(RuntimeAssetCatalog::IsPackageNameValid(StringAnsiView("CanonicalSources/Texture.png")));
    CHECK_FALSE(RuntimeAssetCatalog::IsPackageNameValid(StringAnsiView("Library/Artifacts/texture.flax")));
    CHECK_FALSE(RuntimeAssetCatalog::IsPackageNameValid(StringAnsiView("C:/Project/output.pak")));
    CHECK(RuntimeAssetCatalog::IsPackageNameValid(StringAnsiView("base/objects.pak")));
    ContentHash firstAlias;
    ContentHash secondAlias;
    CHECK_FALSE(RuntimeAssetCatalog::HashPathAlias(TEXT("Content/Models/Hero.flax"), firstAlias));
    CHECK_FALSE(RuntimeAssetCatalog::HashPathAlias(TEXT("content\\models\\hero.flax"), secondAlias));
    CHECK(firstAlias == secondAlias);
    CHECK(RuntimeAssetCatalog::HashPathAlias(TEXT("Library/Artifacts/hero.flax"), firstAlias));
    CHECK(RuntimeAssetCatalog::HashPathAlias(TEXT("C:/Project/Content/Hero.flax"), firstAlias));

    entries[0].PackageName = "packages/Library/texture.pak";
    CHECK(catalog.Set(StringAnsiView("build"), TestHash("target"), entries, diagnostic));

    const Guid subAsset = Guid::Empty;
    entries.Clear();
    entries.Add(CatalogEntry(subAsset, "FlaxEngine.Texture", "base/objects.pak", "sub"));
    CHECK(catalog.Set(StringAnsiView("build"), TestHash("target"), entries, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
}

TEST_CASE("Runtime asset catalog resolves persistent object GUIDs")
{
    const Guid object(6, 0, 0, 0);
    Array<RuntimeAssetCatalogEntry> entries;
    entries.Add(CatalogEntry(object, "FlaxEngine.Texture", "base/objects.pak", "object"));
    AssetPipelineDiagnostic diagnostic;
    RuntimeAssetCatalog catalog;
    REQUIRE_FALSE(catalog.Set(StringAnsiView("build"), TestHash("target"), entries, diagnostic));

    RuntimeAssetCatalogEntry found;
    CHECK(catalog.TryGet(object, found));
    CHECK(found.Object == object);
    CHECK_FALSE(catalog.TryGet(Guid(99, 98, 97, 96), found));
    CHECK_FALSE(catalog.TryGet(Guid::Empty, found));
}

TEST_CASE("Loaded runtime GUID index rejects collisions and recovers uniqueness")
{
    const AssetObjectId subAsset(AssetGuid(Guid(7, 0, 0, 0)), 2);
    const Guid runtimeId(70, 71, 72, 73);
    const AssetObjectId collidingMain = AssetObjectId::Main(AssetGuid(runtimeId));
    LoadedAssetRuntimeIdIndex index;
    index.EnsureCapacity(2);

    AssetObjectId found;
    index.Add(runtimeId, subAsset);
    REQUIRE(index.TryGetUnique(runtimeId, found));
    CHECK(found == subAsset);

    index.Add(runtimeId, subAsset);
    REQUIRE(index.TryGetUnique(runtimeId, found));
    CHECK(found == subAsset);

    index.Add(runtimeId, collidingMain);
    CHECK(index.Contains(runtimeId));
    CHECK_FALSE(index.TryGetUnique(runtimeId, found));
    CHECK_FALSE(found.IsValid());

    index.Remove(runtimeId, subAsset);
    REQUIRE(index.TryGetUnique(runtimeId, found));
    CHECK(found == collidingMain);

    index.Remove(runtimeId, collidingMain);
    CHECK_FALSE(index.Contains(runtimeId));
    CHECK_FALSE(index.TryGetUnique(runtimeId, found));
    CHECK_FALSE(found.IsValid());
}

TEST_CASE("Runtime dependency closure follows recorded object edges without asset loading")
{
    const AssetObjectId model(AssetGuid(Guid(10, 0, 0, 0)), 2);
    const AssetObjectId material(AssetGuid(Guid(10, 0, 0, 0)), 3);
    const AssetObjectId texture(AssetGuid(Guid(10, 0, 0, 0)), 4);
    RuntimeObjectDependencyRecord modelRecord;
    modelRecord.Object = model;
    modelRecord.Dependencies.Add(material);
    RuntimeObjectDependencyRecord materialRecord;
    materialRecord.Object = material;
    materialRecord.Dependencies.Add(texture);
    RuntimeObjectDependencyRecord textureRecord;
    textureRecord.Object = texture;
    textureRecord.Dependencies.Add(model); // Runtime cycles are legal and terminate through identity visitation.
    Array<RuntimeObjectDependencyRecord> records;
    records.Add(textureRecord);
    records.Add(modelRecord);
    records.Add(materialRecord);
    Array<AssetObjectId> roots;
    roots.Add(model);

    RuntimeDependencyClosureResult result;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(RuntimeDependencyClosure::Build(roots, records, result, diagnostic));
    CHECK(result.Objects.Count() == 3);
    CHECK(result.Edges.Count() == 3);
    CHECK(result.Objects.Contains(model));
    CHECK(result.Objects.Contains(material));
    CHECK(result.Objects.Contains(texture));

    records.RemoveLast();
    CHECK(RuntimeDependencyClosure::Build(roots, records, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactMissing);
    CHECK(diagnostic.AssetGuid == material.Asset.Value);
}

TEST_CASE("Asset build snapshot pins deterministic exact object publications")
{
    const AssetObjectId model(AssetGuid(Guid(20, 0, 0, 0)), 2);
    const AssetObjectId texture(AssetGuid(Guid(20, 0, 0, 0)), 7);
    AssetBuildSnapshot snapshot;
    snapshot.DatabaseRevision = 91;
    snapshot.Target.Platform = "Windows";
    snapshot.Target.Architecture = "x64";
    snapshot.Target.Role = "Runtime";
    snapshot.TargetHash = TestHash("target");
    snapshot.ProjectSettingsHash = TestHash("settings");
    snapshot.RootObjects.Add(model);
    AssetBuildSnapshotArtifact textureArtifact;
    textureArtifact.Object = texture;
    textureArtifact.Manifest = ArtifactKey(TestHash("texture-manifest"));
    textureArtifact.ObjectContent = TestHash("texture-content");
    snapshot.Artifacts.Add(textureArtifact);
    AssetBuildSnapshotArtifact modelArtifact;
    modelArtifact.Object = model;
    modelArtifact.Manifest = ArtifactKey(TestHash("model-manifest"));
    modelArtifact.ObjectContent = TestHash("model-content");
    snapshot.Artifacts.Add(modelArtifact);

    AssetPipelineDiagnostic diagnostic;
    ArtifactKey first;
    REQUIRE_FALSE(snapshot.ComputeFingerprint(first, diagnostic));
    std::swap(snapshot.Artifacts[0], snapshot.Artifacts[1]);
    ArtifactKey second;
    REQUIRE_FALSE(snapshot.ComputeFingerprint(second, diagnostic));
    CHECK(first == second);

    snapshot.Artifacts.RemoveAt(0);
    CHECK(snapshot.NormalizeAndValidate(diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SnapshotInvalid);
}
