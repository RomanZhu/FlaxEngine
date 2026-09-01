// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetHotReloadCoordinator.h"
#include "Engine/Core/Collections/HashSet.h"
#include <algorithm>
#include <functional>

namespace
{
    bool Less(const Guid& a, const Guid& b)
    {
        if (a.A != b.A)
            return a.A < b.A;
        if (a.B != b.B)
            return a.B < b.B;
        if (a.C != b.C)
            return a.C < b.C;
        return a.D < b.D;
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const Guid& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object;
        diagnostic.Message = message;
        return true;
    }
}

bool AssetHotReloadCoordinator::BuildNotificationOrder(const Array<LoadedAssetReplacement>& replacements,
    Array<int32>& order, AssetPipelineDiagnostic& diagnostic)
{
    order.Clear();
    const int32 count = replacements.Count();
    Array<int32> canonical;
    canonical.Resize(count);
    for (int32 i = 0; i < count; i++)
        canonical[i] = i;
    if (count > 1)
    {
        std::sort(canonical.Get(), canonical.Get() + count, [&replacements](int32 a, int32 b)
        {
            return Less(replacements[a].Object, replacements[b].Object);
        });
    }

    Array<byte> state;
    state.Resize(count);
    Platform::MemoryClear(state.Get(), state.Count());
    std::function<void(int32)> visit = [&](int32 index)
    {
        if (state[index] == 2)
            return;
        if (state[index] == 1)
            return; // A runtime-reference cycle is one deterministic notification group.
        state[index] = 1;
        Array<int32> dependencies;
        for (const Guid& dependency : replacements[index].Dependencies)
        {
            for (int32 candidate : canonical)
            {
                if (replacements[candidate].Object == dependency)
                {
                    dependencies.Add(candidate);
                    break;
                }
            }
        }
        if (dependencies.Count() > 1)
        {
            std::sort(dependencies.Get(), dependencies.Get() + dependencies.Count(), [&replacements](int32 a, int32 b)
            {
                return Less(replacements[a].Object, replacements[b].Object);
            });
        }
        for (int32 dependency : dependencies)
            visit(dependency);
        state[index] = 2;
        order.Add(index);
    };
    for (int32 index : canonical)
        visit(index);
    if (order.Count() != count)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
            TEXT("Could not calculate a complete asset replacement notification order."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetHotReloadCoordinator::Reload(const Array<AssetObjectRevision>& changes,
    AssetPipelineDiagnostic& diagnostic)
{
    if (changes.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
            TEXT("Asset hot-reload batch cannot be empty."));
    HashSet<Guid> changedObjects;
    Array<LoadedAssetReplacement> replacements;
    replacements.EnsureCapacity(changes.Count());
    for (const AssetObjectRevision& change : changes)
    {
        if (!change.Object.IsValid() || change.Revision == 0 || !changedObjects.Add(change.Object))
        {
            for (const LoadedAssetReplacement& replacement : replacements)
                _loader.DiscardInstance(replacement.Instance);
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, change.Object,
                TEXT("Asset hot-reload batch contains an invalid or duplicate object revision."));
        }
        LoadedAssetReplacement replacement;
        if (_loader.PrepareReplacement(change.Object, change.Revision, replacement, diagnostic))
        {
            for (const LoadedAssetReplacement& prepared : replacements)
                _loader.DiscardInstance(prepared.Instance);
            return true;
        }
        replacements.Add(MoveTemp(replacement));
    }

    Array<int32> notificationOrder;
    if (BuildNotificationOrder(replacements, notificationOrder, diagnostic))
    {
        for (const LoadedAssetReplacement& replacement : replacements)
            _loader.DiscardInstance(replacement.Instance);
        return true;
    }

    Array<LoadedAssetSwap> swaps;
    AssetPipelineDiagnostic swapDiagnostic;
    bool swapFailed = false;
    const bool dispatchFailed = _dispatcher.InvokeAndWait([&]()
    {
        swapFailed = _registry.ReplaceBatch(replacements, swaps, swapDiagnostic);
        if (swapFailed)
            return;
        for (int32 index : notificationOrder)
        {
            const LoadedAssetSwap* swap = nullptr;
            for (const LoadedAssetSwap& candidate : swaps)
            {
                if (candidate.Object == replacements[index].Object)
                {
                    swap = &candidate;
                    break;
                }
            }
            ASSERT(swap);
            _listener.OnAssetObjectReplaced(*swap);
        }
        for (const LoadedAssetSwap& swap : swaps)
            _loader.DiscardInstance(swap.PreviousInstance);
    });
    if (dispatchFailed || swapFailed)
    {
        for (const LoadedAssetReplacement& replacement : replacements)
            _loader.DiscardInstance(replacement.Instance);
        if (dispatchFailed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
                TEXT("Asset replacement batch could not run on the main thread."));
        diagnostic = swapDiagnostic;
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetHotReloadCoordinator::ReloadInventory(const AssetObjectInventoryChange& change,
    AssetPipelineDiagnostic& diagnostic)
{
    if (!change.Source.IsValid() || !change.PreviousMainObject.IsValid() || !change.MainObject.IsValid())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, change.Source,
            TEXT("Asset inventory change has an invalid source or main-object identity."));

    HashSet<Guid> previousObjects;
    HashSet<Guid> objects;
    for (const Guid& object : change.PreviousObjects)
    {
        if (!object.IsValid() || !previousObjects.Add(object))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, object,
                TEXT("Previous asset inventory contains an invalid or duplicate object GUID."));
    }
    for (const Guid& object : change.Objects)
    {
        if (!object.IsValid() || !objects.Add(object))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, object,
                TEXT("Published asset inventory contains an invalid or duplicate object GUID."));
    }
    if (!previousObjects.Contains(change.PreviousMainObject) || !objects.Contains(change.MainObject))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, change.Source,
            TEXT("Asset inventory does not contain its declared main object."));

    HashSet<Guid> revisionObjects;
    for (const AssetObjectRevision& revision : change.Revisions)
    {
        if (!revision.Object.IsValid() || revision.Revision == 0 || !revisionObjects.Add(revision.Object))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, revision.Object,
                TEXT("Asset inventory contains an invalid or duplicate object revision."));
    }

    Array<Guid> removed;
    bool inventoryChanged = previousObjects.Count() != objects.Count() ||
        change.PreviousMainObject != change.MainObject;
    for (const Guid& object : previousObjects)
    {
        if (!objects.Contains(object))
        {
            inventoryChanged = true;
            removed.Add(object);
        }
    }
    if (!inventoryChanged)
        return false; // Ordering is not identity and cannot invalidate a loaded object.

    Array<LoadedAssetRecord> loaded;
    _registry.GetLoadedRecords(loaded);
    HashSet<Guid> loadedObjects;
    for (const LoadedAssetRecord& record : loaded)
        loadedObjects.Add(record.Object);

    Array<Guid> loadedRemovals;
    HashSet<Guid> affected;
    for (const Guid& object : removed)
    {
        affected.Add(object);
        if (loadedObjects.Contains(object))
            loadedRemovals.Add(object);
    }
    if (loadedObjects.Contains(change.MainObject))
        affected.Add(change.MainObject);
    if (change.PreviousMainObject != change.MainObject && objects.Contains(change.PreviousMainObject) &&
        loadedObjects.Contains(change.PreviousMainObject))
        affected.Add(change.PreviousMainObject);

    bool addedDependent = true;
    while (addedDependent)
    {
        addedDependent = false;
        for (const LoadedAssetRecord& record : loaded)
        {
            if (affected.Contains(record.Object) ||
                (!objects.Contains(record.Object) && previousObjects.Contains(record.Object)))
                continue;
            for (const Guid& dependency : record.Dependencies)
            {
                if (affected.Contains(dependency))
                {
                    affected.Add(record.Object);
                    addedDependent = true;
                    break;
                }
            }
        }
    }

    Array<AssetObjectRevision> reloads;
    for (const LoadedAssetRecord& record : loaded)
    {
        if (!affected.Contains(record.Object) ||
            (!objects.Contains(record.Object) && previousObjects.Contains(record.Object)))
            continue;
        const AssetObjectRevision* revision = nullptr;
        for (const AssetObjectRevision& candidate : change.Revisions)
        {
            if (candidate.Object == record.Object)
            {
                revision = &candidate;
                break;
            }
        }
        if (!revision)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, record.Object,
                TEXT("A loaded object affected by inventory publication has no exact published revision."));
        reloads.Add(*revision);
    }
    if (reloads.IsEmpty() && loadedRemovals.IsEmpty())
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    Array<LoadedAssetReplacement> replacements;
    replacements.EnsureCapacity(reloads.Count());
    for (const AssetObjectRevision& reload : reloads)
    {
        LoadedAssetReplacement replacement;
        if (_loader.PrepareReplacement(reload.Object, reload.Revision, replacement, diagnostic))
        {
            for (const LoadedAssetReplacement& prepared : replacements)
                _loader.DiscardInstance(prepared.Instance);
            return true;
        }
        replacements.Add(MoveTemp(replacement));
    }

    Array<int32> notificationOrder;
    if (!replacements.IsEmpty() && BuildNotificationOrder(replacements, notificationOrder, diagnostic))
    {
        for (const LoadedAssetReplacement& replacement : replacements)
            _loader.DiscardInstance(replacement.Instance);
        return true;
    }
    if (loadedRemovals.Count() > 1)
        std::sort(loadedRemovals.Get(), loadedRemovals.Get() + loadedRemovals.Count(), Less);

    Array<LoadedAssetSwap> swaps;
    Array<LoadedAssetInvalidation> invalidations;
    AssetPipelineDiagnostic publicationDiagnostic;
    bool publicationFailed = false;
    const bool dispatchFailed = _dispatcher.InvokeAndWait([&]()
    {
        publicationFailed = _registry.PublishBatch(replacements, loadedRemovals, swaps, invalidations,
            publicationDiagnostic);
        if (publicationFailed)
            return;
        for (const LoadedAssetInvalidation& invalidation : invalidations)
            _listener.OnAssetObjectInvalidated(invalidation);
        for (int32 index : notificationOrder)
        {
            for (const LoadedAssetSwap& swap : swaps)
            {
                if (swap.Object == replacements[index].Object)
                {
                    _listener.OnAssetObjectReplaced(swap);
                    break;
                }
            }
        }
        for (const LoadedAssetInvalidation& invalidation : invalidations)
            _loader.DiscardInstance(invalidation.PreviousInstance);
        for (const LoadedAssetSwap& swap : swaps)
            _loader.DiscardInstance(swap.PreviousInstance);
    });
    if (dispatchFailed || publicationFailed)
    {
        for (const LoadedAssetReplacement& replacement : replacements)
            _loader.DiscardInstance(replacement.Instance);
        if (dispatchFailed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, change.Source,
                TEXT("Asset inventory publication could not run on the main thread."));
        diagnostic = publicationDiagnostic;
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
