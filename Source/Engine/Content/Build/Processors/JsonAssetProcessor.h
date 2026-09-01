// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>One validated scene partition captured during controlled preparation.</summary>
struct JsonAssetPreparedPartition
{
    int64 RootFileId = 0;
    ContentHash SourceHash;
    StringAnsi SourceJson;
};

/// <summary>Prepared state for a canonical JSON-authored runtime object.</summary>
class FLAXENGINE_API JsonAssetPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SourceHash;
    bool SceneUsesPartitions = false;
    Array<JsonAssetPreparedPartition> ScenePartitions;

    uint64 GetMemoryUsage() const override
    {
        uint64 result = sizeof(JsonAssetPreparedPayload) + ScenePartitions.Count() * sizeof(JsonAssetPreparedPartition);
        for (const JsonAssetPreparedPartition& partition : ScenePartitions)
            result += partition.SourceJson.Length();
        return result;
    }
};

/// <summary>Imports scene, prefab, and generic JSON source documents into immutable runtime artifacts.</summary>
class FLAXENGINE_API JsonAssetProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 4;
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
