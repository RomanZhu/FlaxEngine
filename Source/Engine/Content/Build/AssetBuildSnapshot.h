// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"
#include "Engine/Content/Artifacts/ArtifactTarget.h"

/// <summary>One exact object publication pinned by a runtime build.</summary>
struct FLAXENGINE_API AssetBuildSnapshotArtifact
{
    AssetObjectId Object;
    ArtifactKey Manifest;
    ContentHash ObjectContent;
};

/// <summary>Immutable object-level asset inputs captured at the start of a runtime build.</summary>
struct FLAXENGINE_API AssetBuildSnapshot
{
    uint64 DatabaseRevision = 0;
    ArtifactTarget Target;
    ContentHash TargetHash;
    ContentHash ProjectSettingsHash;
    Array<AssetObjectId> RootObjects;
    Array<AssetBuildSnapshotArtifact> Artifacts;

    /// <summary>Sorts the snapshot canonically and validates its complete object-level identity.</summary>
    /// <returns>True on failure.</returns>
    bool NormalizeAndValidate(AssetPipelineDiagnostic& diagnostic);

    /// <summary>Calculates a deterministic fingerprint without modifying this snapshot.</summary>
    /// <returns>True on failure.</returns>
    bool ComputeFingerprint(ArtifactKey& result, AssetPipelineDiagnostic& diagnostic) const;
};
