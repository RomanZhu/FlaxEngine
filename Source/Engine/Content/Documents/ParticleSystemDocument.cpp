// Copyright (c) Wojciech Figat. All rights reserved.

#include "ParticleSystemDocument.h"
#include "CanonicalJsonWriter.h"
#include "GraphDocument.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"

namespace
{
    typedef rapidjson_flax::Value JsonValue;
    typedef rapidjson_flax::Document::AllocatorType JsonAlloc;
    typedef ParticleSystem::Track Track;

    StringAnsi GuidText(const Guid& id)
    {
        return StringAnsi(id.ToString(Guid::FormatType::N)).ToLower();
    }

    StringAnsi TrackId(int32 index)
    {
        return StringAnsi::Format("track-{0}", index);
    }

    bool ParseTrackId(const JsonValue& value, int32 limit, int32& index)
    {
        if (!value.IsString())
            return true;
        const StringAnsiView text(value.GetString(), value.GetStringLength());
        if (!text.StartsWith("track-") || StringUtils::Parse(String(text.Get() + 6, text.Length() - 6).Get(), &index))
            return true;
        return index < 0 || index >= limit;
    }

    void AddString(JsonValue& object, const char* name, const StringAnsiView& value, JsonAlloc& allocator)
    {
        object.AddMember(JsonValue(name, allocator), JsonValue(value.Get(), value.Length(), allocator), allocator);
    }

    JsonValue MakeReference(const Guid& id, JsonAlloc& allocator)
    {
        JsonValue result(rapidjson::kObjectType);
        AddString(result, "$type", "AssetReference", allocator);
        AddString(result, "value", GuidText(id), allocator);
        return result;
    }

    bool ReadReference(const JsonValue& value, Guid& id)
    {
        if (!value.IsObject())
            return true;
        const auto type = value.FindMember("$type");
        const auto payload = value.FindMember("value");
        if (type == value.MemberEnd() || !type->value.IsString() ||
            StringAnsiView(type->value.GetString(), type->value.GetStringLength()) != "AssetReference" ||
            payload == value.MemberEnd() || !payload->value.IsString())
            return true;
        return Guid::Parse(String(StringAnsiView(payload->value.GetString(), payload->value.GetStringLength())), id);
    }

    const char* TrackTypeName(Track::Types type)
    {
        switch (type)
        {
        case Track::Types::Emitter: return "Emitter";
        case Track::Types::Folder: return "Folder";
        default: return nullptr;
        }
    }

    bool ReadTrackType(const JsonValue& value, Track::Types& type)
    {
        if (!value.IsString())
            return true;
        const StringAnsiView name(value.GetString(), value.GetStringLength());
        if (name == "Emitter") type = Track::Types::Emitter;
        else if (name == "Folder") type = Track::Types::Folder;
        else return true;
        return false;
    }

    bool ReadInt(const JsonValue& object, const char* name, int32& value)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd() || !member->value.IsInt())
            return true;
        value = member->value.GetInt();
        return false;
    }
}

