// Copyright (c) Wojciech Figat. All rights reserved.

#include "RuntimeDependencyClosure.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Core/Collections/HashSet.h"
#include <algorithm>

namespace
{
    bool Less(const AssetObjectId& a, const AssetObjectId& b)
    {
        const Guid& left = a.Asset.Value;
        const Guid& right = b.Asset.Value;
        if (left.A != right.A)
            return left.A < right.A;
        if (left.B != right.B)
            return left.B < right.B;
        if (left.C != right.C)
            return left.C < right.C;
        if (left.D != right.D)
            return left.D < right.D;
        return a.LocalId < b.LocalId;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, const AssetObjectId& object, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ArtifactMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Cook;
        diagnostic.AssetGuid = object.Asset.Value;
        diagnostic.Message = message;
        return true;
    }
}

bool RuntimeDependencyClosure::Build(const Array<AssetObjectId>& roots, const Array<RuntimeObjectDependencyRecord>& records,
    RuntimeDependencyClosureResult& result, AssetPipelineDiagnostic& diagnostic)
{
    result = RuntimeDependencyClosureResult();
    if (roots.IsEmpty())
        return Fail(diagnostic, AssetObjectId(), TEXT("Runtime dependency closure requires at least one root object."));

    Dictionary<AssetObjectId, const RuntimeObjectDependencyRecord*> byObject;
    for (const RuntimeObjectDependencyRecord& record : records)
    {
        if (!record.Object.IsValid() || byObject.ContainsKey(record.Object))
            return Fail(diagnostic, record.Object, TEXT("Runtime dependency graph contains an invalid or duplicate object record."));
        HashSet<AssetObjectId> dependencies;
        for (const AssetObjectId& dependency : record.Dependencies)
        {
            if (!dependency.IsValid() || !dependencies.Add(dependency))
                return Fail(diagnostic, record.Object, TEXT("Runtime dependency graph contains an invalid or duplicate object dependency."));
        }
        byObject.Add(record.Object, &record);
    }

    Array<AssetObjectId> pending = roots;
    std::sort(pending.Get(), pending.Get() + pending.Count(), Less);
    HashSet<AssetObjectId> visited;
    for (int32 index = 0; index < pending.Count(); index++)
    {
        const AssetObjectId object = pending[index];
        if (!object.IsValid())
            return Fail(diagnostic, object, TEXT("Runtime dependency closure contains an invalid root object."));
        if (!visited.Add(object))
            continue;
        const RuntimeObjectDependencyRecord* const* record = byObject.TryGet(object);
        if (!record)
            return Fail(diagnostic, object, TEXT("Runtime dependency closure could not resolve an object record."));

        result.Objects.Add(object);
        Array<AssetObjectId> dependencies = (*record)->Dependencies;
        if (dependencies.Count() > 1)
            std::sort(dependencies.Get(), dependencies.Get() + dependencies.Count(), Less);
        for (const AssetObjectId& dependency : dependencies)
        {
            RuntimeDependencyEdge edge;
            edge.Owner = object;
            edge.Dependency = dependency;
            result.Edges.Add(edge);
            if (!visited.Contains(dependency))
                pending.Add(dependency);
        }
    }

    if (result.Objects.Count() > 1)
        std::sort(result.Objects.Get(), result.Objects.Get() + result.Objects.Count(), Less);
    if (result.Edges.Count() > 1)
    {
        std::sort(result.Edges.Get(), result.Edges.Get() + result.Edges.Count(), [](const RuntimeDependencyEdge& a, const RuntimeDependencyEdge& b)
        {
            return a.Owner == b.Owner ? Less(a.Dependency, b.Dependency) : Less(a.Owner, b.Owner);
        });
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
