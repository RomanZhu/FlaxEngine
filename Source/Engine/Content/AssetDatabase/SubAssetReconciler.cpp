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
}

SubAssetReconcileResult SubAssetReconciler::Reconcile(const AssetMeta& currentMeta, const Array<SubAssetCandidate>& candidates, bool mayStageTrackedMetadata)
{
    SubAssetReconcileResult result;
    Array<SubAssetCandidate> normalized = candidates;
    for (SubAssetCandidate& candidate : normalized)
    {
        candidate.StableKey = SubAssetPolicy::NormalizeKey(candidate.StableKey);
        for (String& previous : candidate.PreviousKeys)
            previous = SubAssetPolicy::NormalizeKey(previous);
    }
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
    for (const auto& existing : currentMeta.SubAssets)
    {
        if (existing.Value.LocalId <= 1 || !reservedLocalIds.Add(existing.Value.LocalId))
            RequireUser(result, currentMeta.ID, TEXT("Tracked subasset mappings contain an invalid or duplicate local file ID."));
    }
    for (const SubAssetCandidate& candidate : normalized)
    {
        if (!SubAssetPolicy::IsKeyValid(candidate.StableKey) || candidate.TypeName.IsEmpty() || !candidateKeys.Add(candidate.StableKey))
            RequireUser(result, currentMeta.ID, TEXT("Prepared subassets contain an invalid or duplicate stable key."));
        HashSet<String> aliases;
        for (const String& previous : candidate.PreviousKeys)
        {
            if (!SubAssetPolicy::IsKeyValid(previous) || previous == candidate.StableKey || !aliases.Add(previous))
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
            if (existing && !claimedExisting.Contains(previous) && !exactCandidateKeys.Contains(previous) && Compatible(*existing, candidate))
                matches.Add(previous);
        }
        for (const auto& existing : currentMeta.SubAssets)
        {
            if (!claimedExisting.Contains(existing.Key) && !exactCandidateKeys.Contains(existing.Key) &&
                Compatible(existing.Value, candidate) && existing.Value.PreviousKeys.Contains(candidate.StableKey) && !matches.Contains(existing.Key))
                matches.Add(existing.Key);
        }
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
        added.LocalId = SubAssetPolicy::AllocateLocalId(currentMeta.Processor.ID, candidate.StableKey, candidate.TypeName, reservedLocalIds, added.CollisionSalt);
        added.TypeName = candidate.TypeName;
        added.DisplayName = candidate.DisplayName;
        reservedLocalIds.Add(added.LocalId);
        result.Resolved.Add(candidate.StableKey, added);
        result.HasTrackedChanges = true;
        SubAssetReconcileChange change;
        change.Kind = SubAssetChangeKind::Add;
        change.NewKey = candidate.StableKey;
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
            change.LocalId = tombstone.LocalId;
            result.Changes.Add(change);
            if (!mayStageTrackedMetadata)
                RequireUser(result, currentMeta.ID, TEXT("Removing a subasset requires an explicit tombstone metadata update."));
        }
        result.Resolved.Add(existing.Key, MoveTemp(tombstone));
    }
    return result;
}
