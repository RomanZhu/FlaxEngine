// Copyright (c) Wojciech Figat. All rights reserved.

#include "CollisionDataDocument.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/AssetDatabase/SubAsset.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;
    typedef rapidjson_flax::Document::AllocatorType JsonAlloc;

    StringAnsi GuidText(const Guid& id)
    {
        return StringAnsi(id.ToString(Guid::FormatType::N)).ToLower();
    }

    void AddString(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    JsonValue MakeReference(const Guid& id, JsonAlloc& allocator)
    {
        if (!id.IsValid())
            return JsonValue(rapidjson::kNullType);
        JsonValue result(rapidjson::kObjectType);
        AddString(result, "$type", "AssetReference", allocator);
        AssetObjectId objectId;
        if (Content::GetAssetObjectId(id, objectId))
        {
            AddString(result, "guid", GuidText(objectId.Guid), allocator);
            result.AddMember("localId", objectId.LocalId, allocator);
        }
        else
        {
            AddString(result, "value", GuidText(id), allocator);
        }
        return result;
    }

    bool ReadReference(const JsonValue& value, Guid& id)
    {
        id = Guid::Empty;
        if (value.IsNull())
            return false;
        if (!value.IsObject())
            return true;
        const auto type = value.FindMember("$type");
        const auto guid = value.FindMember("guid");
        const auto localId = value.FindMember("localId");
        const auto payload = value.FindMember("value");
        if (type == value.MemberEnd() || !type->value.IsString() ||
            StringAnsiView(type->value.GetString(), type->value.GetStringLength()) != "AssetReference")
            return true;
        if (guid != value.MemberEnd() && guid->value.IsString() && localId != value.MemberEnd() && localId->value.IsInt64())
        {
            Guid fileGuid;
            if (Guid::Parse(StringAnsiView(guid->value.GetString(), guid->value.GetStringLength()), fileGuid) || localId->value.GetInt64() == 0)
                return true;
            id = SubAssetPolicy::GetBackingAssetId(fileGuid, localId->value.GetInt64());
            return !id.IsValid();
        }
        if (payload == value.MemberEnd() || !payload->value.IsString())
            return true;
        return Guid::Parse(String(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength())), id);
    }

    const char* CollisionTypeName(CollisionDataType type)
    {
        switch (type)
        {
        case CollisionDataType::None: return "None";
        case CollisionDataType::ConvexMesh: return "ConvexMesh";
        case CollisionDataType::TriangleMesh: return "TriangleMesh";
        default: return nullptr;
        }
    }

    bool ReadCollisionType(const JsonValue& value, CollisionDataType& type)
    {
        if (!value.IsString())
            return true;
        const StringAnsiView name(value.GetString(), value.GetStringLength());
        if (name == "None") type = CollisionDataType::None;
        else if (name == "ConvexMesh") type = CollisionDataType::ConvexMesh;
        else if (name == "TriangleMesh") type = CollisionDataType::TriangleMesh;
        else return true;
        return false;
    }

    struct FlagName
    {
        ConvexMeshGenerationFlags Flag;
        const char* Name;
    };

    const FlagName FlagNames[] =
    {
        { ConvexMeshGenerationFlags::SkipValidation, "SkipValidation" },
        { ConvexMeshGenerationFlags::UsePlaneShifting, "UsePlaneShifting" },
        { ConvexMeshGenerationFlags::UseFastInteriaComputation, "UseFastInteriaComputation" },
        { ConvexMeshGenerationFlags::ShiftVertices, "ShiftVertices" },
        { ConvexMeshGenerationFlags::SuppressFaceRemapTable, "SuppressFaceRemapTable" },
    };
}

