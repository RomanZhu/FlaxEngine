// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Prepared state for one authored scene partition.</summary>
class FLAXENGINE_API SceneChunkPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;
    int64 RootFileId = 0;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(SceneChunkPreparedPayload);
    }
};

/// <summary>Imports authoritative .scenechunk sources into independently reusable runtime artifacts.</summary>
class FLAXENGINE_API SceneChunkProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 1;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static const String& ProcessorID();
    static AssetProcessorDescriptor CreateDescriptor();
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
