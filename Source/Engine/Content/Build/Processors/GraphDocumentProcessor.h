// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Build/AssetProcessor.h"
#include "Engine/Content/Build/PreparedAsset.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

/// <summary>Bounded graph state retained between Prepare and Build.</summary>
class FLAXENGINE_API GraphDocumentPreparedPayload : public PreparedAssetPayload
{
public:
    ContentHash SemanticHash;
    ContentHash FunctionInterfaceHash;
    int32 SurfaceBytes = 0;
    int32 NodeCount = 0;

    uint64 GetMemoryUsage() const override
    {
        return sizeof(GraphDocumentPreparedPayload);
    }
};

/// <summary>Built-in deterministic graph-document compatibility processor.</summary>
class FLAXENGINE_API GraphDocumentProcessor
{
public:
    static constexpr uint32 ImplementationVersion = 4;
    static constexpr uint32 RuntimeFormatVersion = 1;

    static const String& ProcessorID();
    static AssetProcessorDescriptor CreateDescriptor();
    static bool Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic);
    static bool BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
        ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic);
    static bool ExtractSemanticInterface(const AssetRecord& record, AssetSemanticInterface& result, AssetPipelineDiagnostic& diagnostic);

private:
    static bool Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic);
};

#endif
