// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "ArtifactTarget.h"
#include "Engine/Content/AssetDatabase/AssetPath.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>
/// Project Library lifecycle and safe-clean boundary. Artifact publication is added by later phases.
/// </summary>
class FLAXENGINE_API ArtifactStore
{
public:
    static constexpr int32 SchemaVersion = 1;

    /// <summary>Creates and validates the Library directory schema.</summary>
    /// <returns>True on failure.</returns>
    static bool EnsureLayout(const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Conservatively removes a known interrupted staging directory only when it is empty.</summary>
    /// <returns>True on failure.</returns>
    static bool Recover(const StringView& libraryRoot, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Deletes and recreates only the configured project Library. Active leases block cleaning.</summary>
    /// <returns>True on failure.</returns>
    static bool CleanEntireLibrary(AssetPipelineDiagnostic& diagnostic);

    static String GetArtifactsPath(const StringView& libraryRoot);
    static String GetManifestsPath(const StringView& libraryRoot);
    static String GetTemporaryPath(const StringView& libraryRoot);
    static String GetLocksPath(const StringView& libraryRoot);
    static String GetLogsPath(const StringView& libraryRoot);
    static String GetGcPath(const StringView& libraryRoot);

    /// <summary>Builds one immutable output path from engine-owned semantic fields.</summary>
    static bool TryGetArtifactPath(const StringView& libraryRoot, const ArtifactTarget& target, ArtifactTargetDimension dimensions,
        const Guid& assetId, const StringAnsiView& outputKind, const ArtifactKey& key, const StringAnsiView& extension,
        ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Builds the mutable current-manifest path for one asset and complete target.</summary>
    static bool TryGetManifestPath(const StringView& libraryRoot, const ArtifactTarget& target, const Guid& assetId,
        ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic);

    static bool TryGetLockPath(const StringView& libraryRoot, const ArtifactKey& key, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic);
    static bool TryGetJobStagingPath(const StringView& libraryRoot, const Guid& jobId, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic);
    static bool TryGetJobLogPath(const StringView& libraryRoot, const StringView& buildId, ArtifactStoragePath& path, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Converts and validates an absolute Library path for manifest storage.</summary>
    static bool TryMakeLibraryRelative(const StringView& libraryRoot, const StringView& absolutePath, String& relativePath, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Resolves a manifest-relative path without permitting root escape.</summary>
    static bool TryResolveLibraryRelative(const StringView& libraryRoot, const StringView& relativePath, ArtifactStoragePath& absolutePath, AssetPipelineDiagnostic& diagnostic);
};
