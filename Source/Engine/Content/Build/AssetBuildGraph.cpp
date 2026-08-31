// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetBuildGraph.h"
#include <algorithm>
#include <queue>
#include <vector>

namespace
{
    int32 CompareObjectId(const AssetObjectId& a, const AssetObjectId& b)
    {
        const Guid& aGuid = a.Asset.Value;
        const Guid& bGuid = b.Asset.Value;
        for (int32 i = 0; i < 4; i++)
        {
            if (aGuid.Values[i] < bGuid.Values[i])
                return -1;
            if (aGuid.Values[i] > bGuid.Values[i])
                return 1;
        }
        return a.LocalId < b.LocalId ? -1 : a.LocalId > b.LocalId ? 1 : 0;
    }

    struct GuidIndexGreater
    {
        const Array<AssetObjectId>* Nodes = nullptr;

        bool operator()(int32 a, int32 b) const
        {
            return CompareObjectId((*Nodes)[a], (*Nodes)[b]) > 0;
        }
    };

    String DescribeOrigin(const AssetObjectId& owner, const AssetBuildGraphEdge& edge)
    {
        String result = owner.ToString() + TEXT(" -> ") + edge.Dependency.ToString();
        if (!edge.Origin.Path.IsEmpty())
            result += TEXT(" at ") + edge.Origin.Path;
        if (!edge.Origin.GraphNode.IsEmpty())
            result += TEXT(" node ") + edge.Origin.GraphNode;
        if (!edge.Origin.GraphPin.IsEmpty())
            result += TEXT(" pin ") + edge.Origin.GraphPin;
        return result;
    }
}

void AssetBuildGraph::Clear()
{
    _databaseRevision = 0;
    _nodes.Clear();
    _nodeIndices.Clear();
    _inputs.Clear();
    _buildOrder.Clear();
}

bool AssetBuildGraph::Build(const Array<PreparedAsset>& assets, uint64 databaseRevision, AssetPipelineDiagnostic& diagnostic)
{
    Clear();
    diagnostic = AssetPipelineDiagnostic();
    _databaseRevision = databaseRevision;
    _nodes.EnsureCapacity(assets.Count());
    for (const PreparedAsset& asset : assets)
    {
        if (!asset.ObjectID.IsValid() || asset.DatabaseRevision != databaseRevision || _nodeIndices.ContainsKey(asset.ObjectID))
        {
            diagnostic.Code = asset.DatabaseRevision != databaseRevision ? AssetPipelineDiagnosticCode::PrepareInvalidated : AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.AssetGuid = asset.ObjectID.Asset.Value;
            diagnostic.Message = asset.DatabaseRevision != databaseRevision
                ? TEXT("Prepared asset revision does not match the graph revision.")
                : TEXT("Build graph contains an invalid or duplicate asset identity.");
            Clear();
            return true;
        }
        _nodeIndices.Add(asset.ObjectID, _nodes.Count());
        _nodes.Add(asset.ObjectID);
    }
    _inputs.Resize(_nodes.Count());
    Array<Array<int32>> dependants;
    dependants.Resize(_nodes.Count());
    Array<int32> indegrees;
    indegrees.Resize(_nodes.Count());
    Platform::MemoryClear(indegrees.Get(), indegrees.Count() * sizeof(int32));

    for (int32 ownerIndex = 0; ownerIndex < assets.Count(); ownerIndex++)
    {
        const PreparedAsset& asset = assets[ownerIndex];
        for (const AssetDependency& dependency : asset.Dependencies)
        {
            if (dependency.Kind != AssetDependencyKind::BuildInput)
                continue;
            const int32* dependencyIndex = _nodeIndices.TryGet(dependency.ObjectID);
            if (!dependencyIndex)
                continue;
            AssetBuildGraphEdge edge;
            edge.Dependency = dependency.ObjectID;
            edge.Origin = dependency.Origin;
            _inputs[ownerIndex].Add(MoveTemp(edge));
            dependants[*dependencyIndex].Add(ownerIndex);
            indegrees[ownerIndex]++;
        }
    }
    for (Array<AssetBuildGraphEdge>& inputs : _inputs)
    {
        std::sort(inputs.Get(), inputs.Get() + inputs.Count(), [](const AssetBuildGraphEdge& a, const AssetBuildGraphEdge& b)
        {
            return CompareObjectId(a.Dependency, b.Dependency) < 0;
        });
        for (int32 i = 1; i < inputs.Count(); i++)
        {
            if (inputs[i - 1].Dependency == inputs[i].Dependency)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.Message = TEXT("Build graph contains a duplicate build-input edge.");
                diagnostic.Location.File = inputs[i].Origin.Path;
                Clear();
                return true;
            }
        }
    }
    for (Array<int32>& entries : dependants)
    {
        std::sort(entries.Get(), entries.Get() + entries.Count(), [this](int32 a, int32 b)
        {
            return CompareObjectId(_nodes[a], _nodes[b]) < 0;
        });
    }

    GuidIndexGreater comparison;
    comparison.Nodes = &_nodes;
    std::priority_queue<int32, std::vector<int32>, GuidIndexGreater> ready(comparison);
    for (int32 i = 0; i < indegrees.Count(); i++)
    {
        if (indegrees[i] == 0)
            ready.push(i);
    }
    while (!ready.empty())
    {
        const int32 index = ready.top();
        ready.pop();
        _buildOrder.Add(_nodes[index]);
        for (const int32 dependant : dependants[index])
        {
            if (--indegrees[dependant] == 0)
                ready.push(dependant);
        }
    }
    if (_buildOrder.Count() != _nodes.Count())
    {
        if (!FindCycle(diagnostic))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildCycle;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            diagnostic.Message = TEXT("Build-input dependency graph contains a cycle.");
        }
        _buildOrder.Clear();
        return true;
    }
    return false;
}

