// Copyright (c) Wojciech Figat. All rights reserved.

#include "ArtifactResolver.h"
#include "ArtifactCompatibility.h"
#include "ArtifactStore.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    struct ArtifactInspection
    {
        bool HasOutput = false;
        bool IsCompatible = false;
        ArtifactManifest Manifest;
        ResolvedArtifact Artifact;
        AssetPipelineDiagnostic InvalidDiagnostic;
    };

    bool ResolveFail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const ArtifactRequest& request,
        const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = request.Object.Asset.Value;
        diagnostic.SourcePath = path;
        diagnostic.OutputKind = String(request.OutputKind);
        diagnostic.Message = message;
        return true;
    }

    bool IsBuildableStatus(AssetRecordStatus status)
    {
        return status == AssetRecordStatus::Ready || status == AssetRecordStatus::Stale || status == AssetRecordStatus::Building || status == AssetRecordStatus::Failed;
    }

    bool IsCancelled(const ArtifactRequest& request)
    {
        return request.IsCancellationRequested.IsBinded() && request.IsCancellationRequested();
    }

    AssetPipelineDiagnosticCode StatusCode(AssetRecordStatus status)
    {
        if (status == AssetRecordStatus::MissingSource || status == AssetRecordStatus::OrphanMeta)
            return AssetPipelineDiagnosticCode::SourceMissing;
        if (status == AssetRecordStatus::UnsupportedProcessor)
            return AssetPipelineDiagnosticCode::ProcessorMissing;
        if (status == AssetRecordStatus::MissingDependency)
            return AssetPipelineDiagnosticCode::ArtifactMissing;
        if (status == AssetRecordStatus::DuplicateGuid)
            return AssetPipelineDiagnosticCode::DuplicateGuid;
        if (status == AssetRecordStatus::MetaUpgradeRequired || status == AssetRecordStatus::DocumentUpgradeRequired)
            return AssetPipelineDiagnosticCode::MetaUpgradeRequired;
        return AssetPipelineDiagnosticCode::InvalidMeta;
    }

    bool Inspect(const StringView& libraryRoot, const AssetRecord& record, const ArtifactRequest& request, ArtifactInspection& result)
    {
        result = ArtifactInspection();
        ArtifactStoragePath manifestPath;
        AssetPipelineDiagnostic diagnostic;
        if (ArtifactStore::TryGetManifestPath(libraryRoot, request.Target, request.Object, manifestPath, diagnostic))
        {
            result.InvalidDiagnostic = diagnostic;
            return false;
        }
        if (!FileSystem::FileExists(manifestPath.Get()))
            return false;
        StringAnsi json;
        if (File::ReadAllText(manifestPath.Get(), json) || ArtifactManifest::Parse(json, manifestPath.Get(), result.Manifest, diagnostic) ||
            result.Manifest.ObjectID != request.Object || result.Manifest.Target.BuildKey(ArtifactTargetDimension::All) != request.Target.BuildKey(ArtifactTargetDimension::All))
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                ResolveFail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, request, manifestPath.Get(), TEXT("Current artifact manifest identity or target is invalid."));
            result.InvalidDiagnostic = diagnostic;
            return false;
        }
        const ArtifactManifestOutput* selected = nullptr;
        for (const ArtifactManifestOutput& output : result.Manifest.Outputs)
        {
            if (output.Kind == request.OutputKind)
            {
                selected = &output;
                break;
            }
        }
        if (!selected)
            return false;
        ArtifactStoragePath outputPath;
        if (ArtifactStore::TryResolveLibraryRelative(libraryRoot, selected->RelativePath, outputPath, diagnostic) || !FileSystem::FileExists(outputPath.Get()))
        {
            ResolveFail(result.InvalidDiagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, request, selected->RelativePath, TEXT("Selected artifact output is missing or outside Library."));
            return false;
        }
        Array<byte> bytes;
        if (File::ReadAllBytes(outputPath.Get(), bytes) || static_cast<uint64>(bytes.Count()) != selected->Size ||
            ContentHash::Compute(bytes.Get(), bytes.Count()) != selected->Content)
        {
            ResolveFail(result.InvalidDiagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, request, outputPath.Get(), TEXT("Selected artifact output bytes do not match the manifest."));
            return false;
        }
        result.HasOutput = true;
        result.IsCompatible = ArtifactCompatibility::IsCompatible(request.RequiredCompatibility, selected->Compatibility);
        result.Artifact.ObjectID = request.Object;
        result.Artifact.AssetID = record.ID;
        result.Artifact.TypeName = record.TypeName;
        result.Artifact.StoragePath = outputPath;
        result.Artifact.OutputKind = String(selected->Kind);
        result.Artifact.Key = String(selected->Key.ToString());
        result.Artifact.Content = selected->Content;
        result.Artifact.Size = selected->Size;
        result.Artifact.StorageKind = ArtifactStorageKind::Generated;
        return false;
    }
}

