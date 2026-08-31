// Copyright (c) Wojciech Figat. All rights reserved.

#include "GraphDocument.h"
#include "CanonicalJsonWriter.h"
#include "Engine/Content/Config.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/AssetDatabase/AssetDatabase.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include "Engine/Content/Assets/AnimationGraph.h"
#include "Engine/Content/Assets/AnimationGraphFunction.h"
#include "Engine/Content/Assets/MaterialFunction.h"
#include "Engine/Content/Assets/VisualScript.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Particles/ParticleEmitter.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/ContentImporters/Types.h"
#include "Engine/Graphics/Shaders/Cache/ShaderStorage.h"
#include "Engine/Utilities/Encryption.h"
#if USE_EDITOR
#ifndef COMPILE_WITH_MATERIAL_GRAPH
#define COMPILE_WITH_MATERIAL_GRAPH 1
#endif
#include "Engine/Graphics/Materials/MaterialShader.h"
#include "Engine/Tools/MaterialGenerator/MaterialLayer.h"
#include "Engine/Tools/MaterialGenerator/MaterialGenerator.h"
#if COMPILE_WITH_PARTICLE_GPU_GRAPH
#include "Engine/Particles/Graph/GPU/ParticleEmitterGraph.GPU.h"
#endif
#endif
#endif
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/BoundingSphere.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Core/Math/Quaternion.h"
#include "Engine/Core/Math/Ray.h"
#include "Engine/Core/Math/Rectangle.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Math/Vector2.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Vector4.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Serialization/Json.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#if USE_CSHARP
#include "Engine/Scripting/Internal/ManagedSerialization.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MCore.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#endif
#include "Engine/Threading/Threading.h"
#include "Engine/Utilities/Encryption.h"
#include "Engine/Visject/VisjectGraph.h"
#include <algorithm>
#if PLATFORM_WINDOWS
#include "Engine/Platform/Win32/IncludeWindowsHeaders.h"
#endif

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;
    typedef JsonDocument::AllocatorType JsonAlloc;
    typedef VisjectGraph<> SurfaceGraph;

    constexpr uint32 VisjectMagic = 1963542358u;
    constexpr byte FunctionInputType = 1;
    constexpr byte FunctionOutputType = 2;
    constexpr uint16 FunctionGroupId = 16;

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.ProcessorId = TEXT("Flax.GraphDocument");
        diagnostic.Message = message;
        return true;
    }

    StringAnsi HexToken(const char* prefix, uint32 value, int32 digits)
    {
        static const char Hex[] = "0123456789abcdef";
        StringAnsi result(prefix);
        const int32 start = result.Length();
        result.Resize(start + digits);
        for (int32 i = digits - 1; i >= 0; i--)
        {
            result[start + i] = Hex[value & 15];
            value >>= 4;
        }
        return result;
    }

    StringAnsi GuidToken(const Guid& id)
    {
        const String wide = id.ToString(Guid::FormatType::N);
        StringAnsi result;
        result.Resize(wide.Length());
        for (int32 i = 0; i < wide.Length(); i++)
        {
            const Char c = wide[i];
            result[i] = (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        }
        return result;
    }

    bool ParseGuidToken(const StringAnsiView& text, Guid& id)
    {
        return Guid::Parse(String(text.Get(), text.Length()), id);
    }

    StringAnsi PinToken(byte id)
    {
        return StringAnsi("box:") + StringAnsi::Format("{0}", static_cast<int32>(id));
    }

    bool ParsePinToken(const StringAnsiView& text, byte& id)
    {
        if (text.Length() < 5 || !(text.StartsWith("box:") || text.StartsWith("Box:")))
            return true;
        int32 value = 0;
        if (StringUtils::Parse(String(text.Get() + 4, text.Length() - 4).Get(), &value) || value < 0 || value > 255)
            return true;
        id = static_cast<byte>(value);
        return false;
    }

    const char* VariantTypeName(VariantType::Types type)
    {
        switch (type)
        {
        case VariantType::Null: return "Null";
        case VariantType::Void: return "Void";
        case VariantType::Bool: return "Bool";
        case VariantType::Int: return "Int";
        case VariantType::Uint: return "Uint";
        case VariantType::Int64: return "Int64";
        case VariantType::Uint64: return "Uint64";
        case VariantType::Float: return "Float";
        case VariantType::Double: return "Double";
        case VariantType::Pointer: return "Pointer";
        case VariantType::String: return "String";
        case VariantType::Object: return "ObjectReference";
        case VariantType::Structure: return "Structure";
        case VariantType::Asset: return "AssetReference";
        case VariantType::Blob: return "Blob";
        case VariantType::Enum: return "Enum";
        case VariantType::Float2: return "Vector2";
        case VariantType::Float3: return "Vector3";
        case VariantType::Float4: return "Vector4";
        case VariantType::Color: return "Color";
        case VariantType::Guid: return "Guid";
        case VariantType::BoundingBox: return "BoundingBox";
        case VariantType::BoundingSphere: return "BoundingSphere";
        case VariantType::Quaternion: return "Quaternion";
        case VariantType::Transform: return "Transform";
        case VariantType::Rectangle: return "Rectangle";
        case VariantType::Ray: return "Ray";
        case VariantType::Matrix: return "Matrix";
        case VariantType::Array: return "Array";
        case VariantType::Dictionary: return "Dictionary";
        case VariantType::Typename: return "Typename";
        case VariantType::Int2: return "Int2";
        case VariantType::Int3: return "Int3";
        case VariantType::Int4: return "Int4";
        case VariantType::Int16: return "Int16";
        case VariantType::Uint16: return "Uint16";
        case VariantType::Double2: return "Double2";
        case VariantType::Double3: return "Double3";
        case VariantType::Double4: return "Double4";
        default: return "VariantBinary";
        }
    }

    bool ParseVariantTypeName(const StringAnsiView& name, VariantType::Types& type)
    {
        for (int32 i = 0; i < static_cast<int32>(VariantType::MAX); i++)
        {
            if (name == VariantTypeName(static_cast<VariantType::Types>(i)))
            {
                type = static_cast<VariantType::Types>(i);
                return false;
            }
        }
        if (name == "VariantBinary")
        {
            type = VariantType::Null;
            return false;
        }
        return true;
    }

    bool DecodeBase64(const StringAnsiView& encoded, Array<byte>& output)
    {
        if ((encoded.Length() & 3) != 0)
            return true;
        Encryption::Base64Decode(encoded.Get(), encoded.Length(), output);
        return false;
    }

    JsonValue MakeString(const StringAnsiView& value, JsonAlloc& allocator)
    {
        return JsonValue(value.Get(), value.Length(), allocator);
    }

    void AddString(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), MakeString(value, allocator), allocator);
    }

    void AddInt(JsonValue& object, const char* name, int32 value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value), allocator);
    }

    void AddBool(JsonValue& object, const char* name, bool value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value), allocator);
    }

    bool IsVisualScriptType(const StringView& typeName)
    {
        return typeName == VisualScript::TypeName;
    }

    bool IsBehaviorTreeType(const StringView& typeName)
    {
        return typeName == TEXT("FlaxEngine.BehaviorTree");
    }

    bool IsParticleEmitterFunctionType(const StringView& typeName)
    {
        return typeName == TEXT("FlaxEngine.ParticleEmitterFunction");
    }

    bool IsParticleEmitterType(const StringView& typeName)
    {
        return typeName == TEXT("FlaxEngine.ParticleEmitter");
    }

    bool IsMaterialType(const StringView& typeName)
    {
        return typeName == Material::TypeName;
    }

    StringAnsi MakeMaterialPropertiesJson()
    {
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        AddInt(json, "domain", static_cast<int32>(MaterialDomain::Surface), allocator);
        AddInt(json, "blendMode", static_cast<int32>(MaterialBlendMode::Opaque), allocator);
        AddInt(json, "shadingModel", static_cast<int32>(MaterialShadingModel::Lit), allocator);
        json.AddMember("maskThreshold", JsonValue(0.3f), allocator);
        json.AddMember("opacityThreshold", JsonValue(0.12f), allocator);
        StringAnsi output;
        CanonicalJsonError error;
        Array<StringAnsi> order;
        order.Add("blendMode");
        order.Add("domain");
        order.Add("maskThreshold");
        order.Add("opacityThreshold");
        order.Add("shadingModel");
        CanonicalJsonWriter::Write(json, output, error, &order);
        return output;
    }

    int32 JsonInt(const JsonDocument& json, const char* name, int32 fallback)
    {
        if (!json.HasMember(name))
            return fallback;
        const JsonValue& value = json[name];
        if (value.IsInt())
            return value.GetInt();
        if (value.IsUint())
            return static_cast<int32>(value.GetUint());
        return fallback;
    }

    float JsonFloat(const JsonDocument& json, const char* name, float fallback)
    {
        if (!json.HasMember(name))
            return fallback;
        const JsonValue& value = json[name];
        if (value.IsNumber())
            return value.GetFloat();
        return fallback;
    }

    void ParseMaterialInfo(const StringAnsiView& propertiesJson, MaterialInfo& info)
    {
        info.Domain = MaterialDomain::Surface;
        info.BlendMode = MaterialBlendMode::Opaque;
        info.ShadingModel = MaterialShadingModel::Lit;
        info.UsageFlags = MaterialUsageFlags::None;
        info.FeaturesFlags = MaterialFeaturesFlags::None;
        info.DecalBlendingMode = MaterialDecalBlendingMode::Translucent;
        info.TransparentLightingMode = MaterialTransparentLightingMode::Surface;
        info.PostFxLocation = MaterialPostFxLocation::AfterPostProcessingPass;
        info.CullMode = CullMode::Normal;
        info.MaskThreshold = 0.3f;
        info.OpacityThreshold = 0.12f;
        info.TessellationMode = TessellationMethod::None;
        info.MaxTessellationFactor = 15;
        if (propertiesJson.Length() == 0)
            return;
        JsonDocument json;
        json.Parse(propertiesJson.Get(), propertiesJson.Length());
        if (json.HasParseError() || !json.IsObject())
            return;
        info.Domain = static_cast<MaterialDomain>(JsonInt(json, "domain", static_cast<int32>(info.Domain)));
        info.BlendMode = static_cast<MaterialBlendMode>(JsonInt(json, "blendMode", static_cast<int32>(info.BlendMode)));
        info.ShadingModel = static_cast<MaterialShadingModel>(JsonInt(json, "shadingModel", static_cast<int32>(info.ShadingModel)));
        info.MaskThreshold = JsonFloat(json, "maskThreshold", info.MaskThreshold);
        info.OpacityThreshold = JsonFloat(json, "opacityThreshold", info.OpacityThreshold);
    }

    StringAnsi MakeVisualScriptPropertiesJson(const StringView& baseType, int32 flags)
    {
        JsonDocument json;
        json.SetObject();
        JsonAlloc& allocator = json.GetAllocator();
        const String resolved = baseType.HasChars() ? String(baseType) : String(TEXT("FlaxEngine.Script"));
        AddString(json, "baseType", StringAnsi(resolved), allocator);
        AddInt(json, "flags", flags, allocator);
        StringAnsi output;
        CanonicalJsonError error;
        Array<StringAnsi> order;
        order.Add("baseType");
        order.Add("flags");
        CanonicalJsonWriter::Write(json, output, error, &order);
        return output;
    }

    void WriteVisualScriptMetadata(const StringAnsiView& propertiesJson, Array<byte>& output)
    {
        String baseType(TEXT("FlaxEngine.Script"));
        int32 flags = 0;
        if (propertiesJson.Length() > 0)
        {
            JsonDocument json;
            json.Parse(propertiesJson.Get(), propertiesJson.Length());
            if (!json.HasParseError() && json.IsObject())
            {
                if (json.HasMember("baseType") && json["baseType"].IsString())
                    baseType = String(json["baseType"].GetString());
                if (json.HasMember("flags") && json["flags"].IsInt())
                    flags = json["flags"].GetInt();
                else if (json.HasMember("flags") && json["flags"].IsUint())
                    flags = static_cast<int32>(json["flags"].GetUint());
            }
        }
        MemoryWriteStream stream(256);
        stream.Write(1);
        stream.Write(baseType, 31);
        stream.Write(flags);
        output.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
    }

    JsonValue MakeNumberArray(const float* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue array(rapidjson::kArrayType);
        for (int32 i = 0; i < count; i++)
            array.PushBack(JsonValue(static_cast<double>(values[i])), allocator);
        return array;
    }

    JsonValue MakeDoubleArray(const double* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue array(rapidjson::kArrayType);
        for (int32 i = 0; i < count; i++)
            array.PushBack(JsonValue(values[i]), allocator);
        return array;
    }

    JsonValue MakeIntArray(const int32* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue array(rapidjson::kArrayType);
        for (int32 i = 0; i < count; i++)
            array.PushBack(JsonValue(values[i]), allocator);
        return array;
    }

    JsonValue EncodeVec3(const Vector3& value, JsonAlloc& allocator)
    {
        const double values[3] = { static_cast<double>(value.X), static_cast<double>(value.Y), static_cast<double>(value.Z) };
        return MakeDoubleArray(values, 3, allocator);
    }

    JsonValue EncodeByteArray(const byte* data, int32 length, JsonAlloc& allocator)
    {
        JsonValue array(rapidjson::kArrayType);
        for (int32 i = 0; i < length; i++)
            array.PushBack(JsonValue(static_cast<uint32>(data[i])), allocator);
        return array;
    }

    bool DecodeByteArray(const JsonValue& value, Array<byte>& output)
    {
        if (!value.IsArray())
            return true;
        output.Resize(static_cast<int32>(value.Size()));
        for (rapidjson::SizeType i = 0; i < value.Size(); i++)
        {
            if (!value[i].IsUint() || value[i].GetUint() > 255)
                return true;
            output[i] = static_cast<byte>(value[i].GetUint());
        }
        return false;
    }

    bool ReadNumberArray(const JsonValue& value, float* output, int32 count);

    bool ReadVec3(const JsonValue& value, Vector3& output)
    {
        if (!value.IsArray() || value.Size() != 3)
            return true;
        for (int32 i = 0; i < 3; i++)
        {
            if (!value[i].IsNumber())
                return true;
        }
        output = Vector3(static_cast<Real>(value[0].GetDouble()), static_cast<Real>(value[1].GetDouble()), static_cast<Real>(value[2].GetDouble()));
        return false;
    }

    StringAnsi Utf16LeToUtf8(const byte* data, int32 length)
    {
        if (length <= 0 || (length & 1) != 0)
            return StringAnsi();
        const int32 count = length / static_cast<int32>(sizeof(uint16));
        String wide;
        wide.ReserveSpace(count);
        const uint16* src = reinterpret_cast<const uint16*>(data);
        for (int32 i = 0; i < count; i++)
            wide[i] = static_cast<Char>(src[i]);
        return StringAnsi(wide);
    }

    void Utf8ToUtf16Le(const StringAnsiView& text, Array<byte>& output)
    {
        const String wide(text.Get(), text.Length());
        output.Resize(wide.Length() * static_cast<int32>(sizeof(uint16)));
        uint16* dst = reinterpret_cast<uint16*>(output.Get());
        for (int32 i = 0; i < wide.Length(); i++)
            dst[i] = static_cast<uint16>(wide[i]);
    }

    bool WriteCompactJson(const JsonValue& value, StringAnsi& output)
    {
        rapidjson_flax::StringBuffer buffer;
        rapidjson_flax::Writer<rapidjson_flax::StringBuffer> writer(buffer);
        value.Accept(writer);
        output.Set(buffer.GetString(), static_cast<int32>(buffer.GetSize()));
        return false;
    }

    bool ParseJsonCopy(const StringAnsiView& text, JsonValue& output, JsonAlloc& allocator)
    {
        JsonDocument parsed;
        parsed.Parse(text.Get(), text.Length());
        if (parsed.HasParseError())
            return true;
        output.CopyFrom(parsed, allocator);
        return false;
    }

    bool EncodeComments(const Array<byte>& data, JsonValue& output, JsonAlloc& allocator)
    {
        MemoryReadStream stream(data.Get(), data.Count());
        int32 count = 0;
        stream.ReadInt32(&count);
        if (stream.HasError() || count < 0 || count > 10000)
            return true;
        output.SetArray();
        for (int32 i = 0; i < count; i++)
        {
            String title;
            stream.Read(title, 71);
            float color[4];
            float bounds[4];
            stream.ReadBytes(color, sizeof(color));
            stream.ReadBytes(bounds, sizeof(bounds));
            if (stream.HasError())
                return true;
            JsonValue item(rapidjson::kObjectType);
            AddString(item, "title", StringAnsi(title), allocator);
            item.AddMember("color", MakeNumberArray(color, 4, allocator), allocator);
            item.AddMember("bounds", MakeNumberArray(bounds, 4, allocator), allocator);
            output.PushBack(item, allocator);
        }
        return false;
    }

    bool DecodeComments(const JsonValue& value, Array<byte>& output)
    {
        if (!value.IsArray())
            return true;
        MemoryWriteStream stream(256);
        stream.WriteInt32(static_cast<int32>(value.Size()));
        for (rapidjson::SizeType i = 0; i < value.Size(); i++)
        {
            const JsonValue& item = value[i];
            if (!item.IsObject())
                return true;
            const auto title = item.FindMember("title");
            const auto color = item.FindMember("color");
            const auto bounds = item.FindMember("bounds");
            if (title == item.MemberEnd() || !title->value.IsString() || color == item.MemberEnd() || bounds == item.MemberEnd())
                return true;
            float colorValues[4];
            float boundValues[4];
            if (ReadNumberArray(color->value, colorValues, 4) || ReadNumberArray(bounds->value, boundValues, 4))
                return true;
            const String wide(title->value.GetString(), static_cast<int32>(title->value.GetStringLength()));
            stream.Write(StringView(wide), 71);
            stream.WriteBytes(colorValues, sizeof(colorValues));
            stream.WriteBytes(boundValues, sizeof(boundValues));
        }
        output.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
        return false;
    }

    JsonValue EncodeMetaEntry(const VisjectMeta::Entry& entry, JsonAlloc& allocator)
    {
        JsonValue item(rapidjson::kObjectType);
        item.AddMember("typeId", entry.TypeID, allocator);
        switch (entry.TypeID)
        {
        case 10:
            if (entry.Data.Count() >= 12)
            {
                AddString(item, "kind", "view", allocator);
                item.AddMember("center", MakeNumberArray(reinterpret_cast<const float*>(entry.Data.Get()), 2, allocator), allocator);
                item.AddMember("scale", JsonValue(static_cast<double>(*reinterpret_cast<const float*>(entry.Data.Get() + 8))), allocator);
                return item;
            }
            break;
        case 11:
            if (entry.Data.Count() >= 8)
            {
                AddString(item, "kind", "layout", allocator);
                item.AddMember("position", MakeNumberArray(reinterpret_cast<const float*>(entry.Data.Get()), 2, allocator), allocator);
                item.AddMember("selected", JsonValue(entry.Data.Count() >= 9 && entry.Data.Get()[8] != 0), allocator);
                return item;
            }
            break;
        case 12:
            AddString(item, "kind", "legacyAttributes", allocator);
            item.AddMember("bytes", EncodeByteArray(entry.Data.Get(), entry.Data.Count(), allocator), allocator);
            return item;
        case 13:
        {
            const StringAnsi json = Utf16LeToUtf8(entry.Data.Get(), entry.Data.Count());
            JsonValue parsed;
            if (!json.IsEmpty() && !ParseJsonCopy(json, parsed, allocator))
            {
                AddString(item, "kind", "attributes", allocator);
                item.AddMember("value", parsed, allocator);
                return item;
            }
            break;
        }
        case 666:
        {
            JsonValue comments;
            if (!EncodeComments(entry.Data, comments, allocator))
            {
                AddString(item, "kind", "comments", allocator);
                item.AddMember("value", comments, allocator);
                return item;
            }
            break;
        }
        default:
            break;
        }
        item.AddMember("bytes", EncodeByteArray(entry.Data.Get(), entry.Data.Count(), allocator), allocator);
        return item;
    }

    JsonValue EncodeMeta(const VisjectMeta& meta, JsonAlloc& allocator)
    {
        JsonValue array(rapidjson::kArrayType);
        Array<int32> order;
        order.EnsureCapacity(meta.Entries.Count());
        for (int32 i = 0; i < meta.Entries.Count(); i++)
        {
            if (meta.Entries[i].Data.Count() > 0)
                order.Add(i);
        }
        if (order.Count() > 1)
        {
            std::stable_sort(order.Get(), order.Get() + order.Count(), [&meta](int32 a, int32 b)
            {
                return meta.Entries[a].TypeID < meta.Entries[b].TypeID;
            });
        }
        for (int32 index : order)
            array.PushBack(EncodeMetaEntry(meta.Entries[index], allocator), allocator);
        return array;
    }

    bool DecodeMetaEntry(const JsonValue& item, int32 typeId, Array<byte>& bytes)
    {
        const auto data = item.FindMember("data");
        if (data != item.MemberEnd() && data->value.IsString())
            return DecodeBase64(StringAnsiView(data->value.GetString(), data->value.GetStringLength()), bytes);

        const auto bytesMember = item.FindMember("bytes");
        const auto value = item.FindMember("value");
        switch (typeId)
        {
        case 10:
        {
            const auto center = item.FindMember("center");
            const auto scale = item.FindMember("scale");
            if (center == item.MemberEnd() || scale == item.MemberEnd() || !scale->value.IsNumber())
                return true;
            float raw[3] = {};
            if (ReadNumberArray(center->value, raw, 2))
                return true;
            raw[2] = static_cast<float>(scale->value.GetDouble());
            bytes.Set(reinterpret_cast<const byte*>(raw), sizeof(raw));
            return false;
        }
        case 11:
        {
            const auto position = item.FindMember("position");
            if (position == item.MemberEnd())
                return true;
            byte raw[12] = {};
            if (ReadNumberArray(position->value, reinterpret_cast<float*>(raw), 2))
                return true;
            const auto selected = item.FindMember("selected");
            if (selected != item.MemberEnd() && selected->value.IsBool() && selected->value.GetBool())
                raw[8] = 1;
            bytes.Set(raw, 12);
            return false;
        }
        case 13:
            if (value != item.MemberEnd())
            {
                StringAnsi json;
                WriteCompactJson(value->value, json);
                Utf8ToUtf16Le(json, bytes);
                return false;
            }
            break;
        case 666:
            if (value != item.MemberEnd())
                return DecodeComments(value->value, bytes);
            break;
        default:
            break;
        }
        if (bytesMember != item.MemberEnd())
            return DecodeByteArray(bytesMember->value, bytes);
        return true;
    }

    bool DecodeMeta(const JsonValue& value, VisjectMeta& meta, AssetPipelineDiagnostic& diagnostic)
    {
        meta.Release();
        if (!value.IsArray())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph visject metadata must be an array."));
        for (rapidjson::SizeType i = 0; i < value.Size(); i++)
        {
            const JsonValue& item = value[i];
            if (!item.IsObject())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph visject metadata entries must be objects."));
            const auto typeId = item.FindMember("typeId");
            if (typeId == item.MemberEnd() || !typeId->value.IsInt())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph visject metadata entry is missing typeId."));
            Array<byte> bytes;
            if (DecodeMetaEntry(item, typeId->value.GetInt(), bytes))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph visject metadata entry is invalid."));
            if (bytes.Count() > 0)
                meta.AddEntry(typeId->value.GetInt(), bytes.Get(), bytes.Count());
        }
        return false;
    }

    JsonValue EncodeVariant(const Variant& value, JsonAlloc& allocator)
    {
        JsonValue object(rapidjson::kObjectType);
        auto writeBinary = [&]()
        {
            MemoryWriteStream stream(64);
            stream.Write(value);
            AddString(object, "$type", "VariantBinary", allocator);
            object.AddMember("value", EncodeByteArray(stream.GetHandle(), static_cast<int32>(stream.GetPosition()), allocator), allocator);
        };

        switch (value.Type.Type)
        {
        case VariantType::Null:
        case VariantType::Void:
            AddString(object, "$type", VariantTypeName(value.Type.Type), allocator);
            break;
        case VariantType::Bool:
            AddString(object, "$type", "Bool", allocator);
            object.AddMember("value", value.AsBool, allocator);
            break;
        case VariantType::Int:
            AddString(object, "$type", "Int", allocator);
            object.AddMember("value", value.AsInt, allocator);
            break;
        case VariantType::Uint:
            AddString(object, "$type", "Uint", allocator);
            object.AddMember("value", value.AsUint, allocator);
            break;
        case VariantType::Int64:
            AddString(object, "$type", "Int64", allocator);
            object.AddMember("value", static_cast<int64>(value.AsInt64), allocator);
            break;
        case VariantType::Uint64:
            AddString(object, "$type", "Uint64", allocator);
            object.AddMember("value", static_cast<uint64>(value.AsUint64), allocator);
            break;
        case VariantType::Float:
            AddString(object, "$type", "Float", allocator);
            object.AddMember("value", static_cast<double>(value.AsFloat), allocator);
            break;
        case VariantType::Double:
            AddString(object, "$type", "Double", allocator);
            object.AddMember("value", value.AsDouble, allocator);
            break;
        case VariantType::String:
            AddString(object, "$type", "String", allocator);
            AddString(object, "value", StringAnsi((StringView)value), allocator);
            break;
        case VariantType::Guid:
        case VariantType::Object:
        {
            const Guid id = (Guid)value;
            AddString(object, "$type", VariantTypeName(value.Type.Type), allocator);
            AddString(object, "guid", GuidToken(id), allocator);
            if (value.Type.TypeName)
                AddString(object, "typeName", StringAnsiView(value.Type.TypeName), allocator);
            break;
        }
        case VariantType::Asset:
        {
            const Guid runtimeId = (Guid)value;
            AssetObjectId objectId = Content::ResolveAssetObjectId(runtimeId);
            if (objectId.IsNull() && runtimeId.IsValid())
                objectId = AssetObjectId::Main(AssetGuid(runtimeId));
            AddString(object, "$type", "AssetReference", allocator);
            AddString(object, "guid", GuidToken(objectId.Asset.Value), allocator);
            object.AddMember("fileId", static_cast<int64>(objectId.LocalId), allocator);
            if (value.Type.TypeName)
                AddString(object, "typeName", StringAnsiView(value.Type.TypeName), allocator);
            break;
        }
        case VariantType::Enum:
            AddString(object, "$type", "Enum", allocator);
            if (value.Type.TypeName)
                AddString(object, "enum", StringAnsiView(value.Type.TypeName), allocator);
            object.AddMember("value", static_cast<uint64>(value.AsUint64), allocator);
            break;
        case VariantType::Float2:
            AddString(object, "$type", "Vector2", allocator);
            object.AddMember("value", MakeNumberArray(reinterpret_cast<const float*>(&value.AsData), 2, allocator), allocator);
            break;
        case VariantType::Float3:
            AddString(object, "$type", "Vector3", allocator);
            object.AddMember("value", MakeNumberArray(reinterpret_cast<const float*>(&value.AsData), 3, allocator), allocator);
            break;
        case VariantType::Float4:
            AddString(object, "$type", "Vector4", allocator);
            object.AddMember("value", MakeNumberArray(reinterpret_cast<const float*>(&value.AsData), 4, allocator), allocator);
            break;
        case VariantType::Color:
            AddString(object, "$type", "Color", allocator);
            object.AddMember("value", MakeNumberArray(reinterpret_cast<const float*>(&value.AsColor()), 4, allocator), allocator);
            break;
        case VariantType::Blob:
            AddString(object, "$type", "Blob", allocator);
            object.AddMember("value", EncodeByteArray(static_cast<const byte*>(value.AsBlob.Data), value.AsBlob.Length, allocator), allocator);
            break;
        case VariantType::Typename:
            AddString(object, "$type", "Typename", allocator);
            AddString(object, "value", (StringAnsiView)value, allocator);
            break;
        case VariantType::Pointer:
            AddString(object, "$type", "Pointer", allocator);
            object.AddMember("value", static_cast<uint64>(reinterpret_cast<uintptr>(value.AsPointer)), allocator);
            break;
        case VariantType::Int16:
            AddString(object, "$type", "Int16", allocator);
            object.AddMember("value", static_cast<int32>(value.AsInt16), allocator);
            break;
        case VariantType::Uint16:
            AddString(object, "$type", "Uint16", allocator);
            object.AddMember("value", static_cast<uint32>(value.AsUint16), allocator);
            break;
        case VariantType::Int2:
        {
            AddString(object, "$type", "Int2", allocator);
            const int32 raw[2] = { value.AsInt2().X, value.AsInt2().Y };
            object.AddMember("value", MakeIntArray(raw, 2, allocator), allocator);
            break;
        }
        case VariantType::Int3:
        {
            AddString(object, "$type", "Int3", allocator);
            const int32 raw[3] = { value.AsInt3().X, value.AsInt3().Y, value.AsInt3().Z };
            object.AddMember("value", MakeIntArray(raw, 3, allocator), allocator);
            break;
        }
        case VariantType::Int4:
        {
            AddString(object, "$type", "Int4", allocator);
            const int32 raw[4] = { value.AsInt4().X, value.AsInt4().Y, value.AsInt4().Z, value.AsInt4().W };
            object.AddMember("value", MakeIntArray(raw, 4, allocator), allocator);
            break;
        }
        case VariantType::Double2:
        {
            AddString(object, "$type", "Double2", allocator);
            const double raw[2] = { value.AsDouble2().X, value.AsDouble2().Y };
            object.AddMember("value", MakeDoubleArray(raw, 2, allocator), allocator);
            break;
        }
        case VariantType::Double3:
        {
            AddString(object, "$type", "Double3", allocator);
            const double raw[3] = { value.AsDouble3().X, value.AsDouble3().Y, value.AsDouble3().Z };
            object.AddMember("value", MakeDoubleArray(raw, 3, allocator), allocator);
            break;
        }
        case VariantType::Double4:
        {
            AddString(object, "$type", "Double4", allocator);
            const double raw[4] = { value.AsDouble4().X, value.AsDouble4().Y, value.AsDouble4().Z, value.AsDouble4().W };
            object.AddMember("value", MakeDoubleArray(raw, 4, allocator), allocator);
            break;
        }
        case VariantType::Quaternion:
        {
            AddString(object, "$type", "Quaternion", allocator);
            const float raw[4] = { value.AsQuaternion().X, value.AsQuaternion().Y, value.AsQuaternion().Z, value.AsQuaternion().W };
            object.AddMember("value", MakeNumberArray(raw, 4, allocator), allocator);
            break;
        }
        case VariantType::Transform:
        {
            AddString(object, "$type", "Transform", allocator);
            const Transform& transform = value.AsTransform();
            JsonValue payload(rapidjson::kObjectType);
            payload.AddMember("translation", EncodeVec3(transform.Translation, allocator), allocator);
            const float orientation[4] = { transform.Orientation.X, transform.Orientation.Y, transform.Orientation.Z, transform.Orientation.W };
            payload.AddMember("orientation", MakeNumberArray(orientation, 4, allocator), allocator);
            payload.AddMember("scale", MakeNumberArray(reinterpret_cast<const float*>(&transform.Scale), 3, allocator), allocator);
            object.AddMember("value", payload, allocator);
            break;
        }
        case VariantType::BoundingBox:
        {
            AddString(object, "$type", "BoundingBox", allocator);
            const BoundingBox& box = value.AsBoundingBox();
            JsonValue payload(rapidjson::kObjectType);
            payload.AddMember("min", EncodeVec3(box.Minimum, allocator), allocator);
            payload.AddMember("max", EncodeVec3(box.Maximum, allocator), allocator);
            object.AddMember("value", payload, allocator);
            break;
        }
        case VariantType::BoundingSphere:
        {
            AddString(object, "$type", "BoundingSphere", allocator);
            const BoundingSphere& sphere = value.AsBoundingSphere();
            JsonValue payload(rapidjson::kObjectType);
            payload.AddMember("center", EncodeVec3(sphere.Center, allocator), allocator);
            payload.AddMember("radius", JsonValue(static_cast<double>(sphere.Radius)), allocator);
            object.AddMember("value", payload, allocator);
            break;
        }
        case VariantType::Rectangle:
        {
            AddString(object, "$type", "Rectangle", allocator);
            const Rectangle& rectangle = value.AsRectangle();
            JsonValue payload(rapidjson::kObjectType);
            payload.AddMember("location", MakeNumberArray(reinterpret_cast<const float*>(&rectangle.Location), 2, allocator), allocator);
            payload.AddMember("size", MakeNumberArray(reinterpret_cast<const float*>(&rectangle.Size), 2, allocator), allocator);
            object.AddMember("value", payload, allocator);
            break;
        }
        case VariantType::Ray:
        {
            AddString(object, "$type", "Ray", allocator);
            const Ray& ray = value.AsRay();
            JsonValue payload(rapidjson::kObjectType);
            payload.AddMember("position", EncodeVec3(ray.Position, allocator), allocator);
            payload.AddMember("direction", EncodeVec3(ray.Direction, allocator), allocator);
            object.AddMember("value", payload, allocator);
            break;
        }
        case VariantType::Matrix:
            AddString(object, "$type", "Matrix", allocator);
            object.AddMember("value", MakeNumberArray(value.AsMatrix().Raw, 16, allocator), allocator);
            break;
        case VariantType::Array:
        {
            AddString(object, "$type", "Array", allocator);
            JsonValue items(rapidjson::kArrayType);
            const Array<Variant>& array = value.AsArray();
            for (int32 i = 0; i < array.Count(); i++)
                items.PushBack(EncodeVariant(array[i], allocator), allocator);
            object.AddMember("value", items, allocator);
            break;
        }
        case VariantType::Dictionary:
        {
            AddString(object, "$type", "Dictionary", allocator);
            JsonValue items(rapidjson::kArrayType);
            if (value.AsDictionary)
            {
                struct DictItem
                {
                    Variant Key;
                    Variant Value;
                    StringAnsi KeyJson;
                };
                Array<DictItem> sorted;
                for (auto i = value.AsDictionary->Begin(); i.IsNotEnd(); ++i)
                {
                    DictItem item;
                    item.Key = i->Key;
                    item.Value = i->Value;
                    JsonValue encodedKey = EncodeVariant(item.Key, allocator);
                    CanonicalJsonError error;
                    CanonicalJsonWriter::Write(encodedKey, item.KeyJson, error);
                    sorted.Add(MoveTemp(item));
                }
                if (sorted.Count() > 1)
                {
                    std::sort(sorted.Get(), sorted.Get() + sorted.Count(), [](const DictItem& a, const DictItem& b)
                    {
                        return a.KeyJson < b.KeyJson;
                    });
                }
                for (const DictItem& item : sorted)
                {
                    JsonValue pair(rapidjson::kObjectType);
                    pair.AddMember("key", EncodeVariant(item.Key, allocator), allocator);
                    pair.AddMember("value", EncodeVariant(item.Value, allocator), allocator);
                    items.PushBack(pair, allocator);
                }
            }
            object.AddMember("value", items, allocator);
            break;
        }
        case VariantType::Structure:
        {
#if USE_CSHARP
            AddString(object, "$type", "Structure", allocator);
            if (value.Type.TypeName)
                AddString(object, "typeName", StringAnsiView(value.Type.TypeName), allocator);
            MCore::Thread::Attach();
            MObject* obj = MUtils::BoxVariant(value);
            if (obj)
            {
                rapidjson_flax::StringBuffer json;
                CompactJsonWriter writer(json);
                ManagedSerialization::Serialize(writer, obj);
                JsonValue parsed;
                if (!ParseJsonCopy(StringAnsiView(json.GetString(), static_cast<int32>(json.GetSize())), parsed, allocator))
                    object.AddMember("value", parsed, allocator);
                else
                    object.AddMember("value", JsonValue(rapidjson::kObjectType), allocator);
            }
            else
            {
                object.AddMember("value", JsonValue(rapidjson::kObjectType), allocator);
            }
            break;
#else
            writeBinary();
            break;
#endif
        }
        default:
            writeBinary();
            break;
        }
        return object;
    }

    bool ReadNumberArray(const JsonValue& value, float* output, int32 count)
    {
        if (!value.IsArray() || static_cast<int32>(value.Size()) != count)
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber())
                return true;
            output[i] = static_cast<float>(value[i].GetDouble());
        }
        return false;
    }

    bool ReadIntArray(const JsonValue& value, int32* output, int32 count)
    {
        if (!value.IsArray() || static_cast<int32>(value.Size()) != count)
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber())
                return true;
            output[i] = value[i].IsInt() ? value[i].GetInt() : static_cast<int32>(value[i].GetDouble());
        }
        return false;
    }

    bool ReadDoubleArray(const JsonValue& value, double* output, int32 count)
    {
        if (!value.IsArray() || static_cast<int32>(value.Size()) != count)
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber())
                return true;
            output[i] = value[i].GetDouble();
        }
        return false;
    }

    bool DecodeVariant(const JsonValue& value, Variant& result, AssetPipelineDiagnostic& diagnostic)
    {
        result = Variant();
        if (!value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph typed values must be objects."));
        const auto typeMember = value.FindMember("$type");
        if (typeMember == value.MemberEnd() || !typeMember->value.IsString())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph typed values require $type."));
        const StringAnsiView typeName(typeMember->value.GetString(), typeMember->value.GetStringLength());
        const auto payload = value.FindMember("value");
        const auto guidMember = value.FindMember("guid");
        if (typeName == "VariantBinary")
        {
            if (payload == value.MemberEnd())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("VariantBinary values require a byte-array payload."));
            Array<byte> bytes;
            const bool decodeFailed = payload->value.IsString()
                ? DecodeBase64(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength()), bytes)
                : DecodeByteArray(payload->value, bytes);
            if (decodeFailed)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("VariantBinary payload is not a valid byte array or legacy base64 value."));
            MemoryReadStream stream(bytes.Get(), bytes.Count());
            stream.Read(result);
            return stream.HasError() ? Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("VariantBinary payload is truncated.")) : false;
        }
        if (typeName == "Null" || typeName == "Void")
        {
            result.Type.Type = typeName == "Null" ? VariantType::Null : VariantType::Void;
            return false;
        }
        if (typeName == "Bool" && payload != value.MemberEnd() && payload->value.IsBool())
        {
            result = payload->value.GetBool();
            return false;
        }
        if (typeName == "Int" && payload != value.MemberEnd() && payload->value.IsInt())
        {
            result = payload->value.GetInt();
            return false;
        }
        if (typeName == "Uint" && payload != value.MemberEnd() && payload->value.IsUint())
        {
            result = payload->value.GetUint();
            return false;
        }
        if (typeName == "Int64" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = payload->value.IsInt64() ? payload->value.GetInt64() : static_cast<int64>(payload->value.GetDouble());
            return false;
        }
        if (typeName == "Uint64" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = payload->value.IsUint64() ? payload->value.GetUint64() : static_cast<uint64>(payload->value.GetDouble());
            return false;
        }
        if (typeName == "Float" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = static_cast<float>(payload->value.GetDouble());
            return false;
        }
        if (typeName == "Double" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = payload->value.GetDouble();
            return false;
        }
        if (typeName == "String" && payload != value.MemberEnd() && payload->value.IsString())
        {
            result.SetString(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength()));
            return false;
        }
        if ((typeName == "AssetReference" || typeName == "Guid" || typeName == "ObjectReference") && guidMember != value.MemberEnd() && guidMember->value.IsString())
        {
            Guid id;
            if (ParseGuidToken(StringAnsiView(guidMember->value.GetString(), guidMember->value.GetStringLength()), id))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph GUID value is invalid."));
            if (typeName == "AssetReference")
            {
                const auto fileId = value.FindMember("fileId");
                if (fileId == value.MemberEnd() || !fileId->value.IsInt64() || (id.IsValid() && fileId->value.GetInt64() == 0))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph asset references require a persistent fileId."));
                result.SetAsset(id.IsValid()
                    ? Content::LoadAsync<Asset>(AssetObjectId(AssetGuid(id), fileId->value.GetInt64()))
                    : nullptr);
            }
            else if (typeName == "ObjectReference")
                result.SetObject(FindObject(id, ScriptingObject::GetStaticClass()));
            else
                result = Variant(id);
            const auto typeNameMember = value.FindMember("typeName");
            if (typeNameMember != value.MemberEnd() && typeNameMember->value.IsString())
                result.Type.SetTypeName(StringAnsiView(typeNameMember->value.GetString(), typeNameMember->value.GetStringLength()));
            return false;
        }
        if (typeName == "Enum")
        {
            result.Type.Type = VariantType::Enum;
            const auto enumName = value.FindMember("enum");
            if (enumName != value.MemberEnd() && enumName->value.IsString())
                result.Type.SetTypeName(StringAnsiView(enumName->value.GetString(), enumName->value.GetStringLength()));
            if (payload != value.MemberEnd() && payload->value.IsNumber())
                result.AsUint64 = payload->value.GetUint64();
            return false;
        }
        if (typeName == "Vector2" && payload != value.MemberEnd())
        {
            Float2 vector;
            if (ReadNumberArray(payload->value, reinterpret_cast<float*>(&vector), 2))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Vector2 value is invalid."));
            result = vector;
            return false;
        }
        if (typeName == "Vector3" && payload != value.MemberEnd())
        {
            Float3 vector;
            if (ReadNumberArray(payload->value, reinterpret_cast<float*>(&vector), 3))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Vector3 value is invalid."));
            result = vector;
            return false;
        }
        if (typeName == "Vector4" && payload != value.MemberEnd())
        {
            Float4 vector;
            if (ReadNumberArray(payload->value, reinterpret_cast<float*>(&vector), 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Vector4 value is invalid."));
            result = vector;
            return false;
        }
        if (typeName == "Color" && payload != value.MemberEnd())
        {
            Color color;
            if (ReadNumberArray(payload->value, reinterpret_cast<float*>(&color), 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Color value is invalid."));
            result = color;
            return false;
        }
        if (typeName == "Blob" && payload != value.MemberEnd())
        {
            Array<byte> bytes;
            if (payload->value.IsString())
            {
                if (DecodeBase64(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength()), bytes))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Blob value is not valid base64."));
            }
            else if (DecodeByteArray(payload->value, bytes))
            {
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Blob value is invalid."));
            }
            result.SetBlob(bytes.Get(), bytes.Count());
            return false;
        }
        if (typeName == "Typename" && payload != value.MemberEnd() && payload->value.IsString())
        {
            result.SetTypename(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength()));
            return false;
        }
        if (typeName == "Pointer" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            const uint64 raw = payload->value.IsUint64() ? payload->value.GetUint64() : static_cast<uint64>(payload->value.GetUint());
            result = reinterpret_cast<void*>(static_cast<uintptr>(raw));
            return false;
        }
        if (typeName == "Int16" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = static_cast<int16>(payload->value.IsInt() ? payload->value.GetInt() : static_cast<int32>(payload->value.GetDouble()));
            return false;
        }
        if (typeName == "Uint16" && payload != value.MemberEnd() && payload->value.IsNumber())
        {
            result = static_cast<uint16>(payload->value.IsUint() ? payload->value.GetUint() : static_cast<uint32>(payload->value.GetDouble()));
            return false;
        }
        if (typeName == "Int2" && payload != value.MemberEnd())
        {
            int32 raw[2];
            if (ReadIntArray(payload->value, raw, 2))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Int2 value is invalid."));
            result = Int2(raw[0], raw[1]);
            return false;
        }
        if (typeName == "Int3" && payload != value.MemberEnd())
        {
            int32 raw[3];
            if (ReadIntArray(payload->value, raw, 3))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Int3 value is invalid."));
            result = Int3(raw[0], raw[1], raw[2]);
            return false;
        }
        if (typeName == "Int4" && payload != value.MemberEnd())
        {
            int32 raw[4];
            if (ReadIntArray(payload->value, raw, 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Int4 value is invalid."));
            result = Int4(raw[0], raw[1], raw[2], raw[3]);
            return false;
        }
        if (typeName == "Double2" && payload != value.MemberEnd())
        {
            double raw[2];
            if (ReadDoubleArray(payload->value, raw, 2))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Double2 value is invalid."));
            result = Double2(raw[0], raw[1]);
            return false;
        }
        if (typeName == "Double3" && payload != value.MemberEnd())
        {
            double raw[3];
            if (ReadDoubleArray(payload->value, raw, 3))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Double3 value is invalid."));
            result = Double3(raw[0], raw[1], raw[2]);
            return false;
        }
        if (typeName == "Double4" && payload != value.MemberEnd())
        {
            double raw[4];
            if (ReadDoubleArray(payload->value, raw, 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Double4 value is invalid."));
            result = Double4(raw[0], raw[1], raw[2], raw[3]);
            return false;
        }
        if (typeName == "Quaternion" && payload != value.MemberEnd())
        {
            float raw[4];
            if (ReadNumberArray(payload->value, raw, 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Quaternion value is invalid."));
            result = Quaternion(raw[0], raw[1], raw[2], raw[3]);
            return false;
        }
        if (typeName == "Transform" && payload != value.MemberEnd() && payload->value.IsObject())
        {
            const auto translation = payload->value.FindMember("translation");
            const auto orientation = payload->value.FindMember("orientation");
            const auto scale = payload->value.FindMember("scale");
            Transform transform = Transform::Identity;
            if (translation != payload->value.MemberEnd() && ReadVec3(translation->value, transform.Translation))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Transform translation is invalid."));
            float orientationRaw[4];
            if (orientation != payload->value.MemberEnd() && ReadNumberArray(orientation->value, orientationRaw, 4))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Transform orientation is invalid."));
            if (orientation != payload->value.MemberEnd())
                transform.Orientation = Quaternion(orientationRaw[0], orientationRaw[1], orientationRaw[2], orientationRaw[3]);
            if (scale != payload->value.MemberEnd() && ReadNumberArray(scale->value, reinterpret_cast<float*>(&transform.Scale), 3))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Transform scale is invalid."));
            result = Variant(transform);
            return false;
        }
        if (typeName == "BoundingBox" && payload != value.MemberEnd() && payload->value.IsObject())
        {
            const auto min = payload->value.FindMember("min");
            const auto max = payload->value.FindMember("max");
            BoundingBox box;
            if (min == payload->value.MemberEnd() || max == payload->value.MemberEnd() || ReadVec3(min->value, box.Minimum) || ReadVec3(max->value, box.Maximum))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("BoundingBox value is invalid."));
            result = Variant(box);
            return false;
        }
        if (typeName == "BoundingSphere" && payload != value.MemberEnd() && payload->value.IsObject())
        {
            const auto center = payload->value.FindMember("center");
            const auto radius = payload->value.FindMember("radius");
            BoundingSphere sphere;
            if (center == payload->value.MemberEnd() || radius == payload->value.MemberEnd() || !radius->value.IsNumber() || ReadVec3(center->value, sphere.Center))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("BoundingSphere value is invalid."));
            sphere.Radius = static_cast<Real>(radius->value.GetDouble());
            result = sphere;
            return false;
        }
        if (typeName == "Rectangle" && payload != value.MemberEnd() && payload->value.IsObject())
        {
            const auto location = payload->value.FindMember("location");
            const auto size = payload->value.FindMember("size");
            Rectangle rectangle;
            if (location == payload->value.MemberEnd() || size == payload->value.MemberEnd() || ReadNumberArray(location->value, reinterpret_cast<float*>(&rectangle.Location), 2) || ReadNumberArray(size->value, reinterpret_cast<float*>(&rectangle.Size), 2))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Rectangle value is invalid."));
            result = rectangle;
            return false;
        }
        if (typeName == "Ray" && payload != value.MemberEnd() && payload->value.IsObject())
        {
            const auto position = payload->value.FindMember("position");
            const auto direction = payload->value.FindMember("direction");
            Ray ray;
            if (position == payload->value.MemberEnd() || direction == payload->value.MemberEnd() || ReadVec3(position->value, ray.Position) || ReadVec3(direction->value, ray.Direction))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Ray value is invalid."));
            result = Variant(ray);
            return false;
        }
        if (typeName == "Matrix" && payload != value.MemberEnd())
        {
            Matrix matrix;
            if (ReadNumberArray(payload->value, matrix.Raw, 16))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Matrix value is invalid."));
            result = Variant(matrix);
            return false;
        }
        if (typeName == "Array" && payload != value.MemberEnd() && payload->value.IsArray())
        {
            Array<Variant> items;
            items.Resize(static_cast<int32>(payload->value.Size()));
            for (rapidjson::SizeType i = 0; i < payload->value.Size(); i++)
            {
                if (DecodeVariant(payload->value[i], items[i], diagnostic))
                    return true;
            }
            result = items;
            return false;
        }
        if (typeName == "Dictionary" && payload != value.MemberEnd() && payload->value.IsArray())
        {
            Dictionary<Variant, Variant> items;
            for (rapidjson::SizeType i = 0; i < payload->value.Size(); i++)
            {
                const JsonValue& pair = payload->value[i];
                if (!pair.IsObject())
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Dictionary entries must be objects."));
                const auto key = pair.FindMember("key");
                const auto itemValue = pair.FindMember("value");
                if (key == pair.MemberEnd() || itemValue == pair.MemberEnd())
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Dictionary entries require key and value."));
                Variant decodedKey;
                Variant decodedValue;
                if (DecodeVariant(key->value, decodedKey, diagnostic) || DecodeVariant(itemValue->value, decodedValue, diagnostic))
                    return true;
                items.Add(decodedKey, decodedValue);
            }
            result = Variant(items);
            return false;
        }
        if (typeName == "Structure")
        {
#if USE_CSHARP
            result.SetType(VariantType(VariantType::Structure));
            const auto typeNameMember = value.FindMember("typeName");
            if (typeNameMember != value.MemberEnd() && typeNameMember->value.IsString())
                result.Type.SetTypeName(StringAnsiView(typeNameMember->value.GetString(), typeNameMember->value.GetStringLength()));
            if (payload == value.MemberEnd())
                return false;
            StringAnsi json;
            WriteCompactJson(payload->value, json);
            MCore::Thread::Attach();
            MClass* klass = MUtils::GetClass(result.Type);
            if (!klass)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Structure type is unknown."));
            MObject* obj = MCore::Object::New(klass);
            if (!obj)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Structure instance could not be created."));
            if (!klass->IsValueType())
                MCore::Object::Init(obj);
            ManagedSerialization::Deserialize(json, obj);
            result = MUtils::UnboxVariant(obj);
            return false;
#else
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Structure values require C#."));
#endif
        }
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Unsupported graph value type."));
    }

    JsonValue EncodeVariantType(const VariantType& type, JsonAlloc& allocator)
    {
        JsonValue object(rapidjson::kObjectType);
        AddString(object, "$type", "VariantType", allocator);
        AddString(object, "type", VariantTypeName(type.Type), allocator);
        if (type.TypeName)
            AddString(object, "typeName", StringAnsiView(type.TypeName), allocator);
        return object;
    }

    bool DecodeVariantType(const JsonValue& value, VariantType& type, AssetPipelineDiagnostic& diagnostic)
    {
        type = VariantType();
        if (value.IsObject())
        {
            const auto name = value.FindMember("type");
            if (name == value.MemberEnd() || !name->value.IsString())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("VariantType objects require type."));
            if (ParseVariantTypeName(StringAnsiView(name->value.GetString(), name->value.GetStringLength()), type.Type))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("VariantType name is unknown."));
            const auto typeName = value.FindMember("typeName");
            if (typeName != value.MemberEnd() && typeName->value.IsString())
                type.SetTypeName(StringAnsiView(typeName->value.GetString(), typeName->value.GetStringLength()));
            return false;
        }
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Pin types must be objects."));
    }

    void ReadNodeLayout(const VisjectMeta& meta, float& x, float& y)
    {
        x = 0.0f;
        y = 0.0f;
        const VisjectMeta::Entry* entry = meta.GetEntry(11);
        if (entry && entry->Data.Count() >= static_cast<int32>(sizeof(float) * 2))
        {
            memcpy(&x, entry->Data.Get(), sizeof(float));
            memcpy(&y, entry->Data.Get() + sizeof(float), sizeof(float));
        }
    }

    void WriteNodeLayout(VisjectMeta& meta, float x, float y)
    {
        byte data[12] = {};
        memcpy(data, &x, sizeof(float));
        memcpy(data + sizeof(float), &y, sizeof(float));
        if (VisjectMeta::Entry* entry = meta.GetEntry(11))
        {
            if (entry->Data.Count() < 8)
                entry->Data.Resize(12, true);
            memcpy(entry->Data.Get(), data, 8);
            entry->IsLoaded = true;
            return;
        }
        meta.AddEntry(11, data, 12);
    }

    template<typename DestinationAllocation, typename SourceAllocation>
    void CopyVariants(Array<Variant, DestinationAllocation>& destination, const Array<Variant, SourceAllocation>& source)
    {
        destination.Clear();
        destination.Add(source);
    }

    bool ConnectionLess(const GraphDocumentConnection& a, const GraphDocumentConnection& b)
    {
        if (a.FromNode != b.FromNode)
            return a.FromNode < b.FromNode;
        if (a.FromPin != b.FromPin)
            return a.FromPin < b.FromPin;
        if (a.ToNode != b.ToNode)
            return a.ToNode < b.ToNode;
        return a.ToPin < b.ToPin;
    }

    bool FlushWrittenFile(const StringView& path)
    {
#if PLATFORM_WINDOWS
        const String value(path);
        HANDLE handle = CreateFileW(*value, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        const bool failed = FlushFileBuffers(handle) == 0;
        CloseHandle(handle);
        return failed;
#else
        return false;
#endif
    }

    bool AtomicReplace(const StringView& destination, const StringView& staging)
    {
#if PLATFORM_WINDOWS
        const String destinationPath(destination);
        const String stagingPath(staging);
        return MoveFileExW(*stagingPath, *destinationPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0;
#else
        return FileSystem::MoveFile(destination, staging, true);
#endif
    }

    struct GraphArtifactArguments
    {
        const Array<byte>* Surface = nullptr;
        String TypeName;
        StringAnsi PropertiesJson;
        Guid ID = Guid::Empty;
    };

#if COMPILE_WITH_ASSETS_IMPORTER
    CreateAssetResult CreateGraphArtifact(CreateAssetContext& context)
    {
        const auto* arguments = static_cast<const GraphArtifactArguments*>(context.CustomArg);
        if (!arguments || !arguments->Surface || arguments->Surface->IsEmpty() || arguments->TypeName.IsEmpty())
            return CreateAssetResult::Error;
        context.Data.Header.TypeName = arguments->TypeName;
        if (arguments->ID.IsValid())
            context.Data.Header.ID = arguments->ID;
        if (IsMaterialType(arguments->TypeName))
        {
            context.Data.SerializedVersion = 20;
            context.SkipMetadata = true;
            if (context.AllocateChunk(SHADER_FILE_CHUNK_VISJECT_SURFACE))
                return CreateAssetResult::CannotAllocateChunk;
            context.Data.Header.Chunks[SHADER_FILE_CHUNK_VISJECT_SURFACE]->Data.Copy(ToSpan(*arguments->Surface));
            ShaderStorage::Header20 shaderHeader;
            Platform::MemoryClear(&shaderHeader, sizeof(shaderHeader));
            ParseMaterialInfo(arguments->PropertiesJson, shaderHeader.Material.Info);
            shaderHeader.Material.GraphVersion = MATERIAL_GRAPH_VERSION;
#if USE_EDITOR
            if (context.IsArtifactStagingMode())
            {
                if (context.AllocateChunk(SHADER_FILE_CHUNK_MATERIAL_PARAMS) || context.AllocateChunk(SHADER_FILE_CHUNK_SOURCE))
                    return CreateAssetResult::CannotAllocateChunk;
                MemoryReadStream stream(arguments->Surface->Get(), arguments->Surface->Count());
                MaterialLayer* layer = MaterialLayer::Load(context.Data.Header.ID, &stream, shaderHeader.Material.Info, TEXT("GraphDocument"));
                MaterialGenerator generator;
                generator.AddLayer(layer);
                MemoryWriteStream source(64 * 1024);
                MaterialInfo generatedInfo = shaderHeader.Material.Info;
                if (generator.Generate(source, generatedInfo, context.Data.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS]->Data))
                {
                    return CreateAssetResult::Error;
                }
                Encryption::EncryptBytes(static_cast<byte*>(source.GetHandle()), source.GetPosition());
                context.Data.Header.Chunks[SHADER_FILE_CHUNK_SOURCE]->Data.Copy(ToSpan(source));
                shaderHeader.Material.Info = generatedInfo;
            }
#endif
            context.Data.CustomData.Copy(&shaderHeader);
            return CreateAssetResult::Ok;
        }
        if (IsParticleEmitterType(arguments->TypeName))
        {
            context.Data.SerializedVersion = 20;
            context.SkipMetadata = true;
            if (context.AllocateChunk(SHADER_FILE_CHUNK_VISJECT_SURFACE))
                return CreateAssetResult::CannotAllocateChunk;
            context.Data.Header.Chunks[SHADER_FILE_CHUNK_VISJECT_SURFACE]->Data.Copy(ToSpan(*arguments->Surface));
            ShaderStorage::Header20 shaderHeader;
            Platform::MemoryClear(&shaderHeader, sizeof(shaderHeader));
#if USE_EDITOR && COMPILE_WITH_PARTICLE_GPU_GRAPH && COMPILE_WITH_SHADER_COMPILER
            if (context.IsArtifactStagingMode())
            {
                if (context.AllocateChunk(SHADER_FILE_CHUNK_MATERIAL_PARAMS) || context.AllocateChunk(SHADER_FILE_CHUNK_SOURCE))
                    return CreateAssetResult::CannotAllocateChunk;
                MemoryReadStream stream(arguments->Surface->Get(), arguments->Surface->Count());
                auto* graph = New<ParticleEmitterGraphGPU>();
                if (graph->Load(&stream, false))
                {
                    Delete(graph);
                    return CreateAssetResult::Error;
                }
                ParticleEmitterGPUGenerator generator;
                generator.AddGraph(graph);
                MemoryWriteStream source(16 * 1024);
                int32 customDataSize;
                if (generator.Generate(source, context.Data.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS]->Data, customDataSize))
                    return CreateAssetResult::Error;
                Encryption::EncryptBytes(static_cast<byte*>(source.GetHandle()), source.GetPosition());
                context.Data.Header.Chunks[SHADER_FILE_CHUNK_SOURCE]->Data.Copy(ToSpan(source));
                shaderHeader.ParticleEmitter.GraphVersion = PARTICLE_GPU_GRAPH_VERSION;
                shaderHeader.ParticleEmitter.CustomDataSize = customDataSize;
            }
#endif
            context.Data.CustomData.Copy(&shaderHeader);
            return CreateAssetResult::Ok;
        }
        context.Data.SerializedVersion = 1;
        if (context.AllocateChunk(0))
            return CreateAssetResult::CannotAllocateChunk;
        context.Data.Header.Chunks[0]->Data.Copy(ToSpan(*arguments->Surface));
        if (IsVisualScriptType(arguments->TypeName))
        {
            if (context.AllocateChunk(1))
                return CreateAssetResult::CannotAllocateChunk;
            Array<byte> metadata;
            WriteVisualScriptMetadata(arguments->PropertiesJson, metadata);
            context.Data.Header.Chunks[1]->Data.Copy(ToSpan(metadata));
        }
        return CreateAssetResult::Ok;
    }
#endif

    bool WriteCompatibilityFlax(const StringView& path, const Guid& id, const StringView& typeName, const Array<byte>& surface, const StringAnsiView& propertiesJson, AssetPipelineDiagnostic& diagnostic, bool artifactStagingMode)
    {
#if COMPILE_WITH_ASSETS_IMPORTER
        GraphArtifactArguments arguments;
        arguments.Surface = &surface;
        arguments.TypeName = typeName;
        arguments.PropertiesJson = StringAnsi(propertiesJson.Get(), propertiesJson.Length());
        arguments.ID = id;
        CreateAssetContext importerContext(StringView::Empty, path, id, &arguments, artifactStagingMode, typeName);
        const CreateAssetResult result = importerContext.Run(&CreateGraphArtifact);
        if (result != CreateAssetResult::Ok)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, TEXT("Graph compatibility artifact writer failed."));
        return false;
#else
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, TEXT("Graph compatibility artifacts require the importer."));
#endif
    }

    ContentHash HashText(const StringAnsiView& text)
    {
        return ContentHash::Compute(text.Get(), text.Length());
    }

    void CollectGuid(const Variant& value, Array<Guid>& ids)
    {
        const Guid id = (Guid)value;
        if (id.IsValid())
            ids.Add(id);
        if (value.Type.Type == VariantType::Array)
        {
            const auto* items = reinterpret_cast<const Array<Variant>*>(value.AsData);
            if (items)
            {
                for (const Variant& item : *items)
                    CollectGuid(item, ids);
            }
        }
    }

    bool IsFunctionAssetType(const StringView& typeName)
    {
        return typeName == MaterialFunction::TypeName || typeName == AnimationGraphFunction::TypeName || IsParticleEmitterFunctionType(typeName);
    }

#if USE_EDITOR
    Dictionary<Guid, ArtifactLease> PreviewLeases;
    CriticalSection PreviewLocker;
#endif
}