bool CollisionDataDocument::DecodeLegacy(const CollisionData::SerializedOptions& options, rapidjson_flax::Document& document, String& error)
{
    error.Clear();
    const char* typeName = CollisionTypeName(options.Type);
    if (!typeName)
    {
        error = TEXT("Legacy collision data type is invalid.");
        return true;
    }
    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    document.AddMember("documentVersion", 1, allocator);
    AddString(document, "type", "FlaxEngine.CollisionData", allocator);
    AddString(document, "collisionType", typeName, allocator);
    document.AddMember("sourceModel", MakeReference(options.Model, allocator), allocator);
    document.AddMember("modelLodIndex", options.ModelLodIndex, allocator);
    document.AddMember("materialSlotsMask", options.MaterialSlotsMask == 0 ? MAX_uint32 : options.MaterialSlotsMask, allocator);
    JsonValue flags(rapidjson::kArrayType);
    for (const FlagName& entry : FlagNames)
    {
        if (EnumHasAnyFlags(options.ConvexFlags, entry.Flag))
            flags.PushBack(JsonValue(entry.Name, allocator), allocator);
    }
    document.AddMember("convexFlags", flags, allocator);
    document.AddMember("convexVertexLimit", options.ConvexVertexLimit < 4 ? 255 : options.ConvexVertexLimit, allocator);
    return false;
}

bool CollisionDataDocument::Parse(const rapidjson_flax::Value& document, CollisionData::SerializedOptions& options, String& error)
{
    Platform::MemoryClear(&options, sizeof(options));
    options.MaterialSlotsMask = MAX_uint32;
    options.ConvexVertexLimit = 255;
    error.Clear();
    if (!document.IsObject())
    {
        error = TEXT("Collision data document must be an object.");
        return true;
    }
    const auto type = document.FindMember("type");
    const auto collisionType = document.FindMember("collisionType");
    const auto sourceModel = document.FindMember("sourceModel");
    const auto lod = document.FindMember("modelLodIndex");
    const auto slots = document.FindMember("materialSlotsMask");
    const auto flags = document.FindMember("convexFlags");
    const auto limit = document.FindMember("convexVertexLimit");
    if (type == document.MemberEnd() || !type->value.IsString() ||
        StringAnsiView(type->value.GetString(), type->value.GetStringLength()) != "FlaxEngine.CollisionData" ||
        collisionType == document.MemberEnd() || ReadCollisionType(collisionType->value, options.Type) ||
        sourceModel == document.MemberEnd() || ReadReference(sourceModel->value, options.Model) ||
        lod == document.MemberEnd() || !lod->value.IsInt() || lod->value.GetInt() < 0 ||
        slots == document.MemberEnd() || !slots->value.IsUint() ||
        flags == document.MemberEnd() || !flags->value.IsArray() ||
        limit == document.MemberEnd() || !limit->value.IsInt())
    {
        error = TEXT("Collision data recipe fields are invalid.");
        return true;
    }
    options.ModelLodIndex = lod->value.GetInt();
    options.MaterialSlotsMask = slots->value.GetUint();
    options.ConvexVertexLimit = limit->value.GetInt();
    options.ConvexFlags = ConvexMeshGenerationFlags::None;
    for (const JsonValue& value : flags->value.GetArray())
    {
        if (!value.IsString())
        {
            error = TEXT("Collision data convex flags must be names.");
            return true;
        }
        const StringAnsiView name(value.GetString(), value.GetStringLength());
        bool found = false;
        for (const FlagName& entry : FlagNames)
        {
            if (name == entry.Name)
            {
                options.ConvexFlags |= entry.Flag;
                found = true;
                break;
            }
        }
        if (!found)
        {
            error = TEXT("Collision data contains an unknown convex flag.");
            return true;
        }
    }
    if (options.Type == CollisionDataType::None)
    {
        options.Model = Guid::Empty;
        return false;
    }
    if (!options.Model.IsValid())
    {
        error = TEXT("Cooked collision data requires a source model reference.");
        return true;
    }
    if (options.Type == CollisionDataType::ConvexMesh && (options.ConvexVertexLimit < 8 || options.ConvexVertexLimit > 255))
    {
        error = TEXT("Collision convex vertex limit must be between 8 and 255.");
        return true;
    }
    return false;
}
