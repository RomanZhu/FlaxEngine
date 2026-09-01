// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessorSettings.h"
#include "Engine/Tools/TextureTool/TextureTool.h"

#if COMPILE_WITH_TEXTURE_TOOL

/// <summary>Optional per-platform texture setting overrides. Unset values inherit the root settings.</summary>
struct FLAXENGINE_API TextureProcessorPlatformOverride
{
    bool HasCompression = false;
    bool Compress = true;
    int32 MaxSize = 0;
    PixelFormat InternalFormat = PixelFormat::Unknown;
    int32 TextureGroup = MIN_int32;
};

/// <summary>Editor-facing description of one tracked texture setting.</summary>
struct FLAXENGINE_API TextureProcessorSettingDescriptor
{
    StringAnsi Path;
    String DisplayName;
    String Category;
    StringAnsi ValueType;
    double Minimum = 0.0;
    double Maximum = 0.0;
    bool HasRange = false;
};

/// <summary>Versioned tracked texture processor settings stored in an asset sidecar.</summary>
struct FLAXENGINE_API TextureProcessorSettings
{
    static constexpr int32 CurrentVersion = 2;

    TextureTool::Options Import;
    Dictionary<StringAnsi, TextureProcessorPlatformOverride> PlatformOverrides;
    Dictionary<StringAnsi, StringAnsi> UnknownFields;

    static const String& ProcessorID();
    static TextureProcessorSettings Defaults();
    static AssetProcessorSettingsSchema Schema();

    /// <summary>Parses the current normalized settings schema. Returns true on failure.</summary>
    static bool Parse(const StringAnsiView& json, int32 version, TextureProcessorSettings& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Serializes all defaults and authored values as canonical JSON. Returns true on failure.</summary>
    bool ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Validates all author-controlled values. Returns true on failure.</summary>
    bool Validate(AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Copies every persistable legacy importer option into tracked settings.</summary>
    static TextureProcessorSettings FromLegacyOptions(const TextureTool::Options& options);

    /// <summary>Applies a matching platform override to a copy of the legacy importer options.</summary>
    TextureTool::Options ToImportOptions(const StringAnsiView& platform) const;

    static void GetInspectorDescriptors(Array<TextureProcessorSettingDescriptor>& descriptors);
};

#endif
