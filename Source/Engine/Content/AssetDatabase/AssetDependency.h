// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Core/Delegate.h"

/// <summary>How a declared dependency participates in building and packaging.</summary>
enum class AssetDependencyKind : byte
{
    ExactSourceFile = 0,
    SourceFile = ExactSourceFile,
    Artifact = 1,
    BuildInput = Artifact,
    RuntimeReference = 2,
    Toolchain = 3,
    SourceAsset = 4,
    Custom = 5,
    Global = 6,
    Target = 7,
    ImporterProvider = 8,
    LogicalPath = 9,
    Environment = 10,
};

/// <summary>How a dependency fingerprint was resolved during preparation.</summary>
enum class AssetDependencyState : byte
{
    Present,
    Missing,
    CurrentArtifact,
    ExactArtifact,
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
    AssetDependencyState State = AssetDependencyState::Present;
    String StableIdentity;
    /// <summary>Exact persistent dependency identity when the dependency targets an asset object.</summary>
    AssetObjectId ObjectID;
    /// <summary>Legacy/runtime backing GUID projection.</summary>
    Guid AssetID = Guid::Empty;
    ContentHash Content;
    ContentHash Metadata;
    ArtifactKey ExactArtifact;
    ContentHash SemanticInterface;
    uint32 InterfaceVersion = 0;
    AssetDependencyOrigin Origin;

    bool AffectsBuildKey() const;
    bool IsSourceDependency() const
    {
        return Kind == AssetDependencyKind::ExactSourceFile || Kind == AssetDependencyKind::SourceAsset;
    }
    void AppendKeyComponents(ArtifactKeyBuilder& builder, int32 index) const;
    StringAnsi DescribeFingerprint() const;

    /// <summary>Sorts declarations by kind and stable identity. Returns true on duplicate/invalid declarations.</summary>
    static bool NormalizeAndSort(Array<AssetDependency>& dependencies, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>One durable node in an import-reason tree. Parent indices always precede children.</summary>
struct FLAXENGINE_API AssetImportReasonNode
{
    int32 Parent = -1;
    StringAnsi Code;
    String Identity;
    StringAnsi PreviousFingerprint;
    StringAnsi CurrentFingerprint;
    String Explanation;
};

/// <summary>Versioned semantic interface exposed to build dependants.</summary>
struct FLAXENGINE_API AssetSemanticInterface
{
    uint32 Version = 0;
    ContentHash Hash;
};

/// <summary>Optional processor hook that extracts a dependant-facing semantic interface.</summary>
using AssetSemanticInterfaceExtractor = Function<bool(const AssetRecord&, AssetSemanticInterface&, AssetPipelineDiagnostic&)>;
