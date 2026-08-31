// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "TextureProcessorSettings.h"
#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_TEXTURE_TOOL

/// <summary>Source container identified by the bounded texture header probe.</summary>
enum class TextureSourceFormat : byte
{
    Unknown,
    Png,
    Tga,
    Exr,
    Bmp,
    Gif,
    Tiff,
    Jpeg,
    Dds,
    Hdr,
    Raw,
};

/// <summary>Small deterministic texture state retained between Prepare and Build.</summary>
class FLAXENGINE_API TexturePreparedPayload : public PreparedAssetPayload
{
public:
    TextureProcessorSettings Settings;
    TextureSourceFormat SourceFormat = TextureSourceFormat::Unknown;
    int32 Width = 0;
    int32 Height = 0;
    int32 SourceArraySize = 1;
    int32 SourceChannels = 0;
    int32 SourceBitsPerChannel = 0;
    uint64 EstimatedDecodedBytes = 0;
    uint64 EstimatedOutputBytes = 0;

    uint64 GetMemoryUsage() const override;
};

/// <summary>Built-in source texture processor.</summary>
class FLAXENGINE_API TextureProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 2;
    static constexpr uint32 RuntimeFormatVersion = 4;
    static constexpr int32 MaximumDimension = 32768;
    static constexpr uint64 MaximumDecodedBytes = 2ull * 1024ull * 1024ull * 1024ull;

    static AssetProcessorDescriptor CreateDescriptor();

    /// <summary>Builds one independently reusable output key and its explanation fields.</summary>
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Validates and probes a texture source without decoding its pixels.</summary>
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