bool ParticleSystemDocument::DecodeLegacy(const Span<byte>& timeline, rapidjson_flax::Document& document, String& error)
{
    error.Clear();
    if (!timeline.IsValid())
    {
        error = TEXT("Particle system timeline is empty.");
        return true;
    }
    MemoryReadStream stream(timeline.Get(), timeline.Length());
    int32 version = 0;
    stream.Read(version);
    if (version != 4)
    {
        error = TEXT("Unsupported particle system timeline version.");
        return true;
    }
    float framesPerSecond = 0.0f;
    int32 durationFrames = 0;
    int32 emittersCount = 0;
    int32 tracksCount = 0;
    stream.ReadFloat(&framesPerSecond);
    stream.ReadInt32(&durationFrames);
    stream.ReadInt32(&emittersCount);
    stream.ReadInt32(&tracksCount);
    if (framesPerSecond <= 0.0f || durationFrames < 0 || emittersCount < 0 || tracksCount < 0 || tracksCount > 100000)
    {
        error = TEXT("Particle system timeline header is invalid.");
        return true;
    }

    document.SetObject();
    JsonAlloc& allocator = document.GetAllocator();
    document.AddMember("documentVersion", 1, allocator);
    AddString(document, "type", "FlaxEngine.ParticleSystem", allocator);
    document.AddMember("framesPerSecond", framesPerSecond, allocator);
    document.AddMember("durationFrames", durationFrames, allocator);
    JsonValue tracks(rapidjson::kArrayType);
    for (int32 i = 0; i < tracksCount; i++)
    {
        const auto type = static_cast<Track::Types>(stream.ReadByte());
        const byte flags = stream.ReadByte();
        int32 parentIndex = -1;
        int32 childrenCount = 0;
        String name;
        Color32 color;
        stream.ReadInt32(&parentIndex);
        stream.ReadInt32(&childrenCount);
        stream.Read(name, -13);
        stream.Read(color);
        const char* typeName = TrackTypeName(type);
        if (!typeName || parentIndex >= i || parentIndex < -1)
        {
            error = TEXT("Particle system track is invalid.");
            return true;
        }
        JsonValue track(rapidjson::kObjectType);
        AddString(track, "id", TrackId(i), allocator);
        AddString(track, "type", typeName, allocator);
        track.AddMember("flags", flags, allocator);
        if (parentIndex == -1)
            track.AddMember("parent", JsonValue(rapidjson::kNullType), allocator);
        else
            track.AddMember("parent", JsonValue(TrackId(parentIndex).Get(), TrackId(parentIndex).Length(), allocator), allocator);
        AddString(track, "name", StringAnsi(name), allocator);
        JsonValue rgba(rapidjson::kArrayType);
        rgba.PushBack(color.R, allocator).PushBack(color.G, allocator).PushBack(color.B, allocator).PushBack(color.A, allocator);
        track.AddMember("color", rgba, allocator);
        JsonValue data(rapidjson::kObjectType);
        if (type == Track::Types::Emitter)
        {
            Guid emitter;
            int32 emitterIndex = 0;
            int32 startFrame = 0;
            int32 trackDuration = 0;
            stream.Read(emitter);
            stream.ReadInt32(&emitterIndex);
            stream.ReadInt32(&startFrame);
            stream.ReadInt32(&trackDuration);
            if (emitterIndex < 0 || emitterIndex >= emittersCount)
            {
                error = TEXT("Particle system emitter index is invalid.");
                return true;
            }
            data.AddMember("emitter", MakeReference(emitter, allocator), allocator);
            data.AddMember("emitterIndex", emitterIndex, allocator);
            data.AddMember("startFrame", startFrame, allocator);
            data.AddMember("durationFrames", trackDuration, allocator);
        }
        track.AddMember("data", data, allocator);
        tracks.PushBack(track, allocator);
    }
    document.AddMember("tracks", tracks, allocator);

    int32 overridesCount = 0;
    if (stream.CanRead())
        stream.ReadInt32(&overridesCount);
    if (overridesCount < 0 || overridesCount > 100000)
    {
        error = TEXT("Particle system parameter override count is invalid.");
        return true;
    }
    JsonValue overrides(rapidjson::kArrayType);
    for (int32 i = 0; i < overridesCount; i++)
    {
        int32 emitterIndex = 0;
        Guid parameter;
        Variant value;
        stream.ReadInt32(&emitterIndex);
        stream.Read(parameter);
        stream.Read(value);
        StringAnsi variantJson;
        AssetPipelineDiagnostic diagnostic;
        if (GraphDocumentCodec::EncodeVariantJson(value, variantJson, diagnostic))
        {
            error = diagnostic.Message;
            return true;
        }
        rapidjson_flax::Document parsed;
        parsed.Parse(variantJson.Get(), variantJson.Length());
        if (parsed.HasParseError())
        {
            error = TEXT("Particle system override value could not be serialized.");
            return true;
        }
        JsonValue item(rapidjson::kObjectType);
        item.AddMember("emitterIndex", emitterIndex, allocator);
        AddString(item, "parameter", GuidText(parameter), allocator);
        JsonValue encoded;
        encoded.CopyFrom(parsed, allocator);
        item.AddMember("value", encoded, allocator);
        overrides.PushBack(item, allocator);
    }
    document.AddMember("parameterOverrides", overrides, allocator);
    return false;
}

