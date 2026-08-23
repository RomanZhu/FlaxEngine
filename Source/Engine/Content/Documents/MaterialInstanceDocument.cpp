// Copyright (c) Wojciech Figat. All rights reserved.

#include "MaterialInstanceDocument.h"
#include "Engine/Graphics/Materials/MaterialParams.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Platform/Platform.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;
    typedef rapidjson_flax::Document::AllocatorType JsonAlloc;

    StringAnsi GuidText(const Guid& id)
    {
        const String wide = id.ToString(Guid::FormatType::N);
        StringAnsi result(wide);
        for (int32 i = 0; i < result.Length(); i++)
        {
            if (result[i] >= 'A' && result[i] <= 'F')
                result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
        return result;
    }

    JsonValue StringValue(const StringAnsiView& text, JsonAlloc& allocator)
    {
        return JsonValue(text.Get(), text.Length(), allocator);
    }

    JsonValue GuidReference(const Guid& id, const char* kind, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kObjectType);
        result.AddMember("$type", JsonValue(kind, allocator), allocator);
        const StringAnsi text = GuidText(id);
        result.AddMember("guid", StringValue(text, allocator), allocator);
        return result;
    }

    const char* TypeName(MaterialParameterType type)
    {
        switch (type)
        {
        case MaterialParameterType::Bool: return "Bool";
        case MaterialParameterType::Integer: return "Integer";
        case MaterialParameterType::Float: return "Float";
        case MaterialParameterType::Vector2: return "Vector2";
        case MaterialParameterType::Vector3: return "Vector3";
        case MaterialParameterType::Vector4: return "Vector4";
        case MaterialParameterType::Color: return "Color";
        case MaterialParameterType::Texture: return "Texture";
        case MaterialParameterType::CubeTexture: return "CubeTexture";
        case MaterialParameterType::NormalMap: return "NormalMap";
        case MaterialParameterType::SceneTexture: return "SceneTexture";
        case MaterialParameterType::GPUTexture: return "GPUTexture";
        case MaterialParameterType::Matrix: return "Matrix";
        case MaterialParameterType::GPUTextureArray: return "GPUTextureArray";
        case MaterialParameterType::GPUTextureVolume: return "GPUTextureVolume";
        case MaterialParameterType::GPUTextureCube: return "GPUTextureCube";
        case MaterialParameterType::ChannelMask: return "ChannelMask";
        case MaterialParameterType::GameplayGlobal: return "GameplayGlobal";
        case MaterialParameterType::TextureGroupSampler: return "TextureGroupSampler";
        default: return nullptr;
        }
    }

    bool ParseType(const StringAnsiView& text, MaterialParameterType& type)
    {
        for (int32 i = static_cast<int32>(MaterialParameterType::Bool); i <= static_cast<int32>(MaterialParameterType::TextureGroupSampler); i++)
        {
            const auto candidate = static_cast<MaterialParameterType>(i);
            const char* name = TypeName(candidate);
            if (name && text == StringAnsiView(name))
            {
                type = candidate;
                return false;
            }
        }
        return true;
    }

    JsonValue NumberArray(const float* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kArrayType);
        result.Reserve(count, allocator);
        for (int32 i = 0; i < count; i++)
            result.PushBack(values[i], allocator);
        return result;
    }

    bool ReadLegacyParameter(MemoryReadStream& stream, uint16 version, SerializedMaterialParam& parameter)
    {
        Platform::MemoryClear(parameter.AsData, sizeof(parameter.AsData));
        parameter.Type = static_cast<MaterialParameterType>(stream.ReadByte());
        if (version == 1)
            parameter.ID = Guid::Empty;
        else
            stream.Read(parameter.ID);
        parameter.IsPublic = stream.ReadBool();
        parameter.Override = version >= 3 ? stream.ReadBool() : parameter.IsPublic;
        stream.Read(parameter.Name, 10421);
        parameter.RegisterIndex = stream.ReadByte();
        stream.ReadUint16(&parameter.Offset);
        switch (parameter.Type)
        {
        case MaterialParameterType::Bool:
            parameter.AsBool = stream.ReadBool();
            break;
        case MaterialParameterType::Integer:
        case MaterialParameterType::SceneTexture:
        case MaterialParameterType::ChannelMask:
        case MaterialParameterType::TextureGroupSampler:
            stream.ReadInt32(&parameter.AsInteger);
            break;
        case MaterialParameterType::Float:
            stream.ReadFloat(&parameter.AsFloat);
            break;
        case MaterialParameterType::Vector2:
            stream.Read(parameter.AsFloat2);
            break;
        case MaterialParameterType::Vector3:
            stream.Read(parameter.AsFloat3);
            break;
        case MaterialParameterType::Vector4:
            stream.Read(*reinterpret_cast<Float4*>(parameter.AsData));
            break;
        case MaterialParameterType::Color:
            stream.Read(parameter.AsColor);
            break;
        case MaterialParameterType::Matrix:
            stream.Read(*reinterpret_cast<Matrix*>(parameter.AsData));
            break;
        case MaterialParameterType::NormalMap:
        case MaterialParameterType::Texture:
        case MaterialParameterType::CubeTexture:
        case MaterialParameterType::GameplayGlobal:
        case MaterialParameterType::GPUTextureVolume:
        case MaterialParameterType::GPUTextureCube:
        case MaterialParameterType::GPUTextureArray:
        case MaterialParameterType::GPUTexture:
            stream.Read(parameter.AsGuid);
            break;
        default:
            return true;
        }
        return false;
    }

    JsonValue EncodeValue(const SerializedMaterialParam& parameter, JsonAlloc& allocator)
    {
        switch (parameter.Type)
        {
        case MaterialParameterType::Bool:
            return JsonValue(parameter.AsBool);
        case MaterialParameterType::Integer:
        case MaterialParameterType::SceneTexture:
        case MaterialParameterType::ChannelMask:
        case MaterialParameterType::TextureGroupSampler:
            return JsonValue(parameter.AsInteger);
        case MaterialParameterType::Float:
            return JsonValue(parameter.AsFloat);
        case MaterialParameterType::Vector2:
            return NumberArray(&parameter.AsFloat2.X, 2, allocator);
        case MaterialParameterType::Vector3:
            return NumberArray(&parameter.AsFloat3.X, 3, allocator);
        case MaterialParameterType::Vector4:
            return NumberArray(reinterpret_cast<const float*>(parameter.AsData), 4, allocator);
        case MaterialParameterType::Color:
            return NumberArray(&parameter.AsColor.R, 4, allocator);
        case MaterialParameterType::Matrix:
            return NumberArray(reinterpret_cast<const float*>(parameter.AsData), 16, allocator);
        case MaterialParameterType::NormalMap:
        case MaterialParameterType::Texture:
        case MaterialParameterType::CubeTexture:
        case MaterialParameterType::GameplayGlobal:
            return GuidReference(parameter.AsGuid, "AssetReference", allocator);
        case MaterialParameterType::GPUTextureVolume:
        case MaterialParameterType::GPUTextureCube:
        case MaterialParameterType::GPUTextureArray:
        case MaterialParameterType::GPUTexture:
            return GuidReference(parameter.AsGuid, "ObjectReference", allocator);
        default:
            return JsonValue();
        }
    }

    bool ReadGuidReference(const JsonValue& value, const char* expectedKind, Guid& id)
    {
        if (!value.IsObject())
            return true;
        const auto kind = value.FindMember("$type");
        const auto guid = value.FindMember("guid");
        if (kind == value.MemberEnd() || !kind->value.IsString() ||
            guid == value.MemberEnd() || !guid->value.IsString() ||
            StringAnsiView(kind->value.GetString(), kind->value.GetStringLength()) != StringAnsiView(expectedKind))
            return true;
        return Guid::Parse(StringAnsiView(guid->value.GetString(), guid->value.GetStringLength()), id);
    }

    bool ReadNumberArray(const JsonValue& value, float* output, int32 count)
    {
        if (!value.IsArray() || value.Size() != static_cast<rapidjson::SizeType>(count))
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber())
                return true;
            output[i] = value[i].GetFloat();
        }
        return false;
    }

    bool DecodeValue(const JsonValue& value, SerializedMaterialParam& parameter)
    {
        switch (parameter.Type)
        {
        case MaterialParameterType::Bool:
            if (!value.IsBool()) return true;
            parameter.AsBool = value.GetBool();
            return false;
        case MaterialParameterType::Integer:
        case MaterialParameterType::SceneTexture:
        case MaterialParameterType::ChannelMask:
        case MaterialParameterType::TextureGroupSampler:
            if (!value.IsInt()) return true;
            parameter.AsInteger = value.GetInt();
            return false;
        case MaterialParameterType::Float:
            if (!value.IsNumber()) return true;
            parameter.AsFloat = value.GetFloat();
            return false;
        case MaterialParameterType::Vector2:
            return ReadNumberArray(value, &parameter.AsFloat2.X, 2);
        case MaterialParameterType::Vector3:
            return ReadNumberArray(value, &parameter.AsFloat3.X, 3);
        case MaterialParameterType::Vector4:
            return ReadNumberArray(value, reinterpret_cast<float*>(parameter.AsData), 4);
        case MaterialParameterType::Color:
            return ReadNumberArray(value, &parameter.AsColor.R, 4);
        case MaterialParameterType::Matrix:
            return ReadNumberArray(value, reinterpret_cast<float*>(parameter.AsData), 16);
        case MaterialParameterType::NormalMap:
        case MaterialParameterType::Texture:
        case MaterialParameterType::CubeTexture:
        case MaterialParameterType::GameplayGlobal:
            return ReadGuidReference(value, "AssetReference", parameter.AsGuid);
        case MaterialParameterType::GPUTextureVolume:
        case MaterialParameterType::GPUTextureCube:
        case MaterialParameterType::GPUTextureArray:
        case MaterialParameterType::GPUTexture:
            return ReadGuidReference(value, "ObjectReference", parameter.AsGuid);
        default:
            return true;
        }
    }
}

