// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Content/AssetDatabase/AssetDependency.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Immutable decoded authored-document state shared by validators and compilers.</summary>
struct FLAXENGINE_API AssetDocumentSnapshot
{
    String TypeName;
    int32 DocumentVersion = 0;
    StringAnsi CanonicalText;
    ContentHash FullHash;
    ContentHash SemanticHash;
    Array<AssetDependency> Dependencies;
};

/// <summary>Decodes authored bytes into a validated immutable snapshot. Does not require BinaryAsset.</summary>
class FLAXENGINE_API IAssetDocumentCodec
{
public:
    virtual ~IAssetDocumentCodec() = default;

    /// <returns>True on failure.</returns>
    virtual bool Decode(const StringAnsiView& source, AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const = 0;
};

/// <summary>Performs type-specific structural and semantic validation.</summary>
class FLAXENGINE_API IAssetDocumentValidator
{
public:
    virtual ~IAssetDocumentValidator() = default;

    /// <returns>True on failure.</returns>
    virtual bool Validate(const AssetDocumentSnapshot& snapshot, AssetPipelineDiagnostic& diagnostic) const = 0;
};

/// <summary>Compiles one immutable document snapshot into compatibility artifact bytes.</summary>
class FLAXENGINE_API IAssetDocumentCompiler
{
public:
    virtual ~IAssetDocumentCompiler() = default;

    /// <returns>True on failure.</returns>
    virtual bool Compile(const AssetDocumentSnapshot& snapshot, Array<byte>& output, AssetPipelineDiagnostic& diagnostic) const = 0;
};
