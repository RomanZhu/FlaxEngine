// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Level/ScenePartitionDocument.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Authored scene partition schema")
{
    rapidjson_flax::Document scene;
    scene.Parse(R"({"sceneVersion":4,"externalActors":true,"partitions":[{"guid":"11111111111111111111111111111111","fileId":1,"rootFileId":42}],"objects":[]})");
    Array<ScenePartitionReference> references;
    String error;
    REQUIRE_FALSE(ScenePartitionDocument::ReadSceneReferences(scene, references, error));
    REQUIRE(references.Count() == 1);
    CHECK(references[0].Object.IsMainObject());
    CHECK(references[0].RootFileId == 42);

    rapidjson_flax::Document chunk;
    chunk.Parse(R"({"sceneChunkVersion":1,"rootFileId":42,"objects":[{"fileId":42,"type":"FlaxEngine.EmptyActor","parentFileId":1}]})");
    int64 rootFileId;
    const rapidjson_flax::Value* objects;
    REQUIRE_FALSE(ScenePartitionDocument::ReadChunk(chunk, rootFileId, objects, error));
    CHECK(rootFileId == 42);
    REQUIRE(objects != nullptr);
    CHECK(objects->Size() == 1);
}

TEST_CASE("Authored scene partition identity rejects mismatches")
{
    rapidjson_flax::Document duplicateScene;
    duplicateScene.Parse(R"({"externalActors":true,"partitions":[{"guid":"11111111111111111111111111111111","fileId":1,"rootFileId":42},{"guid":"22222222222222222222222222222222","fileId":1,"rootFileId":42}]})");
    Array<ScenePartitionReference> references;
    String error;
    CHECK(ScenePartitionDocument::ReadSceneReferences(duplicateScene, references, error));

    rapidjson_flax::Document mismatchedChunk;
    mismatchedChunk.Parse(R"({"sceneChunkVersion":1,"rootFileId":42,"objects":[{"fileId":43,"type":"FlaxEngine.EmptyActor","parentFileId":1}]})");
    int64 rootFileId;
    const rapidjson_flax::Value* objects;
    CHECK(ScenePartitionDocument::ReadChunk(mismatchedChunk, rootFileId, objects, error));
}
