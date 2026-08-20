// Copyright (c) Wojciech Figat. All rights reserved.

#include "TextureProcessorSettings.h"

#if COMPILE_WITH_TEXTURE_TOOL

#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include <cmath>

namespace
{
    typedef rapidjson_flax::Document JsonDocument;
    typedef rapidjson_flax::Value JsonValue;

    bool SettingsFail(AssetPipelineDiagnostic& diagnostic, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = TextureProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    bool IsKnown(const StringAnsiView& name, const char* const* fields, int32 count)
    {
        for (int32 i = 0; i < count; i++)
        {
            if (name == fields[i])
                return true;
        }
        return false;
    }

    bool ReadBool(const JsonValue& object, const char* name, bool& value, AssetPipelineDiagnostic& diagnostic)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd())
            return false;
        if (!member->value.IsBool())
            return SettingsFail(diagnostic, String::Format(TEXT("Texture setting '{0}' must be a boolean."), String(StringAnsi(name))));
        value = member->value.GetBool();
        return false;
    }

    bool ReadInt(const JsonValue& object, const char* name, int32& value, AssetPipelineDiagnostic& diagnostic)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd())
            return false;
        if (!member->value.IsInt())
            return SettingsFail(diagnostic, String::Format(TEXT("Texture setting '{0}' must be an integer."), String(StringAnsi(name))));
        value = member->value.GetInt();
        return false;
    }

    bool ReadFloat(const JsonValue& object, const char* name, float& value, AssetPipelineDiagnostic& diagnostic)
    {
        const auto member = object.FindMember(name);
        if (member == object.MemberEnd())
            return false;
        if (!member->value.IsNumber())
            return SettingsFail(diagnostic, String::Format(TEXT("Texture setting '{0}' must be numeric."), String(StringAnsi(name))));
        value = member->value.GetFloat();
        return false;
    }

    const char* TextureTypeName(TextureFormatType value)
    {
        switch (value)
        {
        case TextureFormatType::ColorRGB: return "ColorRGB";
        case TextureFormatType::ColorRGBA: return "ColorRGBA";
        case TextureFormatType::NormalMap: return "NormalMap";
        case TextureFormatType::GrayScale: return "GrayScale";
        case TextureFormatType::HdrRGBA: return "HdrRGBA";
        case TextureFormatType::HdrRGB: return "HdrRGB";
        default: return nullptr;
        }
    }

    bool ParseTextureType(const StringAnsiView& value, TextureFormatType& result)
    {
        if (value == "ColorRGB") result = TextureFormatType::ColorRGB;
        else if (value == "ColorRGBA") result = TextureFormatType::ColorRGBA;
        else if (value == "NormalMap") result = TextureFormatType::NormalMap;
        else if (value == "GrayScale") result = TextureFormatType::GrayScale;
        else if (value == "HdrRGBA") result = TextureFormatType::HdrRGBA;
        else if (value == "HdrRGB") result = TextureFormatType::HdrRGB;
        else return true;
        return false;
    }

    const char* AlphaSourceName(TextureTool::TextureAlphaSource value)
    {
        switch (value)
        {
        case TextureTool::TextureAlphaSource::None: return "None";
        case TextureTool::TextureAlphaSource::InputTextureAlpha: return "InputTextureAlpha";
        case TextureTool::TextureAlphaSource::FromGrayScale: return "FromGrayScale";
        default: return nullptr;
        }
    }

    bool ParseAlphaSource(const StringAnsiView& value, TextureTool::TextureAlphaSource& result)
    {
        if (value == "None") result = TextureTool::TextureAlphaSource::None;
        else if (value == "InputTextureAlpha") result = TextureTool::TextureAlphaSource::InputTextureAlpha;
        else if (value == "FromGrayScale") result = TextureTool::TextureAlphaSource::FromGrayScale;
        else return true;
        return false;
    }

    void AddKey(JsonValue& object, const char* name, JsonValue&& value, JsonDocument::AllocatorType& allocator)
    {
        JsonValue key(name, allocator);
        object.AddMember(key.Move(), value.Move(), allocator);
    }

    void AddBool(JsonValue& object, const char* name, bool value, JsonDocument::AllocatorType& allocator)
    {
        AddKey(object, name, JsonValue(value), allocator);
    }

    void AddInt(JsonValue& object, const char* name, int32 value, JsonDocument::AllocatorType& allocator)
    {
        AddKey(object, name, JsonValue(value), allocator);
    }

    void AddFloat(JsonValue& object, const char* name, float value, JsonDocument::AllocatorType& allocator)
    {
        JsonValue number;
        number.SetFloat(value);
        AddKey(object, name, MoveTemp(number), allocator);
    }

    void AddString(JsonValue& object, const char* name, const StringAnsiView& value, JsonDocument::AllocatorType& allocator)
    {
        JsonValue text(value.Get(), value.Length(), allocator);
        AddKey(object, name, MoveTemp(text), allocator);
    }

    bool IsPlatformNameValid(const StringAnsiView& value)
    {
        if (value.IsEmpty())
            return false;
        for (int32 i = 0; i < value.Length(); i++)
        {
            const char c = value[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
        }
        return true;
    }
}

