// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Level/ScenePartitionDocument.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("Private scene fragment payload decoding")
{
    rapidjson_flax::Document fragment;
    fragment.Parse(R"({"formatVersion":1,"ownerSceneGuid":"11111111111111111111111111111111","rootActorLocalId":42,"containedLocalIds":[42],"serializerVersion":1,"payload":[{"fileId":42,"type":"FlaxEngine.EmptyActor","parentFileId":1}]})");
    int64 rootActorLocalId;
    const rapidjson_flax::Value* objects;
    String error;
    REQUIRE_FALSE(ScenePartitionDocument::ReadFragment(fragment, rootActorLocalId, objects, error));
    CHECK(rootActorLocalId == 42);
    REQUIRE(objects != nullptr);
    CHECK(objects->Size() == 1);

    rapidjson_flax::Document runtime;
    runtime.SetArray();
    REQUIRE_FALSE(ScenePartitionDocument::AppendRuntimeObjects(fragment, 42, runtime, runtime.GetAllocator(), error));
    REQUIRE(runtime.Size() == 1);
    CHECK(runtime[0]["FileId"].GetInt64() == 42);
}

TEST_CASE("Private scene fragment payload rejects mismatched roots")
{
    rapidjson_flax::Document fragment;
    fragment.Parse(R"({"formatVersion":1,"rootActorLocalId":42,"payload":[{"fileId":43,"type":"FlaxEngine.EmptyActor","parentFileId":1}]})");
    int64 rootActorLocalId;
    const rapidjson_flax::Value* objects;
    String error;
    CHECK(ScenePartitionDocument::ReadFragment(fragment, rootActorLocalId, objects, error));

    fragment.Parse(R"({"formatVersion":1,"rootActorLocalId":42,"payload":[{"fileId":42,"type":"FlaxEngine.EmptyActor","parentFileId":1}]})");
    rapidjson_flax::Document runtime;
    runtime.SetArray();
    CHECK(ScenePartitionDocument::AppendRuntimeObjects(fragment, 43, runtime, runtime.GetAllocator(), error));
}
