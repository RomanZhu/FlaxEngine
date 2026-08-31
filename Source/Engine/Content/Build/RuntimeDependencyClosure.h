// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Content/AssetPipeline/AssetPipelineDiagnostics.h"

/// <summary>Committed runtime dependency edges for one imported object.</summary>
struct FLAXENGINE_API RuntimeObjectDependencyRecord
{
    AssetObjectId Object;
    Array<AssetObjectId> Dependencies;
};

/// <summary>One normalized object-level edge retained for build diagnostics.</summary>
struct FLAXENGINE_API RuntimeDependencyEdge
{
    AssetObjectId Owner;
    AssetObjectId Dependency;
};

/// <summary>Deterministic transitive closure over committed object dependency records.</summary>
struct FLAXENGINE_API RuntimeDependencyClosureResult
{
    Array<AssetObjectId> Objects;
    Array<RuntimeDependencyEdge> Edges;
};

class FLAXENGINE_API RuntimeDependencyClosure
{
public:
    /// <summary>Traverses recorded dependencies without loading any runtime or editor asset objects.</summary>
    /// <returns>True on missing/malformed database records.</returns>
    static bool Build(const Array<AssetObjectId>& roots, const Array<RuntimeObjectDependencyRecord>& records,
        RuntimeDependencyClosureResult& result, AssetPipelineDiagnostic& diagnostic);
};