bool ParticleSystemDocument::Compile(const rapidjson_flax::Value& document, Array<byte>& timeline, Array<Guid>* references, String& error)
{
    timeline.Clear();
    if (references)
        references->Clear();
    error.Clear();
    if (!document.IsObject())
    {
        error = TEXT("Particle system document must be an object.");
        return true;
    }
    const auto typeMember = document.FindMember("type");
    const auto fpsMember = document.FindMember("framesPerSecond");
    const auto durationMember = document.FindMember("durationFrames");
    const auto tracksMember = document.FindMember("tracks");
    const auto overridesMember = document.FindMember("parameterOverrides");
    if (typeMember == document.MemberEnd() || !typeMember->value.IsString() ||
        StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength()) != "FlaxEngine.ParticleSystem" ||
        fpsMember == document.MemberEnd() || !fpsMember->value.IsNumber() || fpsMember->value.GetFloat() <= 0.0f ||
        durationMember == document.MemberEnd() || !durationMember->value.IsInt() || durationMember->value.GetInt() < 0 ||
        tracksMember == document.MemberEnd() || !tracksMember->value.IsArray() ||
        overridesMember == document.MemberEnd() || !overridesMember->value.IsArray())
    {
        error = TEXT("Particle system document header is invalid.");
        return true;
    }
    const auto tracks = tracksMember->value.GetArray();
    int32 emitterCount = 0;
    for (const JsonValue& track : tracks)
    {
        const auto trackType = track.FindMember("type");
        Track::Types type;
        if (!track.IsObject() || trackType == track.MemberEnd() || ReadTrackType(trackType->value, type))
        {
            error = TEXT("Particle system track type is invalid.");
            return true;
        }
        if (type == Track::Types::Emitter)
            emitterCount++;
    }

    MemoryWriteStream stream(512);
    stream.WriteInt32(4);
    stream.WriteFloat(fpsMember->value.GetFloat());
    stream.WriteInt32(durationMember->value.GetInt());
    stream.WriteInt32(emitterCount);
    stream.WriteInt32(static_cast<int32>(tracks.Size()));
    for (rapidjson::SizeType i = 0; i < tracks.Size(); i++)
    {
        const JsonValue& track = tracks[i];
        const auto idMember = track.FindMember("id");
        const auto typeValue = track.FindMember("type");
        const auto flags = track.FindMember("flags");
        const auto parent = track.FindMember("parent");
        const auto name = track.FindMember("name");
        const auto color = track.FindMember("color");
        const auto data = track.FindMember("data");
        Track::Types type;
        int32 idIndex = -1;
        if (idMember == track.MemberEnd() || ParseTrackId(idMember->value, static_cast<int32>(tracks.Size()), idIndex) || idIndex != static_cast<int32>(i) ||
            typeValue == track.MemberEnd() || ReadTrackType(typeValue->value, type) ||
            flags == track.MemberEnd() || !flags->value.IsUint() || flags->value.GetUint() > 255 ||
            parent == track.MemberEnd() || name == track.MemberEnd() || !name->value.IsString() ||
            color == track.MemberEnd() || !color->value.IsArray() || color->value.Size() != 4 ||
            data == track.MemberEnd() || !data->value.IsObject())
        {
            error = TEXT("Particle system track fields are invalid.");
            return true;
        }
        int32 parentIndex = -1;
        if (!parent->value.IsNull() && (ParseTrackId(parent->value, static_cast<int32>(i), parentIndex) || parentIndex >= static_cast<int32>(i)))
        {
            error = TEXT("Particle system track parent is invalid.");
            return true;
        }
        int32 childrenCount = 0;
        for (const JsonValue& candidate : tracks)
        {
            const auto candidateParent = candidate.FindMember("parent");
            int32 candidateIndex = -1;
            if (candidateParent != candidate.MemberEnd() && !candidateParent->value.IsNull() &&
                !ParseTrackId(candidateParent->value, static_cast<int32>(tracks.Size()), candidateIndex) && candidateIndex == static_cast<int32>(i))
                childrenCount++;
        }
        Color32 rgba;
        for (int32 component = 0; component < 4; component++)
        {
            const JsonValue& value = color->value[component];
            if (!value.IsUint() || value.GetUint() > 255)
            {
                error = TEXT("Particle system track color is invalid.");
                return true;
            }
            (&rgba.R)[component] = static_cast<byte>(value.GetUint());
        }
        stream.WriteByte(static_cast<byte>(type));
        stream.WriteByte(static_cast<byte>(flags->value.GetUint()));
        stream.WriteInt32(parentIndex);
        stream.WriteInt32(childrenCount);
        stream.Write(String(StringAnsiView(name->value.GetString(), name->value.GetStringLength())), -13);
        stream.Write(rgba);
        if (type == Track::Types::Emitter)
        {
            const auto emitter = data->value.FindMember("emitter");
            Guid emitterId;
            int32 emitterIndex = 0;
            int32 startFrame = 0;
            int32 durationFrames = 0;
            if (emitter == data->value.MemberEnd() || ReadReference(emitter->value, emitterId) ||
                ReadInt(data->value, "emitterIndex", emitterIndex) || emitterIndex < 0 || emitterIndex >= emitterCount ||
                ReadInt(data->value, "startFrame", startFrame) || ReadInt(data->value, "durationFrames", durationFrames))
            {
                error = TEXT("Particle system emitter track data is invalid.");
                return true;
            }
            stream.Write(emitterId);
            stream.WriteInt32(emitterIndex);
            stream.WriteInt32(startFrame);
            stream.WriteInt32(durationFrames);
            if (references && emitterId.IsValid())
                references->Add(emitterId);
        }
    }

    stream.WriteInt32(static_cast<int32>(overridesMember->value.Size()));
    for (const JsonValue& item : overridesMember->value.GetArray())
    {
        if (!item.IsObject())
        {
            error = TEXT("Particle system parameter override is invalid.");
            return true;
        }
        int32 emitterIndex = 0;
        const auto parameter = item.FindMember("parameter");
        const auto value = item.FindMember("value");
        Guid parameterId;
        if (ReadInt(item, "emitterIndex", emitterIndex) || emitterIndex < 0 || emitterIndex >= emitterCount ||
            parameter == item.MemberEnd() || !parameter->value.IsString() ||
            Guid::Parse(String(StringAnsiView(parameter->value.GetString(), parameter->value.GetStringLength())), parameterId) ||
            value == item.MemberEnd())
        {
            error = TEXT("Particle system parameter override fields are invalid.");
            return true;
        }
        StringAnsi variantJson;
        CanonicalJsonError jsonError;
        if (CanonicalJsonWriter::Write(value->value, variantJson, jsonError))
        {
            error = TEXT("Particle system parameter override value is invalid.");
            return true;
        }
        Variant variant;
        AssetPipelineDiagnostic diagnostic;
        if (GraphDocumentCodec::DecodeVariantJson(variantJson, variant, diagnostic))
        {
            error = diagnostic.Message;
            return true;
        }
        stream.WriteInt32(emitterIndex);
        stream.Write(parameterId);
        stream.Write(variant);
    }
    timeline.Set(stream.GetHandle(), static_cast<int32>(stream.GetPosition()));
    return false;
}