const String& TextureProcessorSettings::ProcessorID()
{
    static const String value(TEXT("Flax.Texture"));
    return value;
}

TextureProcessorSettings TextureProcessorSettings::Defaults()
{
    TextureProcessorSettings result;
    result.Import = TextureTool::Options();
    return result;
}

AssetProcessorSettingsSchema TextureProcessorSettings::Schema()
{
    AssetProcessorSettingsSchema schema;
    schema.ProcessorID = ProcessorID();
    schema.CurrentVersion = CurrentVersion;
    schema.ImplementationVersion = TEXT("texture-settings-v2");
    schema.Upgrade = &Upgrade;
    AssetPipelineDiagnostic diagnostic;
    Defaults().ToJson(schema.NormalizedDefaults, diagnostic);
    ASSERT(diagnostic.Code == AssetPipelineDiagnosticCode::None);
    return schema;
}

bool TextureProcessorSettings::Validate(AssetPipelineDiagnostic& diagnostic) const
{
    diagnostic = AssetPipelineDiagnostic();
    if (!TextureTypeName(Import.Type))
        return SettingsFail(diagnostic, TEXT("Texture type is unsupported."));
    if (!AlphaSourceName(Import.AlphaSource))
        return SettingsFail(diagnostic, TEXT("Texture alpha source is unsupported."));
    if (!std::isfinite(Import.Scale) || Import.Scale < 0.0001f || Import.Scale > 1000.0f)
        return SettingsFail(diagnostic, TEXT("Texture scale must be finite and within 0.0001 through 1000."));
    if (Import.MaxSize < 1 || Import.MaxSize > GPU_MAX_TEXTURE_SIZE || Import.SizeX < 1 || Import.SizeX > GPU_MAX_TEXTURE_SIZE ||
        Import.SizeY < 1 || Import.SizeY > GPU_MAX_TEXTURE_SIZE)
        return SettingsFail(diagnostic, TEXT("Texture maximum and resize dimensions are outside the supported range."));
    if (!std::isfinite(Import.PreserveAlphaCoverageReference) || Import.PreserveAlphaCoverageReference < 0.0f || Import.PreserveAlphaCoverageReference > 1.0f)
        return SettingsFail(diagnostic, TEXT("Texture alpha coverage reference must be between zero and one."));
    if (Import.TextureGroup < -1 || static_cast<uint32>(Import.InternalFormat) >= static_cast<uint32>(PixelFormat::MAX))
        return SettingsFail(diagnostic, TEXT("Texture group or internal pixel format is invalid."));
    for (const Sprite& sprite : Import.Sprites)
    {
        const Rectangle& area = sprite.Area;
        if (sprite.Name.IsEmpty() || area.Location.IsNaN() || area.Location.IsInfinity() || area.Size.IsNaN() || area.Size.IsInfinity() ||
            area.Location.X < 0.0f || area.Location.Y < 0.0f || area.Size.X <= 0.0f || area.Size.Y <= 0.0f ||
            area.Location.X + area.Size.X > 1.0f || area.Location.Y + area.Size.Y > 1.0f)
            return SettingsFail(diagnostic, TEXT("Texture atlas sprite name or normalized area is invalid."));
    }
    for (const auto& entry : PlatformOverrides)
    {
        const TextureProcessorPlatformOverride& value = entry.Value;
        if (!IsPlatformNameValid(entry.Key) || value.MaxSize < 0 || value.MaxSize > GPU_MAX_TEXTURE_SIZE ||
            static_cast<uint32>(value.InternalFormat) >= static_cast<uint32>(PixelFormat::MAX) ||
            (value.TextureGroup != MIN_int32 && value.TextureGroup < -1))
            return SettingsFail(diagnostic, TEXT("Texture platform override name or value is invalid."));
    }
    return false;
}

