// Copyright (c) Wojciech Figat. All rights reserved.

#include "SceneAnimationDocument.h"
#include "CanonicalJsonWriter.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Platform/StringUtils.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;
    typedef rapidjson_flax::Document::AllocatorType JsonAlloc;
    typedef SceneAnimation::Track Track;

    template<typename T>
    void ReadRaw(MemoryReadStream& stream, T& value)
    {
        stream.ReadBytes(&value, sizeof(T));
    }

    template<typename T>
    void WriteRaw(MemoryWriteStream& stream, const T& value)
    {
        stream.WriteBytes(&value, sizeof(T));
    }

    const char* TrackTypeName(Track::Types type)
    {
        switch (type)
        {
        case Track::Types::Folder: return "Folder";
        case Track::Types::PostProcessMaterial: return "PostProcessMaterial";
        case Track::Types::NestedSceneAnimation: return "NestedSceneAnimation";
        case Track::Types::ScreenFade: return "ScreenFade";
        case Track::Types::Audio: return "Audio";
        case Track::Types::AudioVolume: return "AudioVolume";
        case Track::Types::Actor: return "Actor";
        case Track::Types::Script: return "Script";
        case Track::Types::KeyframesProperty: return "KeyframesProperty";
        case Track::Types::CurveProperty: return "CurveProperty";
        case Track::Types::StringProperty: return "StringProperty";
        case Track::Types::ObjectReferenceProperty: return "ObjectReferenceProperty";
        case Track::Types::StructProperty: return "StructProperty";
        case Track::Types::ObjectProperty: return "ObjectProperty";
        case Track::Types::Event: return "Event";
        case Track::Types::CameraCut: return "CameraCut";
        default: return nullptr;
        }
    }

    bool ParseTrackType(const JsonValue& value, Track::Types& type)
    {
        if (!value.IsString())
            return true;
        const StringAnsiView text(value.GetString(), value.GetStringLength());
        for (int32 i = static_cast<int32>(Track::Types::Folder); i <= static_cast<int32>(Track::Types::CameraCut); i++)
        {
            const auto candidate = static_cast<Track::Types>(i);
            const char* name = TrackTypeName(candidate);
            if (name && text == StringAnsiView(name))
            {
                type = candidate;
                return false;
            }
        }
        return true;
    }

    StringAnsi GuidText(const Guid& id)
    {
        StringAnsi result(id.ToString(Guid::FormatType::N));
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

    JsonValue Reference(const Guid& id, const char* kind, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kObjectType);
        result.AddMember("$type", JsonValue(kind, allocator), allocator);
        const StringAnsi text = GuidText(id);
        result.AddMember("guid", StringValue(text, allocator), allocator);
        return result;
    }

    bool ReadReference(const JsonValue& value, const char* kind, Guid& id)
    {
        if (!value.IsObject())
            return true;
        const auto type = value.FindMember("$type");
        const auto guid = value.FindMember("guid");
        if (type == value.MemberEnd() || !type->value.IsString() || guid == value.MemberEnd() || !guid->value.IsString() ||
            StringAnsiView(type->value.GetString(), type->value.GetStringLength()) != StringAnsiView(kind))
            return true;
        return Guid::Parse(StringAnsiView(guid->value.GetString(), guid->value.GetStringLength()), id);
    }

    JsonValue FloatArray(const float* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kArrayType);
        result.Reserve(count, allocator);
        for (int32 i = 0; i < count; i++)
            result.PushBack(values[i], allocator);
        return result;
    }

    JsonValue DoubleArray(const double* values, int32 count, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kArrayType);
        result.Reserve(count, allocator);
        for (int32 i = 0; i < count; i++)
            result.PushBack(values[i], allocator);
        return result;
    }

    JsonValue Color32Value(const Color32& color, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kArrayType);
        result.PushBack(color.R, allocator);
        result.PushBack(color.G, allocator);
        result.PushBack(color.B, allocator);
        result.PushBack(color.A, allocator);
        return result;
    }

    bool ReadFloatArray(const JsonValue& value, float* values, int32 count)
    {
        if (!value.IsArray() || value.Size() != static_cast<rapidjson::SizeType>(count))
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber()) return true;
            values[i] = value[i].GetFloat();
        }
        return false;
    }

    bool ReadDoubleArray(const JsonValue& value, double* values, int32 count)
    {
        if (!value.IsArray() || value.Size() != static_cast<rapidjson::SizeType>(count))
            return true;
        for (int32 i = 0; i < count; i++)
        {
            if (!value[i].IsNumber()) return true;
            values[i] = value[i].GetDouble();
        }
        return false;
    }

    bool ReadColor32(const JsonValue& value, Color32& color)
    {
        if (!value.IsArray() || value.Size() != 4)
            return true;
        byte* channels = &color.R;
        for (int32 i = 0; i < 4; i++)
        {
            if (!value[i].IsUint() || value[i].GetUint() > 255) return true;
            channels[i] = static_cast<byte>(value[i].GetUint());
        }
        return false;
    }

    StringAnsi StableId(const char* prefix, int32 index)
    {
        return StringAnsi::Format("{0}-{1}", prefix, index);
    }

    Guid LegacyTrackId(int32 index)
    {
        return Guid(0x53434e41u, 0x54524143u, static_cast<uint32>(index + 1), 0x4b494400u);
    }

    JsonValue ReadTypedValue(MemoryReadStream& stream, const StringAnsiView& typeName, int32 size, JsonAlloc& allocator, bool& failed)
    {
        failed = false;
        if (typeName == "System.Boolean" && size == 1) return JsonValue(stream.ReadBool());
        if (typeName == "System.Byte" && size == 1) return JsonValue(stream.ReadByte());
        if (typeName == "System.SByte" && size == 1) return JsonValue(static_cast<int32>(static_cast<int8>(stream.ReadByte())));
        if ((typeName == "System.Char" || typeName == "System.UInt16") && size == 2)
        {
            uint16 value; stream.ReadUint16(&value); return JsonValue(value);
        }
        if (typeName == "System.Int16" && size == 2)
        {
            uint16 value; stream.ReadUint16(&value); return JsonValue(static_cast<int32>(static_cast<int16>(value)));
        }
        if (typeName == "System.Int32" && size == 4)
        {
            int32 value; stream.ReadInt32(&value); return JsonValue(value);
        }
        if (typeName == "System.UInt32" && size == 4)
        {
            uint32 value; stream.ReadUint32(&value); return JsonValue(value);
        }
        if ((typeName == "System.Int64" || typeName == "System.DateTime" || typeName == "System.TimeSpan") && size == 8)
        {
            int64 value; stream.Read(value); return JsonValue(value);
        }
        if (typeName == "System.UInt64" && size == 8)
        {
            uint64 value; stream.Read(value); return JsonValue(value);
        }
        if (typeName == "System.Single" && size == 4)
        {
            float value; stream.ReadFloat(&value); return JsonValue(value);
        }
        if (typeName == "System.Double" && size == 8)
        {
            double value; stream.Read(value); return JsonValue(value);
        }
        if (typeName == "System.Guid" && size == 16)
        {
            Guid value; stream.Read(value); return StringValue(GuidText(value), allocator);
        }
        int32 components = 0;
        bool doubles = false;
        if (typeName == "FlaxEngine.Float2") components = 2;
        else if (typeName == "FlaxEngine.Float3") components = 3;
        else if (typeName == "FlaxEngine.Float4" || typeName == "FlaxEngine.Quaternion" || typeName == "FlaxEngine.Color") components = 4;
        else if (typeName == "FlaxEngine.Double2") { components = 2; doubles = true; }
        else if (typeName == "FlaxEngine.Double3") { components = 3; doubles = true; }
        else if (typeName == "FlaxEngine.Double4") { components = 4; doubles = true; }
        else if (typeName == "FlaxEngine.Vector2") { components = 2; doubles = size == 16; }
        else if (typeName == "FlaxEngine.Vector3") { components = 3; doubles = size == 24; }
        else if (typeName == "FlaxEngine.Vector4") { components = 4; doubles = size == 32; }
        if (components && size == components * (doubles ? 8 : 4))
        {
            if (doubles)
            {
                double values[4];
                for (int32 i = 0; i < components; i++) stream.Read(values[i]);
                return DoubleArray(values, components, allocator);
            }
            float values[4];
            for (int32 i = 0; i < components; i++) stream.ReadFloat(&values[i]);
            return FloatArray(values, components, allocator);
        }
        if (typeName == "FlaxEngine.Color32" && size == 4)
        {
            Color32 color; stream.Read(color); return Color32Value(color, allocator);
        }
        // Unknown enum types remain semantic as their named type plus integral value.
        if (size == 1) return JsonValue(stream.ReadByte());
        if (size == 2) { uint16 value; stream.ReadUint16(&value); return JsonValue(value); }
        if (size == 4) { int32 value; stream.ReadInt32(&value); return JsonValue(value); }
        if (size == 8) { int64 value; stream.Read(value); return JsonValue(value); }
        failed = true;
        return JsonValue();
    }

    bool WriteTypedValue(MemoryWriteStream& stream, const StringAnsiView& typeName, int32 size, const JsonValue& value)
    {
        const uint32 begin = stream.GetPosition();
        if (typeName == "System.Boolean" && size == 1 && value.IsBool()) stream.WriteBool(value.GetBool());
        else if ((typeName == "System.Byte" || typeName == "System.SByte") && size == 1 && value.IsInt()) stream.WriteByte(static_cast<byte>(value.GetInt()));
        else if ((typeName == "System.Char" || typeName == "System.UInt16" || typeName == "System.Int16") && size == 2 && value.IsInt()) stream.WriteUint16(static_cast<uint16>(value.GetInt()));
        else if (typeName == "System.Int32" && size == 4 && value.IsInt()) stream.WriteInt32(value.GetInt());
        else if (typeName == "System.UInt32" && size == 4 && value.IsUint()) stream.WriteUint32(value.GetUint());
        else if ((typeName == "System.Int64" || typeName == "System.DateTime" || typeName == "System.TimeSpan") && size == 8 && value.IsInt64()) stream.Write(value.GetInt64());
        else if (typeName == "System.UInt64" && size == 8 && value.IsUint64()) stream.Write(value.GetUint64());
        else if (typeName == "System.Single" && size == 4 && value.IsNumber()) stream.WriteFloat(value.GetFloat());
        else if (typeName == "System.Double" && size == 8 && value.IsNumber()) stream.Write(value.GetDouble());
        else if (typeName == "System.Guid" && size == 16 && value.IsString())
        {
            Guid guid;
            if (Guid::Parse(StringAnsiView(value.GetString(), value.GetStringLength()), guid)) return true;
            stream.Write(guid);
        }
        else
        {
            int32 components = 0;
            bool doubles = false;
            if (typeName == "FlaxEngine.Float2") components = 2;
            else if (typeName == "FlaxEngine.Float3") components = 3;
            else if (typeName == "FlaxEngine.Float4" || typeName == "FlaxEngine.Quaternion" || typeName == "FlaxEngine.Color") components = 4;
            else if (typeName == "FlaxEngine.Double2") { components = 2; doubles = true; }
            else if (typeName == "FlaxEngine.Double3") { components = 3; doubles = true; }
            else if (typeName == "FlaxEngine.Double4") { components = 4; doubles = true; }
            else if (typeName == "FlaxEngine.Vector2") { components = 2; doubles = size == 16; }
            else if (typeName == "FlaxEngine.Vector3") { components = 3; doubles = size == 24; }
            else if (typeName == "FlaxEngine.Vector4") { components = 4; doubles = size == 32; }
            if (components && value.IsArray() && value.Size() == static_cast<rapidjson::SizeType>(components) && size == components * (doubles ? 8 : 4))
            {
                for (int32 i = 0; i < components; i++)
                {
                    if (!value[i].IsNumber()) return true;
                    if (doubles) stream.Write(value[i].GetDouble()); else stream.WriteFloat(value[i].GetFloat());
                }
            }
            else if (typeName == "FlaxEngine.Color32" && size == 4)
            {
                Color32 color;
                if (ReadColor32(value, color)) return true;
                stream.Write(color);
            }
            else if (value.IsInt64() && (size == 1 || size == 2 || size == 4 || size == 8))
            {
                const int64 number = value.GetInt64();
                if (size == 1) stream.WriteByte(static_cast<byte>(number));
                else if (size == 2) stream.WriteUint16(static_cast<uint16>(number));
                else if (size == 4) stream.WriteInt32(static_cast<int32>(number));
                else stream.Write(number);
            }
            else
                return true;
        }
        return stream.GetPosition() - begin != static_cast<uint32>(size);
    }
}

