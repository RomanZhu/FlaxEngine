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
    bool GenericObjectDocument = false;
    int64 ObjectLocalId = 0;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(AuthoredAssetPreparedPayload);
    }
};

/// <summary>Rebuilds small authored JSON assets into runtime flax artifacts.</summary>
class FLAXENGINE_API AuthoredAssetProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 4;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static bool Owns(const StringView& processorID);
    static const String& MaterialInstanceID();
    static const String& SkeletonMaskID();
    static const String& SceneAnimationID();
    static const String& ParticleSystemID();
    static const String& CollisionDataID();
    static const String& AnimationID();
    static const String& GameplayGlobalsID();
    static const String& GenericObjectID();
    static AssetProcessorDescriptor CreateDescriptor(const StringView& processorID);
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