TextureProcessorSettings TextureProcessorSettings::FromLegacyOptions(const TextureTool::Options& options)
{
    TextureProcessorSettings result;
    result.Import = options;
    result.Import.InternalLoad.Unbind();
    return result;
}

TextureTool::Options TextureProcessorSettings::ToImportOptions(const StringAnsiView& platform) const
{
    TextureTool::Options result = Import;
    StringAnsi key(platform);
    key = key.ToLower();
    const TextureProcessorPlatformOverride* overrideValue = PlatformOverrides.TryGet(key);
    if (overrideValue)
    {
        if (overrideValue->HasCompression)
            result.Compress = overrideValue->Compress;
        if (overrideValue->MaxSize > 0)
            result.MaxSize = overrideValue->MaxSize;
        if (overrideValue->InternalFormat != PixelFormat::Unknown)
            result.InternalFormat = overrideValue->InternalFormat;
        if (overrideValue->TextureGroup != MIN_int32)
            result.TextureGroup = overrideValue->TextureGroup;
    }
    result.InternalLoad.Unbind();
    return result;
}

bool TextureProcessorSettings::Parse(const StringAnsiView& json, int32 version, TextureProcessorSettings& result, AssetPipelineDiagnostic& diagnostic)
{
    result = Defaults();
    diagnostic = AssetPipelineDiagnostic();
    if (version != CurrentVersion)
        return SettingsFail(diagnostic, version < CurrentVersion ? TEXT("Texture settings require a tracked schema upgrade.") : TEXT("Texture settings were written by a newer unsupported schema."));

    JsonDocument document;
    document.Parse(json.Get(), json.Length());
    if (document.HasParseError())
    {
        diagnostic.Location.Column = static_cast<int32>(document.GetErrorOffset());
        return SettingsFail(diagnostic, TEXT("Texture settings JSON parsing failed."));
    }
    CanonicalJsonError canonicalError;
    if (CanonicalJsonWriter::Validate(document, canonicalError) || !document.IsObject())
        return SettingsFail(diagnostic, TEXT("Texture settings must be a valid object without duplicate keys or non-finite numbers."));

    const auto type = document.FindMember("type");
    if (type != document.MemberEnd())
    {
        if (!type->value.IsString() || ParseTextureType(StringAnsiView(type->value.GetString(), type->value.GetStringLength()), result.Import.Type))
            return SettingsFail(diagnostic, TEXT("Texture setting 'type' is invalid."));
    }
    const auto compression = document.FindMember("compression");
    if (compression != document.MemberEnd())
    {
        if (!compression->value.IsString())
            return SettingsFail(diagnostic, TEXT("Texture setting 'compression' must be a string."));
        const StringAnsiView value(compression->value.GetString(), compression->value.GetStringLength());
        if (value == "Default") result.Import.Compress = true;
        else if (value == "Uncompressed") result.Import.Compress = false;
        else return SettingsFail(diagnostic, TEXT("Texture setting 'compression' must be Default or Uncompressed."));
    }
    const auto alphaSource = document.FindMember("alphaSource");
    if (alphaSource != document.MemberEnd())
    {
        if (!alphaSource->value.IsString() || ParseAlphaSource(StringAnsiView(alphaSource->value.GetString(), alphaSource->value.GetStringLength()), result.Import.AlphaSource))
            return SettingsFail(diagnostic, TEXT("Texture setting 'alphaSource' is invalid."));
    }
    if (ReadBool(document, "atlas", result.Import.IsAtlas, diagnostic) ||
        ReadBool(document, "neverStream", result.Import.NeverStream, diagnostic) ||
        ReadBool(document, "independentChannels", result.Import.IndependentChannels, diagnostic) ||
        ReadBool(document, "srgb", result.Import.sRGB, diagnostic) ||
        ReadBool(document, "alphaIsTransparency", result.Import.AlphaIsTransparency, diagnostic) ||
        ReadBool(document, "generateMipMaps", result.Import.GenerateMipMaps, diagnostic) ||
        ReadBool(document, "flipY", result.Import.FlipY, diagnostic) ||
        ReadBool(document, "flipX", result.Import.FlipX, diagnostic) ||
        ReadBool(document, "reconstructZ", result.Import.ReconstructZChannel, diagnostic) ||
        ReadFloat(document, "scale", result.Import.Scale, diagnostic) ||
        ReadInt(document, "maxSize", result.Import.MaxSize, diagnostic) ||
        ReadInt(document, "textureGroup", result.Import.TextureGroup, diagnostic))
        return true;

    const auto internalFormat = document.FindMember("internalFormat");
    if (internalFormat != document.MemberEnd())
    {
        if (!internalFormat->value.IsUint() || internalFormat->value.GetUint() >= static_cast<uint32>(PixelFormat::MAX))
            return SettingsFail(diagnostic, TEXT("Texture setting 'internalFormat' is invalid."));
        result.Import.InternalFormat = static_cast<PixelFormat>(internalFormat->value.GetUint());
    }
    const auto invert = document.FindMember("invertChannels");
    if (invert != document.MemberEnd())
    {
        if (!invert->value.IsObject())
            return SettingsFail(diagnostic, TEXT("Texture setting 'invertChannels' must be an object."));
        if (ReadBool(invert->value, "red", result.Import.InvertRedChannel, diagnostic) ||
            ReadBool(invert->value, "green", result.Import.InvertGreenChannel, diagnostic) ||
            ReadBool(invert->value, "blue", result.Import.InvertBlueChannel, diagnostic) ||
            ReadBool(invert->value, "alpha", result.Import.InvertAlphaChannel, diagnostic))
            return true;
    }
    const auto resize = document.FindMember("resize");
    if (resize != document.MemberEnd())
    {
        if (!resize->value.IsObject())
            return SettingsFail(diagnostic, TEXT("Texture setting 'resize' must be an object."));
        if (ReadBool(resize->value, "enabled", result.Import.Resize, diagnostic) ||
            ReadBool(resize->value, "keepAspectRatio", result.Import.KeepAspectRatio, diagnostic) ||
            ReadInt(resize->value, "width", result.Import.SizeX, diagnostic) ||
            ReadInt(resize->value, "height", result.Import.SizeY, diagnostic))
            return true;
    }
    const auto alphaCoverage = document.FindMember("preserveAlphaCoverage");
    if (alphaCoverage != document.MemberEnd())
    {
        if (!alphaCoverage->value.IsObject())
            return SettingsFail(diagnostic, TEXT("Texture setting 'preserveAlphaCoverage' must be an object."));
        if (ReadBool(alphaCoverage->value, "enabled", result.Import.PreserveAlphaCoverage, diagnostic) ||
            ReadFloat(alphaCoverage->value, "reference", result.Import.PreserveAlphaCoverageReference, diagnostic))
            return true;
    }

    const auto sprites = document.FindMember("sprites");
    if (sprites != document.MemberEnd())
    {
        if (!sprites->value.IsArray() || sprites->value.Size() > 4096)
            return SettingsFail(diagnostic, TEXT("Texture setting 'sprites' must be a bounded array."));
        result.Import.Sprites.Clear();
        for (const JsonValue& item : sprites->value.GetArray())
        {
            if (!item.IsObject())
                return SettingsFail(diagnostic, TEXT("Texture sprite entry must be an object."));
            const auto name = item.FindMember("name");
            const auto position = item.FindMember("position");
            const auto size = item.FindMember("size");
            if (name == item.MemberEnd() || !name->value.IsString() || position == item.MemberEnd() || !position->value.IsArray() || position->value.Size() != 2 ||
                size == item.MemberEnd() || !size->value.IsArray() || size->value.Size() != 2 ||
                !position->value[0].IsNumber() || !position->value[1].IsNumber() || !size->value[0].IsNumber() || !size->value[1].IsNumber())
                return SettingsFail(diagnostic, TEXT("Texture sprite entry has an invalid name, position, or size."));
            Sprite sprite;
            sprite.Name = String(StringAnsiView(name->value.GetString(), name->value.GetStringLength()));
            sprite.Area.Location = Float2(position->value[0].GetFloat(), position->value[1].GetFloat());
            sprite.Area.Size = Float2(size->value[0].GetFloat(), size->value[1].GetFloat());
            result.Import.Sprites.Add(MoveTemp(sprite));
        }
    }

    const auto platformOverrides = document.FindMember("platformOverrides");
    if (platformOverrides != document.MemberEnd())
    {
        if (!platformOverrides->value.IsObject())
            return SettingsFail(diagnostic, TEXT("Texture setting 'platformOverrides' must be an object."));
        result.PlatformOverrides.Clear();
        for (auto member = platformOverrides->value.MemberBegin(); member != platformOverrides->value.MemberEnd(); ++member)
        {
            StringAnsi platform(member->name.GetString(), member->name.GetStringLength());
            platform = platform.ToLower();
            if (!IsPlatformNameValid(platform) || !member->value.IsObject() || result.PlatformOverrides.ContainsKey(platform))
                return SettingsFail(diagnostic, TEXT("Texture platform override key or value is invalid."));
            TextureProcessorPlatformOverride overrideValue;
            const auto overrideCompression = member->value.FindMember("compression");
            if (overrideCompression != member->value.MemberEnd())
            {
                if (!overrideCompression->value.IsString())
                    return SettingsFail(diagnostic, TEXT("Texture platform compression override must be a string."));
                const StringAnsiView value(overrideCompression->value.GetString(), overrideCompression->value.GetStringLength());
                if (value == "Inherit") overrideValue.HasCompression = false;
                else if (value == "Default") { overrideValue.HasCompression = true; overrideValue.Compress = true; }
                else if (value == "Uncompressed") { overrideValue.HasCompression = true; overrideValue.Compress = false; }
                else return SettingsFail(diagnostic, TEXT("Texture platform compression override is invalid."));
            }
            if (ReadInt(member->value, "maxSize", overrideValue.MaxSize, diagnostic) ||
                ReadInt(member->value, "textureGroup", overrideValue.TextureGroup, diagnostic))
                return true;
            const auto format = member->value.FindMember("internalFormat");
            if (format != member->value.MemberEnd())
            {
                if (!format->value.IsUint() || format->value.GetUint() >= static_cast<uint32>(PixelFormat::MAX))
                    return SettingsFail(diagnostic, TEXT("Texture platform internal format override is invalid."));
                overrideValue.InternalFormat = static_cast<PixelFormat>(format->value.GetUint());
            }
            result.PlatformOverrides.Add(MoveTemp(platform), overrideValue);
        }
    }

    const char* known[] =
    {
        "type", "atlas", "neverStream", "compression", "independentChannels", "srgb", "alphaSource",
        "alphaIsTransparency", "generateMipMaps", "flipY", "flipX", "invertChannels", "reconstructZ", "scale",
        "maxSize", "resize", "preserveAlphaCoverage", "textureGroup", "internalFormat", "sprites", "platformOverrides"
    };
    for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member)
    {
        const StringAnsiView name(member->name.GetString(), member->name.GetStringLength());
        if (IsKnown(name, known, ARRAY_COUNT(known)))
            continue;
        StringAnsi fragment;
        if (CanonicalJsonWriter::Write(member->value, fragment, canonicalError))
            return SettingsFail(diagnostic, TEXT("Texture settings contain an unknown field that cannot be preserved."));
        result.UnknownFields.Add(StringAnsi(name), MoveTemp(fragment));
    }
    return result.Validate(diagnostic);
}