bool SceneAnimationDocument::DecodeLegacy(const Span<byte>& timeline, rapidjson_flax::Document& document, String& error)
{
    error.Clear();
    if (timeline.Length() < 16)
    {
        error = TEXT("Scene animation timeline is truncated.");
        return true;
    }
    MemoryReadStream stream(timeline.Get(), timeline.Length());
    int32 version;
    float framesPerSecond;
    int32 durationFrames;
    int32 tracksCount;
    stream.ReadInt32(&version);
    stream.ReadFloat(&framesPerSecond);
    stream.ReadInt32(&durationFrames);
    stream.ReadInt32(&tracksCount);
    if (version < 2 || version > 5 || tracksCount < 0)
    {
        error = TEXT("Scene animation timeline version or track count is invalid.");
        return true;
    }

    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    document.AddMember("documentVersion", 1, allocator);
    document.AddMember("type", JsonValue("FlaxEngine.SceneAnimation", allocator), allocator);
    document.AddMember("framesPerSecond", framesPerSecond, allocator);
    document.AddMember("durationFrames", durationFrames, allocator);
    JsonValue tracks(rapidjson::kArrayType);
    tracks.Reserve(tracksCount, allocator);
    Array<StringAnsi> trackIds;
    trackIds.Resize(tracksCount);
    for (int32 i = 0; i < tracksCount; i++)
        trackIds[i] = GuidText(LegacyTrackId(i));

    for (int32 i = 0; i < tracksCount; i++)
    {
        const auto type = static_cast<Track::Types>(stream.ReadByte());
        const byte flags = stream.ReadByte();
        int32 parentIndex;
        int32 childrenCount;
        String name;
        Color32 color;
        stream.ReadInt32(&parentIndex);
        stream.ReadInt32(&childrenCount);
        stream.Read(name, -13);
        stream.Read(color);
        if (version >= 5)
        {
            Guid trackId;
            stream.Read(trackId);
            trackIds[i] = GuidText(trackId);
        }
        const char* typeName = TrackTypeName(type);
        if (!typeName || parentIndex < -1 || parentIndex >= i)
        {
            error = TEXT("Scene animation contains an unsupported track or invalid parent.");
            return true;
        }
        JsonValue track(rapidjson::kObjectType);
        track.AddMember("id", StringValue(trackIds[i], allocator), allocator);
        track.AddMember("type", JsonValue(typeName, allocator), allocator);
        track.AddMember("flags", flags, allocator);
        if (parentIndex == -1)
            track.AddMember("parent", JsonValue(), allocator);
        else
            track.AddMember("parent", StringValue(trackIds[parentIndex], allocator), allocator);
        track.AddMember("name", StringValue(StringAnsi(name), allocator), allocator);
        track.AddMember("color", Color32Value(color, allocator), allocator);
        JsonValue data(rapidjson::kObjectType);

        switch (type)
        {
        case Track::Types::Folder:
            break;
        case Track::Types::PostProcessMaterial:
        {
            SceneAnimation::PostProcessMaterialTrack::Data header;
            ReadRaw(stream, header);
            data.AddMember("material", Reference(header.AssetID, "AssetReference", allocator), allocator);
            int32 count = 1;
            if (version >= 4) stream.ReadInt32(&count);
            JsonValue clips(rapidjson::kArrayType);
            for (int32 j = 0; j < count; j++)
            {
                SceneAnimation::Media media; ReadRaw(stream, media);
                JsonValue clip(rapidjson::kObjectType);
                clip.AddMember("id", StringValue(StableId("clip", j), allocator), allocator);
                clip.AddMember("startFrame", media.StartFrame, allocator);
                clip.AddMember("durationFrames", media.DurationFrames, allocator);
                clips.PushBack(clip, allocator);
            }
            data.AddMember("clips", clips, allocator);
            break;
        }
        case Track::Types::NestedSceneAnimation:
        {
            SceneAnimation::NestedSceneAnimationTrack::Data header; ReadRaw(stream, header);
            data.AddMember("animation", Reference(header.AssetID, "AssetReference", allocator), allocator);
            data.AddMember("startFrame", header.StartFrame, allocator);
            data.AddMember("durationFrames", header.DurationFrames, allocator);
            break;
        }
        case Track::Types::ScreenFade:
        {
            SceneAnimation::ScreenFadeTrack::Data header; ReadRaw(stream, header);
            data.AddMember("startFrame", header.StartFrame, allocator);
            data.AddMember("durationFrames", header.DurationFrames, allocator);
            JsonValue stops(rapidjson::kArrayType);
            for (int32 j = 0; j < header.GradientStopsCount; j++)
            {
                SceneAnimation::ScreenFadeTrack::GradientStop stop; ReadRaw(stream, stop);
                JsonValue value(rapidjson::kObjectType);
                value.AddMember("id", StringValue(StableId("stop", j), allocator), allocator);
                value.AddMember("frame", stop.Frame, allocator);
                value.AddMember("color", FloatArray(&stop.Value.R, 4, allocator), allocator);
                stops.PushBack(value, allocator);
            }
            data.AddMember("gradientStops", stops, allocator);
            break;
        }
        case Track::Types::Audio:
        {
            SceneAnimation::AudioTrack::Data header; ReadRaw(stream, header);
            data.AddMember("audio", Reference(header.AssetID, "AssetReference", allocator), allocator);
            int32 count = 1;
            if (version >= 4) stream.ReadInt32(&count);
            JsonValue clips(rapidjson::kArrayType);
            for (int32 j = 0; j < count; j++)
            {
                SceneAnimation::AudioTrack::Media media;
                stream.ReadInt32(&media.StartFrame);
                stream.ReadInt32(&media.DurationFrames);
                if (version >= 4) stream.ReadFloat(&media.Offset); else media.Offset = 0.0f;
                JsonValue clip(rapidjson::kObjectType);
                clip.AddMember("id", StringValue(StableId("clip", j), allocator), allocator);
                clip.AddMember("startFrame", media.StartFrame, allocator);
                clip.AddMember("durationFrames", media.DurationFrames, allocator);
                clip.AddMember("offset", media.Offset, allocator);
                clips.PushBack(clip, allocator);
            }
            data.AddMember("clips", clips, allocator);
            break;
        }
        case Track::Types::AudioVolume:
        {
            SceneAnimation::AudioVolumeTrack::Data header; ReadRaw(stream, header);
            JsonValue keys(rapidjson::kArrayType);
            for (int32 j = 0; j < header.KeyframesCount; j++)
            {
                BezierCurveKeyframe<float> key; ReadRaw(stream, key);
                JsonValue value(rapidjson::kObjectType);
                value.AddMember("id", StringValue(StableId("key", j), allocator), allocator);
                value.AddMember("time", key.Time, allocator);
                value.AddMember("value", key.Value, allocator);
                value.AddMember("tangentIn", key.TangentIn, allocator);
                value.AddMember("tangentOut", key.TangentOut, allocator);
                keys.PushBack(value, allocator);
            }
            data.AddMember("keyframes", keys, allocator);
            break;
        }
        case Track::Types::Actor:
        case Track::Types::Script:
        {
            SceneAnimation::ObjectTrack::Data header; ReadRaw(stream, header);
            data.AddMember("object", Reference(header.ID, "ObjectReference", allocator), allocator);
            break;
        }
        case Track::Types::KeyframesProperty:
        case Track::Types::ObjectReferenceProperty:
        case Track::Types::CurveProperty:
        case Track::Types::StringProperty:
        {
            SceneAnimation::KeyframesPropertyTrack::Data header; ReadRaw(stream, header);
            const char* propertyName = stream.Move<char>(header.PropertyNameLength + 1);
            const char* propertyType = stream.Move<char>(header.PropertyTypeNameLength + 1);
            const StringAnsiView propertyNameView(propertyName, header.PropertyNameLength);
            const StringAnsiView propertyTypeView(propertyType, header.PropertyTypeNameLength);
            data.AddMember("property", StringValue(propertyNameView, allocator), allocator);
            data.AddMember("valueType", StringValue(propertyTypeView, allocator), allocator);
            data.AddMember("valueSize", header.ValueSize, allocator);
            JsonValue keys(rapidjson::kArrayType);
            for (int32 j = 0; j < header.KeyframesCount; j++)
            {
                float time; stream.ReadFloat(&time);
                JsonValue key(rapidjson::kObjectType);
                key.AddMember("id", StringValue(StableId("key", j), allocator), allocator);
                key.AddMember("time", time, allocator);
                if (type == Track::Types::StringProperty)
                {
                    int32 length; stream.ReadInt32(&length);
                    const Char* text = stream.Move<Char>(length);
                    key.AddMember("value", StringValue(StringAnsi(StringView(text, length)), allocator), allocator);
                }
                else if (type == Track::Types::ObjectReferenceProperty)
                {
                    Guid value; stream.Read(value);
                    key.AddMember("value", Reference(value, "ObjectReference", allocator), allocator);
                }
                else if (type == Track::Types::CurveProperty)
                {
                    bool failed;
                    JsonValue value = ReadTypedValue(stream, propertyTypeView, header.ValueSize, allocator, failed);
                    if (failed) { error = TEXT("Curve property value type has no semantic encoding."); return true; }
                    JsonValue tangentIn = ReadTypedValue(stream, propertyTypeView, header.ValueSize, allocator, failed);
                    if (failed) { error = TEXT("Curve property tangent type has no semantic encoding."); return true; }
                    JsonValue tangentOut = ReadTypedValue(stream, propertyTypeView, header.ValueSize, allocator, failed);
                    if (failed) { error = TEXT("Curve property tangent type has no semantic encoding."); return true; }
                    key.AddMember("value", value, allocator);
                    key.AddMember("tangentIn", tangentIn, allocator);
                    key.AddMember("tangentOut", tangentOut, allocator);
                }
                else if (header.ValueSize == 0)
                {
                    int32 length; stream.ReadInt32(&length);
                    if (length == 0)
                    {
                        key.AddMember("value", JsonValue(), allocator);
                    }
                    else
                    {
                        const char* jsonBytes = stream.Move<char>(length);
                        rapidjson_flax::Document valueDocument;
                        valueDocument.Parse(jsonBytes, length);
                        if (valueDocument.HasParseError())
                        {
                            error = TEXT("Generic property keyframe contains invalid JSON.");
                            return true;
                        }
                        JsonValue semanticValue;
                        semanticValue.CopyFrom(valueDocument, allocator);
                        key.AddMember("value", semanticValue, allocator);
                    }
                }
                else
                {
                    bool failed;
                    JsonValue value = ReadTypedValue(stream, propertyTypeView, header.ValueSize, allocator, failed);
                    if (failed) { error = TEXT("Property value type has no semantic encoding."); return true; }
                    key.AddMember("value", value, allocator);
                }
                keys.PushBack(key, allocator);
            }
            data.AddMember("keyframes", keys, allocator);
            break;
        }
        case Track::Types::StructProperty:
        case Track::Types::ObjectProperty:
        {
            SceneAnimation::PropertyTrack::Data header; ReadRaw(stream, header);
            const char* propertyName = stream.Move<char>(header.PropertyNameLength + 1);
            const char* propertyType = stream.Move<char>(header.PropertyTypeNameLength + 1);
            data.AddMember("property", StringValue(StringAnsiView(propertyName, header.PropertyNameLength), allocator), allocator);
            data.AddMember("valueType", StringValue(StringAnsiView(propertyType, header.PropertyTypeNameLength), allocator), allocator);
            data.AddMember("valueSize", header.ValueSize, allocator);
            break;
        }
        case Track::Types::Event:
        {
            int32 parametersCount;
            int32 eventsCount;
            int32 nameLength;
            stream.ReadInt32(&parametersCount);
            stream.ReadInt32(&eventsCount);
            stream.ReadInt32(&nameLength);
            const char* eventName = stream.Move<char>(nameLength + 1);
            data.AddMember("event", StringValue(StringAnsiView(eventName, nameLength), allocator), allocator);
            JsonValue parameterTypes(rapidjson::kArrayType);
            Array<StringAnsi> typeNames;
            Array<int32> typeSizes;
            typeNames.Resize(parametersCount);
            typeSizes.Resize(parametersCount);
            for (int32 j = 0; j < parametersCount; j++)
            {
                int32 typeNameLength;
                stream.ReadInt32(&typeSizes[j]);
                stream.ReadInt32(&typeNameLength);
                const char* typeNameText = stream.Move<char>(typeNameLength + 1);
                typeNames[j].Set(typeNameText, typeNameLength);
                JsonValue parameterType(rapidjson::kObjectType);
                parameterType.AddMember("type", StringValue(typeNames[j], allocator), allocator);
                parameterType.AddMember("size", typeSizes[j], allocator);
                parameterTypes.PushBack(parameterType, allocator);
            }
            data.AddMember("parameters", parameterTypes, allocator);
            JsonValue events(rapidjson::kArrayType);
            for (int32 j = 0; j < eventsCount; j++)
            {
                float time; stream.ReadFloat(&time);
                JsonValue eventValue(rapidjson::kObjectType);
                eventValue.AddMember("id", StringValue(StableId("event", j), allocator), allocator);
                eventValue.AddMember("time", time, allocator);
                JsonValue arguments(rapidjson::kArrayType);
                for (int32 k = 0; k < parametersCount; k++)
                {
                    bool failed;
                    JsonValue argument = ReadTypedValue(stream, typeNames[k], typeSizes[k], allocator, failed);
                    if (failed) { error = TEXT("Event parameter type has no semantic encoding."); return true; }
                    arguments.PushBack(argument, allocator);
                }
                eventValue.AddMember("arguments", arguments, allocator);
                events.PushBack(eventValue, allocator);
            }
            data.AddMember("events", events, allocator);
            break;
        }
        case Track::Types::CameraCut:
        {
            SceneAnimation::CameraCutTrack::Data header; ReadRaw(stream, header);
            data.AddMember("camera", Reference(header.ID, "ObjectReference", allocator), allocator);
            int32 count = 1;
            if (version >= 4) stream.ReadInt32(&count);
            JsonValue clips(rapidjson::kArrayType);
            for (int32 j = 0; j < count; j++)
            {
                SceneAnimation::Media media; ReadRaw(stream, media);
                JsonValue clip(rapidjson::kObjectType);
                clip.AddMember("id", StringValue(StableId("clip", j), allocator), allocator);
                clip.AddMember("startFrame", media.StartFrame, allocator);
                clip.AddMember("durationFrames", media.DurationFrames, allocator);
                clips.PushBack(clip, allocator);
            }
            data.AddMember("clips", clips, allocator);
            break;
        }
        default:
            error = TEXT("Scene animation track has no semantic encoding.");
            return true;
        }

        track.AddMember("data", data, allocator);
        tracks.PushBack(track, allocator);
    }
    if (stream.GetPosition() != timeline.Length())
    {
        error = TEXT("Scene animation timeline contains unparsed data.");
        return true;
    }
    document.AddMember("tracks", tracks, allocator);
    return false;
}

