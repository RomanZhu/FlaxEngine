// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessorSettings.h"
#include "Engine/Tools/ModelTool/ModelTool.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

/// <summary>Versioned tracked settings for canonical model, mesh, animation, LOD, and SDF sources.</summary>
struct FLAXENGINE_API ModelProcessorSettings
{
    static constexpr int32 CurrentVersion = 1;

    ModelTool::Options Import;

    static const String& ProcessorID();
    static ModelProcessorSettings Defaults();
    static AssetProcessorSettingsSchema Schema();

    /// <summary>Parses the normalized grouped settings object. Returns true on failure.</summary>
    static bool Parse(const StringAnsiView& json, int32 version, ModelProcessorSettings& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Serializes all current model importer options into canonical grouped JSON.</summary>
    bool ToJson(StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Serializes one settings group for independently keyed outputs.</summary>
    bool ToSectionJson(const StringAnsiView& section, StringAnsi& json, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Validates values before they reach the compatibility importer.</summary>
    bool Validate(AssetPipelineDiagnostic& diagnostic) const;

    static ModelProcessorSettings FromLegacyOptions(const ModelTool::Options& options);
};

#endif