bool MaterialInstanceDocument::DecodeLegacy(const Span<byte>& chunk, rapidjson_flax::Document& document, String& error)
{
    error.Clear();
    if (chunk.Length() < static_cast<int32>(sizeof(Guid)))
    {
        error = TEXT("Material instance chunk is truncated.");
        return true;
    }
    MemoryReadStream stream(chunk.Get(), chunk.Length());
    Guid baseMaterial;
    stream.Read(baseMaterial);

    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    document.AddMember("documentVersion", 1, allocator);
    document.AddMember("type", JsonValue("FlaxEngine.MaterialInstance", allocator), allocator);
    document.AddMember("baseMaterial", GuidReference(baseMaterial, "AssetReference", allocator), allocator);
    JsonValue overrides(rapidjson::kObjectType);
    if (stream.GetPosition() < chunk.Length())
    {
        uint16 version;
        uint16 count;
        stream.ReadUint16(&version);
        stream.ReadUint16(&count);
        if (version < 1 || version > 3)
        {
            error = TEXT("Material parameter stream version is unsupported.");
            return true;
        }
        for (int32 i = 0; i < count; i++)
        {
            SerializedMaterialParam parameter;
            if (ReadLegacyParameter(stream, version, parameter))
            {
                error = TEXT("Material parameter type is unsupported.");
                return true;
            }
            if (!parameter.ID.IsValid())
                parameter.ID = Guid(0x4d495041u, 0x52414d49u, static_cast<uint32>(i + 1), 0x44000000u);
            if (!parameter.Override)
                continue;
            const char* typeName = TypeName(parameter.Type);
            if (!typeName)
            {
                error = TEXT("Material parameter type has no semantic encoding.");
                return true;
            }
            JsonValue overrideValue(rapidjson::kObjectType);
            overrideValue.AddMember("type", JsonValue(typeName, allocator), allocator);
            overrideValue.AddMember("value", EncodeValue(parameter, allocator), allocator);
            const StringAnsi key = GuidText(parameter.ID);
            overrides.AddMember(StringValue(key, allocator), overrideValue, allocator);
        }
    }
    document.AddMember("overrides", overrides, allocator);
    return false;
}

