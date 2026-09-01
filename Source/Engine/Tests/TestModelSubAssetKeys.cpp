// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/Processors/ModelSubAssetKeys.h"
#include "Engine/Content/Assets/Texture.h"
#include <ThirdParty/catch2/catch.hpp>

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

namespace
{
    MeshData* MakeMesh(float offset)
    {
        auto* mesh = New<MeshData>();
        mesh->Name = TEXT("Shared");
        mesh->Positions.Add(Float3(offset, 0.0f, 0.0f));
        mesh->Positions.Add(Float3(offset, 1.0f, 0.0f));
        mesh->Positions.Add(Float3(offset, 0.0f, 1.0f));
        mesh->Indices.Add(0);
        mesh->Indices.Add(1);
        mesh->Indices.Add(2);
        return mesh;
    }
}

TEST_CASE("Model stable mesh keys ignore source ordering and disambiguate identical slots")
{
    ModelData data;
    data.LODs.Resize(1);
    data.LODs[0].Meshes.Add(MakeMesh(0.0f));
    data.LODs[0].Meshes.Add(MakeMesh(2.0f));

    Array<ModelSubAssetInfo> firstInfos;
    Array<SubAssetCandidate> firstCandidates;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ModelSubAssetKeys::Enumerate(data, firstInfos, firstCandidates, diagnostic));
    REQUIRE(firstInfos.Count() == 1);
    const String firstKey = firstInfos[0].StableKey;
    const ContentHash firstHash = firstInfos[0].SemanticHash;

    Swap(data.LODs[0].Meshes[0], data.LODs[0].Meshes[1]);
    Array<ModelSubAssetInfo> reorderedInfos;
    Array<SubAssetCandidate> reorderedCandidates;
    REQUIRE_FALSE(ModelSubAssetKeys::Enumerate(data, reorderedInfos, reorderedCandidates, diagnostic));
    REQUIRE(reorderedInfos.Count() == 1);
    CHECK(reorderedInfos[0].StableKey == firstKey);
    CHECK(reorderedInfos[0].SemanticHash == firstHash);

    data.Materials.Resize(2);
    data.Materials[0].Name = TEXT("Duplicate");
    data.Materials[1].Name = TEXT("Duplicate");
    REQUIRE_FALSE(ModelSubAssetKeys::Enumerate(data, reorderedInfos, reorderedCandidates, diagnostic));
    REQUIRE(reorderedInfos.Count() == 3);
    CHECK(reorderedInfos[0].StableKey != reorderedInfos[1].StableKey);
    CHECK(reorderedInfos[1].StableKey != reorderedInfos[2].StableKey);
}

TEST_CASE("Model embedded textures produce canonical texture subassets")
{
    ModelData data;
    TextureEntry& texture = data.Textures.AddOne();
    texture.Name = TEXT("Albedo");
    texture.EmbeddedFormat = TEXT("png");
    texture.EmbeddedIndex = 0;
    texture.EmbeddedData.Add(1);

    Array<ModelSubAssetInfo> infos;
    Array<SubAssetCandidate> candidates;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ModelSubAssetKeys::Enumerate(data, infos, candidates, diagnostic));
    REQUIRE(infos.Count() == 1);
    CHECK(infos[0].Kind == ModelSubAssetKind::Texture);
    CHECK(infos[0].StableKey == TEXT("texture:Albedo"));
    CHECK(candidates[0].TypeName == Texture::TypeName);
}

#endif
