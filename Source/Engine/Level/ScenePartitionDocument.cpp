// Copyright (c) Wojciech Figat. All rights reserved.

#include "ScenePartitionDocument.h"
#include "ScenePrefabDocument.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;

    bool Fail(String& error, const Char* message)
    {
        error = message;
        return true;
    }
}

bool ScenePartitionDocument::ReadFragment(const rapidjson_flax::Value& fragment, int64& rootActorLocalId,
    const rapidjson_flax::Value*& objects, String& error)
{
    rootActorLocalId = 0;
    objects = nullptr;
    error.Clear();
    if (!fragment.IsObject())
        return Fail(error, TEXT("Scene fragment must be a JSON object."));
    const auto version = fragment.FindMember("formatVersion");
    const auto root = fragment.FindMember("rootActorLocalId");
    const auto objectTable = fragment.FindMember("payload");
    if (version == fragment.MemberEnd() || !version->value.IsUint() || version->value.GetUint() != 1 ||
        root == fragment.MemberEnd() || !root->value.IsInt64() || root->value.GetInt64() <= 1 ||
        objectTable == fragment.MemberEnd() || !objectTable->value.IsArray())
    {
        return Fail(error, TEXT("Scene fragment must contain formatVersion 1, a positive rootActorLocalId, and a payload array."));
    }
    if (ScenePrefabDocument::ValidateObjects(objectTable->value, false, error))
        return true;
    const auto firstId = objectTable->value[0].FindMember("fileId");
    if (firstId == objectTable->value[0].MemberEnd() || !firstId->value.IsInt64() || firstId->value.GetInt64() != root->value.GetInt64())
        return Fail(error, TEXT("The first scene fragment object must match rootActorLocalId."));
    rootActorLocalId = root->value.GetInt64();
    objects = &objectTable->value;
    return false;
}

bool ScenePartitionDocument::AppendRuntimeObjects(const rapidjson_flax::Value& fragment, int64 expectedRootActorLocalId,
    rapidjson_flax::Value& runtimeObjects, rapidjson_flax::Document::AllocatorType& allocator, String& error)
{
    int64 rootActorLocalId;
    const JsonValue* sourceObjects;
    if (ReadFragment(fragment, rootActorLocalId, sourceObjects, error))
        return true;
    if (rootActorLocalId != expectedRootActorLocalId)
        return Fail(error, TEXT("Scene fragment rootActorLocalId does not match its owner index."));
    JsonValue converted;
    if (ScenePrefabDocument::ToRuntimeObjects(*sourceObjects, converted, allocator, false, error))
        return true;
    for (JsonValue& value : converted.GetArray())
        runtimeObjects.PushBack(value.Move(), allocator);
    return false;
}