ArtifactResolver& ArtifactResolver::Get()
{
    static ArtifactResolver instance;
    return instance;
}

void ArtifactResolver::Configure(AssetDatabase& database, AssetBuildService& buildService, const StringView& libraryRoot,
    const ArtifactTarget& defaultTarget, const ArtifactResolutionPlanProvider& planProvider)
{
    _database = &database;
    _buildService = &buildService;
    _libraryRoot = libraryRoot;
    _defaultTarget = defaultTarget;
    _planProvider = planProvider;
}

void ArtifactResolver::Reset()
{
    _database = nullptr;
    _buildService = nullptr;
    _libraryRoot.Clear();
    _defaultTarget = ArtifactTarget();
    _planProvider.Unbind();
}

bool ArtifactResolver::IsConfigured() const
{
    return _database && _buildService && !_libraryRoot.IsEmpty() && _planProvider.IsBinded();
}

bool ArtifactResolver::IsExactCurrent(const ArtifactRequest& request, const ArtifactKey& inputFingerprint) const
{
    if (!IsConfigured() || !request.Object.IsValid() || request.OutputKind.IsEmpty() || inputFingerprint.IsZero())
        return false;
    AssetRecord record;
    if (!_database->TryGetRecord(request.Object, record))
        return false;
    ArtifactInspection inspection;
    Inspect(_libraryRoot, record, request, inspection);
    const bool compatibilityMatches = request.RequiredCompatibility.IsEmpty() || inspection.IsCompatible;
    return inspection.HasOutput && compatibilityMatches && inspection.Manifest.InputFingerprint == inputFingerprint;
}

