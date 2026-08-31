// Copyright (c) Wojciech Figat. All rights reserved.

#include "ScenePrefabDocument.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Serialization/JsonWriters.h"

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAllocator;

    struct FieldName
    {
        const char* Source;
        const char* Runtime;
    };

    const FieldName Fields[] =
    {
        { "fileId", "FileId" },
        { "type", "TypeName" },
        { "name", "Name" },
        { "parentFileId", "ParentFileId" },
        { "prefabGuid", "PrefabID" },
        { "prefabObjectFileId", "PrefabObjectFileId" },
        { "removedObjects", "RemovedObjects" },
    };

    const char* MapName(const rapidjson_flax::Value& name, bool toRuntime)
    {
        const StringAnsiView source(name.GetString(), name.GetStringLength());
        for (const FieldName& field : Fields)
        {
            const char* match = toRuntime ? field.Source : field.Runtime;
            if (source == StringAnsiView(match))
                return toRuntime ? field.Runtime : field.Source;
        }
        return nullptr;
    }

    bool TransformObjects(const JsonValue& input, JsonValue& output, JsonAllocator& allocator, bool toRuntime, String& error)
    {
        if (!input.IsArray())
        {
            error = TEXT("Scene/prefab objects must be an array.");
            return true;
        }
        output.SetArray();
        output.Reserve(input.Size(), allocator);
        for (const JsonValue& sourceObject : input.GetArray())
        {
            if (!sourceObject.IsObject())
            {
                error = TEXT("Every scene/prefab object record must be an object.");
                return true;
            }
            JsonValue object(rapidjson::kObjectType);
            for (auto member = sourceObject.MemberBegin(); member != sourceObject.MemberEnd(); ++member)
            {
                const char* mapped = MapName(member->name, toRuntime);
                JsonValue key;
                if (mapped)
                    key.SetString(mapped, allocator);
                else
                    key.SetString(member->name.GetString(), member->name.GetStringLength(), allocator);
                if (object.HasMember(key))
                {
                    error = TEXT("Scene/prefab object contains both canonical and runtime spellings of the same field.");
                    return true;
                }
                JsonValue value;
                value.CopyFrom(member->value, allocator);
                object.AddMember(key.Move(), value.Move(), allocator);
            }
            output.PushBack(object.Move(), allocator);
        }
        return false;
    }
}

bool ScenePrefabDocument::ValidateObjects(const rapidjson_flax::Value& objects, bool scene, String& error)
{
    error.Clear();
    if (!objects.IsArray() || objects.Empty())
    {
        error = TEXT("Scene/prefab source must contain a non-empty objects array.");
        return true;
    }
    HashSet<int64> ids;
    for (rapidjson::SizeType i = 0; i < objects.Size(); i++)
    {
        const JsonValue& object = objects[i];
        if (!object.IsObject())
        {
            error = TEXT("Every scene/prefab object record must be an object.");
            return true;
        }
        const auto fileId = object.FindMember("fileId");
        if (fileId == object.MemberEnd() || !fileId->value.IsInt64() || fileId->value.GetInt64() == 0)
        {
            error = TEXT("Every scene/prefab object record must contain a nonzero signed int64 fileId.");
            return true;
        }
        if (!ids.Add(fileId->value.GetInt64()))
        {
            error = TEXT("Scene/prefab source contains duplicate authored fileId values.");
            return true;
        }
        const auto type = object.FindMember("type");
        const auto prefabGuid = object.FindMember("prefabGuid");
        if ((type == object.MemberEnd() || !type->value.IsString() || type->value.GetStringLength() == 0) &&
            (prefabGuid == object.MemberEnd() || !prefabGuid->value.IsString() || prefabGuid->value.GetStringLength() == 0))
        {
            error = TEXT("Every scene/prefab object must contain a type or prefabGuid.");
            return true;
        }
        const auto parent = object.FindMember("parentFileId");
        if (parent != object.MemberEnd() && (!parent->value.IsInt64() || parent->value.GetInt64() == 0))
        {
            error = TEXT("parentFileId must be a nonzero signed int64 when present.");
            return true;
        }
        const auto prefabObject = object.FindMember("prefabObjectFileId");
        if (prefabObject != object.MemberEnd() && (!prefabObject->value.IsInt64() || prefabObject->value.GetInt64() == 0))
        {
            error = TEXT("prefabObjectFileId must be a nonzero signed int64 when present.");
            return true;
        }
        const auto removed = object.FindMember("removedObjects");
        if (removed != object.MemberEnd())
        {
            if (!removed->value.IsArray())
            {
                error = TEXT("removedObjects must be an array of authored file IDs.");
                return true;
            }
            for (const JsonValue& value : removed->value.GetArray())
            {
                if (!value.IsInt64() || value.GetInt64() == 0)
                {
                    error = TEXT("removedObjects must contain only nonzero signed int64 values.");
                    return true;
                }
            }
        }
    }
    if (scene && objects[0]["fileId"].GetInt64() != 1)
    {
        error = TEXT("The scene root object must use authored fileId 1.");
        return true;
    }
    return false;
}

bool ScenePrefabDocument::ToRuntimeObjects(const rapidjson_flax::Value& source, rapidjson_flax::Value& runtime,
                                           rapidjson_flax::Document::AllocatorType& allocator, bool scene, String& error)
{
    if (ValidateObjects(source, scene, error))
        return true;
    return TransformObjects(source, runtime, allocator, true, error);
}

bool ScenePrefabDocument::ToSourceObjects(const rapidjson_flax::Value& runtime, rapidjson_flax::Value& source,
                                          rapidjson_flax::Document::AllocatorType& allocator, bool scene, String& error)
{
    if (TransformObjects(runtime, source, allocator, false, error))
        return true;
    return ValidateObjects(source, scene, error);
}

bool ScenePrefabDocument::RuntimeEnvelopeToSource(rapidjson_flax::StringBuffer& json, bool scene, bool pretty, String& error)
{
    JsonDocument runtime;
    runtime.Parse(json.GetString(), json.GetSize());
    const auto data = runtime.IsObject() ? runtime.FindMember("Data") : runtime.MemberEnd();
    if (runtime.HasParseError() || !runtime.IsObject() || data == runtime.MemberEnd() || !data->value.IsArray())
    {
        error = TEXT("Runtime scene/prefab serialization did not produce an object array.");
        return true;
    }

    JsonDocument source;
    source.SetObject();
    JsonAllocator& allocator = source.GetAllocator();
    if (scene)
        source.AddMember("sceneVersion", 4, allocator);
    else
        source.AddMember("prefabVersion", 4, allocator);
    const auto external = runtime.FindMember("ExternalActors");
    if (scene && external != runtime.MemberEnd())
    {
        if (!external->value.IsBool())
        {
            error = TEXT("Runtime scene ExternalActors flag is malformed.");
            return true;
        }
        source.AddMember("externalActors", external->value.GetBool(), allocator);
    }
    JsonValue objects;
    if (ToSourceObjects(data->value, objects, allocator, scene, error))
        return true;
    source.AddMember("objects", objects.Move(), allocator);

    json.Clear();
    if (pretty)
    {
        PrettyJsonWriter writer(json);
        source.Accept(writer.GetWriter());
    }
    else
    {
        CompactJsonWriter writer(json);
        source.Accept(writer.GetWriter());
    }
    return false;
}
