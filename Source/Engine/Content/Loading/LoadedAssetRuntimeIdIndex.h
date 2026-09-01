// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/AssetDatabase/Identity/AssetObjectId.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Collections/Dictionary.h"

/// <summary>
/// Tracks all loaded composite objects sharing a transient runtime GUID.
/// GUID-only lookup succeeds only while exactly one loaded object owns the value.
/// </summary>
class LoadedAssetRuntimeIdIndex
{
private:
    Dictionary<Guid, Array<AssetObjectId>> _objects;

public:
    void EnsureCapacity(int32 capacity)
    {
        _objects.EnsureCapacity(capacity);
    }

    void Add(const Guid& runtimeId, const AssetObjectId& object)
    {
        if (!runtimeId.IsValid() || !object.IsValid())
            return;
        Array<AssetObjectId>& objects = _objects[runtimeId];
        if (!objects.Contains(object))
            objects.Add(object);
    }

    void Remove(const Guid& runtimeId, const AssetObjectId& object)
    {
        Array<AssetObjectId>* objects = _objects.TryGet(runtimeId);
        if (!objects)
            return;
        objects->Remove(object);
        if (objects->IsEmpty())
            _objects.Remove(runtimeId);
    }

    bool TryGetUnique(const Guid& runtimeId, AssetObjectId& result) const
    {
        result = AssetObjectId();
        const Array<AssetObjectId>* objects = _objects.TryGet(runtimeId);
        if (!objects || objects->Count() != 1)
            return false;
        result = (*objects)[0];
        return true;
    }

    bool Contains(const Guid& runtimeId) const
    {
        return _objects.ContainsKey(runtimeId);
    }
};