bool ArtifactResolver::Resolve(const ArtifactRequest& request, ResolvedArtifact& result, AssetPipelineDiagnostic& diagnostic)
{
    result = ResolvedArtifact();
    diagnostic = AssetPipelineDiagnostic();
    if (!IsConfigured() || !request.Object.IsValid() || request.OutputKind.IsEmpty())
        return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, StringView::Empty, TEXT("Artifact resolver is not configured or the request is incomplete."));
    constexpr int32 MaxExactResolveAttempts = 8;
    for (int32 resolveAttempt = 0; resolveAttempt < MaxExactResolveAttempts; resolveAttempt++)
    {
        if (IsCancelled(request))
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request, StringView::Empty, TEXT("Exact artifact resolution was cancelled."));
        AssetRecord record;
        if (!_database->TryGetRecord(request.Object, record))
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, request, StringView::Empty, TEXT("Asset database contains no record for the requested object."));
        ArtifactInspection inspection;
        Inspect(_libraryRoot, record, request, inspection);
        if (request.Policy == ArtifactResolvePolicy::PublishedOnly)
        {
            if (inspection.HasOutput && inspection.IsCompatible)
            {
                result = inspection.Artifact;
                result.IsExact = false;
                result.IsLastGood = true;
                return false;
            }
            if (inspection.InvalidDiagnostic.Code != AssetPipelineDiagnosticCode::None)
            {
                diagnostic = inspection.InvalidDiagnostic;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                return true;
            }
            const AssetPipelineDiagnosticCode code = inspection.HasOutput
                ? AssetPipelineDiagnosticCode::ArtifactIncompatible
                : AssetPipelineDiagnosticCode::ArtifactMissing;
            return ResolveFail(diagnostic, code, request, record.SourcePath.Get(), TEXT("No compatible published artifact is available."));
        }
        if (!IsBuildableStatus(record.Status))
        {
            if (request.Policy == ArtifactResolvePolicy::Interactive && inspection.HasOutput && inspection.IsCompatible)
            {
                result = inspection.Artifact;
                result.IsExact = false;
                result.IsLastGood = true;
                return false;
            }
            return ResolveFail(diagnostic, StatusCode(record.Status), request, record.SourcePath.Get(), TEXT("Asset database state blocks an exact artifact build."));
        }

        ArtifactResolutionPlan plan;
        if (_planProvider(record, request, plan, diagnostic) || plan.CurrentInputFingerprint.IsZero() || !plan.BuildRequest.Key.IsValid())
        {
            if (diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated && resolveAttempt + 1 < MaxExactResolveAttempts)
                continue;
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, record.SourcePath.Get(), TEXT("Artifact resolution plan is incomplete."));
            return true;
        }
        plan.BuildRequest.RefreshId = request.RefreshId;
        plan.BuildRequest.Pass = request.Pass;
        const bool compatibilityMatches = request.RequiredCompatibility.IsEmpty() || inspection.IsCompatible;
        const bool hasExact = inspection.HasOutput && compatibilityMatches && inspection.Manifest.InputFingerprint == plan.CurrentInputFingerprint;
        if (hasExact)
        {
            result = inspection.Artifact;
            result.IsExact = true;
            result.IsLastGood = false;
            return false;
        }
        if (request.Policy == ArtifactResolvePolicy::NoBuild)
        {
            if (inspection.InvalidDiagnostic.Code != AssetPipelineDiagnosticCode::None)
            {
                diagnostic = inspection.InvalidDiagnostic;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
                return true;
            }
            const AssetPipelineDiagnosticCode code = inspection.HasOutput && !compatibilityMatches
                ? AssetPipelineDiagnosticCode::ArtifactIncompatible
                : AssetPipelineDiagnosticCode::ArtifactRebuildRequired;
            return ResolveFail(diagnostic, code, request, record.SourcePath.Get(), TEXT("No exact artifact is available and NoBuild policy forbids scheduling work."));
        }
        if (request.Policy == ArtifactResolvePolicy::Interactive && inspection.HasOutput && inspection.IsCompatible)
        {
            result = inspection.Artifact;
            result.IsExact = false;
            result.IsLastGood = true;
            _buildService->Request(plan.BuildRequest);
            return false;
        }

        // A caller is about to wait for this exact object. Let it pass queued bulk refresh work,
        // while an interactive last-good refresh above remains normal background work.
        plan.BuildRequest.Priority = AssetBuildJobPriority::Foreground;
        const AssetBuildRequestHandle handle = _buildService->Request(plan.BuildRequest);
        if (!handle.IsValid())
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, record.SourcePath.Get(), TEXT("Exact artifact build could not be awaited."));
        while (!handle.Wait(50))
        {
            if (IsCancelled(request))
            {
                _buildService->CancelRequester(handle);
                return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request, record.SourcePath.Get(), TEXT("Exact artifact build wait was cancelled."));
            }
        }
        if (IsCancelled(request))
        {
            _buildService->CancelRequester(handle);
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, request, record.SourcePath.Get(), TEXT("Exact artifact build wait was cancelled."));
        }
        AssetBuildJobResult buildResult;
        if (!handle.TryGetResult(buildResult) || buildResult.Status != AssetBuildJobStatus::Succeeded)
        {
            diagnostic = buildResult.Diagnostic;
            if (diagnostic.Code == AssetPipelineDiagnosticCode::PrepareInvalidated && resolveAttempt + 1 < MaxExactResolveAttempts)
                continue;
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                ResolveFail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, request, record.SourcePath.Get(), TEXT("Exact artifact build failed."));
            diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
            return true;
        }
        AssetRecord currentRecord;
        if (!_database->TryGetRecord(request.Object, currentRecord))
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, request, record.SourcePath.Get(), TEXT("Asset database lost the requested record while waiting for the exact build."));
        if (currentRecord.DatabaseRevision != record.DatabaseRevision)
        {
            if (resolveAttempt + 1 < MaxExactResolveAttempts)
                continue;
            return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, request, record.SourcePath.Get(), TEXT("Asset database changed while waiting for the exact build."));
        }
        ArtifactInspection built;
        Inspect(_libraryRoot, currentRecord, request, built);
        const bool builtCompatibilityMatches = request.RequiredCompatibility.IsEmpty() || built.IsCompatible;
        if (!built.HasOutput || !builtCompatibilityMatches || built.Manifest.InputFingerprint != plan.CurrentInputFingerprint)
        {
            if (built.InvalidDiagnostic.Code != AssetPipelineDiagnosticCode::None)
                diagnostic = built.InvalidDiagnostic;
            else
                ResolveFail(diagnostic, built.HasOutput && !builtCompatibilityMatches ? AssetPipelineDiagnosticCode::ArtifactIncompatible : AssetPipelineDiagnosticCode::ArtifactMissing,
                    request, currentRecord.SourcePath.Get(), TEXT("Build completed without publishing the required exact compatible output."));
            diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
            return true;
        }
        result = built.Artifact;
        result.IsExact = true;
        result.IsLastGood = false;
        return false;
    }
    return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, request, StringView::Empty, TEXT("Asset database did not stabilize while resolving the exact artifact."));
}

bool ArtifactResolver::ResolveLoadLocation(const ArtifactRequest& request, AssetLoadLocation& result, AssetPipelineDiagnostic& diagnostic)
{
    result = AssetLoadLocation();
    ResolvedArtifact artifact;
    if (Resolve(request, artifact, diagnostic))
        return true;
    AssetRecord record;
    if (!_database->TryGetRecord(request.Object, record))
        return ResolveFail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, request, StringView::Empty, TEXT("Resolved artifact lost its canonical database record."));
    result.Info = record.ToAssetInfo();
    result.Artifact = artifact;
    return false;
}
