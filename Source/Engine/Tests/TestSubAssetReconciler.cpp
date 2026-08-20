// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    SubAssetMeta Mapping(const Guid& id, const StringView& type, const StringView& name, bool removed = false)
    {
        SubAssetMeta result;
        result.ID = id;
        result.TypeName = type;
        result.DisplayName = name;
        result.Removed = removed;
        return result;
    }

    SubAssetCandidate Candidate(const StringView& key, const StringView& type, const StringView& name)
    {
        SubAssetCandidate result;
        result.StableKey = key;
        result.TypeName = type;
        result.DisplayName = name;
        return result;
    }
}

TEST_CASE("Subasset reconciliation retains identities across candidate reorder")
{
    AssetMeta meta;
    meta.ID = Guid(61, 62, 63, 64);
    const Guid bodyId(71, 72, 73, 74);
    const Guid headId(81, 82, 83, 84);
    meta.SubAssets.Add(TEXT("mesh:/Body"), Mapping(bodyId, TEXT("FlaxEngine.Model"), TEXT("Body")));
    meta.SubAssets.Add(TEXT("mesh:/Head"), Mapping(headId, TEXT("FlaxEngine.Model"), TEXT("Head")));
    Array<SubAssetCandidate> candidates;
    candidates.Add(Candidate(TEXT("mesh:/Head"), TEXT("FlaxEngine.Model"), TEXT("Head")));
    candidates.Add(Candidate(TEXT("mesh:/Body"), TEXT("FlaxEngine.Model"), TEXT("Body")));
    const SubAssetReconcileResult result = SubAssetReconciler::Reconcile(meta, candidates, false);
    CHECK_FALSE(result.RequiresUserReconciliation);
    CHECK_FALSE(result.HasTrackedChanges);
    CHECK(result.Resolved[TEXT("mesh:/Body")].ID == bodyId);
    CHECK(result.Resolved[TEXT("mesh:/Head")].ID == headId);
}

TEST_CASE("Subasset reconciliation accepts only reliable unambiguous compatible rename evidence")
{
    AssetMeta meta;
    meta.ID = Guid(91, 92, 93, 94);
    const Guid walkId(101, 102, 103, 104);
    meta.SubAssets.Add(TEXT("animation:Walk"), Mapping(walkId, TEXT("FlaxEngine.Animation"), TEXT("Walk")));
    SubAssetCandidate renamed = Candidate(TEXT("animation:WalkFast"), TEXT("FlaxEngine.Animation"), TEXT("Walk Fast"));
    renamed.PreviousKeys.Add(TEXT("animation:Walk"));
    renamed.RenameEvidenceReliable = true;
    Array<SubAssetCandidate> candidates;
    candidates.Add(renamed);
    const SubAssetReconcileResult interactive = SubAssetReconciler::Reconcile(meta, candidates, true);
    CHECK_FALSE(interactive.RequiresUserReconciliation);
    CHECK(interactive.HasTrackedChanges);
    CHECK(interactive.Resolved[renamed.StableKey].ID == walkId);
    REQUIRE(interactive.Changes.Count() == 1);
    CHECK(interactive.Changes[0].Kind == SubAssetChangeKind::Move);

    candidates[0].RenameEvidenceReliable = false;
    const SubAssetReconcileResult ambiguous = SubAssetReconciler::Reconcile(meta, candidates, true);
    CHECK(ambiguous.RequiresUserReconciliation);
    CHECK_FALSE(ambiguous.Resolved.ContainsKey(renamed.StableKey));
}

TEST_CASE("Subasset reconciliation blocks tracked additions and tombstones in headless mode")
{
    AssetMeta meta;
    meta.ID = Guid(111, 112, 113, 114);
    const Guid oldId(121, 122, 123, 124);
    meta.SubAssets.Add(TEXT("mesh:/Old"), Mapping(oldId, TEXT("FlaxEngine.Model"), TEXT("Old")));
    Array<SubAssetCandidate> candidates;
    candidates.Add(Candidate(TEXT("mesh:/New"), TEXT("FlaxEngine.Model"), TEXT("New")));
    const SubAssetReconcileResult headless = SubAssetReconciler::Reconcile(meta, candidates, false);
    CHECK(headless.RequiresUserReconciliation);
    CHECK(headless.HasTrackedChanges);
    CHECK_FALSE(headless.Resolved.ContainsKey(TEXT("mesh:/New")));
    REQUIRE(headless.Resolved.ContainsKey(TEXT("mesh:/Old")));
    CHECK(headless.Resolved[TEXT("mesh:/Old")].Removed);
    CHECK(headless.Resolved[TEXT("mesh:/Old")].ID == oldId);

    const SubAssetReconcileResult interactive = SubAssetReconciler::Reconcile(meta, candidates, true);
    CHECK_FALSE(interactive.RequiresUserReconciliation);
    CHECK(interactive.Resolved.ContainsKey(TEXT("mesh:/New")));
    CHECK(interactive.Resolved[TEXT("mesh:/New")].ID != oldId);
    CHECK(interactive.Resolved[TEXT("mesh:/Old")].Removed);
}

#endif