StringAnsi GraphDocumentNode::GetStableID() const
{
    return HexToken("node-", LegacyID, 8);
}

StringAnsi GraphDocumentNode::GetStableType() const
{
    StringAnsi result = HexToken("group:", GroupID, 4);
    result += "/";
    result += HexToken("node:", TypeID, 4);
    return result;
}

bool GraphDocumentCodec::IsSupportedType(const StringView& typeName)
{
    return typeName == MaterialFunction::TypeName ||
        typeName == AnimationGraphFunction::TypeName ||
        typeName == AnimationGraph::TypeName ||
        IsVisualScriptType(typeName) ||
        IsBehaviorTreeType(typeName) ||
        IsParticleEmitterFunctionType(typeName) ||
        IsParticleEmitterType(typeName) ||
        IsMaterialType(typeName);
}

const Char* GraphDocumentCodec::ExtensionForType(const StringView& typeName)
{
    if (typeName == MaterialFunction::TypeName)
        return TEXT(".materialfunction");
    if (typeName == AnimationGraphFunction::TypeName)
        return TEXT(".animgraphfunction");
    if (typeName == AnimationGraph::TypeName)
        return TEXT(".animgraph");
    if (IsVisualScriptType(typeName))
        return TEXT(".visualscript");
    if (IsBehaviorTreeType(typeName))
        return TEXT(".behaviortree");
    if (IsParticleEmitterFunctionType(typeName))
        return TEXT(".particlefunction");
    if (IsParticleEmitterType(typeName))
        return TEXT(".particleemitter");
    if (IsMaterialType(typeName))
        return TEXT(".material");
    return nullptr;
}

