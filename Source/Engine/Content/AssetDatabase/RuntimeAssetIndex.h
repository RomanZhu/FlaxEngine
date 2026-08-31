// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Identity/AssetObjectId.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

/// <summary>Runtime load-location flags emitted by the cooker.</summary>
enum class RuntimeAssetIndexFlags : uint32
{
    None = 0,
    ExactArtifact = 1,
    LegacyConverted = 2,
};

DECLARE_ENUM_OPERATORS(RuntimeAssetIndexFlags);

/// <summary>One packaged runtime object addressable by file GUID and local file ID without a project Library.</summary>
struct FLAXENGINE_API RuntimeAssetIndexEntry
{
    AssetObjectId ID;
    Guid BackingAssetID;
    String TypeName;
    String CanonicalPath;
    String PackagedPath;
    Guid PackageID;
    uint32 ChunkID = 0;
    uint64 Offset = 0;
    uint64 Size = 0;
    uint32 AssetFormatVersion = 0;
    RuntimeAssetIndexFlags Flags = RuntimeAssetIndexFlags::None;
    ArtifactKey ExactArtifact;
};

/// <summary>Deterministic, hash-verified asset-object/package index used as the cooked runtime registry.</summary>
class FLAXENGINE_API RuntimeAssetIndex
{
public:
    static constexpr int32 FormatVersion = 3;

    static bool ContainsLibraryPath(const StringView& path);
    static bool WriteCanonicalJson(const Array<RuntimeAssetIndexEntry>& entries, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);
    static bool Parse(const StringAnsiView& input, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
    static bool Load(const StringView& path, Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
    static bool SaveAtomic(const StringView& path, const Array<RuntimeAssetIndexEntry>& entries, AssetPipelineDiagnostic& diagnostic);
};