bool AssetBuildGraph::FindCycle(AssetPipelineDiagnostic& diagnostic) const
{
    Array<byte> colors;
    colors.Resize(_nodes.Count());
    Platform::MemoryClear(colors.Get(), colors.Count());
    Array<int32> nodeStack;
    Array<const AssetBuildGraphEdge*> edgeStack;

    Function<bool(int32)> visit;
    visit = [&](int32 nodeIndex)
    {
        colors[nodeIndex] = 1;
        nodeStack.Add(nodeIndex);
        for (const AssetBuildGraphEdge& edge : _inputs[nodeIndex])
        {
            const int32* dependencyIndex = _nodeIndices.TryGet(edge.Dependency);
            if (!dependencyIndex)
                continue;
            if (colors[*dependencyIndex] == 0)
            {
                edgeStack.Add(&edge);
                if (visit(*dependencyIndex))
                    return true;
                edgeStack.RemoveLast();
            }
            else if (colors[*dependencyIndex] == 1)
            {
                int32 cycleStart = 0;
                while (cycleStart < nodeStack.Count() && nodeStack[cycleStart] != *dependencyIndex)
                    cycleStart++;
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::BuildCycle;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                diagnostic.AssetGuid = _nodes[*dependencyIndex].Asset.Value;
                diagnostic.SourcePath = edge.Origin.Path;
                diagnostic.Location.File = edge.Origin.Path;
                diagnostic.Location.Line = edge.Origin.Line;
                diagnostic.Location.Column = edge.Origin.Column;
                diagnostic.Location.GraphNode = edge.Origin.GraphNode;
                diagnostic.Location.GraphPin = edge.Origin.GraphPin;
                diagnostic.Message = TEXT("Build-input dependency cycle: ");
                for (int32 i = cycleStart; i < nodeStack.Count(); i++)
                {
                    if (i != cycleStart)
                        diagnostic.Message += TEXT(" -> ");
                    diagnostic.Message += _nodes[nodeStack[i]].ToString();
                    if (i > cycleStart)
                        diagnostic.Related.Add(DescribeOrigin(_nodes[nodeStack[i - 1]], *edgeStack[i - 1]));
                }
                diagnostic.Message += TEXT(" -> ") + _nodes[*dependencyIndex].ToString();
                diagnostic.Related.Add(DescribeOrigin(_nodes[nodeIndex], edge));
                return true;
            }
        }
        nodeStack.RemoveLast();
        colors[nodeIndex] = 2;
        return false;
    };

    Array<int32> orderedIndices;
    orderedIndices.Resize(_nodes.Count());
    for (int32 i = 0; i < orderedIndices.Count(); i++)
        orderedIndices[i] = i;
    std::sort(orderedIndices.Get(), orderedIndices.Get() + orderedIndices.Count(), [this](int32 a, int32 b)
    {
        return CompareObjectId(_nodes[a], _nodes[b]) < 0;
    });
    for (const int32 index : orderedIndices)
    {
        if (colors[index] == 0 && visit(index))
            return true;
    }
    return false;
}

bool AssetBuildGraph::TryGetBuildInputs(const AssetObjectId& assetId, Array<AssetBuildGraphEdge>& result) const
{
    const int32* index = _nodeIndices.TryGet(assetId);
    if (!index)
    {
        result.Clear();
        return false;
    }
    result = _inputs[*index];
    return true;
}