bool GraphDocumentCodec::TypeForExtension(const StringView& extension, String& typeName)
{
    String value(extension);
    value.ToLower();
    if (value == TEXT("materialfunction") || value == TEXT(".materialfunction"))
        typeName = MaterialFunction::TypeName;
    else if (value == TEXT("animgraphfunction") || value == TEXT(".animgraphfunction"))
        typeName = AnimationGraphFunction::TypeName;
    else if (value == TEXT("animgraph") || value == TEXT(".animgraph"))
        typeName = AnimationGraph::TypeName;
    else if (value == TEXT("visualscript") || value == TEXT(".visualscript"))
        typeName = VisualScript::TypeName;
    else if (value == TEXT("behaviortree") || value == TEXT(".behaviortree"))
        typeName = TEXT("FlaxEngine.BehaviorTree");
    else if (value == TEXT("particlefunction") || value == TEXT(".particlefunction"))
        typeName = TEXT("FlaxEngine.ParticleEmitterFunction");
    else if (value == TEXT("particleemitter") || value == TEXT(".particleemitter"))
        typeName = TEXT("FlaxEngine.ParticleEmitter");
    else if (value == TEXT("material") || value == TEXT(".material"))
        typeName = Material::TypeName;
    else
        return true;
    return false;
}

bool GraphDocumentCodec::EncodeVariantJson(const Variant& value, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument json;
    json.SetObject();
    JsonValue encoded = EncodeVariant(value, json.GetAllocator());
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(encoded, output, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Variant canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::DecodeVariantJson(const StringAnsiView& source, Variant& value, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument json;
    json.Parse(source.Get(), source.Length());
    if (json.HasParseError())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Variant JSON is malformed."));
    return DecodeVariant(json, value, diagnostic);
}

bool GraphDocumentCodec::FromSurface(const StringView& typeName, const Span<byte>& surface, GraphDocument& document, AssetPipelineDiagnostic& diagnostic)
{
    document = GraphDocument();
    if (typeName.IsEmpty() || !surface.IsValid() || surface.Length() > MaximumSurfaceBytes)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph compatibility surface is missing or too large."));
    MemoryReadStream stream(surface.Get(), surface.Length());
    SurfaceGraph graph;
    if (graph.Load(&stream, true))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph compatibility surface is not a supported Visject 7000 payload."));

    document.TypeName = typeName;
    document.DocumentVersion = CurrentDocumentVersion;
    document.GraphVersion = CurrentGraphVersion;
    document.GraphMeta = graph.Meta;
    document.Nodes.EnsureCapacity(graph.Nodes.Count());
    HashSet<uint32> ids;
    for (int32 i = 0; i < graph.Nodes.Count(); i++)
    {
        const auto& source = graph.Nodes[i];
        if (source.ID == 0 || !ids.Add(source.ID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node identifiers must be non-zero and unique."));
        GraphDocumentNode node;
        node.LegacyID = source.ID;
        node.GroupID = source.GroupID;
        node.TypeID = source.TypeID;
        CopyVariants(node.Values, source.Values);
        node.Meta = source.Meta;
        ReadNodeLayout(node.Meta, node.PositionX, node.PositionY);
        Array<const SurfaceGraph::Box*, InlinedAllocation<32>> boxes;
        source.GetBoxes(boxes);
        for (const SurfaceGraph::Box* box : boxes)
        {
            GraphDocumentPin pin;
            pin.BoxID = box->ID;
            pin.Type = box->Type;
            node.Pins.Add(pin);
        }
        document.Nodes.Add(MoveTemp(node));
    }
    for (int32 i = 0; i < graph.Parameters.Count(); i++)
    {
        const auto& source = graph.Parameters[i];
        GraphDocumentParameter parameter;
        parameter.ID = source.Identifier;
        parameter.Name = source.Name;
        parameter.Type = source.Type;
        parameter.Default = source.Value;
        parameter.IsPublic = source.IsPublic;
        parameter.Meta = source.Meta;
        document.Parameters.Add(MoveTemp(parameter));
    }
    for (int32 i = 0; i < graph.Nodes.Count(); i++)
    {
        const auto& source = graph.Nodes[i];
        Array<const SurfaceGraph::Box*, InlinedAllocation<32>> boxes;
        source.GetBoxes(boxes);
        for (const SurfaceGraph::Box* box : boxes)
        {
            for (int32 k = 0; k < box->Connections.Count(); k++)
            {
                const GraphBox* target = box->Connections[k];
                if (!target)
                    continue;
                GraphDocumentConnection connection;
                connection.FromNode = source.ID;
                connection.FromPin = box->ID;
                connection.ToNode = target->GetParent<SurfaceGraph::Node>()->ID;
                connection.ToPin = target->ID;
                if (ConnectionLess(connection, GraphDocumentConnection{ connection.ToNode, connection.ToPin, connection.FromNode, connection.FromPin }))
                    document.Connections.Add(connection);
            }
        }
    }
    if (document.Connections.Count() > 1)
        std::sort(document.Connections.Get(), document.Connections.Get() + document.Connections.Count(), ConnectionLess);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::ToCanonicalJson(const GraphDocument& document, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    JsonDocument json;
    json.SetObject();
    JsonAlloc& allocator = json.GetAllocator();
    json.AddMember("documentVersion", document.DocumentVersion, allocator);
    json.AddMember("graphVersion", document.GraphVersion, allocator);
    AddString(json, "type", StringAnsi(document.TypeName), allocator);

    JsonValue propertiesValue(rapidjson::kObjectType);
    JsonDocument properties;
    properties.Parse(document.PropertiesJson.Get(), document.PropertiesJson.Length());
    if (!properties.HasParseError() && properties.IsObject())
        propertiesValue.CopyFrom(properties, allocator);
    json.AddMember("properties", propertiesValue, allocator);

    JsonValue parameters(rapidjson::kObjectType);
    for (const GraphDocumentParameter& parameter : document.Parameters)
    {
        JsonValue item(rapidjson::kObjectType);
        AddString(item, "name", StringAnsi(parameter.Name), allocator);
        item.AddMember("type", EncodeVariantType(parameter.Type, allocator), allocator);
        item.AddMember("default", EncodeVariant(parameter.Default, allocator), allocator);
        AddBool(item, "isPublic", parameter.IsPublic, allocator);
        item.AddMember("visjectMeta", EncodeMeta(parameter.Meta, allocator), allocator);
        parameters.AddMember(MakeString(GuidToken(parameter.ID), allocator), item, allocator);
    }
    json.AddMember("parameters", parameters, allocator);

    JsonValue graph(rapidjson::kObjectType);
    JsonValue nodes(rapidjson::kObjectType);
    for (const GraphDocumentNode& node : document.Nodes)
    {
        JsonValue item(rapidjson::kObjectType);
        AddString(item, "type", node.GetStableType(), allocator);
        AddInt(item, "typeVersion", node.TypeVersion, allocator);
        float position[2] = { node.PositionX, node.PositionY };
        item.AddMember("position", MakeNumberArray(position, 2, allocator), allocator);
        JsonValue values(rapidjson::kArrayType);
        for (const Variant& value : node.Values)
            values.PushBack(EncodeVariant(value, allocator), allocator);
        item.AddMember("values", values, allocator);
        JsonValue pins(rapidjson::kArrayType);
        for (const GraphDocumentPin& pin : node.Pins)
        {
            JsonValue pinJson(rapidjson::kObjectType);
            AddString(pinJson, "id", PinToken(pin.BoxID), allocator);
            pinJson.AddMember("type", EncodeVariantType(pin.Type, allocator), allocator);
            pins.PushBack(pinJson, allocator);
        }
        item.AddMember("pins", pins, allocator);
        JsonValue custom(rapidjson::kObjectType);
        JsonDocument parsedCustom;
        parsedCustom.Parse(node.CustomJson.Get(), node.CustomJson.Length());
        if (!parsedCustom.HasParseError() && parsedCustom.IsObject())
            custom.CopyFrom(parsedCustom, allocator);
        if (custom.HasMember("legacyId"))
            custom.RemoveMember("legacyId");
        if (custom.HasMember("visjectMeta"))
            custom.RemoveMember("visjectMeta");
        if (custom.HasMember("unknown"))
            custom.RemoveMember("unknown");
        custom.AddMember("legacyId", JsonValue(node.LegacyID), allocator);
        custom.AddMember("visjectMeta", EncodeMeta(node.Meta, allocator), allocator);
        if (node.Unknown)
            AddBool(custom, "unknown", true, allocator);
        item.AddMember("custom", custom, allocator);
        nodes.AddMember(MakeString(node.GetStableID(), allocator), item, allocator);
    }
    graph.AddMember("nodes", nodes, allocator);

    JsonValue connections(rapidjson::kArrayType);
    Array<GraphDocumentConnection> sorted = document.Connections;
    if (sorted.Count() > 1)
        std::sort(sorted.Get(), sorted.Get() + sorted.Count(), ConnectionLess);
    for (const GraphDocumentConnection& connection : sorted)
    {
        JsonValue item(rapidjson::kObjectType);
        JsonValue from(rapidjson::kObjectType);
        AddString(from, "node", HexToken("node-", connection.FromNode, 8), allocator);
        AddString(from, "pin", PinToken(connection.FromPin), allocator);
        JsonValue to(rapidjson::kObjectType);
        AddString(to, "node", HexToken("node-", connection.ToNode, 8), allocator);
        AddString(to, "pin", PinToken(connection.ToPin), allocator);
        item.AddMember("from", from, allocator);
        item.AddMember("to", to, allocator);
        connections.PushBack(item, allocator);
    }
    graph.AddMember("connections", connections, allocator);

    JsonValue editorValue(rapidjson::kObjectType);
    JsonDocument editor;
    editor.Parse(document.EditorJson.Get(), document.EditorJson.Length());
    if (!editor.HasParseError() && editor.IsObject())
        editorValue.CopyFrom(editor, allocator);
    if (editorValue.HasMember("visjectMeta"))
        editorValue.RemoveMember("visjectMeta");
    editorValue.AddMember("visjectMeta", EncodeMeta(document.GraphMeta, allocator), allocator);
    graph.AddMember("editor", editorValue, allocator);
    json.AddMember("graph", graph, allocator);

    Array<StringAnsi> rootOrder;
    rootOrder.Add("documentVersion");
    rootOrder.Add("graphVersion");
    rootOrder.Add("type");
    rootOrder.Add("properties");
    rootOrder.Add("parameters");
    rootOrder.Add("graph");
    Dictionary<StringAnsi, Array<StringAnsi>> objectOrders;
    Array<StringAnsi> graphOrder;
    graphOrder.Add("nodes");
    graphOrder.Add("connections");
    graphOrder.Add("editor");
    objectOrders.Add("graph", graphOrder);
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(json, output, error, &rootOrder, &objectOrders))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document canonical serialization failed."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::Encode(const StringView& typeName, const Span<byte>& surface, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    GraphDocument document;
    return FromSurface(typeName, surface, document, diagnostic) || ToCanonicalJson(document, output, diagnostic);
}

bool GraphDocumentCodec::CreateStarter(const StringView& typeName, GraphDocument& document, AssetPipelineDiagnostic& diagnostic)
{
    document = GraphDocument();
    SurfaceGraph graph;
    if (typeName == MaterialFunction::TypeName || typeName == AnimationGraphFunction::TypeName || IsParticleEmitterFunctionType(typeName))
    {
        auto& outputNode = graph.Nodes.AddOne();
        outputNode.ID = 1;
        outputNode.Type = GRAPH_NODE_MAKE_TYPE(FunctionGroupId, FunctionOutputType);
        outputNode.Values.Resize(2);
        outputNode.Values[0] = TEXT("System.Single");
        outputNode.Values[1] = TEXT("Output");
        auto& outputBox = outputNode.Boxes.AddOne();
        outputBox.Parent = &outputNode;
        outputBox.ID = 0;
        outputBox.Type = VariantType::Float;
    }
    else if (IsParticleEmitterType(typeName))
    {
        ParticleEmitterGraphCPU particles;
        particles.CreateDefault();
        MemoryWriteStream stream(512);
        if (particles.Save(&stream, true))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Starter particle emitter serialization failed."));
        if (FromSurface(typeName, Span<byte>(stream.GetHandle(), static_cast<int32>(stream.GetPosition())), document, diagnostic))
            return true;
        return false;
    }
    else if (typeName == AnimationGraph::TypeName)
    {
        graph.Nodes.Resize(1);
        auto& rootNode = graph.Nodes[0];
        rootNode.Type = GRAPH_NODE_MAKE_TYPE(9, 1);
        rootNode.ID = 1;
        rootNode.Values.Resize(1);
        rootNode.Values[0] = 0;
        rootNode.Boxes.Resize(1);
        rootNode.Boxes[0] = VisjectGraphBox(&rootNode, 0, VariantType::Void);
        graph.Parameters.Resize(1);
        graph.Parameters[0].Identifier = Guid(1000, 0, 0, 0);
        graph.Parameters[0].Type = VariantType::Asset;
        graph.Parameters[0].IsPublic = false;
        graph.Parameters[0].Value = Guid::Empty;
    }
    else if (IsMaterialType(typeName))
    {
        auto& rootNode = graph.Nodes.AddOne();
        rootNode.ID = 1;
        rootNode.Type = GRAPH_NODE_MAKE_TYPE(1, 1);
        rootNode.Boxes.Resize(15);
        const VariantType::Types boxTypes[] = {
            VariantType::Void, VariantType::Float3, VariantType::Float, VariantType::Float3, VariantType::Float,
            VariantType::Float, VariantType::Float, VariantType::Float, VariantType::Float3, VariantType::Float,
            VariantType::Float, VariantType::Float3, VariantType::Float, VariantType::Float3, VariantType::Float3
        };
        for (int32 i = 0; i < 15; i++)
            rootNode.Boxes[i] = VisjectGraphBox(&rootNode, static_cast<byte>(i), boxTypes[i]);
    }
    else if (IsVisualScriptType(typeName) || IsBehaviorTreeType(typeName))
    {
        // Empty Visject surface. Visual Script metadata lives in PropertiesJson.
    }
    else
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Unsupported graph document type."));
    MemoryWriteStream stream(512);
    if (IsVisualScriptType(typeName))
    {
        VisualScriptGraph visualScript;
        if (visualScript.Save(&stream, true))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Starter graph serialization failed."));
    }
    else if (graph.Save(&stream, true))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Starter graph serialization failed."));
    if (FromSurface(typeName, Span<byte>(stream.GetHandle(), static_cast<int32>(stream.GetPosition())), document, diagnostic))
        return true;
    if (IsVisualScriptType(typeName))
        document.PropertiesJson = MakeVisualScriptPropertiesJson(TEXT("FlaxEngine.Script"), 0);
    else if (IsMaterialType(typeName))
        document.PropertiesJson = MakeMaterialPropertiesJson();
    return false;
}

bool GraphDocumentCodec::WriteCompatibilityAsset(const StringView& path, const Guid& id, const StringView& typeName, const Array<byte>& surface, const StringAnsiView& propertiesJson, AssetPipelineDiagnostic& diagnostic, bool artifactStagingMode)
{
    return WriteCompatibilityFlax(path, id, typeName, surface, propertiesJson, diagnostic, artifactStagingMode);
}

bool GraphDocumentCompiler::CompileDocument(const GraphDocument& document, Array<byte>& output, AssetPipelineDiagnostic& diagnostic)
{
    output.Clear();
    if (GraphDocumentValidator::ValidateDocument(document, diagnostic))
        return true;
    SurfaceGraph graph;
    graph.Nodes.Resize(document.Nodes.Count());
    for (int32 i = 0; i < document.Nodes.Count(); i++)
    {
        const GraphDocumentNode& source = document.Nodes[i];
        auto& node = graph.Nodes[i];
        node.ID = source.LegacyID;
        node.GroupID = source.GroupID;
        node.TypeID = source.TypeID;
        CopyVariants(node.Values, source.Values);
        node.Meta = source.Meta;
        WriteNodeLayout(node.Meta, source.PositionX, source.PositionY);
        int32 maxBox = 0;
        for (const GraphDocumentPin& pin : source.Pins)
            maxBox = Math::Max(maxBox, static_cast<int32>(pin.BoxID) + 1);
        node.Boxes.Resize(maxBox);
        for (const GraphDocumentPin& pin : source.Pins)
        {
            GraphBox& box = node.Boxes[pin.BoxID];
            box.Parent = &node;
            box.ID = pin.BoxID;
            box.Type = pin.Type;
        }
    }
    graph.Parameters.Resize(document.Parameters.Count());
    for (int32 i = 0; i < document.Parameters.Count(); i++)
    {
        const GraphDocumentParameter& source = document.Parameters[i];
        auto& parameter = graph.Parameters[i];
        parameter.Identifier = source.ID;
        parameter.Name = source.Name;
        parameter.Type = source.Type;
        parameter.Value = source.Default;
        parameter.IsPublic = source.IsPublic;
        parameter.Meta = source.Meta;
    }
    graph.Meta = document.GraphMeta;
    for (const GraphDocumentConnection& connection : document.Connections)
    {
        SurfaceGraph::Node* fromNode = graph.GetNode(connection.FromNode);
        SurfaceGraph::Node* toNode = graph.GetNode(connection.ToNode);
        GraphBox* from = fromNode ? fromNode->TryGetBox(connection.FromPin) : nullptr;
        GraphBox* to = toNode ? toNode->TryGetBox(connection.ToPin) : nullptr;
        if (!from || !to)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph connection endpoint is missing during compilation."));
        if (!from->Connections.Contains(to))
            from->Connections.Add(to);
        if (!to->Connections.Contains(from))
            to->Connections.Add(from);
    }
    MemoryWriteStream stream(1024);
    if (graph.Save(&stream, true))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build, TEXT("Graph compatibility compilation failed."));
    output.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentValidator::ValidateDocument(const GraphDocument& document, AssetPipelineDiagnostic& diagnostic)
{
    if (!GraphDocumentCodec::IsSupportedType(document.TypeName))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document type is not supported."));
    if (document.DocumentVersion != GraphDocumentCodec::CurrentDocumentVersion || document.GraphVersion != GraphDocumentCodec::CurrentGraphVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("Graph document requires an explicit tracked migration."));
    HashSet<uint32> nodeIds;
    for (const GraphDocumentNode& node : document.Nodes)
    {
        if (node.LegacyID == 0 || !nodeIds.Add(node.LegacyID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node identifiers must be non-zero and unique."));
        HashSet<int32> pins;
        for (const GraphDocumentPin& pin : node.Pins)
        {
            if (!pins.Add(pin.BoxID))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node pins must be unique."));
        }
    }
    for (int32 i = 0; i < document.Parameters.Count(); i++)
    {
        if (!document.Parameters[i].ID.IsValid())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph parameter identifiers must be unique."));
        for (int32 j = i + 1; j < document.Parameters.Count(); j++)
        {
            if (document.Parameters[i].ID == document.Parameters[j].ID)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph parameter identifiers must be unique."));
        }
    }
    auto findNode = [&](uint32 id) -> const GraphDocumentNode*
    {
        for (const GraphDocumentNode& node : document.Nodes)
        {
            if (node.LegacyID == id)
                return &node;
        }
        return nullptr;
    };
    auto hasPin = [](const GraphDocumentNode& node, byte pinId)
    {
        for (const GraphDocumentPin& pin : node.Pins)
        {
            if (pin.BoxID == pinId)
                return true;
        }
        return false;
    };
    for (const GraphDocumentConnection& connection : document.Connections)
    {
        const GraphDocumentNode* from = findNode(connection.FromNode);
        const GraphDocumentNode* to = findNode(connection.ToNode);
        if (!from || !to || !hasPin(*from, connection.FromPin) || !hasPin(*to, connection.ToPin))
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.ProcessorId = TEXT("Flax.GraphDocument");
            diagnostic.Message = TEXT("Graph connection endpoint is dangling.");
            diagnostic.Location.GraphNode = String(HexToken("node-", connection.FromNode, 8));
            diagnostic.Location.GraphPin = String(PinToken(connection.FromPin));
            return true;
        }
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDependencyExtractor::Extract(const GraphDocument& document, Array<AssetDependency>& dependencies, ContentHash& functionInterfaceHash, AssetPipelineDiagnostic& diagnostic)
{
    dependencies.Clear();
    MemoryWriteStream interfaceStream(256);
    for (const GraphDocumentNode& node : document.Nodes)
    {
        if (node.GroupID == FunctionGroupId && (node.TypeID == FunctionInputType || node.TypeID == FunctionOutputType))
        {
            interfaceStream.WriteUint16(node.TypeID);
            interfaceStream.WriteInt32(node.Values.Count());
            for (const Variant& value : node.Values)
                interfaceStream.Write(value);
        }
        Array<Guid> referenced;
        for (const Variant& value : node.Values)
            CollectGuid(value, referenced);
        for (const Guid& id : referenced)
        {
            AssetDependency dependency;
            dependency.Origin.GraphNode = String(node.GetStableID());
            AssetRecord record;
            const bool hasRecord = AssetDatabase::Get().TryGetRecord(id, record);
            dependency.ObjectID = hasRecord
                ? AssetObjectId(AssetGuid(record.SourceAssetID), record.LocalId)
                : AssetObjectId::Main(AssetGuid(id));
            dependency.StableIdentity = dependency.ObjectID.ToString();
            if (hasRecord && IsFunctionAssetType(record.TypeName))
            {
                dependency.Kind = AssetDependencyKind::BuildInput;
                dependency.SemanticInterface = record.MetaSemanticHash != 0
                    ? ContentHash::Compute(&record.MetaSemanticHash, sizeof(record.MetaSemanticHash))
                    : ContentHash();
            }
            else
                dependency.Kind = AssetDependencyKind::RuntimeReference;
            dependencies.Add(MoveTemp(dependency));
        }
        if (node.Unknown)
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Severity = AssetPipelineDiagnosticSeverity::Warning;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.ProcessorId = TEXT("Flax.GraphDocument");
            diagnostic.Message = TEXT("Graph contains an unknown or plugin node. It is preserved but compilation may be partial.");
            diagnostic.Location.GraphNode = String(node.GetStableID());
        }
    }
    for (const GraphDocumentParameter& parameter : document.Parameters)
    {
        Array<Guid> referenced;
        CollectGuid(parameter.Default, referenced);
        for (const Guid& id : referenced)
        {
            AssetDependency dependency;
            dependency.Kind = AssetDependencyKind::RuntimeReference;
            AssetRecord record;
            dependency.ObjectID = AssetDatabase::Get().TryGetRecord(id, record)
                ? AssetObjectId(AssetGuid(record.SourceAssetID), record.LocalId)
                : AssetObjectId::Main(AssetGuid(id));
            dependency.StableIdentity = dependency.ObjectID.ToString();
            dependency.Origin.GraphPin = String(GuidToken(parameter.ID));
            dependencies.Add(MoveTemp(dependency));
        }
    }
    functionInterfaceHash = ContentHash::Compute(interfaceStream.GetHandle(), interfaceStream.GetPosition());
    if (diagnostic.Severity != AssetPipelineDiagnosticSeverity::Warning)
        diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::DecodeGraph(const StringAnsiView& source, GraphDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    snapshot = GraphDocumentSnapshot();
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(source, snapshot.CanonicalText, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaParseError, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is not valid canonicalizable JSON."));
    JsonDocument json;
    json.Parse(snapshot.CanonicalText.Get(), snapshot.CanonicalText.Length());
    if (json.HasParseError() || !json.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MetaParseError, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document root must be an object."));
    const auto documentVersion = json.FindMember("documentVersion");
    const auto graphVersion = json.FindMember("graphVersion");
    const auto type = json.FindMember("type");
    const auto properties = json.FindMember("properties");
    const auto parameters = json.FindMember("parameters");
    const auto graph = json.FindMember("graph");
    if (documentVersion == json.MemberEnd() || !documentVersion->value.IsInt() ||
        graphVersion == json.MemberEnd() || !graphVersion->value.IsInt() ||
        type == json.MemberEnd() || !type->value.IsString() || type->value.GetStringLength() == 0 ||
        properties == json.MemberEnd() || !properties->value.IsObject() ||
        parameters == json.MemberEnd() || !parameters->value.IsObject() ||
        graph == json.MemberEnd() || !graph->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is missing required typed fields."));
    snapshot.DocumentVersion = documentVersion->value.GetInt();
    snapshot.GraphVersion = graphVersion->value.GetInt();
    snapshot.TypeName = String(StringAnsiView(type->value.GetString(), type->value.GetStringLength()));
    snapshot.Document.TypeName = snapshot.TypeName;
    snapshot.Document.DocumentVersion = snapshot.DocumentVersion;
    snapshot.Document.GraphVersion = snapshot.GraphVersion;
    if (snapshot.DocumentVersion != CurrentDocumentVersion || snapshot.GraphVersion != CurrentGraphVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("Graph document requires an explicit tracked migration."));
    CanonicalJsonError propertiesError;
    if (CanonicalJsonWriter::Write(properties->value, snapshot.Document.PropertiesJson, propertiesError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph properties are not canonical JSON."));

    for (auto i = parameters->value.MemberBegin(); i != parameters->value.MemberEnd(); ++i)
    {
        if (!i->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph parameters must be objects."));
        GraphDocumentParameter parameter;
        if (ParseGuidToken(StringAnsiView(i->name.GetString(), i->name.GetStringLength()), parameter.ID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph parameter keys must be GUIDs."));
        const auto name = i->value.FindMember("name");
        const auto typeJson = i->value.FindMember("type");
        const auto defaultValue = i->value.FindMember("default");
        if (name == i->value.MemberEnd() || !name->value.IsString() || typeJson == i->value.MemberEnd())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph parameter is missing name or type."));
        parameter.Name = String(name->value.GetString(), name->value.GetStringLength());
        if (DecodeVariantType(typeJson->value, parameter.Type, diagnostic))
            return true;
        if (defaultValue != i->value.MemberEnd() && DecodeVariant(defaultValue->value, parameter.Default, diagnostic))
            return true;
        const auto isPublic = i->value.FindMember("isPublic");
        parameter.IsPublic = isPublic == i->value.MemberEnd() || (isPublic->value.IsBool() && isPublic->value.GetBool());
        const auto meta = i->value.FindMember("visjectMeta");
        if (meta != i->value.MemberEnd() && DecodeMeta(meta->value, parameter.Meta, diagnostic))
            return true;
        snapshot.Document.Parameters.Add(MoveTemp(parameter));
    }

    const auto nodes = graph->value.FindMember("nodes");
    const auto connections = graph->value.FindMember("connections");
    const auto editor = graph->value.FindMember("editor");
    if (nodes == graph->value.MemberEnd() || !nodes->value.IsObject() ||
        connections == graph->value.MemberEnd() || !connections->value.IsArray() ||
        editor == graph->value.MemberEnd() || !editor->value.IsObject())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document graph shape is invalid."));
    CanonicalJsonError editorError;
    if (CanonicalJsonWriter::Write(editor->value, snapshot.Document.EditorJson, editorError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph editor data is not canonical JSON."));
    const auto graphMeta = editor->value.FindMember("visjectMeta");
    if (graphMeta != editor->value.MemberEnd() && DecodeMeta(graphMeta->value, snapshot.Document.GraphMeta, diagnostic))
        return true;

    for (auto i = nodes->value.MemberBegin(); i != nodes->value.MemberEnd(); ++i)
    {
        if (!i->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph nodes must be objects."));
        GraphDocumentNode node;
        const StringAnsiView nodeId(i->name.GetString(), i->name.GetStringLength());
        const auto typeJson = i->value.FindMember("type");
        const auto typeVersion = i->value.FindMember("typeVersion");
        const auto position = i->value.FindMember("position");
        const auto values = i->value.FindMember("values");
        if (typeJson == i->value.MemberEnd() || !typeJson->value.IsString() ||
            typeVersion == i->value.MemberEnd() || !typeVersion->value.IsInt() ||
            position == i->value.MemberEnd() || !position->value.IsArray() || position->value.Size() != 2 ||
            values == i->value.MemberEnd() || !values->value.IsArray())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node is missing required fields."));
        node.TypeVersion = typeVersion->value.GetInt();
        node.PositionX = static_cast<float>(position->value[0].GetDouble());
        node.PositionY = static_cast<float>(position->value[1].GetDouble());
        const auto custom = i->value.FindMember("custom");
        if (custom != i->value.MemberEnd() && custom->value.IsObject())
        {
            CanonicalJsonError customError;
            if (CanonicalJsonWriter::Write(custom->value, node.CustomJson, customError))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node custom data is not canonical JSON."));
            const auto legacyId = custom->value.FindMember("legacyId");
            if (legacyId != custom->value.MemberEnd() && legacyId->value.IsUint())
                node.LegacyID = legacyId->value.GetUint();
            const auto unknown = custom->value.FindMember("unknown");
            node.Unknown = unknown != custom->value.MemberEnd() && unknown->value.IsBool() && unknown->value.GetBool();
            const auto meta = custom->value.FindMember("visjectMeta");
            if (meta != custom->value.MemberEnd() && DecodeMeta(meta->value, node.Meta, diagnostic))
                return true;
        }
        if (node.LegacyID == 0 && nodeId.Length() == 13 && nodeId.StartsWith("node-"))
        {
            uint32 parsed = 0;
            for (int32 c = 5; c < nodeId.Length(); c++)
            {
                const char ch = nodeId[c];
                parsed <<= 4;
                if (ch >= '0' && ch <= '9')
                    parsed |= static_cast<uint32>(ch - '0');
                else if (ch >= 'a' && ch <= 'f')
                    parsed |= static_cast<uint32>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F')
                    parsed |= static_cast<uint32>(ch - 'A' + 10);
            }
            node.LegacyID = parsed;
        }
        const StringAnsiView typeToken(typeJson->value.GetString(), typeJson->value.GetStringLength());
        if (typeToken.Length() == 20 && typeToken.StartsWith("group:") && StringAnsiView(typeToken.Get() + 10, 6) == "/node:")
        {
            auto parseHex = [](const char* text, int32 length, uint16& result)
            {
                uint32 value = 0;
                for (int32 i = 0; i < length; i++)
                {
                    const char ch = text[i];
                    value <<= 4;
                    if (ch >= '0' && ch <= '9')
                        value |= static_cast<uint32>(ch - '0');
                    else if (ch >= 'a' && ch <= 'f')
                        value |= static_cast<uint32>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F')
                        value |= static_cast<uint32>(ch - 'A' + 10);
                    else
                        return true;
                }
                result = static_cast<uint16>(value);
                return false;
            };
            if (parseHex(typeToken.Get() + 6, 4, node.GroupID) || parseHex(typeToken.Get() + 16, 4, node.TypeID))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph node type token is invalid."));
        }
        else
            node.Unknown = true;
        for (rapidjson::SizeType v = 0; v < values->value.Size(); v++)
        {
            Variant decoded;
            if (DecodeVariant(values->value[v], decoded, diagnostic))
                return true;
            node.Values.Add(MoveTemp(decoded));
        }
        const auto pins = i->value.FindMember("pins");
        if (pins != i->value.MemberEnd() && pins->value.IsArray())
        {
            for (rapidjson::SizeType p = 0; p < pins->value.Size(); p++)
            {
                const JsonValue& pinJson = pins->value[p];
                if (!pinJson.IsObject())
                    continue;
                GraphDocumentPin pin;
                const auto pinId = pinJson.FindMember("id");
                const auto pinType = pinJson.FindMember("type");
                if (pinId == pinJson.MemberEnd() || !pinId->value.IsString() || ParsePinToken(StringAnsiView(pinId->value.GetString(), pinId->value.GetStringLength()), pin.BoxID))
                    return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph pin identifiers must use the box:{id} mapping."));
                if (pinType != pinJson.MemberEnd() && DecodeVariantType(pinType->value, pin.Type, diagnostic))
                    return true;
                node.Pins.Add(pin);
            }
        }
        snapshot.Document.Nodes.Add(MoveTemp(node));
    }

    for (rapidjson::SizeType i = 0; i < connections->value.Size(); i++)
    {
        const JsonValue& item = connections->value[i];
        if (!item.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph connections must be objects."));
        const auto from = item.FindMember("from");
        const auto to = item.FindMember("to");
        if (from == item.MemberEnd() || !from->value.IsObject() || to == item.MemberEnd() || !to->value.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph connections require from and to endpoints."));
        auto parseEndpoint = [&](const JsonValue& endpoint, uint32& nodeId, byte& pinId) -> bool
        {
            const auto node = endpoint.FindMember("node");
            const auto pin = endpoint.FindMember("pin");
            if (node == endpoint.MemberEnd() || !node->value.IsString() || pin == endpoint.MemberEnd() || !pin->value.IsString())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph connection endpoints require node and pin."));
            const StringAnsiView nodeToken(node->value.GetString(), node->value.GetStringLength());
            nodeId = 0;
            if (nodeToken.StartsWith("node-"))
            {
                for (int32 c = 5; c < nodeToken.Length(); c++)
                {
                    const char ch = nodeToken[c];
                    nodeId <<= 4;
                    if (ch >= '0' && ch <= '9')
                        nodeId |= static_cast<uint32>(ch - '0');
                    else if (ch >= 'a' && ch <= 'f')
                        nodeId |= static_cast<uint32>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F')
                        nodeId |= static_cast<uint32>(ch - 'A' + 10);
                }
            }
            return ParsePinToken(StringAnsiView(pin->value.GetString(), pin->value.GetStringLength()), pinId)
                ? Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph connection pins must use the box:{id} mapping."))
                : false;
        };
        GraphDocumentConnection connection;
        if (parseEndpoint(from->value, connection.FromNode, connection.FromPin) || parseEndpoint(to->value, connection.ToNode, connection.ToPin))
            return true;
        snapshot.Document.Connections.Add(connection);
    }

    AssetPipelineDiagnostic warning;
    if (GraphDocumentValidator::ValidateDocument(snapshot.Document, diagnostic))
        return true;
    if (GraphDocumentCompiler::CompileDocument(snapshot.Document, snapshot.CompatibilitySurface, diagnostic))
        return true;
    if (GraphDependencyExtractor::Extract(snapshot.Document, snapshot.Dependencies, snapshot.FunctionInterfaceHash, diagnostic))
        return true;
    if (diagnostic.Severity == AssetPipelineDiagnosticSeverity::Warning)
        warning = diagnostic;
    snapshot.FullHash = HashText(snapshot.CanonicalText);
    GraphDocument semantic = snapshot.Document;
    for (GraphDocumentNode& node : semantic.Nodes)
    {
        node.PositionX = 0.0f;
        node.PositionY = 0.0f;
        if (VisjectMeta::Entry* layout = node.Meta.GetEntry(11))
            layout->Data.Clear();
    }
    semantic.EditorJson = "{}\n";
    semantic.GraphMeta.Release();
    StringAnsi semanticJson;
    if (ToCanonicalJson(semantic, semanticJson, diagnostic))
        return true;
    snapshot.SemanticHash = HashText(semanticJson);
    diagnostic = warning.Severity == AssetPipelineDiagnosticSeverity::Warning ? warning : AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCodec::Decode(const StringAnsiView& source, AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentSnapshot graph;
    if (DecodeGraph(source, graph, diagnostic))
        return true;
    snapshot.TypeName = graph.TypeName;
    snapshot.DocumentVersion = graph.DocumentVersion;
    snapshot.CanonicalText = MoveTemp(graph.CanonicalText);
    snapshot.FullHash = graph.FullHash;
    snapshot.SemanticHash = graph.SemanticHash;
    snapshot.Dependencies = MoveTemp(graph.Dependencies);
    return false;
}

bool GraphDocumentCodec::SaveAtomic(const StringView& path, const StringAnsiView& canonicalText, AssetPipelineDiagnostic& diagnostic, ContentHash* previousHash)
{
    GraphDocumentCodec codec;
    GraphDocumentSnapshot reparsed;
    if (codec.DecodeGraph(canonicalText, reparsed, diagnostic))
        return true;
    return SaveJsonAtomic(path, canonicalText, diagnostic, previousHash);
}

bool GraphDocumentCodec::SaveJsonAtomic(const StringView& path, const StringAnsiView& canonicalText, AssetPipelineDiagnostic& diagnostic, ContentHash* previousHash)
{
    StringAnsi reparsed;
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Canonicalize(canonicalText, reparsed, jsonError))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Authored document is not valid JSON."));
    if (previousHash && !previousHash->IsZero() && FileSystem::FileExists(path))
    {
        Array<byte> existing;
        if (!File::ReadAllBytes(path, existing))
        {
            const ContentHash current = ContentHash::Compute(existing.Get(), existing.Count());
            if (current != *previousHash)
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceBusy, AssetPipelineDiagnosticStage::DatabaseScan, TEXT("Graph document changed externally and will not be overwritten."));
        }
    }
    const String staging = String(path) + TEXT(".stage-") + Guid::New().ToString(Guid::FormatType::N);
    SCOPE_EXIT { FileSystem::DeleteFile(staging); };
    if (File::WriteAllBytes(staging, canonicalText.Get(), canonicalText.Length()) || FlushWrittenFile(staging))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Cannot write or flush authored document staging file."));
    if (FileSystem::FileExists(path) && FileSystem::IsReadOnly(path))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Authored document is read-only."));
    if (AtomicReplace(path, staging))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Cannot atomically replace authored document."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentValidator::Validate(const AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentCodec codec;
    GraphDocumentSnapshot graph;
    if (snapshot.CanonicalText.IsEmpty() || codec.DecodeGraph(snapshot.CanonicalText, graph, diagnostic))
        return true;
    if (graph.TypeName != snapshot.TypeName || graph.DocumentVersion != snapshot.DocumentVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph snapshot identity differs from its canonical bytes."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentMigrator::Migrate(const AssetDocumentSnapshot& source, int32 targetVersion, StringAnsi& canonicalText, AssetPipelineDiagnostic& diagnostic) const
{
    if (source.DocumentVersion > targetVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("Newer unsupported graph documents are never downgraded."));
    if (source.DocumentVersion != targetVersion || targetVersion != GraphDocumentCodec::CurrentDocumentVersion)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::MigrationFailed, AssetPipelineDiagnosticStage::Migration, TEXT("No ordered graph migration is registered for the requested version range."));
    canonicalText = source.CanonicalText;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentCompiler::Compile(const AssetDocumentSnapshot& snapshot, Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const
{
    GraphDocumentCodec codec;
    GraphDocumentSnapshot graph;
    if (codec.DecodeGraph(snapshot.CanonicalText, graph, diagnostic))
        return true;
    output = MoveTemp(graph.CompatibilitySurface);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

BytesContainer AssetDocumentService::LoadGraphSource(const StringView& path)
{
    BytesContainer result;
    AssetPipelineDiagnostic diagnostic;
    GraphDocumentSession session;
    if (session.Open(path, diagnostic))
    {
        LOG(Error, "Cannot open graph source '{0}': {1}", path, diagnostic.Message);
        return result;
    }
    Array<byte> surface;
    if (GraphDocumentCompiler::CompileDocument(session.Document, surface, diagnostic))
    {
        LOG(Error, "Cannot compile editable graph source '{0}': {1}", path, diagnostic.Message);
        return result;
    }
    result.Copy(ToSpan(surface));
    return result;
}

bool AssetDocumentService::SaveGraphSource(const StringView& path, const BytesContainer& surface,
    const StringView& expectedSourceHash, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    String typeName;
    if (GraphDocumentCodec::TypeForExtension(FileSystem::GetExtension(path), typeName))
    {
        LOG(Error, "Cannot save graph source '{0}': unsupported source extension.", path);
        return true;
    }
    GraphDocument document;
    if (GraphDocumentCodec::FromSurface(typeName, surface, document, diagnostic))
    {
        LOG(Error, "Cannot serialize graph source '{0}': {1}", path, diagnostic.Message);
        return true;
    }
    GraphDocumentSession current;
    if (current.Open(path, diagnostic))
    {
        LOG(Error, "Cannot open graph source before save '{0}': {1}", path, diagnostic.Message);
        return true;
    }
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    else
        document.PropertiesJson = current.Document.PropertiesJson;

    ContentHash expected;
    if (expectedSourceHash.HasChars() && ContentHash::Parse(expectedSourceHash, expected))
    {
        LOG(Error, "Cannot save graph source '{0}': invalid expected source hash.", path);
        return true;
    }
    StringAnsi json;
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic) ||
        GraphDocumentCodec::SaveAtomic(path, json, diagnostic, expectedSourceHash.HasChars() ? &expected : nullptr))
    {
        LOG(Error, "Cannot save graph source '{0}': {1}", path, diagnostic.Message);
        return true;
    }
    return false;
}

Guid AssetDocumentService::CreateGraphSource(const StringView& path, const StringView& typeName,
    const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    String extensionType;
    if (path.IsEmpty() || !GraphDocumentCodec::IsSupportedType(typeName) ||
        GraphDocumentCodec::TypeForExtension(FileSystem::GetExtension(path), extensionType) || extensionType != typeName ||
        FileSystem::FileExists(path) || FileSystem::FileExists(String(path) + TEXT(".meta")))
    {
        LOG(Error, "Cannot create graph source '{0}': path, type, or destination is invalid.", path);
        return Guid::Empty;
    }
    GraphDocument document;
    if (GraphDocumentCodec::CreateStarter(typeName, document, diagnostic))
    {
        LOG(Error, "Cannot create starter graph source '{0}': {1}", path, diagnostic.Message);
        return Guid::Empty;
    }
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    StringAnsi json;
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic) || GraphDocumentCodec::SaveAtomic(path, json, diagnostic))
    {
        LOG(Error, "Cannot write starter graph source '{0}': {1}", path, diagnostic.Message);
        return Guid::Empty;
    }
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = TEXT("Flax.GraphDocument");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(String(path) + TEXT(".meta"), meta, diagnostic))
    {
        FileSystem::DeleteFile(path);
        LOG(Error, "Cannot create graph source metadata '{0}': {1}", path, diagnostic.Message);
        return Guid::Empty;
    }
    return meta.ID;
}

bool GraphDocumentSession::Open(const StringView& path, AssetPipelineDiagnostic& diagnostic)
{
    Path = path;
    Array<byte> bytes;
    if (File::ReadAllBytes(path, bytes))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is missing."));
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    const StringAnsiView source(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    if (codec.DecodeGraph(source, snapshot, diagnostic))
        return true;
    TypeName = snapshot.TypeName;
    Document = MoveTemp(snapshot.Document);
    LoadedHash = snapshot.FullHash;
    Dirty = false;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentSession::HasExternalChange(AssetPipelineDiagnostic& diagnostic) const
{
    Array<byte> bytes;
    if (File::ReadAllBytes(Path, bytes))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare, TEXT("Graph document is missing."));
    return ContentHash::Compute(bytes.Get(), bytes.Count()) != LoadedHash;
}

bool GraphDocumentSession::Save(bool allowOverwriteConflict, AssetPipelineDiagnostic& diagnostic)
{
    StringAnsi json;
    if (GraphDocumentCodec::ToCanonicalJson(Document, json, diagnostic))
        return true;
    ContentHash* expected = allowOverwriteConflict ? nullptr : &LoadedHash;
    if (GraphDocumentCodec::SaveAtomic(Path, json, diagnostic, expected))
        return true;
    LoadedHash = HashText(json);
    Dirty = false;
    return false;
}

#if USE_EDITOR
bool GraphDocumentPreview::Publish(const Guid& assetID, const StringView& typeName, const Span<byte>& surface,
    String& storagePath, ArtifactLease& lease, AssetPipelineDiagnostic& diagnostic)
{
    storagePath.Clear();
    lease = ArtifactLease();
    if (!assetID.IsValid() || typeName.IsEmpty() || !surface.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build, TEXT("Preview compilation requires an asset identity and surface."));
    const ContentHash hash = ContentHash::Compute(surface.Get(), surface.Length());
    const String folder = Globals::ProjectLibraryFolder / TEXT("Temp") / TEXT("Preview") / assetID.ToString(Guid::FormatType::N) / String(hash.ToString());
    if (!FileSystem::DirectoryExists(folder) && FileSystem::CreateDirectory(folder))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::LibraryCreationFailed, AssetPipelineDiagnosticStage::Build, TEXT("Cannot create graph preview staging directory."));
    storagePath = folder / TEXT("graph.flax");
    Array<byte> bytes;
    bytes.Set(surface.Get(), surface.Length());
    if (WriteCompatibilityFlax(storagePath, assetID, typeName, bytes, StringAnsiView(), diagnostic, true))
        return true;
    lease = ArtifactLease::Acquire(storagePath);
    {
        ScopeLock lock(PreviewLocker);
        PreviewLeases[assetID] = lease;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void GraphDocumentPreview::Release(const Guid& assetID)
{
    ScopeLock lock(PreviewLocker);
    PreviewLeases.Remove(assetID);
}
#endif

bool GraphDocumentPreview::IsPreviewPath(const StringView& path)
{
    String normalized(path);
    FileSystem::NormalizePath(normalized);
    normalized = normalized.ToLower();
    return normalized.Contains(TEXT("/library/temp/preview/")) || normalized.Contains(TEXT("\\library\\temp\\preview\\"));
}
