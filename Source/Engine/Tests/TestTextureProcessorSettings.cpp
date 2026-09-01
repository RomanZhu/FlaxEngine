// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_TESTS && COMPILE_WITH_TEXTURE_TOOL

#include "Engine/Content/Build/Processors/TextureProcessorSettings.h"
#include "Engine/Content/AssetDatabase/AssetMeta.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    void CheckOptionsEqual(const TextureTool::Options& a, const TextureTool::Options& b)
    {
        CHECK(a.Type == b.Type);
        CHECK(a.IsAtlas == b.IsAtlas);
        CHECK(a.NeverStream == b.NeverStream);
        CHECK(a.Compress == b.Compress);
        CHECK(a.IndependentChannels == b.IndependentChannels);
        CHECK(a.sRGB == b.sRGB);
        CHECK(a.AlphaSource == b.AlphaSource);
        CHECK(a.AlphaIsTransparency == b.AlphaIsTransparency);
        CHECK(a.GenerateMipMaps == b.GenerateMipMaps);
        CHECK(a.FlipY == b.FlipY);
        CHECK(a.FlipX == b.FlipX);
        CHECK(a.InvertRedChannel == b.InvertRedChannel);
        CHECK(a.InvertGreenChannel == b.InvertGreenChannel);
        CHECK(a.InvertBlueChannel == b.InvertBlueChannel);
        CHECK(a.InvertAlphaChannel == b.InvertAlphaChannel);
        CHECK(a.ReconstructZChannel == b.ReconstructZChannel);
        CHECK(a.Scale == b.Scale);
        CHECK(a.MaxSize == b.MaxSize);
        CHECK(a.Resize == b.Resize);
        CHECK(a.KeepAspectRatio == b.KeepAspectRatio);
        CHECK(a.SizeX == b.SizeX);
        CHECK(a.SizeY == b.SizeY);
        CHECK(a.PreserveAlphaCoverage == b.PreserveAlphaCoverage);
        CHECK(a.PreserveAlphaCoverageReference == b.PreserveAlphaCoverageReference);
        CHECK(a.TextureGroup == b.TextureGroup);
        CHECK(a.InternalFormat == b.InternalFormat);
        REQUIRE(a.Sprites.Count() == b.Sprites.Count());
        for (int32 i = 0; i < a.Sprites.Count(); i++)
        {
            CHECK(a.Sprites[i].Name == b.Sprites[i].Name);
            CHECK(a.Sprites[i].Area == b.Sprites[i].Area);
        }
    }
}

TEST_CASE("Texture processor settings round-trip every legacy importer option and platform override")
{
    TextureTool::Options legacy;
    legacy.Type = TextureFormatType::NormalMap;
    legacy.IsAtlas = true;
    legacy.NeverStream = true;
    legacy.Compress = false;
    legacy.IndependentChannels = true;
    legacy.sRGB = true;
    legacy.AlphaSource = TextureTool::TextureAlphaSource::FromGrayScale;
    legacy.AlphaIsTransparency = true;
    legacy.GenerateMipMaps = false;
    legacy.FlipY = true;
    legacy.FlipX = true;
    legacy.InvertRedChannel = true;
    legacy.InvertGreenChannel = true;
    legacy.InvertBlueChannel = true;
    legacy.InvertAlphaChannel = true;
    legacy.ReconstructZChannel = true;
    legacy.Scale = 0.75f;
    legacy.MaxSize = 4096;
    legacy.Resize = true;
    legacy.KeepAspectRatio = true;
    legacy.SizeX = 512;
    legacy.SizeY = 256;
    legacy.PreserveAlphaCoverage = true;
    legacy.PreserveAlphaCoverageReference = 0.37f;
    legacy.TextureGroup = 3;
    legacy.InternalFormat = PixelFormat::R8G8B8A8_UNorm;
    Sprite sprite;
    sprite.Name = TEXT("Main");
    sprite.Area = Rectangle(0.1f, 0.2f, 0.3f, 0.4f);
    legacy.Sprites.Add(sprite);
    legacy.InternalLoad.Bind([](TextureData&) { return false; });

    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(legacy);
    CHECK_FALSE(settings.Import.InternalLoad.IsBinded());
    TextureProcessorPlatformOverride android;
    android.HasCompression = true;
    android.Compress = true;
    android.MaxSize = 1024;
    android.InternalFormat = PixelFormat::BC3_UNorm;
    android.TextureGroup = 5;
    settings.PlatformOverrides.Add("android", android);
    settings.UnknownFields.Add("futureSetting", "{\"enabled\":true}\n");

    AssetPipelineDiagnostic diagnostic;
    StringAnsi json;
    REQUIRE_FALSE(settings.ToJson(json, diagnostic));
    TextureProcessorSettings parsed;
    REQUIRE_FALSE(TextureProcessorSettings::Parse(json, TextureProcessorSettings::CurrentVersion, parsed, diagnostic));
    CheckOptionsEqual(settings.Import, parsed.Import);
    REQUIRE(parsed.PlatformOverrides.Count() == 1);
    const TextureProcessorPlatformOverride* parsedAndroid = parsed.PlatformOverrides.TryGet("android");
    REQUIRE(parsedAndroid);
    CHECK(parsedAndroid->HasCompression);
    CHECK(parsedAndroid->Compress);
    CHECK(parsedAndroid->MaxSize == 1024);
    CHECK(parsedAndroid->InternalFormat == PixelFormat::BC3_UNorm);
    CHECK(parsedAndroid->TextureGroup == 5);
    CHECK(parsed.UnknownFields.ContainsKey("futureSetting"));
    StringAnsi repeated;
    REQUIRE_FALSE(parsed.ToJson(repeated, diagnostic));
    CHECK(repeated == json);

    const TextureTool::Options windows = parsed.ToImportOptions(StringAnsiView("Windows"));
    CheckOptionsEqual(parsed.Import, windows);
    const TextureTool::Options androidOptions = parsed.ToImportOptions(StringAnsiView("Android"));
    CHECK(androidOptions.Compress);
    CHECK(androidOptions.MaxSize == 1024);
    CHECK(androidOptions.InternalFormat == PixelFormat::BC3_UNorm);
    CHECK(androidOptions.TextureGroup == 5);
}

