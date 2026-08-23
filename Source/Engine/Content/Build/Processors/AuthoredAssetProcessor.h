// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Small authored JSON state retained between Prepare and Build.</summary>
class FLAXENGINE_API AuthoredAssetPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(AuthoredAssetPreparedPayload);
    }
};

/// <summary>Rebuilds material instance, skeleton mask, and scene animation flax from authored JSON.</summary>
class FLAXENGINE_API AuthoredAssetProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 2;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static bool Owns(const StringView& processorID);
    static const String& MaterialInstanceID();
    static const String& SkeletonMaskID();
    static const String& SceneAnimationID();
    static AssetProcessorDescriptor CreateDescriptor(const StringView& processorID);
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
