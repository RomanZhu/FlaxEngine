// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/SubAsset.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Graphics/Models/ModelData.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

enum class ModelSubAssetKind : byte
{
    Mesh,
    Animation,
    Material,
    Texture,
};

/// <summary>Processor-private routing data paired with one stable public subasset candidate.</summary>
struct FLAXENGINE_API ModelSubAssetInfo
{
    ModelSubAssetKind Kind = ModelSubAssetKind::Mesh;
    String StableKey;
    String DisplayName;
    String TypeName;
    ContentHash SemanticHash;
    int32 SourceIndex = -1;
    String LegacyStableKey;
};

/// <summary>Format-independent stable identity derivation for independently addressable model outputs.</summary>
class FLAXENGINE_API ModelSubAssetKeys
{
public:
    static constexpr uint32 AlgorithmVersion = 4;

    /// <summary>Enumerates sorted stable candidates. Returns true on collision or invalid source structure.</summary>
    static bool Enumerate(const ModelData& data, Array<ModelSubAssetInfo>& infos, Array<SubAssetCandidate>& candidates, AssetPipelineDiagnostic& diagnostic);

    static const ModelSubAssetInfo* Find(const Array<ModelSubAssetInfo>& infos, const StringView& stableKey);
};

#endif
