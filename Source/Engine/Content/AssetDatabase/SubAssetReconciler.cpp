// Copyright (c) Wojciech Figat. All rights reserved.

#include "SubAssetReconciler.h"
#include <algorithm>

namespace
{
    AssetPipelineDiagnostic Diagnostic(const Guid& id, const StringView& message, const Array<String>* keys = nullptr)
    {
        AssetPipelineDiagnostic result;
        result.Code = AssetPipelineDiagnosticCode::SubAssetReconcileRequired;
        result.Stage = AssetPipelineDiagnosticStage::Prepare;
        result.AssetGuid = id;
        result.Message = message;
        if (keys)
            result.Related = *keys;
        return result;
    }

    bool Compatible(const SubAssetMeta& existing, const SubAssetCandidate& candidate)
    {
        return existing.TypeName == candidate.TypeName;
    }

    void RequireUser(SubAssetReconcileResult& result, const Guid& id, const StringView& message, const Array<String>* keys = nullptr)
    {
        result.RequiresUserReconciliation = true;
        result.Diagnostics.Add(Diagnostic(id, message, keys));
    }

    void ValidateResolvedMappings(SubAssetReconcileResult& result, const Guid& mainId)
    {
        HashSet<Guid> objectIds;
        objectIds.Add(mainId);
        HashSet<int64> localIds;
        localIds.Add(1);
        HashSet<String> reconciliationKeys;
        for (const auto& mapping : result.Resolved)
        {
            if (!mapping.Value.ID.IsValid() || !objectIds.Add(mapping.Value.ID) ||
                mapping.Value.LocalId <= 1 || !localIds.Add(mapping.Value.LocalId) ||
                !SubAssetPolicy::IsKeyValid(mapping.Key) || !reconciliationKeys.Add(mapping.Key))
            {
                RequireUser(result, mainId, TEXT("Resolved subasset mappings contain duplicate or invalid identities."));
                return;
            }
            for (const String& previous : mapping.Value.PreviousKeys)
            {
                if (!SubAssetPolicy::IsKeyValid(previous) || previous == mapping.Key || !reconciliationKeys.Add(previous))
                {
                    RequireUser(result, mainId, TEXT("Resolved subasset mappings contain ambiguous reconciliation identifiers."));
                    return;
                }
            }
        }
    }
}

