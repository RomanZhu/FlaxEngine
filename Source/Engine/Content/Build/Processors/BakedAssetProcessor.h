// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Validated persistent tool-bake source retained between Prepare and Build.</summary>
class FLAXENGINE_API BakedAssetPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(BakedAssetPreparedPayload);
    }
};

/// <summary>Builds canonical tool-authored bake documents into immutable runtime artifacts.</summary>
class FLAXENGINE_API BakedAssetProcessor
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
