// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Tools/ModelTool/ModelTool.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"
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

    const Array<Guid> ids = AssetOperationService::StageDefaultMetadataBatch(sources, staging);
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
