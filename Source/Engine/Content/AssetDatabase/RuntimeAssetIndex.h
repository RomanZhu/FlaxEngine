// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"

/// <summary>One packaged runtime asset addressable by GUID without a project Library.</summary>
struct FLAXENGINE_API RuntimeAssetIndexEntry
{
    Guid ID;
    String TypeName;
    String PackagedPath;
};

/// <summary>Deterministic GUID/package index written next to cooked AssetsCache.dat.</summary>
class FLAXENGINE_API RuntimeAssetIndex
{
public:
    static constexpr int32 FormatVersion = 1;

    static bool ContainsLibraryPath(const StringView& path);
    static bool WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
};
