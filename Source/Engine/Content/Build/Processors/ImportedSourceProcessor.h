// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"
#if COMPILE_WITH_AUDIO_TOOL
#include "Engine/Tools/AudioTool/AudioTool.h"
#endif

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Imported source state retained between Prepare and Build.</summary>
class FLAXENGINE_API ImportedSourcePreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;
    ContentHash SettingsHash;
    String SourceExtension;
#if COMPILE_WITH_AUDIO_TOOL
    AudioTool::Options AudioOptions;
    bool HasAudioOptions = false;
#endif

    uint64 GetMemoryUsage() const override
    {
        return sizeof(ImportedSourcePreparedPayload);
    }
};

/// <summary>Rebuilds text, audio, font, shader, and video runtime data from canonical sources.</summary>
class FLAXENGINE_API ImportedSourceProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 4;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static bool Owns(const StringView& processorID);
    static const String& AudioID();
    static const String& FontID();
    static const String& ShaderID();
    static const String& VideoID();
    static const String& TextID();
    static const String& BinaryID();
    static const String& IESID();
    static AssetProcessorDescriptor CreateDescriptor(const StringView& processorID);
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
