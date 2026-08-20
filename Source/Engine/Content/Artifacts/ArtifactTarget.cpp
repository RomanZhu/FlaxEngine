// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactTarget.h"
#include "Engine/Core/Collections/Sorting.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Utilities/StringConverter.h"

namespace
{
    void WriteUInt32(byte* destination, uint32 value)
    {
        destination[0] = static_cast<byte>(value >> 24);
        destination[1] = static_cast<byte>(value >> 16);
        destination[2] = static_cast<byte>(value >> 8);
        destination[3] = static_cast<byte>(value);
    }
}

ArtifactKey ArtifactTarget::BuildKey(ArtifactTargetDimension dimensions) const
{
    ArtifactKeyBuilder builder(StringAnsiView("flax-artifact-target-v1"));
    builder.AddTarget(*this, dimensions);
    return builder.Finalize();
}

ArtifactKeyBuilder::ArtifactKeyBuilder(const StringAnsiView& domain)
{
    static const char Prefix[] = "FLAX-ARTIFACT-KEY\0";
    _hasher.Update(Prefix, sizeof(Prefix) - 1);
    AddString(StringAnsiView("domain"), domain);
}

void ArtifactKeyBuilder::AddRawUInt32(uint32 value)
{
    byte bytes[4];
    WriteUInt32(bytes, value);
    _hasher.Update(bytes, sizeof(bytes));
}

void ArtifactKeyBuilder::AddField(FieldType type, const StringAnsiView& name, const void* data, uint32 length, const StringAnsiView& display)
{
    const byte typeValue = static_cast<byte>(type);
    _hasher.Update(&typeValue, sizeof(typeValue));
    AddRawUInt32(name.Length());
    _hasher.Update(name.Get(), name.Length());
    AddRawUInt32(length);
    _hasher.Update(data, length);

    ArtifactKeyComponent component;
    component.Name = name.ToStringAnsi();
    switch (type)
    {
    case FieldType::Boolean: component.Type = "bool"; break;
    case FieldType::UInt32: component.Type = "uint32"; break;
    case FieldType::UInt64: component.Type = "uint64"; break;
    case FieldType::String: component.Type = "string"; break;
    case FieldType::Guid: component.Type = "guid"; break;
    case FieldType::ContentHash: component.Type = "content-hash"; break;
    case FieldType::ArtifactKey: component.Type = "artifact-key"; break;
    case FieldType::StringCollection: component.Type = "string-collection"; break;
    }
    component.Value = display.ToStringAnsi();
    _components.Add(MoveTemp(component));
}

void ArtifactKeyBuilder::AddBool(const StringAnsiView& name, bool value)
{
    const byte data = value ? 1 : 0;
    AddField(FieldType::Boolean, name, &data, sizeof(data), value ? StringAnsiView("true") : StringAnsiView("false"));
}

void ArtifactKeyBuilder::AddUInt32(const StringAnsiView& name, uint32 value)
{
    byte bytes[4];
    WriteUInt32(bytes, value);
    const StringAnsi display = StringAnsi::Format("{0}", value);
    AddField(FieldType::UInt32, name, bytes, sizeof(bytes), display);
}

void ArtifactKeyBuilder::AddUInt64(const StringAnsiView& name, uint64 value)
{
    byte bytes[8];
    for (int32 i = 0; i < 8; i++)
        bytes[7 - i] = static_cast<byte>(value >> (i * 8));
    const StringAnsi display = StringAnsi::Format("{0}", value);
    AddField(FieldType::UInt64, name, bytes, sizeof(bytes), display);
}

void ArtifactKeyBuilder::AddString(const StringAnsiView& name, const StringAnsiView& value)
{
    AddField(FieldType::String, name, value.Get(), value.Length(), value);
}

void ArtifactKeyBuilder::AddString(const StringAnsiView& name, const StringView& value)
{
    const StringAsUTF8<> utf8(value.Get(), value.Length());
    const StringAnsiView utf8View(utf8.Get(), utf8.Length());
    AddField(FieldType::String, name, utf8View.Get(), utf8View.Length(), utf8View);
}

void ArtifactKeyBuilder::AddGuid(const StringAnsiView& name, const Guid& value)
{
    char text[33];
    value.ToString(text, Guid::FormatType::N);
    AddField(FieldType::Guid, name, text, 32, StringAnsiView(text, 32));
}

void ArtifactKeyBuilder::AddHash(const StringAnsiView& name, const ContentHash& value)
{
    const StringAnsi display = value.ToString();
    AddField(FieldType::ContentHash, name, value.Bytes, sizeof(value.Bytes), display);
}

void ArtifactKeyBuilder::AddKey(const StringAnsiView& name, const ArtifactKey& value)
{
    const StringAnsi display = value.ToString();
    AddField(FieldType::ArtifactKey, name, value.Digest.Bytes, sizeof(value.Digest.Bytes), display);
}

void ArtifactKeyBuilder::AddSortedStrings(const StringAnsiView& name, const Array<StringAnsi>& values)
{
    Array<StringAnsi> sorted(values);
    Sorting::QuickSort(sorted);
    uint32 payloadLength = 4;
    for (const StringAnsi& value : sorted)
        payloadLength += 4 + value.Length();
    Array<byte> payload;
    payload.Resize(payloadLength);
    byte* cursor = payload.Get();
    WriteUInt32(cursor, sorted.Count());
    cursor += 4;
    StringAnsi display;
    for (int32 i = 0; i < sorted.Count(); i++)
    {
        const StringAnsi& value = sorted[i];
        WriteUInt32(cursor, value.Length());
        cursor += 4;
        Platform::MemoryCopy(cursor, value.Get(), value.Length());
        cursor += value.Length();
        if (i != 0)
            display += ',';
        display += value;
    }
    AddField(FieldType::StringCollection, name, payload.Get(), payload.Count(), display);
}

void ArtifactKeyBuilder::AddTarget(const ArtifactTarget& target, ArtifactTargetDimension dimensions)
{
    AddUInt32(StringAnsiView("target-mask"), static_cast<uint32>(dimensions));
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Platform))
        AddString(StringAnsiView("target-platform"), target.Platform);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Architecture))
        AddString(StringAnsiView("target-architecture"), target.Architecture);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Graphics))
        AddString(StringAnsiView("target-graphics"), target.Graphics);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Configuration))
        AddString(StringAnsiView("target-configuration"), target.Configuration);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Quality))
        AddString(StringAnsiView("target-quality"), target.Quality);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::TextureCompression))
        AddString(StringAnsiView("target-texture-compression"), target.TextureCompression);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::AudioCodec))
        AddString(StringAnsiView("target-audio-codec"), target.AudioCodec);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::ShaderCompiler))
        AddString(StringAnsiView("target-shader-compiler"), target.ShaderCompiler);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::Role))
        AddString(StringAnsiView("target-role"), target.Role);
    if (EnumHasAnyFlags(dimensions, ArtifactTargetDimension::FeatureFlags))
        AddSortedStrings(StringAnsiView("target-feature-flags"), target.FeatureFlags);
}

ArtifactKey ArtifactKeyBuilder::Finalize() const
{
    return ArtifactKey(_hasher.Finalize());
}
