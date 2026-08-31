// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/Artifacts/ResolvedArtifact.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Authoritative compiled inventory for immutable shipped engine-content artifacts.</summary>
class FLAXENGINE_API EngineContentCatalog
{
public:
    static constexpr const Char* ProcessorID = TEXT("Flax.EnginePrebuilt");
    static constexpr const char* Compatibility = "flax-engine-prebuilt-v1";

    /// <summary>Builds canonical database records without inspecting artifact storage headers.</summary>
    static bool Collect(const StringView& engineContentRoot, Array<AssetRecord>& records,
        Array<AssetPipelineDiagnostic>& diagnostics);

    /// <summary>Resolves and verifies one immutable shipped blob from its catalog identity.</summary>
    static bool Resolve(const Guid& assetID, ResolvedArtifact& artifact, ContentHash& content,
        uint64& size, AssetPipelineDiagnostic& diagnostic);
};