bool TextureProcessorSettings::Upgrade(int32 fromVersion, const StringAnsiView& input, StringAnsi& output, AssetPipelineDiagnostic& diagnostic)
{
    output.Clear();
    diagnostic = AssetPipelineDiagnostic();
    if (fromVersion != 1)
        return SettingsFail(diagnostic, TEXT("Texture settings have no deterministic migration from this version."));
    JsonDocument document;
    document.Parse(input.Get(), input.Length());
    CanonicalJsonError error;
    if (document.HasParseError() || !document.IsObject() || CanonicalJsonWriter::Validate(document, error))
        return SettingsFail(diagnostic, TEXT("Legacy texture settings are malformed."));

    if (document.HasMember("Type") || document.HasMember("Compress") || document.HasMember("GenerateMipMaps"))
    {
        const auto sprites = document.FindMember("Sprites");
        if (sprites != document.MemberEnd())
        {
            if (!sprites->value.IsArray() || sprites->value.Size() > 4096)
                return SettingsFail(diagnostic, TEXT("Legacy texture sprite settings are invalid."));
            for (const JsonValue& sprite : sprites->value.GetArray())
            {
                if (!sprite.IsObject())
                    return SettingsFail(diagnostic, TEXT("Legacy texture sprite settings are invalid."));
                const auto position = sprite.FindMember("Position");
                const auto size = sprite.FindMember("Size");
                if (position == sprite.MemberEnd() || !position->value.IsObject() ||
                    size == sprite.MemberEnd() || !size->value.IsObject())
                    return SettingsFail(diagnostic, TEXT("Legacy texture sprite settings are invalid."));
            }
        }
        TextureTool::Options legacy;
        legacy.Deserialize(document, nullptr);
        TextureProcessorSettings settings = FromLegacyOptions(legacy);
        return settings.ToJson(output, diagnostic);
    }
    TextureProcessorSettings settings;
    if (Parse(input, CurrentVersion, settings, diagnostic))
        return true;
    return settings.ToJson(output, diagnostic);
}

