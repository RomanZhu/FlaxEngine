// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "PreparedAsset.h"
#include "Engine/Core/Collections/Dictionary.h"

/// <summary>One internal build-input edge retained with its declaration origin.</summary>
struct FLAXENGINE_API AssetBuildGraphEdge
{
    Guid Dependency = Guid::Empty;
    AssetDependencyOrigin Origin;
};

/// <summary>Deterministic dependency graph for one immutable asset database revision.</summary>
class FLAXENGINE_API AssetBuildGraph
{
public:
    /// <summary>Rebuilds the graph and dependency-first order. Returns true on failure.</summary>
    bool Build(const Array<PreparedAsset>& assets, uint64 databaseRevision, AssetPipelineDiagnostic& diagnostic);

    void Clear();

    bool IsCurrent(uint64 databaseRevision) const
    {
        return _databaseRevision == databaseRevision;
    }

    uint64 GetDatabaseRevision() const
    {
        return _databaseRevision;
    }

    const Array<Guid>& GetBuildOrder() const
    {
        return _buildOrder;
    }

    bool TryGetBuildInputs(const Guid& assetId, Array<AssetBuildGraphEdge>& result) const;

private:
    uint64 _databaseRevision = 0;
    Array<Guid> _nodes;
    Dictionary<Guid, int32> _nodeIndices;
    Array<Array<AssetBuildGraphEdge>> _inputs;
    Array<Guid> _buildOrder;

    bool FindCycle(AssetPipelineDiagnostic& diagnostic) const;
};
