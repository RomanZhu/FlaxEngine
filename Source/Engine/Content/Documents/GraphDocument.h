// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDocument.h"
#include "Engine/Core/Types/Span.h"

/// <summary>Stable structural identity projected from one Visject node.</summary>
struct FLAXENGINE_API GraphDocumentNodeHeader
{
    uint32 LegacyID = 0;
    uint16 GroupID = 0;
    uint16 TypeID = 0;

    StringAnsi GetStableID() const;
    StringAnsi GetStableType() const;
};

/// <summary>Immutable canonical graph document plus its compatibility surface.</summary>
struct FLAXENGINE_API GraphDocumentSnapshot : AssetDocumentSnapshot
{
    int32 GraphVersion = 0;
    Array<byte> CompatibilitySurface;
    Array<GraphDocumentNodeHeader> Nodes;
};

/// <summary>Canonical JSON codec for graph-authored assets.</summary>
class FLAXENGINE_API GraphDocumentCodec : public IAssetDocumentCodec
{
public:
    static constexpr int32 CurrentDocumentVersion = 1;
    static constexpr int32 CurrentGraphVersion = 1;
    static constexpr int32 CompatibilityGraphVersion = 7000;
    static constexpr int32 MaximumSurfaceBytes = 64 * 1024 * 1024;

    bool Decode(const StringAnsiView& source, AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const override;
    bool DecodeGraph(const StringAnsiView& source, GraphDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const;

    /// <summary>Encodes exact Visject surface bytes as one deterministic graph document.</summary>
    static bool Encode(const StringView& typeName, const Span<byte>& surface, StringAnsi& output, AssetPipelineDiagnostic& diagnostic);

    /// <summary>Validates the bounded compatibility header and projects stable node identities.</summary>
    static bool InspectSurface(const Span<byte>& surface, Array<GraphDocumentNodeHeader>& nodes, AssetPipelineDiagnostic& diagnostic);
};

/// <summary>Structural graph validator used before compilation and publication.</summary>
class FLAXENGINE_API GraphDocumentValidator : public IAssetDocumentValidator
{
public:
    bool Validate(const AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const override;
};

/// <summary>Ordered migration entry point. Version one currently needs no rewrite.</summary>
class FLAXENGINE_API GraphDocumentMigrator : public IAssetDocumentMigrator
{
public:
    bool Migrate(const AssetDocumentSnapshot& source, int32 targetVersion, StringAnsi& canonicalText, AssetPipelineDiagnostic& diagnostic) const override;
};

/// <summary>Compatibility compiler that emits the exact validated Visject surface payload.</summary>
class FLAXENGINE_API GraphDocumentCompiler : public IAssetDocumentCompiler
{
public:
    bool Compile(const AssetDocumentSnapshot& snapshot, Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const override;
};
