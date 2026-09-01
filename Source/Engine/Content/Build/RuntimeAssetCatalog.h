// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"
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
    Guid Object;
    StringAnsi TypeName;
    StringAnsi PackageName;
    uint64 Offset = 0;
    uint64 Size = 0;
    RuntimeAssetCompression Compression = RuntimeAssetCompression::None;
    ContentHash Content;
    Array<Guid> Dependencies;
};

/// <summary>Opaque compatibility mapping from a normalized runtime path hash to an exact asset object.</summary>
struct FLAXENGINE_API RuntimeAssetCatalogAlias
{
    ContentHash PathHash;
    Guid Object;
};

/// <summary>Deterministic binary runtime catalog independent of source metadata and the editor database.</summary>
class FLAXENGINE_API RuntimeAssetCatalog
{
public:
    static constexpr uint32 FormatVersion = 5;

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
    const Array<RuntimeAssetCatalogAlias>& GetAliases() const
    {
        return _aliases;
    }
    const Guid& GetGameSettingsObject() const
    {
        return _gameSettingsObject;
    }

    /// <summary>Sets the exact cooked GameSettings bootstrap object.</summary>
    void SetGameSettingsObject(const Guid& value)
    {
        _gameSettingsObject = value;
    }

    /// <summary>Replaces catalog contents, sorts them canonically, and validates all object references.</summary>
    /// <returns>True on failure.</returns>
    bool Set(const StringAnsiView& buildID, const ContentHash& targetHash, const Array<RuntimeAssetCatalogEntry>& entries,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Replaces catalog entries and opaque runtime path aliases.</summary>
    bool Set(const StringAnsiView& buildID, const ContentHash& targetHash, const Array<RuntimeAssetCatalogEntry>& entries,
        const Array<RuntimeAssetCatalogAlias>& aliases, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Finds an entry by its persistent object GUID.</summary>
    bool TryGet(const Guid& object, RuntimeAssetCatalogEntry& result) const;

    /// <summary>Finds an exact object by a normalized runtime path hash.</summary>
    bool TryGetByPathHash(const ContentHash& pathHash, Guid& result) const;

    /// <summary>Hashes a portable logical runtime path without retaining its source string. Returns true on failure.</summary>
    static bool HashPathAlias(const StringView& path, ContentHash& result);

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
    Guid _gameSettingsObject;
    Array<RuntimeAssetCatalogEntry> _entries;
    Array<RuntimeAssetCatalogAlias> _aliases;

    bool ValidateCanonical(AssetPipelineDiagnostic& diagnostic) const;
};