bool TextureProcessorSettings::ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const
{
    json.Clear();
    if (Validate(diagnostic))
        return true;
    JsonDocument document;
    document.SetObject();
    auto& allocator = document.GetAllocator();
    AddString(document, "type", StringAnsiView(TextureTypeName(Import.Type)), allocator);
    AddBool(document, "atlas", Import.IsAtlas, allocator);
    AddBool(document, "neverStream", Import.NeverStream, allocator);
    AddString(document, "compression", Import.Compress ? StringAnsiView("Default") : StringAnsiView("Uncompressed"), allocator);
    AddBool(document, "independentChannels", Import.IndependentChannels, allocator);
    AddBool(document, "srgb", Import.sRGB, allocator);
    AddString(document, "alphaSource", StringAnsiView(AlphaSourceName(Import.AlphaSource)), allocator);
    AddBool(document, "alphaIsTransparency", Import.AlphaIsTransparency, allocator);
    AddBool(document, "generateMipMaps", Import.GenerateMipMaps, allocator);
    AddBool(document, "flipY", Import.FlipY, allocator);
    AddBool(document, "flipX", Import.FlipX, allocator);

    JsonValue invert(rapidjson::kObjectType);
    AddBool(invert, "red", Import.InvertRedChannel, allocator);
    AddBool(invert, "green", Import.InvertGreenChannel, allocator);
    AddBool(invert, "blue", Import.InvertBlueChannel, allocator);
    AddBool(invert, "alpha", Import.InvertAlphaChannel, allocator);
    AddKey(document, "invertChannels", MoveTemp(invert), allocator);
    AddBool(document, "reconstructZ", Import.ReconstructZChannel, allocator);
    AddFloat(document, "scale", Import.Scale, allocator);
    AddInt(document, "maxSize", Import.MaxSize, allocator);

    JsonValue resize(rapidjson::kObjectType);
    AddBool(resize, "enabled", Import.Resize, allocator);
    AddBool(resize, "keepAspectRatio", Import.KeepAspectRatio, allocator);
    AddInt(resize, "width", Import.SizeX, allocator);
    AddInt(resize, "height", Import.SizeY, allocator);
    AddKey(document, "resize", MoveTemp(resize), allocator);

    JsonValue alphaCoverage(rapidjson::kObjectType);
    AddBool(alphaCoverage, "enabled", Import.PreserveAlphaCoverage, allocator);
    AddFloat(alphaCoverage, "reference", Import.PreserveAlphaCoverageReference, allocator);
    AddKey(document, "preserveAlphaCoverage", MoveTemp(alphaCoverage), allocator);
    AddInt(document, "textureGroup", Import.TextureGroup, allocator);
    AddInt(document, "internalFormat", static_cast<int32>(Import.InternalFormat), allocator);

    JsonValue sprites(rapidjson::kArrayType);
    for (const Sprite& sprite : Import.Sprites)
    {
        JsonValue item(rapidjson::kObjectType);
        AddString(item, "name", StringAnsi(sprite.Name), allocator);
        JsonValue position(rapidjson::kArrayType);
        position.PushBack(sprite.Area.Location.X, allocator);
        position.PushBack(sprite.Area.Location.Y, allocator);
        AddKey(item, "position", MoveTemp(position), allocator);
        JsonValue size(rapidjson::kArrayType);
        size.PushBack(sprite.Area.Size.X, allocator);
        size.PushBack(sprite.Area.Size.Y, allocator);
        AddKey(item, "size", MoveTemp(size), allocator);
        sprites.PushBack(item.Move(), allocator);
    }
    AddKey(document, "sprites", MoveTemp(sprites), allocator);

    JsonValue platformOverrides(rapidjson::kObjectType);
    for (const auto& entry : PlatformOverrides)
    {
        JsonValue value(rapidjson::kObjectType);
        const char* compression = !entry.Value.HasCompression ? "Inherit" : entry.Value.Compress ? "Default" : "Uncompressed";
        AddString(value, "compression", StringAnsiView(compression), allocator);
        AddInt(value, "maxSize", entry.Value.MaxSize, allocator);
        AddInt(value, "internalFormat", static_cast<int32>(entry.Value.InternalFormat), allocator);
        if (entry.Value.TextureGroup != MIN_int32)
            AddInt(value, "textureGroup", entry.Value.TextureGroup, allocator);
        JsonValue key(entry.Key.Get(), entry.Key.Length(), allocator);
        platformOverrides.AddMember(key.Move(), value.Move(), allocator);
    }
    AddKey(document, "platformOverrides", MoveTemp(platformOverrides), allocator);

    for (const auto& entry : UnknownFields)
    {
        JsonDocument fragment;
        fragment.Parse(entry.Value.Get(), entry.Value.Length());
        if (fragment.HasParseError())
            return SettingsFail(diagnostic, TEXT("Texture settings contain a malformed preserved unknown field."));
        JsonValue key(entry.Key.Get(), entry.Key.Length(), allocator);
        JsonValue value;
        value.CopyFrom(fragment, allocator);
        document.AddMember(key.Move(), value.Move(), allocator);
    }
    CanonicalJsonError error;
    if (CanonicalJsonWriter::Write(document, json, error))
        return SettingsFail(diagnostic, TEXT("Texture settings cannot be canonicalized."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void TextureProcessorSettings::GetInspectorDescriptors(Array<TextureProcessorSettingDescriptor>& descriptors)
{
    descriptors.Clear();
    auto add = [&descriptors](const char* path, const Char* displayName, const Char* category, const char* type)
    {
        TextureProcessorSettingDescriptor descriptor;
        descriptor.Path = path;
        descriptor.DisplayName = displayName;
        descriptor.Category = category;
        descriptor.ValueType = type;
        descriptors.Add(MoveTemp(descriptor));
    };
    auto addRange = [&descriptors](const char* path, const Char* displayName, const Char* category, const char* type, double minimum, double maximum)
    {
        TextureProcessorSettingDescriptor descriptor;
        descriptor.Path = path;
        descriptor.DisplayName = displayName;
        descriptor.Category = category;
        descriptor.ValueType = type;
        descriptor.Minimum = minimum;
        descriptor.Maximum = maximum;
        descriptor.HasRange = true;
        descriptors.Add(MoveTemp(descriptor));
    };
    add("type", TEXT("Texture Type"), TEXT("Format"), "enum");
    add("atlas", TEXT("Sprite Atlas"), TEXT("Format"), "bool");
    add("compression", TEXT("Compression"), TEXT("Format"), "enum");
    add("independentChannels", TEXT("Independent Channels"), TEXT("Format"), "bool");
    add("srgb", TEXT("sRGB"), TEXT("Color"), "bool");
    add("alphaSource", TEXT("Alpha Source"), TEXT("Color"), "enum");
    add("alphaIsTransparency", TEXT("Alpha Is Transparency"), TEXT("Color"), "bool");
    add("generateMipMaps", TEXT("Generate Mip Maps"), TEXT("Mip Maps"), "bool");
    add("preserveAlphaCoverage.enabled", TEXT("Preserve Alpha Coverage"), TEXT("Mip Maps"), "bool");
    addRange("preserveAlphaCoverage.reference", TEXT("Alpha Coverage Reference"), TEXT("Mip Maps"), "float", 0.0, 1.0);
    add("neverStream", TEXT("Never Stream"), TEXT("Streaming"), "bool");
    addRange("textureGroup", TEXT("Texture Group"), TEXT("Streaming"), "int", -1.0, MAX_int32);
    add("flipX", TEXT("Flip X"), TEXT("Transform"), "bool");
    add("flipY", TEXT("Flip Y"), TEXT("Transform"), "bool");
    add("invertChannels.red", TEXT("Invert Red"), TEXT("Transform"), "bool");
    add("invertChannels.green", TEXT("Invert Green"), TEXT("Transform"), "bool");
    add("invertChannels.blue", TEXT("Invert Blue"), TEXT("Transform"), "bool");
    add("invertChannels.alpha", TEXT("Invert Alpha"), TEXT("Transform"), "bool");
    add("reconstructZ", TEXT("Reconstruct Z"), TEXT("Transform"), "bool");
    addRange("scale", TEXT("Scale"), TEXT("Size"), "float", 0.0001, 1000.0);
    addRange("maxSize", TEXT("Maximum Size"), TEXT("Size"), "int", 1.0, GPU_MAX_TEXTURE_SIZE);
    add("resize.enabled", TEXT("Resize"), TEXT("Size"), "bool");
    add("resize.keepAspectRatio", TEXT("Keep Aspect Ratio"), TEXT("Size"), "bool");
    addRange("resize.width", TEXT("Width"), TEXT("Size"), "int", 1.0, GPU_MAX_TEXTURE_SIZE);
    addRange("resize.height", TEXT("Height"), TEXT("Size"), "int", 1.0, GPU_MAX_TEXTURE_SIZE);
    add("internalFormat", TEXT("Internal Format"), TEXT("Advanced"), "pixel-format");
    add("sprites", TEXT("Sprites"), TEXT("Atlas"), "array");
    add("platformOverrides", TEXT("Platform Overrides"), TEXT("Platforms"), "object");
}

#endif
