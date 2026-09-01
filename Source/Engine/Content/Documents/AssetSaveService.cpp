// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetSaveService.h"
#include "Engine/Content/AssetDatabase/AssetDatabaseServices.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code,
        const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    class DefaultAssetSavePipeline final : public IAssetSavePipeline
    {
    public:
        bool RefreshSource(const StringView& path, AssetPipelineDiagnostic& diagnostic) override
        {
            Array<String> paths;
            paths.Add(String(path));
            if (AssetPipelineService::RefreshSources(paths, true, diagnostic))
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                    Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, path,
                        TEXT("Authored source refresh failed after commit."));
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool ImportSource(const Guid& sourceID, bool force, bool synchronous,
            AssetPipelineDiagnostic& diagnostic) override
        {
            if (AssetPipelineService::BuildAsset(sourceID, force, synchronous))
            {
                diagnostic = AssetPipelineService::GetBuildDiagnostic(sourceID);
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                    Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, StringView::Empty,
                        TEXT("Authored source import failed after commit."));
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        void RegisterSelfWrite(const SourceSaveSelfWrite& write) override
        {
            AssetOperationService::RegisterSelfWrite(write.Path, write.Content);
        }
    } DefaultPipeline;

    bool IsCleanableSourceOutcome(SourceSaveOutcome outcome)
    {
        return outcome == SourceSaveOutcome::Committed || outcome == SourceSaveOutcome::Unchanged;
    }
}

bool AssetSaveResult::IsSourceCommitted() const
{
    return IsCleanableSourceOutcome(Source.Outcome);
}

bool AssetSaveResult::IsSuccessful() const
{
    return IsSourceCommitted() && Refresh != AssetSaveRefreshOutcome::Failed &&
        (Import == AssetSaveImportOutcome::NotRequested || Import == AssetSaveImportOutcome::Succeeded);
}

AssetSaveService::AssetSaveService(const ISourceSaveRevisionProvider* revisionProvider,
    IAssetSavePipeline* pipeline, DirtySourceRegistry* dirtyRegistry)
    : _revisionProvider(revisionProvider)
    , _pipeline(pipeline ? pipeline : &DefaultPipeline)
    , _dirtyRegistry(dirtyRegistry ? dirtyRegistry : &DirtySourceRegistry::Get())
{
}

AssetSaveService& AssetSaveService::Get()
{
    static AssetSaveService service;
    return service;
}

bool AssetSaveService::Save(const AssetSaveRequest& request, AssetSaveResult& result,
    AssetPipelineDiagnostic& diagnostic, ISourceSaveCallback* callback,
    ISourceSaveFailureInjector* failureInjector) const
{
    result = AssetSaveResult();
    if (request.SourcePath.IsEmpty() || request.CanonicalBytes.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, request.SourcePath,
            TEXT("Authored source save request is empty."));
    if (request.ImportMode != AssetSaveImportMode::None && request.RefreshMode == AssetSaveRefreshMode::None)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidSettingsCombination, request.SourcePath,
            TEXT("Authored source import requires targeted source refresh."));

    SourceSaveTransaction transaction(_revisionProvider);
    SourceSaveRevision expected;
    if (transaction.Capture(request.SourcePath, request.RegistrationMode, expected, diagnostic,
        request.ConflictPolicy))
        return true;
    if (request.RegistrationMode == SourceSaveRegistrationMode::AllowUnregistered &&
        !expected.IsTracked && expected.Exists)
    {
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, expected.SourcePath,
            TEXT("Existing authored source is not registered in the durable database."));
    }
    if (request.HasExpectedSourceHash)
        expected.SourceHash = request.ExpectedSourceHash;

    const uint64 editRevision = request.EditRevision;

    SourceSaveRequest commit;
    commit.RegistrationMode = expected.IsTracked
        ? SourceSaveRegistrationMode::RequireTracked
        : request.RegistrationMode;
    commit.ConflictPolicy = request.ConflictPolicy;
    commit.Expected = expected;
    commit.CanonicalBytes = request.CanonicalBytes;
    const bool commitFailed = transaction.Commit(commit, result.Source, diagnostic, callback, failureInjector);
    const AssetPipelineDiagnostic sourceDiagnostic = diagnostic;

    const bool adoptedUnchanged = result.Source.Outcome == SourceSaveOutcome::Unchanged &&
        request.ConflictPolicy == SourceSaveConflictPolicy::AdoptCurrent && result.Source.Current.IsTracked &&
        result.Source.Current.SourceHash != result.Source.Current.DurableSourceHash;
    const bool activatedTracked = result.Source.Current.IsTracked &&
        (result.Source.Outcome == SourceSaveOutcome::Committed ||
            result.Source.Outcome == SourceSaveOutcome::ActivatedDurabilityUncertain);
    const bool shouldRefresh = request.RefreshMode == AssetSaveRefreshMode::TrackedSource &&
        (activatedTracked || adoptedUnchanged);

    if (IsCleanableSourceOutcome(result.Source.Outcome) && editRevision != 0)
    {
        result.CommittedEditRevision = editRevision;
        result.DirtyCleared = _dirtyRegistry->ClearCommitted(expected.SourcePath, editRevision);
    }

    if (shouldRefresh)
    {
        const String& refreshPath = activatedTracked
            ? result.Source.SelfWrite.Path
            : result.Source.Current.SourcePath;
        const bool refreshFailed = _pipeline->RefreshSource(refreshPath, diagnostic);
        result.Refresh = refreshFailed ? AssetSaveRefreshOutcome::Failed : AssetSaveRefreshOutcome::Succeeded;
        if (!refreshFailed && activatedTracked)
            _pipeline->RegisterSelfWrite(result.Source.SelfWrite);
        if (refreshFailed)
        {
            if (request.ImportMode != AssetSaveImportMode::None)
                result.Import = AssetSaveImportOutcome::Blocked;
            if (commitFailed)
                diagnostic = sourceDiagnostic;
            return true;
        }
    }

    if (request.ImportMode != AssetSaveImportMode::None)
    {
        if (commitFailed)
        {
            result.Import = AssetSaveImportOutcome::Blocked;
        }
        else
        {
            if (!result.Source.Current.IsTracked || !result.Source.Current.SourceAssetID.IsValid())
            {
                result.Import = AssetSaveImportOutcome::Failed;
                return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, expected.SourcePath,
                    TEXT("Authored source import requires a tracked source identity."));
            }
            const bool importFailed = _pipeline->ImportSource(result.Source.Current.SourceAssetID,
                request.ForceImport, request.ImportMode == AssetSaveImportMode::Synchronous, diagnostic);
            result.Import = importFailed ? AssetSaveImportOutcome::Failed : AssetSaveImportOutcome::Succeeded;
            if (importFailed)
                return true;
        }
    }

    if (commitFailed)
    {
        diagnostic = sourceDiagnostic;
        return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
