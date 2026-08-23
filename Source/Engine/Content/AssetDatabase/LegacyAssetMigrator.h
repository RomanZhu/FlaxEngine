// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMeta.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Converts legacy .flax binaries into canonical documents/sidecars while preserving GUIDs.</summary>
class FLAXENGINE_API LegacyAssetMigrator
{
public:
    static bool ConvertFlax(const StringView& sourcePath, const StringView& destinationPath, const Guid& preservedId, const StringView& typeName, AssetPipelineDiagnostic& diagnostic);
    static bool SeedModelSubAssets(const StringView& flaxPath, AssetMeta& meta, AssetPipelineDiagnostic& diagnostic);
    static bool WriteSidecar(const StringView& documentPath, const Guid& id, const StringView& typeName, AssetSourceKind kind, const StringView& processorId, const Dictionary<String, SubAssetMeta>* subAssets, AssetPipelineDiagnostic& diagnostic);
};