bool MaterialInstanceDocument::Compile(const rapidjson_flax::Value& document, Array<byte>& chunk, Array<Guid>* references, String& error)
{
    error.Clear();
    chunk.Clear();
    if (!document.IsObject())
    {
        error = TEXT("Material instance document root must be an object.");
        return true;
    }
    const auto baseMember = document.FindMember("baseMaterial");
    const auto overridesMember = document.FindMember("overrides");
    Guid baseMaterial;
    if (baseMember == document.MemberEnd() || ReadGuidReference(baseMember->value, "AssetReference", baseMaterial))
    {
        error = TEXT("Material instance baseMaterial must be an AssetReference.");
        return true;
    }
    if (overridesMember == document.MemberEnd() || !overridesMember->value.IsObject())
    {
        error = TEXT("Material instance overrides must be an object keyed by parameter GUID.");
        return true;
    }
    if (references && baseMaterial.IsValid())
        references->Add(baseMaterial);

    Array<SerializedMaterialParam> parameters;
    parameters.EnsureCapacity(static_cast<int32>(overridesMember->value.MemberCount()));
    for (auto member = overridesMember->value.MemberBegin(); member != overridesMember->value.MemberEnd(); ++member)
    {
        Guid parameterId;
        if (Guid::Parse(StringAnsiView(member->name.GetString(), member->name.GetStringLength()), parameterId) || !member->value.IsObject())
        {
            error = TEXT("Material instance override key is not a parameter GUID.");
            return true;
        }
        const auto typeMember = member->value.FindMember("type");
        const auto valueMember = member->value.FindMember("value");
        MaterialParameterType type;
        if (typeMember == member->value.MemberEnd() || !typeMember->value.IsString() ||
            ParseType(StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength()), type) ||
            valueMember == member->value.MemberEnd())
        {
            error = TEXT("Material instance override is missing a supported type or value.");
            return true;
        }
        SerializedMaterialParam parameter;
        parameter.Type = type;
        parameter.ID = parameterId;
        parameter.IsPublic = true;
        parameter.Override = true;
        parameter.Name.Clear();
        parameter.ShaderName.Clear();
        parameter.RegisterIndex = 0;
        parameter.Offset = 0;
        Platform::MemoryClear(parameter.AsData, sizeof(parameter.AsData));
        if (DecodeValue(valueMember->value, parameter))
        {
            error = TEXT("Material instance override value does not match its declared type.");
            return true;
        }
        if (references &&
            (type == MaterialParameterType::Texture || type == MaterialParameterType::CubeTexture ||
                type == MaterialParameterType::NormalMap || type == MaterialParameterType::GameplayGlobal) &&
            parameter.AsGuid.IsValid())
            references->Add(parameter.AsGuid);
        parameters.Add(MoveTemp(parameter));
    }

    MemoryWriteStream stream(256);
    stream.Write(baseMaterial);
    MaterialParams::Save(&stream, &parameters);
    chunk.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
    return false;
}
