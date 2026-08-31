// Copyright (c) Wojciech Figat. All rights reserved.

#include "CollectAssetsStep.h"
#include "Engine/Content/Build/RuntimeDependencyClosure.h"
#include "Engine/Core/Log.h"

bool CollectAssetsStep::Perform(CookingData& data)
{
    LOG(Info, "Searching the frozen asset database for the dependency closure of {0} root objects.", data.RootAssets.Count());
    data.StepProgress(TEXT("Collecting assets"), 0);

    const AssetDatabaseSnapshot& snapshot = data.DatabaseSnapshot;
    if (snapshot.Revision == 0)
    {
        data.Error(TEXT("Dependency collection requires a frozen asset database snapshot."));
        return true;
    }
    if (data.RootCollectionFailed)
    {
        data.Error(TEXT("Cannot collect assets after root discovery failed."));
        return true;
    }

    Dictionary<Guid, AssetObjectId> objectsByRuntimeId;
    Dictionary<AssetObjectId, const AssetRecord*> recordsByObject;
    Array<RuntimeObjectDependencyRecord> dependencyRecords;
    dependencyRecords.EnsureCapacity(snapshot.Records.Count() + data.BuiltinRootAssets.Count());
    for (const AssetRecord& record : snapshot.Records)
    {
        const AssetObjectId object(AssetGuid(record.SourceAssetID), record.LocalId);
        if (!record.ID.IsValid() || !object.IsValid() || record.ID != object.ToRuntimeObjectGuid() ||
            objectsByRuntimeId.ContainsKey(record.ID) || recordsByObject.ContainsKey(object))
        {
            data.Error(TEXT("The frozen asset database contains an invalid or duplicate object identity."));
            return true;
        }
        objectsByRuntimeId.Add(record.ID, object);
        recordsByObject.Add(object, &record);
    }

    for (auto i = data.BuiltinRootAssets.Begin(); i.IsNotEnd(); ++i)
    {
        const AssetObjectId object = i->Item;
        const Guid runtimeId = object.ToRuntimeObjectGuid();
        const AssetObjectId* existing = objectsByRuntimeId.TryGet(runtimeId);
        if (existing && *existing != object)
        {
            data.Error(TEXT("An engine built-in root collides with a project asset object."));
            return true;
        }
        if (!existing)
            objectsByRuntimeId.Add(runtimeId, object);
    }

    for (const AssetRecord& record : snapshot.Records)
    {
        RuntimeObjectDependencyRecord dependencyRecord;
        dependencyRecord.Object = AssetObjectId(AssetGuid(record.SourceAssetID), record.LocalId);
        HashSet<AssetObjectId> uniqueDependencies;
        for (const Guid& runtimeReference : record.RuntimeReferences)
        {
            const AssetObjectId* dependency = objectsByRuntimeId.TryGet(runtimeReference);
            if (!dependency)
            {
                data.Error(String::Format(TEXT("Recorded runtime reference {0} from {1} does not resolve in database revision {2}."),
                    runtimeReference, dependencyRecord.Object.ToString(), snapshot.Revision));
                return true;
            }
            if (*dependency == dependencyRecord.Object || !uniqueDependencies.Add(*dependency))
            {
                data.Error(String::Format(TEXT("Recorded runtime references for {0} contain a self or duplicate edge."),
                    dependencyRecord.Object.ToString()));
                return true;
            }
            dependencyRecord.Dependencies.Add(*dependency);
        }
        dependencyRecords.Add(MoveTemp(dependencyRecord));
    }
    for (auto i = data.BuiltinRootAssets.Begin(); i.IsNotEnd(); ++i)
    {
        if (recordsByObject.ContainsKey(i->Item))
            continue;
        RuntimeObjectDependencyRecord dependencyRecord;
        dependencyRecord.Object = i->Item;
        dependencyRecords.Add(MoveTemp(dependencyRecord));
    }

    Array<AssetObjectId> roots;
    roots.EnsureCapacity(data.RootAssets.Count());
    for (auto i = data.RootAssets.Begin(); i.IsNotEnd(); ++i)
        roots.Add(i->Item);

    RuntimeDependencyClosureResult closure;
    AssetPipelineDiagnostic diagnostic;
    if (RuntimeDependencyClosure::Build(roots, dependencyRecords, closure, diagnostic))
    {
        data.Error(String::Format(TEXT("Failed to collect the frozen runtime dependency closure. {0}"), diagnostic.Message));
        return true;
    }

    data.Assets.Clear();
    for (const AssetObjectId& object : closure.Objects)
    {
        const AssetRecord* const* record = recordsByObject.TryGet(object);
        if (record && (*record)->Status != AssetRecordStatus::Ready)
        {
            data.Error(String::Format(TEXT("Required asset object {0} is not ready in database revision {1}."),
                object.ToString(), snapshot.Revision));
            return true;
        }
        if (!record && !data.BuiltinRootAssets.Contains(object))
        {
            data.Error(String::Format(TEXT("Required asset object {0} has no frozen database record."), object.ToString()));
            return true;
        }
        data.Assets.Add(object);
    }

    data.Stats.TotalAssets = data.Assets.Count();
    LOG(Info, "Found {0} exact asset objects to deploy from database revision {1}.", data.Assets.Count(), snapshot.Revision);
    return false;
}
