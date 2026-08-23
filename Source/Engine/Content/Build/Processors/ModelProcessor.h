// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ModelProcessorSettings.h"
#include "ModelSubAssetKeys.h"
#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

/// <summary>Bounded structural model state retained between Prepare and Build.</summary>
class FLAXENGINE_API ModelPreparedPayload : public PreparedAssetPayload
{
public:
    ModelProcessorSettings Settings;
    String RootTypeName;
    String RootSourcePath;
    String SelectedStableKey;
    ContentHash RootSourceHash;
    Array<ModelSubAssetInfo> SubAssets;
    Dictionary<String, Guid> AssignedIDs;
    int32 SourceLodCount = 0;
    int32 SourceMeshCount = 0;
    int32 SourceAnimationCount = 0;
    int32 SourceMaterialCount = 0;
    int32 SourceSkeletonNodeCount = 0;
    uint64 EstimatedOutputBytes = 0;

    uint64 GetMemoryUsage() const override;
};

/// <summary>Built-in canonical imported-model processor.</summary>
class FLAXENGINE_API ModelProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 10;
    static constexpr uint32 RuntimeFormatVersion = 1;
    static constexpr uint32 GeometryFormatVersion = 1;
    static constexpr uint32 LodFormatVersion = 1;
    static constexpr uint32 SdfFormatVersion = 1;
    static constexpr uint32 SkeletonFormatVersion = 1;
    static constexpr uint32 AnimationFormatVersion = 1;
    static constexpr uint32 MaterialFormatVersion = 1;

    static AssetProcessorDescriptor CreateDescriptor();
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