TEST_CASE("Texture processor settings accept Stone meta and preserve explicit defaults")
{
    const StringAnsi stoneMeta = R"({
        "fileFormatVersion":2,
        "guid":"36f15f0c4b354af88ba2f72f6cb82e22",
        "folderAsset":false,
        "importer":{"id":"Flax.Texture","version":2,"settings":{
            "type":"NormalMap","srgb":false,"generateMipMaps":true,"maxSize":4096,
            "compression":"Default","platformOverrides":{}}},
        "objectIds":{"main":{"fileId":1,"type":"FlaxEngine.Texture"}},
        "labels":["environment","stone"],"userData":{}})";
    AssetPipelineDiagnostic diagnostic;
    AssetMeta meta;
    REQUIRE_FALSE(AssetMeta::Parse(stoneMeta, TEXT("Stone.png.meta"), meta, diagnostic));
    TextureProcessorSettings settings;
    REQUIRE_FALSE(TextureProcessorSettings::Parse(meta.Processor.SettingsJson, meta.Processor.SettingsVersion, settings, diagnostic));
    CHECK(settings.Import.Type == TextureFormatType::NormalMap);
    CHECK_FALSE(settings.Import.sRGB);
    CHECK(settings.Import.GenerateMipMaps);
    CHECK(settings.Import.MaxSize == 4096);
    CHECK(settings.Import.Compress);

    AssetProcessorSettingsSchema schema = TextureProcessorSettings::Schema();
    AssetMeta created;
    REQUIRE_FALSE(schema.InitializeNewMeta(created, diagnostic));
    const StringAnsi persistedDefaults = created.Processor.SettingsJson;
    CHECK(created.Processor.SettingsVersion == TextureProcessorSettings::CurrentVersion);
    schema.NormalizedDefaults = "{\"changedDefault\":true}";
    CHECK(created.Processor.SettingsJson == persistedDefaults);

    Array<TextureProcessorSettingDescriptor> descriptors;
    TextureProcessorSettings::GetInspectorDescriptors(descriptors);
    CHECK(descriptors.Count() >= 25);
}

TEST_CASE("Texture processor settings reject invalid values")
{
    AssetPipelineDiagnostic diagnostic;
    TextureProcessorSettings parsed;
    TextureProcessorSettings invalid = TextureProcessorSettings::Defaults();
    invalid.Import.MaxSize = 0;
    CHECK(invalid.Validate(diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidMeta);
    CHECK(TextureProcessorSettings::Parse(StringAnsiView("{\"type\":\"Unknown\"}"), TextureProcessorSettings::CurrentVersion, parsed, diagnostic));
}

#endif
