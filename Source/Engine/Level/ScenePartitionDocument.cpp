// Copyright (c) Wojciech Figat. All rights reserved.

#include "ScenePartitionDocument.h"
#include "ScenePrefabDocument.h"
#include "Engine/Core/Collections/HashSet.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(String& error, const Char* message)
    {
        error = message;
        return true;
    }
}

bool ScenePartitionDocument::ReadSceneReferences(const rapidjson_flax::Value& scene,
    Array<ScenePartitionReference>& references, String& error)
{
    references.Clear();
    error.Clear();
    if (!scene.IsObject())
        return Fail(error, TEXT("Scene partition owner must be a JSON object."));
    const auto external = scene.FindMember("externalActors");
    const bool usesPartitions = external != scene.MemberEnd() && external->value.IsBool() && external->value.GetBool();
    if (external != scene.MemberEnd() && !external->value.IsBool())
        return Fail(error, TEXT("externalActors must be a boolean."));
    const auto partitions = scene.FindMember("partitions");
    if (!usesPartitions)
    {
        if (partitions != scene.MemberEnd())
            return Fail(error, TEXT("A scene without externalActors cannot declare partitions."));
        return false;
    }
    if (partitions == scene.MemberEnd() || !partitions->value.IsArray())
        return Fail(error, TEXT("An external-actors scene must declare a partitions array."));

    HashSet<AssetObjectId> objects;
    HashSet<int64> roots;
    for (const JsonValue& value : partitions->value.GetArray())
    {
        if (!value.IsObject())
            return Fail(error, TEXT("Every scene partition reference must be an object."));
        const auto guidValue = value.FindMember("guid");
        const auto fileIdValue = value.FindMember("fileId");
        const auto rootValue = value.FindMember("rootFileId");
        Guid guid;
        if (guidValue == value.MemberEnd() || !guidValue->value.IsString() ||
            Guid::Parse(StringAnsiView(guidValue->value.GetString(), guidValue->value.GetStringLength()), guid) || !guid.IsValid() ||
            fileIdValue == value.MemberEnd() || !fileIdValue->value.IsInt64() || fileIdValue->value.GetInt64() != 1 ||
            rootValue == value.MemberEnd() || !rootValue->value.IsInt64() || rootValue->value.GetInt64() <= 1)
        {
            return Fail(error, TEXT("Scene partitions require a valid {guid,fileId:1,rootFileId} reference."));
        }
        ScenePartitionReference reference;
        reference.Object = AssetObjectId::Main(AssetGuid(guid));
        reference.RootFileId = rootValue->value.GetInt64();
        if (!objects.Add(reference.Object) || !roots.Add(reference.RootFileId))
            return Fail(error, TEXT("Scene partition GUIDs and rootFileId values must be unique."));
        references.Add(reference);
    }
    return false;
}

bool ScenePartitionDocument::ReadChunk(const rapidjson_flax::Value& chunk, int64& rootFileId,
    const rapidjson_flax::Value*& objects, String& error)
{
    rootFileId = 0;
    objects = nullptr;
    error.Clear();
    if (!chunk.IsObject())
        return Fail(error, TEXT("Scene chunk source must be a JSON object."));
    const auto version = chunk.FindMember("sceneChunkVersion");
    const auto root = chunk.FindMember("rootFileId");
    const auto objectTable = chunk.FindMember("objects");
    if (version == chunk.MemberEnd() || !version->value.IsUint() || version->value.GetUint() != 1 ||
        root == chunk.MemberEnd() || !root->value.IsInt64() || root->value.GetInt64() <= 1 ||
        objectTable == chunk.MemberEnd() || !objectTable->value.IsArray())
    {
        return Fail(error, TEXT("Scene chunk must contain sceneChunkVersion 1, a positive rootFileId, and an objects array."));
    }
    if (ScenePrefabDocument::ValidateObjects(objectTable->value, false, error))
        return true;
    const auto firstId = objectTable->value[0].FindMember("fileId");
    if (firstId == objectTable->value[0].MemberEnd() || !firstId->value.IsInt64() || firstId->value.GetInt64() != root->value.GetInt64())
        return Fail(error, TEXT("The first scene chunk object must match rootFileId."));
    rootFileId = root->value.GetInt64();
    objects = &objectTable->value;
    return false;
}

bool ScenePartitionDocument::AppendRuntimeObjects(const rapidjson_flax::Value& chunk, int64 expectedRootFileId,
    rapidjson_flax::Value& runtimeObjects, rapidjson_flax::Document::AllocatorType& allocator, String& error)
{
    int64 rootFileId;
    const JsonValue* sourceObjects;
    if (ReadChunk(chunk, rootFileId, sourceObjects, error))
        return true;
    if (rootFileId != expectedRootFileId)
        return Fail(error, TEXT("Scene partition rootFileId does not match its owner reference."));
    JsonValue converted;
    if (ScenePrefabDocument::ToRuntimeObjects(*sourceObjects, converted, allocator, false, error))
        return true;
    for (JsonValue& value : converted.GetArray())
        runtimeObjects.PushBack(value.Move(), allocator);
    return false;
}
