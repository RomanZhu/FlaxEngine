// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetRefreshCoordinator.h"

namespace
{
    uint32 ToReasons(AssetRefreshReason reason)
    {
        return static_cast<uint32>(reason);
    }
}

AssetRefreshCoordinator::AssetRefreshCoordinator(AssetImporterRegistry& importers, AssetImportPlanner& planner,
                                                 AssetPostprocessorRegistry& postprocessors, int32 maximumIterations)
    : _importers(importers)
    , _planner(planner)
    , _postprocessors(postprocessors)
    , _maximumIterations(maximumIterations > 0 ? maximumIterations : 1)
{
}

void AssetRefreshCoordinator::RequestRefresh(AssetRefreshReason reason)
{
    ScopeLock lock(_locker);
    _pendingReasons |= ToReasons(reason);
}

bool AssetRefreshCoordinator::IsRunning() const
{
    ScopeLock lock(_locker);
    return _running;
}

void AssetRefreshCoordinator::EndRun(uint32 retryReasons)
{
    ScopeLock lock(_locker);
    _pendingReasons |= retryReasons;
    _running = false;
}

bool AssetRefreshCoordinator::Refresh(AssetRefreshReason reason, const AssetRefreshCallbacks& callbacks,
                                      AssetRefreshResult& result, AssetPipelineDiagnostic& diagnostic)
{
    result = AssetRefreshResult();
    uint32 reasons;
    {
        ScopeLock lock(_locker);
        _pendingReasons |= ToReasons(reason);
        if (_running)
        {
            result.Queued = true;
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
        if (!callbacks.Reconcile.IsBinded() || !callbacks.Execute.IsBinded())
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
            diagnostic.Message = TEXT("Refresh coordinator callbacks are incomplete.");
            return true;
        }
        _running = true;
        reasons = _pendingReasons;
        _pendingReasons = 0;
    }

    result.RefreshId = Guid::New();
    diagnostic = AssetPipelineDiagnostic();
    AssetPipelineDiagnostic startPersistenceDiagnostic;
    if (callbacks.Session.IsBinded() && callbacks.Session(result, AssetRefreshRunState::Started, diagnostic,
        startPersistenceDiagnostic))
    {
        diagnostic = MoveTemp(startPersistenceDiagnostic);
        EndRun(reasons);
        return true;
    }
    const auto finish = [&](bool failed, uint32 retryReasons)
    {
        result.Pass = result.Iterations;
        if (callbacks.Session.IsBinded())
        {
            const AssetPipelineDiagnostic terminalDiagnostic = diagnostic;
            AssetPipelineDiagnostic persistenceDiagnostic;
            const AssetRefreshRunState state = failed ? AssetRefreshRunState::Failed : AssetRefreshRunState::Succeeded;
            if (callbacks.Session(result, state, terminalDiagnostic, persistenceDiagnostic))
            {
                if (failed)
                    diagnostic.Related.Add(TEXT("Refresh session terminal persistence failed: ") + persistenceDiagnostic.Message);
                else
                    diagnostic = MoveTemp(persistenceDiagnostic);
                failed = true;
            }
        }
        EndRun(retryReasons);
        if (!failed)
            diagnostic = AssetPipelineDiagnostic();
        return failed;
    };

    for (int32 iteration = 1; iteration <= _maximumIterations; iteration++)
    {
        result.Iterations = iteration;
        result.Pass = iteration;
        const uint64 importerGeneration = _importers.GetGeneration();
        const uint64 postprocessorGeneration = _postprocessors.GetGeneration();
        AssetRefreshIterationContext context;
        context.RefreshId = result.RefreshId;
        context.Pass = iteration;
        context.Iteration = iteration;
        context.ImporterRegistryGeneration = importerGeneration;
        context.Reasons = static_cast<AssetRefreshReason>(reasons);

        bool sourceChanged = false;
        Array<AssetImportPlanRequest> requests;
        if (callbacks.Reconcile(context, requests, sourceChanged, diagnostic))
            return finish(true, reasons);
        const ContentHash effectivePostprocessors = _postprocessors.GetVersionKey().Digest;
        for (AssetImportPlanRequest& request : requests)
        {
            request.RefreshId = context.RefreshId;
            request.Pass = context.Pass;
            if (request.EffectivePostprocessorHash.IsZero())
                request.EffectivePostprocessorHash = effectivePostprocessors;
        }
        Array<AssetImportPlan> plans;
        if (_planner.Build(requests, plans, diagnostic))
            return finish(true, reasons);
        for (const AssetImportPlan& plan : plans)
        {
            if (_postprocessors.RunPreprocess(plan, sourceChanged, diagnostic))
                return finish(true, reasons);
        }
        if (sourceChanged)
        {
            ScopeLock lock(_locker);
            _pendingReasons |= ToReasons(AssetRefreshReason::Postprocessor);
            if (_importers.GetGeneration() != importerGeneration || _postprocessors.GetGeneration() != postprocessorGeneration)
                _pendingReasons |= ToReasons(AssetRefreshReason::ImporterRegistry);
            reasons = _pendingReasons;
            _pendingReasons = 0;
            continue;
        }
        Array<AssetImportCompletion> completed;
        if (callbacks.Execute(context, plans, completed, sourceChanged, diagnostic))
            return finish(true, reasons);
        if (_postprocessors.RunBatch(completed, sourceChanged, diagnostic))
            return finish(true, reasons);
        result.Completed = MoveTemp(completed);

        const bool registryChanged = _importers.GetGeneration() != importerGeneration ||
            _postprocessors.GetGeneration() != postprocessorGeneration;
        bool complete = false;
        {
            ScopeLock lock(_locker);
            reasons = _pendingReasons;
            _pendingReasons = 0;
            if (sourceChanged)
                reasons |= ToReasons(AssetRefreshReason::Postprocessor);
            if (registryChanged)
                reasons |= ToReasons(AssetRefreshReason::ImporterRegistry);
            if (reasons == 0)
                complete = true;
        }
        if (complete)
            return finish(false, 0);
    }

    diagnostic = AssetPipelineDiagnostic();
    diagnostic.Code = AssetPipelineDiagnosticCode::BuildCycle;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.Message = TEXT("Asset refresh did not reach a fixed point within the configured iteration cap.");
    return finish(true, reasons);
}
