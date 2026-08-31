// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabase.h"
#include "SourceHashCache.h"

struct ArtifactManifest;

using AssetDatabaseFileState = SourceHashFileState;

/// <summary>
/// Durable normalized source database stored in Library. SQLite is derived state;
/// adjacent source metadata remains the reconstructable identity authority.
/// </summary>
class FLAXENGINE_API AssetDatabaseStorage
{
public:
    static constexpr int32 SchemaVersion = 3;

    /// <summary>Atomically replaces durable rows with a committed read snapshot.</summary>
    /// <returns>True on failure.</returns>
    static bool Save(const StringView& path, const StringView& projectRoot, const StringView& contentRoot,
        const AssetDatabaseSnapshot& snapshot, const Array<SourceHashFileState>& fileStates,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Loads one integrity-checked committed snapshot.</summary>
    /// <returns>True when absent, corrupt, incompatible, or otherwise unusable.</returns>
    static bool Load(const StringView& path, const StringView& projectRoot, const StringView& contentRoot,
        AssetDatabase& database, Array<SourceHashFileState>& fileStates,
        AssetPipelineDiagnostic& diagnostic, bool readOnly = false);

    /// <summary>Publishes one verified manifest into immutable storage and updates its current mapping transactionally.</summary>
    static bool PublishArtifact(const StringView& libraryRoot, const ArtifactManifest& manifest,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Publishes verified manifests and selects every current mapping in one SQLite transaction.</summary>
    static bool PublishArtifacts(const StringView& libraryRoot, const Array<ArtifactManifest>& manifests,
        AssetPipelineDiagnostic& diagnostic);

    /// <summary>Gets the immutable manifest selected by the durable current mapping.</summary>
    /// <returns>True only on database failure. An empty path means no mapping exists.</returns>
    static bool GetCurrentArtifactManifest(const StringView& libraryRoot, const Guid& assetID,
        const ArtifactKey& targetKey, String& manifestPath, AssetPipelineDiagnostic& diagnostic);

    static bool RegisterCustomDependency(const StringView& libraryRoot, const StringView& name,
        const Guid& hash, AssetPipelineDiagnostic& diagnostic);
    static bool GetCustomDependency(const StringView& libraryRoot, const StringView& name,
        Guid& hash, uint64& revision, bool& found, AssetPipelineDiagnostic& diagnostic);
    static bool UnregisterCustomDependencyPrefix(const StringView& libraryRoot, const StringView& prefix,
        AssetPipelineDiagnostic& diagnostic);
};
