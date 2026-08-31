// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetHotReloadCoordinator.h"
#include "Engine/Core/Collections/HashSet.h"
#include <algorithm>
#include <functional>

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

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const AssetObjectId& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object.Asset.Value;
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
        for (const AssetObjectId& dependency : replacements[index].Dependencies)
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
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetObjectId(),
            TEXT("Could not calculate a complete asset replacement notification order."));
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool AssetHotReloadCoordinator::Reload(const Array<AssetObjectRevision>& changes,
    AssetPipelineDiagnostic& diagnostic)
{
    if (changes.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetObjectId(),
            TEXT("Asset hot-reload batch cannot be empty."));
    HashSet<AssetObjectId> changedObjects;
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
            _listener.OnAssetObjectReplaced(swap->Object, swap->PreviousRevision, swap->Revision);
        }
        for (const LoadedAssetSwap& swap : swaps)
            _loader.DiscardInstance(swap.PreviousInstance);
    });
    if (dispatchFailed || swapFailed)
    {
        for (const LoadedAssetReplacement& replacement : replacements)
            _loader.DiscardInstance(replacement.Instance);
        if (dispatchFailed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetObjectId(),
                TEXT("Asset replacement batch could not run on the main thread."));
        diagnostic = swapDiagnostic;
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
