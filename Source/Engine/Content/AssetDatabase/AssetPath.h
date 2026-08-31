// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Semantic kinds used by asset path diagnostics.</summary>
enum class AssetPathKind : byte
{
    Canonical,
    Source,
    Meta,
    ArtifactStorage,
    PackageEntry,
    SubAsset,
};

#define DECLARE_ASSET_SEMANTIC_PATH(type) \
    class type final \
    { \
    private: \
        String _value; \
    public: \
        type() = default; \
        explicit type(const StringView& value); \
        const String& Get() const { return _value; } \
        bool IsEmpty() const { return _value.IsEmpty(); } \
        bool operator==(const type& other) const { return _value == other._value; } \
        bool operator!=(const type& other) const { return _value != other._value; } \
    }

DECLARE_ASSET_SEMANTIC_PATH(CanonicalAssetPath);
DECLARE_ASSET_SEMANTIC_PATH(SourceFilePath);
DECLARE_ASSET_SEMANTIC_PATH(MetaFilePath);
DECLARE_ASSET_SEMANTIC_PATH(ArtifactStoragePath);
DECLARE_ASSET_SEMANTIC_PATH(PackageEntryPath);
DECLARE_ASSET_SEMANTIC_PATH(SubAssetKey);

#undef DECLARE_ASSET_SEMANTIC_PATH

/// <summary>Validation helpers for explicit asset path types.</summary>
class FLAXENGINE_API AssetPathPolicy
{
public:
    static const Char* GetDebugLabel(AssetPathKind kind);
    static bool IsSameOrChild(const StringView& path, const StringView& root);
    static bool IsCanonicalPathValid(const CanonicalAssetPath& path, const StringView& contentRoot);
    static bool IsSourcePathValid(const SourceFilePath& path, const StringView& contentRoot);
    static bool IsMetaPathValid(const MetaFilePath& path, const StringView& contentRoot);
    static bool IsArtifactPathValid(const ArtifactStoragePath& path, const StringView& libraryRoot);
    static bool IsPackageEntryPathValid(const PackageEntryPath& path);

    /// <summary>Resolves the existing path or nearest existing ancestor to its canonical physical location.</summary>
    /// <returns>True on failure.</returns>
    static bool TryResolvePhysicalPath(const StringView& input, String& resolved, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Portable normalized project path while retaining the user's display spelling.</summary>
    struct ProjectPath
    {
        String DisplayPath;
        String AbsolutePath;
        String ProjectRelativePath;
        String PortabilityKey;
    };

    /// <summary>Normalizes and validates a canonical project Content path.</summary>
    /// <returns>True on failure.</returns>
    static bool TryNormalizeProjectPath(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot, const StringView& input, ProjectPath& result, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Detects case/separator portability collisions in normalized paths.</summary>
    static void FindPortabilityCollisions(const Array<ProjectPath>& paths, Array<AssetPipelineDiagnostic>& diagnostics);
};
