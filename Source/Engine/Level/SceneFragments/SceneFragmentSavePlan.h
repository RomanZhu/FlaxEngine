// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"

/// <summary>Exact source-file revision captured before scene serialization.</summary>
struct FLAXENGINE_API SceneSourceRevision
{
    bool Exists = false;
    ContentHash Content;
};

/// <summary>One fully serialized private fragment prepared without mutating source state.</summary>
struct FLAXENGINE_API PreparedSceneFragment
{
    String RelativePhysicalPath;
    Array<byte> Data;
    ContentHash Content;
};

/// <summary>Prepared private-fragment changes committed with their owning scene source.</summary>
struct FLAXENGINE_API SceneFragmentSavePlan
{
    Guid OwnerSceneGuid = Guid::Empty;
    bool RemoveStore = false;
    bool HadPreviousIndex = false;
    uint64 ExpectedIndexRevision = 0;
    ContentHash ExpectedIndexContent;
    Array<PreparedSceneFragment> Fragments;
    Array<String> RemovedFragments;
    Array<byte> IndexData;
};

/// <summary>One fully prepared scene source and private-fragment update.</summary>
struct FLAXENGINE_API PreparedSceneSave
{
    String SourcePath;
    Array<byte> SourceData;
    SceneSourceRevision ExpectedSource;
    SceneFragmentSavePlan FragmentPlan;
};

/// <summary>Test-only interruption points that leave durable recovery state behind.</summary>
enum class SceneFragmentTransactionFailurePoint : byte
{
    None,
    AfterFirstApply,
    AfterAllApplyBeforeCommit,
    AfterCommitBeforeCleanup,
};
