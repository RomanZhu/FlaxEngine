// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Identity/AssetObjectId.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

/// <summary>One packaged runtime object addressable by asset GUID and local file ID without a project Library.</summary>
struct FLAXENGINE_API RuntimeAssetIndexEntry
{
    AssetObjectId ID;
    String TypeName;
    String PackagedPath;
};

/// <summary>Deterministic asset-object/package index written next to cooked AssetsCache.dat.</summary>
class FLAXENGINE_API RuntimeAssetIndex
{
public:
    static constexpr int32 FormatVersion = 2;

    static bool ContainsLibraryPath(const StringView& path);
    static bool WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
};