SubAssetReconcileResult SubAssetReconciler::Reconcile(const AssetMeta& currentMeta, const Array<SubAssetCandidate>& candidates, bool mayStageTrackedMetadata)
{
    SubAssetReconcileResult result;
    Array<SubAssetCandidate> normalized = candidates;
    if (normalized.Count() > 1)
    {
        std::sort(normalized.Get(), normalized.Get() + normalized.Count(), [](const SubAssetCandidate& a, const SubAssetCandidate& b)
        {
            return a.StableKey < b.StableKey;
        });
    }

    HashSet<String> candidateKeys;
    HashSet<int64> reservedLocalIds;
    reservedLocalIds.Add(1);
    HashSet<Guid> reservedObjectIds;
    reservedObjectIds.Add(currentMeta.ID);
    if (!currentMeta.ID.IsValid())
        RequireUser(result, currentMeta.ID, TEXT("Tracked asset metadata has an invalid main object GUID."));
    HashSet<String> reservedReconciliationKeys;
    for (const auto& existing : currentMeta.SubAssets)
    {
        if (!SubAssetPolicy::IsKeyValid(existing.Key) || !reservedReconciliationKeys.Add(existing.Key))
            RequireUser(result, currentMeta.ID, TEXT("Tracked subasset mappings contain an invalid or duplicate stable identifier."));
        if (!existing.Value.ID.IsValid() || !reservedObjectIds.Add(existing.Value.ID))
            RequireUser(result, currentMeta.ID, TEXT("Tracked subasset mappings contain an invalid or duplicate object GUID."));
        if (existing.Value.LocalId <= 1 || !reservedLocalIds.Add(existing.Value.LocalId))
            RequireUser(result, currentMeta.ID, TEXT("Tracked subasset mappings contain an invalid or duplicate local file ID."));
        for (const String& previous : existing.Value.PreviousKeys)
        {
            if (!SubAssetPolicy::IsKeyValid(previous) || previous == existing.Key || !reservedReconciliationKeys.Add(previous))
                RequireUser(result, currentMeta.ID, TEXT("Tracked subasset mappings contain an invalid or duplicate previous stable identifier."));
        }
    }
    HashSet<String> candidateReconciliationKeys;
    for (const SubAssetCandidate& candidate : normalized)
    {
        if (!SubAssetPolicy::IsKeyValid(candidate.StableKey) || candidate.TypeName.IsEmpty() ||
            !candidateKeys.Add(candidate.StableKey) || !candidateReconciliationKeys.Add(candidate.StableKey))
            RequireUser(result, currentMeta.ID, TEXT("Prepared subassets contain an invalid or duplicate stable key."));
        HashSet<String> aliases;
        for (const String& previous : candidate.PreviousKeys)
        {
            if (!SubAssetPolicy::IsKeyValid(previous) || previous == candidate.StableKey ||
                !aliases.Add(previous) || !candidateReconciliationKeys.Add(previous))
                RequireUser(result, currentMeta.ID, TEXT("Prepared subassets contain an invalid or duplicate previous stable identifier."));
        }
    }
    if (result.RequiresUserReconciliation)
        return result;

    HashSet<String> claimedExisting;
    HashSet<String> exactCandidateKeys;
    for (const SubAssetCandidate& candidate : normalized)
    {
        const SubAssetMeta* exact = currentMeta.SubAssets.TryGet(candidate.StableKey);
        if (!exact)
            continue;
        exactCandidateKeys.Add(candidate.StableKey);
        if (!Compatible(*exact, candidate))
        {
            Array<String> related;
            related.Add(candidate.StableKey);
            RequireUser(result, currentMeta.ID, TEXT("An exact subasset key changed type and cannot inherit the prior identity."), &related);
            continue;
        }
        SubAssetMeta resolved = *exact;
        resolved.DisplayName = candidate.DisplayName;
        if (resolved.Removed)
        {
            resolved.Removed = false;
            result.HasTrackedChanges = true;
            SubAssetReconcileChange change;
            change.Kind = SubAssetChangeKind::Revive;
            change.OldKey = candidate.StableKey;
            change.NewKey = candidate.StableKey;
            change.ID = resolved.ID;
            change.LocalId = resolved.LocalId;
            result.Changes.Add(change);
            if (!mayStageTrackedMetadata)
                RequireUser(result, currentMeta.ID, TEXT("Reviving a tombstoned subasset requires an explicit metadata update."));
        }
        result.Resolved.Add(candidate.StableKey, MoveTemp(resolved));
        claimedExisting.Add(candidate.StableKey);
    }

    for (const SubAssetCandidate& candidate : normalized)
    {
        if (exactCandidateKeys.Contains(candidate.StableKey))
            continue;
        Array<String> matches;
        HashSet<String> seenPrevious;
        for (const String& previous : candidate.PreviousKeys)
        {
            if (!SubAssetPolicy::IsKeyValid(previous) || !seenPrevious.Add(previous))
                continue;
            const SubAssetMeta* existing = currentMeta.SubAssets.TryGet(previous);
            if (existing && !claimedExisting.Contains(previous) && !exactCandidateKeys.Contains(previous))
            {
                if (!Compatible(*existing, candidate))
                    RequireUser(result, currentMeta.ID, TEXT("Subasset rename evidence refers to an incompatible object type."));
                else
                    matches.Add(previous);
            }
        }
        for (const auto& existing : currentMeta.SubAssets)
        {
            if (!claimedExisting.Contains(existing.Key) && !exactCandidateKeys.Contains(existing.Key) && existing.Value.PreviousKeys.Contains(candidate.StableKey))
            {
                if (!Compatible(existing.Value, candidate))
                    RequireUser(result, currentMeta.ID, TEXT("Persisted rename evidence refers to an incompatible object type."));
                else if (!matches.Contains(existing.Key))
                    matches.Add(existing.Key);
            }
        }
        if (result.RequiresUserReconciliation)
            continue;
        if (matches.Count() == 1 && candidate.RenameEvidenceReliable)
        {
            const String oldKey = matches[0];
            SubAssetMeta moved = currentMeta.SubAssets[oldKey];
            moved.DisplayName = candidate.DisplayName;
            moved.Removed = false;
            moved.PreviousKeys.Remove(candidate.StableKey);
            if (!moved.PreviousKeys.Contains(oldKey))
                moved.PreviousKeys.Add(oldKey);
            for (const String& previous : candidate.PreviousKeys)
            {
                if (previous != candidate.StableKey && !moved.PreviousKeys.Contains(previous))
                    moved.PreviousKeys.Add(previous);
            }
            result.Resolved.Add(candidate.StableKey, MoveTemp(moved));
            claimedExisting.Add(oldKey);
            result.HasTrackedChanges = true;
            SubAssetReconcileChange change;
            change.Kind = SubAssetChangeKind::Move;
            change.OldKey = oldKey;
            change.NewKey = candidate.StableKey;
            change.ID = result.Resolved[candidate.StableKey].ID;
            change.LocalId = result.Resolved[candidate.StableKey].LocalId;
            result.Changes.Add(change);
            if (!mayStageTrackedMetadata)
                RequireUser(result, currentMeta.ID, TEXT("A reliable subasset rename requires an explicit metadata update."));
            continue;
        }
        if (matches.HasItems())
        {
            RequireUser(result, currentMeta.ID, TEXT("Subasset rename evidence is ambiguous or not declared reliable."), &matches);
            continue;
        }
        if (!mayStageTrackedMetadata)
        {
            RequireUser(result, currentMeta.ID, TEXT("A new subasset requires an explicit metadata mapping."));
            continue;
        }
        SubAssetMeta added;
        do
        {
            added.ID = Guid::New();
        } while (!reservedObjectIds.Add(added.ID));
        added.LocalId = SubAssetPolicy::AllocateLocalId(currentMeta.Processor.ID, candidate.StableKey, candidate.TypeName, reservedLocalIds, added.CollisionSalt);
        added.TypeName = candidate.TypeName;
        added.DisplayName = candidate.DisplayName;
        reservedLocalIds.Add(added.LocalId);
        result.Resolved.Add(candidate.StableKey, added);
        result.HasTrackedChanges = true;
        SubAssetReconcileChange change;
        change.Kind = SubAssetChangeKind::Add;
        change.NewKey = candidate.StableKey;
        change.ID = added.ID;
        change.LocalId = added.LocalId;
        result.Changes.Add(change);
    }

    for (const auto& existing : currentMeta.SubAssets)
    {
        if (claimedExisting.Contains(existing.Key) || result.Resolved.ContainsKey(existing.Key))
            continue;
        SubAssetMeta tombstone = existing.Value;
        if (!tombstone.Removed)
        {
            tombstone.Removed = true;
            result.HasTrackedChanges = true;
            SubAssetReconcileChange change;
            change.Kind = SubAssetChangeKind::Tombstone;
            change.OldKey = existing.Key;
            change.NewKey = existing.Key;
            change.ID = tombstone.ID;
            change.LocalId = tombstone.LocalId;
            result.Changes.Add(change);
            if (!mayStageTrackedMetadata)
                RequireUser(result, currentMeta.ID, TEXT("Removing a subasset requires an explicit tombstone metadata update."));
        }
        result.Resolved.Add(existing.Key, MoveTemp(tombstone));
    }
    ValidateResolvedMappings(result, currentMeta.ID);
    return result;
}
