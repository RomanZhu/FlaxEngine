// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR

namespace
{
    SubAssetMeta Mapping(int64 localId, const StringView& type, const StringView& name, bool removed = false)
    {
        SubAssetMeta result;
        result.LocalId = localId;
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
    meta.Processor.ID = TEXT("Flax.Model");
    const int64 bodyId = 710000000001;
    const int64 headId = 810000000001;
    meta.SubAssets.Add(TEXT("mesh:/Body"), Mapping(bodyId, TEXT("FlaxEngine.Model"), TEXT("Body")));
    meta.SubAssets.Add(TEXT("mesh:/Head"), Mapping(headId, TEXT("FlaxEngine.Model"), TEXT("Head")));
    Array<SubAssetCandidate> candidates;
    candidates.Add(Candidate(TEXT("mesh:/Head"), TEXT("FlaxEngine.Model"), TEXT("Head")));
    candidates.Add(Candidate(TEXT("mesh:/Body"), TEXT("FlaxEngine.Model"), TEXT("Body")));
    const SubAssetReconcileResult result = SubAssetReconciler::Reconcile(meta, candidates, false);
    CHECK_FALSE(result.RequiresUserReconciliation);
    CHECK_FALSE(result.HasTrackedChanges);
    CHECK(result.Resolved[TEXT("mesh:/Body")].LocalId == bodyId);
    CHECK(result.Resolved[TEXT("mesh:/Head")].LocalId == headId);
}

TEST_CASE("Subasset reconciliation accepts only reliable unambiguous compatible rename evidence")
{
    AssetMeta meta;
    meta.ID = Guid(91, 92, 93, 94);
    meta.Processor.ID = TEXT("Flax.Model");
    const int64 walkId = 1010000000001;
    meta.SubAssets.Add(TEXT("animation:Walk"), Mapping(walkId, TEXT("FlaxEngine.Animation"), TEXT("Walk")));
    SubAssetCandidate renamed = Candidate(TEXT("animation:WalkFast"), TEXT("FlaxEngine.Animation"), TEXT("Walk Fast"));
    renamed.PreviousKeys.Add(TEXT("animation:Walk"));
    renamed.RenameEvidenceReliable = true;
    Array<SubAssetCandidate> candidates;
    candidates.Add(renamed);
    const SubAssetReconcileResult interactive = SubAssetReconciler::Reconcile(meta, candidates, true);
    CHECK_FALSE(interactive.RequiresUserReconciliation);
    CHECK(interactive.HasTrackedChanges);
    CHECK(interactive.Resolved[renamed.StableKey].LocalId == walkId);
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
    meta.Processor.ID = TEXT("Flax.Model");
    const int64 oldId = 1210000000001;
    meta.SubAssets.Add(TEXT("mesh:/Old"), Mapping(oldId, TEXT("FlaxEngine.Model"), TEXT("Old")));
    Array<SubAssetCandidate> candidates;
    candidates.Add(Candidate(TEXT("mesh:/New"), TEXT("FlaxEngine.Model"), TEXT("New")));
    const SubAssetReconcileResult headless = SubAssetReconciler::Reconcile(meta, candidates, false);
    CHECK(headless.RequiresUserReconciliation);
    CHECK(headless.HasTrackedChanges);
    CHECK_FALSE(headless.Resolved.ContainsKey(TEXT("mesh:/New")));
    REQUIRE(headless.Resolved.ContainsKey(TEXT("mesh:/Old")));
    CHECK(headless.Resolved[TEXT("mesh:/Old")].Removed);
    CHECK(headless.Resolved[TEXT("mesh:/Old")].LocalId == oldId);

    const SubAssetReconcileResult interactive = SubAssetReconciler::Reconcile(meta, candidates, true);
    CHECK_FALSE(interactive.RequiresUserReconciliation);
    CHECK(interactive.Resolved.ContainsKey(TEXT("mesh:/New")));
    CHECK(interactive.Resolved[TEXT("mesh:/New")].LocalId != oldId);
    CHECK(interactive.Resolved[TEXT("mesh:/Old")].Removed);
}

TEST_CASE("Subasset local file IDs are deterministic and collision salt is persisted")
{
    AssetMeta meta;
    meta.ID = Guid(131, 132, 133, 134);
    meta.Processor.ID = TEXT("Flax.Model");
    Array<SubAssetCandidate> firstCandidates;
    firstCandidates.Add(Candidate(TEXT("mesh:/Body"), TEXT("FlaxEngine.Mesh"), TEXT("Body")));
    firstCandidates.Add(Candidate(TEXT("material:Body"), TEXT("FlaxEngine.Material"), TEXT("Body")));
    Array<SubAssetCandidate> secondCandidates;
    secondCandidates.Add(firstCandidates[1]);
    secondCandidates.Add(firstCandidates[0]);
    const SubAssetReconcileResult first = SubAssetReconciler::Reconcile(meta, firstCandidates, true);
    const SubAssetReconcileResult second = SubAssetReconciler::Reconcile(meta, secondCandidates, true);
    REQUIRE_FALSE(first.RequiresUserReconciliation);
    REQUIRE_FALSE(second.RequiresUserReconciliation);
    CHECK(first.Resolved[TEXT("mesh:/Body")].LocalId == second.Resolved[TEXT("mesh:/Body")].LocalId);
    CHECK(first.Resolved[TEXT("material:Body")].LocalId == second.Resolved[TEXT("material:Body")].LocalId);

    const int64 saltZero = SubAssetPolicy::CalculateLocalId(TEXT("Flax.Model"), TEXT("mesh:/Body"), TEXT("FlaxEngine.Mesh"), 0);
    HashSet<int64> reserved;
    reserved.Add(saltZero);
    uint32 salt = 0;
    const int64 collisionResolved = SubAssetPolicy::AllocateLocalId(TEXT("Flax.Model"), TEXT("mesh:/Body"), TEXT("FlaxEngine.Mesh"), reserved, salt);
    CHECK(salt == 1);
    CHECK(collisionResolved != saltZero);
}

TEST_CASE("Subasset reconciliation resolves a persisted alias without changing local file ID")
{
    AssetMeta meta;
    meta.ID = Guid(141, 142, 143, 144);
    meta.Processor.ID = TEXT("Flax.Model");
    SubAssetMeta mapping = Mapping(1410000000001, TEXT("FlaxEngine.Animation"), TEXT("Run"));
    mapping.PreviousKeys.Add(TEXT("animation:Walk"));
    meta.SubAssets.Add(TEXT("animation:Run"), mapping);
    SubAssetCandidate candidate = Candidate(TEXT("animation:Walk"), TEXT("FlaxEngine.Animation"), TEXT("Walk"));
    candidate.RenameEvidenceReliable = true;
    Array<SubAssetCandidate> candidates;
    candidates.Add(candidate);
    const SubAssetReconcileResult result = SubAssetReconciler::Reconcile(meta, candidates, true);
    REQUIRE_FALSE(result.RequiresUserReconciliation);
    CHECK(result.Resolved[TEXT("animation:Walk")].LocalId == mapping.LocalId);
}

#endif
