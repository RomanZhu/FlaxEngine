// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMeta.h"

enum class SubAssetChangeKind : byte
{
    Add,
    Move,
    Tombstone,
    Revive,
};

/// <summary>One explicit tracked metadata change proposed by reconciliation.</summary>
struct FLAXENGINE_API SubAssetReconcileChange
{
    SubAssetChangeKind Kind = SubAssetChangeKind::Add;
    String OldKey;
    String NewKey;
    int64 LocalId = 0;
};

/// <summary>Complete deterministic resolution or blocking conflict model.</summary>
struct FLAXENGINE_API SubAssetReconcileResult
{
    Dictionary<String, SubAssetMeta> Resolved;
    Array<SubAssetReconcileChange> Changes;
    Array<AssetPipelineDiagnostic> Diagnostics;
    bool RequiresUserReconciliation = false;
    bool HasTrackedChanges = false;
};

class FLAXENGINE_API SubAssetReconciler
{
public:
    static SubAssetReconcileResult Reconcile(const AssetMeta& currentMeta, const Array<SubAssetCandidate>& candidates, bool mayStageTrackedMetadata);
};
