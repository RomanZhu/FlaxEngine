// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetImportPlanner.h"
#include "AssetPostprocessor.h"
#include "Engine/Core/Delegate.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Platform/CriticalSection.h"

enum class AssetRefreshReason : uint32
{
    None = 0,
    Filesystem = 1 << 0,
    Explicit = 1 << 1,
    ImporterRegistry = 1 << 2,
    CustomDependency = 1 << 3,
    Postprocessor = 1 << 4,
};

DECLARE_ENUM_OPERATORS(AssetRefreshReason);

struct FLAXENGINE_API AssetRefreshIterationContext
{
    int32 Iteration = 0;
    uint64 ImporterRegistryGeneration = 0;
    AssetRefreshReason Reasons = AssetRefreshReason::None;
};

using AssetRefreshReconcileCallback = Function<bool(const AssetRefreshIterationContext&, Array<AssetImportPlanRequest>&, bool&, AssetPipelineDiagnostic&)>;
using AssetRefreshExecuteCallback = Function<bool(const AssetRefreshIterationContext&, const Array<AssetImportPlan>&, Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)>;

struct FLAXENGINE_API AssetRefreshCallbacks
{
    AssetRefreshReconcileCallback Reconcile;
    AssetRefreshExecuteCallback Execute;
};

struct FLAXENGINE_API AssetRefreshResult
{
    int32 Iterations = 0;
    bool Queued = false;
    Array<AssetImportCompletion> Completed;
};

/// <summary>Coalesced, bounded fixed-point refresh with registry-generation restart.</summary>
class FLAXENGINE_API AssetRefreshCoordinator : public NonCopyable
{
    AssetImporterRegistry& _importers;
    AssetImportPlanner& _planner;
    AssetPostprocessorRegistry& _postprocessors;
    mutable CriticalSection _locker;
    uint32 _pendingReasons = 0;
    bool _running = false;
    int32 _maximumIterations;

public:
    AssetRefreshCoordinator(AssetImporterRegistry& importers, AssetImportPlanner& planner,
                            AssetPostprocessorRegistry& postprocessors, int32 maximumIterations = 16);

    void RequestRefresh(AssetRefreshReason reason);
    bool Refresh(AssetRefreshReason reason, const AssetRefreshCallbacks& callbacks,
                 AssetRefreshResult& result, AssetPipelineDiagnostic& diagnostic);
    bool IsRunning() const;

private:
    void EndRun();
};
