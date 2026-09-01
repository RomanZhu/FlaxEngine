// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Core/Delegate.h"

/// <summary>How a declared dependency participates in building and packaging.</summary>
enum class AssetDependencyKind : byte
{
    SourceFile,
    BuildInput,
    RuntimeReference,
    Toolchain,
    LogicalPath,
};

/// <summary>Origin retained for dependency and cycle diagnostics.</summary>
struct FLAXENGINE_API AssetDependencyOrigin
{
    String Path;
    int32 Line = -1;
    int32 Column = -1;
    String GraphNode;
    String GraphPin;
};

/// <summary>One deterministic dependency declaration.</summary>
struct FLAXENGINE_API AssetDependency
{
    AssetDependencyKind Kind = AssetDependencyKind::SourceFile;
    String StableIdentity;
    AssetObjectId ObjectID;
    ContentHash Content;
    ArtifactKey ExactArtifact;
    ContentHash SemanticInterface;
    uint32 InterfaceVersion = 0;
    AssetDependencyOrigin Origin;

    bool AffectsBuildKey() const;
    void AppendKeyComponents(ArtifactKeyBuilder& builder, int32 index) const;

    /// <summary>Sorts declarations by kind and stable identity. Returns true on duplicate/invalid declarations.</summary>
    static bool NormalizeAndSort(Array<AssetDependency>& dependencies, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Versioned semantic interface exposed to build dependants.</summary>
struct FLAXENGINE_API AssetSemanticInterface
{
    uint32 Version = 0;
    ContentHash Hash;
};

/// <summary>Optional processor hook that extracts a dependant-facing semantic interface.</summary>
using AssetSemanticInterfaceExtractor = Function<bool(const AssetRecord&, AssetSemanticInterface&, AssetPipelineDiagnostic&)>;