bool SceneAnimationDocument::Compile(const rapidjson_flax::Value& document, Array<byte>& timeline, Array<Guid>* references, String& error)
{
    error.Clear();
    timeline.Clear();
    if (!document.IsObject())
    {
        error = TEXT("Scene animation document root must be an object.");
        return true;
    }
    const auto fpsMember = document.FindMember("framesPerSecond");
    const auto durationMember = document.FindMember("durationFrames");
    const auto tracksMember = document.FindMember("tracks");
    if (fpsMember == document.MemberEnd() || !fpsMember->value.IsNumber() || fpsMember->value.GetFloat() <= 0.0f ||
        durationMember == document.MemberEnd() || !durationMember->value.IsInt() || durationMember->value.GetInt() < 0 ||
        tracksMember == document.MemberEnd() || !tracksMember->value.IsArray())
    {
        error = TEXT("Scene animation requires framesPerSecond, durationFrames, and tracks.");
        return true;
    }
    const auto& tracks = tracksMember->value;
    Array<StringAnsi> trackIds;
    trackIds.Resize(static_cast<int32>(tracks.Size()));
    for (int32 i = 0; i < trackIds.Count(); i++)
    {
        if (!tracks[i].IsObject()) { error = TEXT("Scene animation track must be an object."); return true; }
        const auto id = tracks[i].FindMember("id");
        if (id == tracks[i].MemberEnd() || !id->value.IsString()) { error = TEXT("Scene animation track is missing its stable id."); return true; }
        trackIds[i].Set(id->value.GetString(), id->value.GetStringLength());
        for (int32 j = 0; j < i; j++)
        {
            if (trackIds[j] == trackIds[i]) { error = TEXT("Scene animation track ids must be unique."); return true; }
        }
    }

    MemoryWriteStream stream(1024);
    stream.WriteInt32(5);
    stream.WriteFloat(fpsMember->value.GetFloat());
    stream.WriteInt32(durationMember->value.GetInt());
    stream.WriteInt32(trackIds.Count());
    for (int32 i = 0; i < trackIds.Count(); i++)
    {
        const JsonValue& track = tracks[i];
        const auto typeMember = track.FindMember("type");
        const auto flagsMember = track.FindMember("flags");
        const auto parentMember = track.FindMember("parent");
        const auto nameMember = track.FindMember("name");
        const auto colorMember = track.FindMember("color");
        const auto dataMember = track.FindMember("data");
        Track::Types type;
        Color32 color;
        if (typeMember == track.MemberEnd() || ParseTrackType(typeMember->value, type) ||
            flagsMember == track.MemberEnd() || !flagsMember->value.IsUint() || flagsMember->value.GetUint() > 255 ||
            parentMember == track.MemberEnd() || nameMember == track.MemberEnd() || !nameMember->value.IsString() ||
            colorMember == track.MemberEnd() || ReadColor32(colorMember->value, color) ||
            dataMember == track.MemberEnd() || !dataMember->value.IsObject())
        {
            error = TEXT("Scene animation track common fields are invalid.");
            return true;
        }
        int32 parentIndex = -1;
        if (!parentMember->value.IsNull())
        {
            if (!parentMember->value.IsString()) { error = TEXT("Scene animation track parent must be null or a track id."); return true; }
            const StringAnsiView parent(parentMember->value.GetString(), parentMember->value.GetStringLength());
            for (int32 j = 0; j < i; j++)
            {
                if (trackIds[j] == parent) { parentIndex = j; break; }
            }
            if (parentIndex == -1) { error = TEXT("Scene animation track parent must precede its child."); return true; }
        }
        int32 childrenCount = 0;
        for (int32 j = 0; j < trackIds.Count(); j++)
        {
            const auto candidateParent = tracks[j].FindMember("parent");
            if (candidateParent != tracks[j].MemberEnd() && candidateParent->value.IsString() &&
                StringAnsiView(candidateParent->value.GetString(), candidateParent->value.GetStringLength()) == trackIds[i])
                childrenCount++;
        }
        stream.WriteByte(static_cast<byte>(type));
        stream.WriteByte(static_cast<byte>(flagsMember->value.GetUint()));
        stream.WriteInt32(parentIndex);
        stream.WriteInt32(childrenCount);
        stream.Write(String(StringAnsiView(nameMember->value.GetString(), nameMember->value.GetStringLength())), -13);
        stream.Write(color);
        Guid trackId;
        if (Guid::Parse(trackIds[i], trackId))
            trackId = LegacyTrackId(i);
        stream.Write(trackId);
        const JsonValue& data = dataMember->value;

        switch (type)
        {
        case Track::Types::Folder:
            break;
        case Track::Types::PostProcessMaterial:
        {
            const auto material = data.FindMember("material");
            const auto clips = data.FindMember("clips");
            Guid assetId;
            if (material == data.MemberEnd() || ReadReference(material->value, "AssetReference", assetId) || clips == data.MemberEnd() || !clips->value.IsArray())
            { error = TEXT("Post-process material track data is invalid."); return true; }
            SceneAnimation::PostProcessMaterialTrack::Data header; header.AssetID = assetId; WriteRaw(stream, header);
            stream.WriteInt32(static_cast<int32>(clips->value.Size()));
            for (const JsonValue& clip : clips->value.GetArray())
            {
                if (!clip.IsObject()) { error = TEXT("Post-process material clip is invalid."); return true; }
                const auto start = clip.FindMember("startFrame"); const auto duration = clip.FindMember("durationFrames");
                if (start == clip.MemberEnd() || !start->value.IsInt() || duration == clip.MemberEnd() || !duration->value.IsInt())
                { error = TEXT("Post-process material clip is invalid."); return true; }
                SceneAnimation::Media media { start->value.GetInt(), duration->value.GetInt() }; WriteRaw(stream, media);
            }
            if (references && assetId.IsValid()) references->Add(assetId);
            break;
        }
        case Track::Types::NestedSceneAnimation:
        {
            const auto animation = data.FindMember("animation"); const auto start = data.FindMember("startFrame"); const auto duration = data.FindMember("durationFrames");
            Guid assetId;
            if (animation == data.MemberEnd() || ReadReference(animation->value, "AssetReference", assetId) || start == data.MemberEnd() || !start->value.IsInt() || duration == data.MemberEnd() || !duration->value.IsInt())
            { error = TEXT("Nested scene animation track data is invalid."); return true; }
            SceneAnimation::NestedSceneAnimationTrack::Data header { assetId, start->value.GetInt(), duration->value.GetInt() }; WriteRaw(stream, header);
            if (references && assetId.IsValid()) references->Add(assetId);
            break;
        }
        case Track::Types::ScreenFade:
        {
            const auto start = data.FindMember("startFrame"); const auto duration = data.FindMember("durationFrames"); const auto stops = data.FindMember("gradientStops");
            if (start == data.MemberEnd() || !start->value.IsInt() || duration == data.MemberEnd() || !duration->value.IsInt() || stops == data.MemberEnd() || !stops->value.IsArray())
            { error = TEXT("Screen fade track data is invalid."); return true; }
            SceneAnimation::ScreenFadeTrack::Data header { start->value.GetInt(), duration->value.GetInt(), static_cast<int32>(stops->value.Size()) }; WriteRaw(stream, header);
            for (const JsonValue& stop : stops->value.GetArray())
            {
                if (!stop.IsObject()) { error = TEXT("Screen fade gradient stop is invalid."); return true; }
                const auto frame = stop.FindMember("frame"); const auto stopColor = stop.FindMember("color");
                SceneAnimation::ScreenFadeTrack::GradientStop value;
                if (frame == stop.MemberEnd() || !frame->value.IsInt() || stopColor == stop.MemberEnd() || ReadFloatArray(stopColor->value, &value.Value.R, 4))
                { error = TEXT("Screen fade gradient stop is invalid."); return true; }
                value.Frame = frame->value.GetInt(); WriteRaw(stream, value);
            }
            break;
        }
        case Track::Types::Audio:
        {
            const auto audio = data.FindMember("audio"); const auto clips = data.FindMember("clips");
            Guid assetId;
            if (audio == data.MemberEnd() || ReadReference(audio->value, "AssetReference", assetId) || clips == data.MemberEnd() || !clips->value.IsArray())
            { error = TEXT("Audio track data is invalid."); return true; }
            SceneAnimation::AudioTrack::Data header; header.AssetID = assetId; WriteRaw(stream, header);
            stream.WriteInt32(static_cast<int32>(clips->value.Size()));
            for (const JsonValue& clip : clips->value.GetArray())
            {
                if (!clip.IsObject()) { error = TEXT("Audio clip is invalid."); return true; }
                const auto start = clip.FindMember("startFrame"); const auto duration = clip.FindMember("durationFrames"); const auto offset = clip.FindMember("offset");
                if (start == clip.MemberEnd() || !start->value.IsInt() || duration == clip.MemberEnd() || !duration->value.IsInt() || offset == clip.MemberEnd() || !offset->value.IsNumber())
                { error = TEXT("Audio clip is invalid."); return true; }
                SceneAnimation::AudioTrack::Media media { start->value.GetInt(), duration->value.GetInt(), offset->value.GetFloat() }; WriteRaw(stream, media);
            }
            if (references && assetId.IsValid()) references->Add(assetId);
            break;
        }
        case Track::Types::AudioVolume:
        {
            const auto keys = data.FindMember("keyframes");
            if (keys == data.MemberEnd() || !keys->value.IsArray()) { error = TEXT("Audio volume track keyframes are invalid."); return true; }
            SceneAnimation::AudioVolumeTrack::Data header { static_cast<int32>(keys->value.Size()) }; WriteRaw(stream, header);
            for (const JsonValue& key : keys->value.GetArray())
            {
                if (!key.IsObject()) { error = TEXT("Audio volume keyframe is invalid."); return true; }
                const auto time = key.FindMember("time"); const auto value = key.FindMember("value"); const auto tangentIn = key.FindMember("tangentIn"); const auto tangentOut = key.FindMember("tangentOut");
                if (time == key.MemberEnd() || !time->value.IsNumber() || value == key.MemberEnd() || !value->value.IsNumber() ||
                    tangentIn == key.MemberEnd() || !tangentIn->value.IsNumber() || tangentOut == key.MemberEnd() || !tangentOut->value.IsNumber())
                { error = TEXT("Audio volume keyframe is invalid."); return true; }
                BezierCurveKeyframe<float> runtimeKey(time->value.GetFloat(), value->value.GetFloat(), tangentIn->value.GetFloat(), tangentOut->value.GetFloat());
                WriteRaw(stream, runtimeKey);
            }
            break;
        }
        case Track::Types::Actor:
        case Track::Types::Script:
        {
            const auto object = data.FindMember("object");
            Guid objectId;
            if (object == data.MemberEnd() || ReadReference(object->value, "ObjectReference", objectId))
            { error = TEXT("Actor or script track object reference is invalid."); return true; }
            SceneAnimation::ObjectTrack::Data header; header.ID = objectId; WriteRaw(stream, header);
            break;
        }
        case Track::Types::KeyframesProperty:
        case Track::Types::ObjectReferenceProperty:
        case Track::Types::CurveProperty:
        case Track::Types::StringProperty:
        {
            const auto property = data.FindMember("property"); const auto valueType = data.FindMember("valueType");
            const auto valueSize = data.FindMember("valueSize"); const auto keys = data.FindMember("keyframes");
            if (property == data.MemberEnd() || !property->value.IsString() || valueType == data.MemberEnd() || !valueType->value.IsString() ||
                valueSize == data.MemberEnd() || !valueSize->value.IsInt() || valueSize->value.GetInt() < 0 || keys == data.MemberEnd() || !keys->value.IsArray())
            { error = TEXT("Property track fields are invalid."); return true; }
            const StringAnsiView propertyText(property->value.GetString(), property->value.GetStringLength());
            const StringAnsiView valueTypeText(valueType->value.GetString(), valueType->value.GetStringLength());
            SceneAnimation::KeyframesPropertyTrack::Data header;
            header.ValueSize = valueSize->value.GetInt();
            header.PropertyNameLength = propertyText.Length();
            header.PropertyTypeNameLength = valueTypeText.Length();
            header.KeyframesCount = static_cast<int32>(keys->value.Size());
            if (type == Track::Types::ObjectReferenceProperty && header.ValueSize != sizeof(Guid))
            { error = TEXT("Object-reference property values must be GUID-sized."); return true; }
            WriteRaw(stream, header);
            stream.WriteBytes(propertyText.Get(), propertyText.Length()); stream.WriteByte(0);
            stream.WriteBytes(valueTypeText.Get(), valueTypeText.Length()); stream.WriteByte(0);
            for (const JsonValue& key : keys->value.GetArray())
            {
                if (!key.IsObject()) { error = TEXT("Property keyframe is invalid."); return true; }
                const auto time = key.FindMember("time"); const auto value = key.FindMember("value");
                if (time == key.MemberEnd() || !time->value.IsNumber() || value == key.MemberEnd())
                { error = TEXT("Property keyframe is missing time or value."); return true; }
                stream.WriteFloat(time->value.GetFloat());
                if (type == Track::Types::StringProperty)
                {
                    if (!value->value.IsString()) { error = TEXT("String property keyframe value must be text."); return true; }
                    const String text(StringAnsiView(value->value.GetString(), value->value.GetStringLength()));
                    stream.WriteInt32(text.Length());
                    stream.WriteBytes(text.Get(), text.Length() * sizeof(Char));
                }
                else if (type == Track::Types::ObjectReferenceProperty)
                {
                    Guid objectId;
                    if (ReadReference(value->value, "ObjectReference", objectId)) { error = TEXT("Object-reference property keyframe is invalid."); return true; }
                    stream.Write(objectId);
                }
                else if (type == Track::Types::CurveProperty)
                {
                    const auto tangentIn = key.FindMember("tangentIn"); const auto tangentOut = key.FindMember("tangentOut");
                    if (tangentIn == key.MemberEnd() || tangentOut == key.MemberEnd() ||
                        WriteTypedValue(stream, valueTypeText, header.ValueSize, value->value) ||
                        WriteTypedValue(stream, valueTypeText, header.ValueSize, tangentIn->value) ||
                        WriteTypedValue(stream, valueTypeText, header.ValueSize, tangentOut->value))
                    { error = TEXT("Curve property keyframe value does not match its type."); return true; }
                }
                else if (header.ValueSize == 0)
                {
                    if (value->value.IsNull())
                    {
                        stream.WriteInt32(0);
                    }
                    else
                    {
                        StringAnsi json;
                        CanonicalJsonError jsonError;
                        if (CanonicalJsonWriter::Write(value->value, json, jsonError))
                        { error = TEXT("Generic property keyframe JSON cannot be serialized."); return true; }
                        stream.WriteInt32(json.Length());
                        stream.WriteBytes(json.Get(), json.Length());
                    }
                }
                else if (WriteTypedValue(stream, valueTypeText, header.ValueSize, value->value))
                {
                    error = TEXT("Property keyframe value does not match its type.");
                    return true;
                }
            }
            break;
        }
        case Track::Types::StructProperty:
        case Track::Types::ObjectProperty:
        {
            const auto property = data.FindMember("property"); const auto valueType = data.FindMember("valueType"); const auto valueSize = data.FindMember("valueSize");
            if (property == data.MemberEnd() || !property->value.IsString() || valueType == data.MemberEnd() || !valueType->value.IsString() ||
                valueSize == data.MemberEnd() || !valueSize->value.IsInt() || valueSize->value.GetInt() < 0)
            { error = TEXT("Struct or object property track fields are invalid."); return true; }
            const StringAnsiView propertyText(property->value.GetString(), property->value.GetStringLength());
            const StringAnsiView valueTypeText(valueType->value.GetString(), valueType->value.GetStringLength());
            SceneAnimation::PropertyTrack::Data header { valueSize->value.GetInt(), propertyText.Length(), valueTypeText.Length() };
            WriteRaw(stream, header);
            stream.WriteBytes(propertyText.Get(), propertyText.Length()); stream.WriteByte(0);
            stream.WriteBytes(valueTypeText.Get(), valueTypeText.Length()); stream.WriteByte(0);
            break;
        }
        case Track::Types::Event:
        {
            const auto eventName = data.FindMember("event"); const auto parameters = data.FindMember("parameters"); const auto events = data.FindMember("events");
            if (eventName == data.MemberEnd() || !eventName->value.IsString() || parameters == data.MemberEnd() || !parameters->value.IsArray() ||
                parameters->value.Size() > SceneAnimation::EventTrack::MaxParams || events == data.MemberEnd() || !events->value.IsArray())
            { error = TEXT("Event track fields are invalid."); return true; }
            const StringAnsiView eventText(eventName->value.GetString(), eventName->value.GetStringLength());
            Array<StringAnsi> parameterTypes;
            Array<int32> parameterSizes;
            parameterTypes.Resize(static_cast<int32>(parameters->value.Size()));
            parameterSizes.Resize(parameterTypes.Count());
            stream.WriteInt32(parameterTypes.Count());
            stream.WriteInt32(static_cast<int32>(events->value.Size()));
            stream.WriteInt32(eventText.Length());
            stream.WriteBytes(eventText.Get(), eventText.Length()); stream.WriteByte(0);
            for (int32 j = 0; j < parameterTypes.Count(); j++)
            {
                const JsonValue& parameter = parameters->value[j];
                if (!parameter.IsObject()) { error = TEXT("Event parameter descriptor is invalid."); return true; }
                const auto typeName = parameter.FindMember("type"); const auto size = parameter.FindMember("size");
                if (typeName == parameter.MemberEnd() || !typeName->value.IsString() || size == parameter.MemberEnd() || !size->value.IsInt() || size->value.GetInt() <= 0)
                { error = TEXT("Event parameter descriptor is invalid."); return true; }
                parameterTypes[j].Set(typeName->value.GetString(), typeName->value.GetStringLength());
                parameterSizes[j] = size->value.GetInt();
                stream.WriteInt32(parameterSizes[j]);
                stream.WriteInt32(parameterTypes[j].Length());
                stream.WriteBytes(parameterTypes[j].Get(), parameterTypes[j].Length()); stream.WriteByte(0);
            }
            for (const JsonValue& eventValue : events->value.GetArray())
            {
                if (!eventValue.IsObject()) { error = TEXT("Event invocation is invalid."); return true; }
                const auto time = eventValue.FindMember("time"); const auto arguments = eventValue.FindMember("arguments");
                if (time == eventValue.MemberEnd() || !time->value.IsNumber() || arguments == eventValue.MemberEnd() || !arguments->value.IsArray() || arguments->value.Size() != parameters->value.Size())
                { error = TEXT("Event invocation arguments are invalid."); return true; }
                stream.WriteFloat(time->value.GetFloat());
                for (int32 j = 0; j < parameterTypes.Count(); j++)
                {
                    if (WriteTypedValue(stream, parameterTypes[j], parameterSizes[j], arguments->value[j]))
                    { error = TEXT("Event argument does not match its declared type."); return true; }
                }
            }
            break;
        }
        case Track::Types::CameraCut:
        {
            const auto camera = data.FindMember("camera"); const auto clips = data.FindMember("clips");
            Guid cameraId;
            if (camera == data.MemberEnd() || ReadReference(camera->value, "ObjectReference", cameraId) || clips == data.MemberEnd() || !clips->value.IsArray())
            { error = TEXT("Camera cut track data is invalid."); return true; }
            SceneAnimation::CameraCutTrack::Data header; header.ID = cameraId; WriteRaw(stream, header);
            stream.WriteInt32(static_cast<int32>(clips->value.Size()));
            for (const JsonValue& clip : clips->value.GetArray())
            {
                if (!clip.IsObject()) { error = TEXT("Camera cut clip is invalid."); return true; }
                const auto start = clip.FindMember("startFrame"); const auto duration = clip.FindMember("durationFrames");
                if (start == clip.MemberEnd() || !start->value.IsInt() || duration == clip.MemberEnd() || !duration->value.IsInt())
                { error = TEXT("Camera cut clip is invalid."); return true; }
                SceneAnimation::Media media { start->value.GetInt(), duration->value.GetInt() }; WriteRaw(stream, media);
            }
            break;
        }
        default:
            error = TEXT("Scene animation track has no runtime compiler.");
            return true;
        }
    }
    timeline.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
    return false;
}
