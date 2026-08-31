// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Types/Span.h"

/// <summary>Compression applied to one object payload inside a runtime package.</summary>
enum class RuntimeAssetCompression : byte
{
    None,
    LZ4,
    Zstd,
};

/// <summary>One object-level address in an immutable runtime package.</summary>
struct FLAXENGINE_API RuntimeAssetCatalogEntry
{
    AssetObjectId Object;
    StringAnsi TypeName;
    StringAnsi PackageName;
    uint64 Offset = 0;
    uint64 Size = 0;
    RuntimeAssetCompression Compression = RuntimeAssetCompression::None;
    ContentHash Content;
    Array<AssetObjectId> Dependencies;
};

/// <summary>Deterministic binary runtime catalog independent of source metadata and the editor database.</summary>
class FLAXENGINE_API RuntimeAssetCatalog
{
public:
    static constexpr uint32 FormatVersion = 1;

    const StringAnsi& GetBuildID() const
    {
        return _buildID;
    }
    const ContentHash& GetTargetHash() const
    {
        return _targetHash;
    }
    const Array<RuntimeAssetCatalogEntry>& GetEntries() const
    {
        return _entries;
    }

    /// <summary>Replaces catalog contents, sorts them canonically, and validates all object references.</summary>
    /// <returns>True on failure.</returns>
    bool Set(const StringAnsiView& buildID, const ContentHash& targetHash, const Array<RuntimeAssetCatalogEntry>& entries,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Finds an exact GUID/local-file-ID entry without loading an asset.</summary>
    bool TryGet(const AssetObjectId& object, RuntimeAssetCatalogEntry& result) const;

    /// <summary>Serializes canonical little-endian bytes with a SHA-256 payload checksum.</summary>
    /// <returns>True on failure.</returns>
    bool ToBytes(Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Parses and validates canonical catalog bytes.</summary>
    /// <returns>True on failure.</returns>
    static bool FromBytes(const Span<byte>& input, RuntimeAssetCatalog& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Writes a complete catalog through a sibling staging file and atomic replacement.</summary>
    /// <returns>True on failure.</returns>
    bool SaveAtomic(const StringView& path, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Loads and validates a catalog from disk.</summary>
    /// <returns>True on failure.</returns>
    static bool Load(const StringView& path, RuntimeAssetCatalog& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Checks that a package identifier cannot refer to project source or Library storage.</summary>
    static bool IsPackageNameValid(const StringAnsiView& value);

private:
    StringAnsi _buildID;
    ContentHash _targetHash;
    Array<RuntimeAssetCatalogEntry> _entries;

    bool ValidateCanonical(AssetPipelineDiagnostic& diagnostic) const;
};
