// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Tools/ModelTool/ModelTool.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseFacade.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/Build/Processors/ModelProcessor.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("ModelTool")
{
    SECTION("Test DetectLodIndex")
    {
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh")) == 0);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh LOD")) == 0);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh LOD0")) == 0);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh LOD1")) == 1);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh_LOD1")) == 1);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh_lod1")) == 1);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh_lod2")) == 2);
        CHECK(ModelTool::DetectLodIndex(TEXT("lod0")) == 0);
        CHECK(ModelTool::DetectLodIndex(TEXT("lod1")) == 1);
        CHECK(ModelTool::DetectLodIndex(TEXT("lod_2")) == 2);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh_lod_0")) == 0);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh_lod_1")) == 1);
        CHECK(ModelTool::DetectLodIndex(TEXT("mesh lod_2")) == 2);
    }
}

#if COMPILE_WITH_ASSETS_IMPORTER && COMPILE_WITH_MODEL_TOOL && USE_EDITOR

namespace
{
    void AddU32LE(Array<byte>& data, uint32 value)
    {
        data.Add(static_cast<byte>(value));
        data.Add(static_cast<byte>(value >> 8));
        data.Add(static_cast<byte>(value >> 16));
        data.Add(static_cast<byte>(value >> 24));
    }

    Array<byte> MakeEmbeddedTextureGlb()
    {
        static const byte png[] =
        {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
            0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc, 0xff, 0x1f, 0x00, 0x02, 0xeb,
            0x01, 0xf5, 0x8f, 0x59, 0x97, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
        };
        static const float positions[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        static const uint16 indices[] = { 0, 1, 2 };
        Array<byte> binary;
        binary.Add(reinterpret_cast<const byte*>(positions), sizeof(positions));
        binary.Add(reinterpret_cast<const byte*>(indices), sizeof(indices));
        while ((binary.Count() & 3) != 0)
            binary.Add(0);
        const int32 imageOffset = binary.Count();
        binary.Add(png, ARRAY_COUNT(png));
        while ((binary.Count() & 3) != 0)
            binary.Add(0);

        StringAnsi json = StringAnsi::Format(
            "{{\"asset\":{{\"version\":\"2.0\"}},\"buffers\":[{{\"byteLength\":{0}}}],"
            "\"bufferViews\":[{{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962}},"
            "{{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}},"
            "{{\"buffer\":0,\"byteOffset\":{1},\"byteLength\":{2}}}],"
            "\"accessors\":[{{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}},"
            "{{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}}],"
            "\"images\":[{{\"bufferView\":2,\"mimeType\":\"image/png\",\"name\":\"Embedded\"}}],"
            "\"textures\":[{{\"source\":0}}],\"materials\":[{{\"name\":\"EmbeddedMaterial\",\"pbrMetallicRoughness\":{{\"baseColorTexture\":{{\"index\":0}}}}}}],"
            "\"meshes\":[{{\"name\":\"Triangle\",\"primitives\":[{{\"attributes\":{{\"POSITION\":0}},\"indices\":1,\"material\":0}}]}}],"
            "\"nodes\":[{{\"mesh\":0}}],\"scenes\":[{{\"nodes\":[0]}}],\"scene\":0}}",
            binary.Count(), imageOffset, ARRAY_COUNT(png));
        Array<byte> jsonBytes;
        jsonBytes.Add(reinterpret_cast<const byte*>(json.Get()), json.Length());
        while ((jsonBytes.Count() & 3) != 0)
            jsonBytes.Add(' ');

        Array<byte> result;
        AddU32LE(result, 0x46546c67);
        AddU32LE(result, 2);
        AddU32LE(result, 12 + 8 + jsonBytes.Count() + 8 + binary.Count());
        AddU32LE(result, jsonBytes.Count());
        AddU32LE(result, 0x4e4f534a);
        result.Add(jsonBytes.Get(), jsonBytes.Count());
        AddU32LE(result, binary.Count());
        AddU32LE(result, 0x004e4942);
        result.Add(binary.Get(), binary.Count());
        return result;
    }
}

TEST_CASE("Model processor analyzes one source into deterministic family records")
{
    const String root = Globals::TemporaryFolder / (TEXT("ModelSourceAnalysis-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = root / TEXT("triangle.obj");
    const char obj[] =
        "o Triangle\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n";
    REQUIRE_FALSE(File::WriteAllBytes(source, obj, ARRAY_COUNT(obj) - 1));

    ModelSourceAnalysis first;
    ModelSourceAnalysis second;
    AssetPipelineDiagnostic diagnostic;
    const ModelProcessorSettings settings = ModelProcessorSettings::Defaults();
    REQUIRE_FALSE(ModelProcessor::AnalyzeSource(source, settings, first, diagnostic));
    REQUIRE_FALSE(ModelProcessor::AnalyzeSource(source, settings, second, diagnostic));
    REQUIRE(first.SourceMeshCount == 1);
    REQUIRE(first.Candidates.HasItems());
    REQUIRE(first.Candidates.Count() == second.Candidates.Count());
    REQUIRE(first.SubAssets.Count() == second.SubAssets.Count());
    bool foundMesh = false;
    for (int32 i = 0; i < first.Candidates.Count(); i++)
    {
        CHECK(first.Candidates[i].StableKey == second.Candidates[i].StableKey);
        CHECK(first.SubAssets[i].SemanticHash == second.SubAssets[i].SemanticHash);
        foundMesh |= first.Candidates[i].StableKey.StartsWith(TEXT("mesh:"));
    }
    CHECK(foundMesh);
}

TEST_CASE("Model processor retains embedded GLB textures as canonical children")
{
    const String root = Globals::TemporaryFolder / (TEXT("EmbeddedGlbTexture-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const String source = root / TEXT("embedded.glb");
    const Array<byte> glb = MakeEmbeddedTextureGlb();
    REQUIRE_FALSE(File::WriteAllBytes(source, glb.Get(), glb.Count()));

    ModelSourceAnalysis analysis;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(ModelProcessor::AnalyzeSource(source, ModelProcessorSettings::Defaults(), analysis, diagnostic));
    bool foundTexture = false;
    for (const SubAssetCandidate& candidate : analysis.Candidates)
        foundTexture |= candidate.StableKey.StartsWith(TEXT("texture:"));
    CHECK(foundTexture);
    CHECK_FALSE(FileSystem::FileExists(root / TEXT("embedded_tex_0.png")));
}

TEST_CASE("Canonical metadata batch prepares model sources concurrently")
{
    const String root = Globals::TemporaryFolder / (TEXT("ModelMetadataBatch-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };
    const char obj[] =
        "o Triangle\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n";
    Array<String> sources;
    Array<String> staging;
    for (int32 i = 0; i < 4; i++)
    {
        sources.Add(root / String::Format(TEXT("triangle-{0}.obj"), i));
        staging.Add(root / String::Format(TEXT("triangle-{0}.staged-meta"), i));
        REQUIRE_FALSE(File::WriteAllBytes(sources.Last(), obj, ARRAY_COUNT(obj) - 1));
    }

    const Array<Guid> ids = AssetDatabaseFacade::StageDefaultCanonicalMetadataBatch(sources, staging);
    REQUIRE(ids.Count() == sources.Count());
    for (int32 i = 0; i < ids.Count(); i++)
    {
        REQUIRE(ids[i].IsValid());
        AssetMeta meta;
        AssetPipelineDiagnostic diagnostic;
        REQUIRE_FALSE(AssetMeta::Load(staging[i], meta, diagnostic));
        CHECK(meta.ID == ids[i]);
        CHECK(meta.SubAssets.HasItems());
    }
}

#endif
