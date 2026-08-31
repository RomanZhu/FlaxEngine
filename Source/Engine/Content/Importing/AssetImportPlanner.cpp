// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetImportPlanner.h"
#include "Engine/Core/Collections/Dictionary.h"
#include <algorithm>

namespace
{
    bool PlanFailure(AssetPipelineDiagnostic& diagnostic, const AssetImportPlanRequest& request, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = request.Asset.Value;
        diagnostic.SourcePath = request.SourcePath;
        diagnostic.Message = message;
        return true;
    }

    String RequestKey(const AssetImportPlanRequest& request)
    {
        return String::Format(TEXT("{0}:{1}"), request.Asset.Value, String(request.Target.BuildKey(ArtifactTargetDimension::All).ToString()));
    }
}

bool AssetImportPlanner::Build(const Array<AssetImportPlanRequest>& requests, Array<AssetImportPlan>& plans, AssetPipelineDiagnostic& diagnostic)
{
    plans.Clear();
    Array<AssetImportPlanRequest> coalesced;
    Dictionary<String, int32> indices;
    for (const AssetImportPlanRequest& request : requests)
    {
        if (!request.Asset.IsValid() || request.SourcePath.IsEmpty() || request.SourceHash.IsZero())
            return PlanFailure(diagnostic, request, TEXT("Import request has no stable asset, source path, or source hash."));
        const String key = RequestKey(request);
        int32* index = indices.TryGet(key);
        if (!index)
        {
            indices.Add(key, coalesced.Count());
            coalesced.Add(request);
        }
        else
        {
            AssetImportPlanRequest& current = coalesced[*index];
            if (request.SourceRevision >= current.SourceRevision)
                current = request;
            else
                current.Force |= request.Force;
        }
    }
    std::sort(coalesced.Get(), coalesced.Get() + coalesced.Count(), [](const AssetImportPlanRequest& a, const AssetImportPlanRequest& b)
    {
        const int32 pathComparison = a.SourcePath.Compare(b.SourcePath);
        if (pathComparison != 0)
            return pathComparison < 0;
        return a.Asset.ToString() < b.Asset.ToString();
    });

    const uint64 registryGeneration = _registry.GetGeneration();
    for (const AssetImportPlanRequest& request : coalesced)
    {
        AssetImporterSelectionRequest selection;
        selection.SourcePath = request.SourcePath;
        selection.ExplicitImporterID = request.ExplicitImporterID;
        selection.PreferTextFallback = request.PreferTextFallback;
        auto lease = std::make_shared<AssetImporterLease>();
        if (_registry.Resolve(selection, *lease, diagnostic))
        {
            diagnostic.AssetGuid = request.Asset.Value;
            diagnostic.SourcePath = request.SourcePath;
            return true;
        }
        const AssetImporterDescriptor descriptor = lease->Get();
        ArtifactKeyBuilder builder("flax-asset-import-plan-v1");
        builder.AddGuid("asset", request.Asset.Value);
        if (descriptor.PathSensitive)
            builder.AddString("source-path", request.SourcePath);
        builder.AddHash("source", request.SourceHash);
        builder.AddHash("metadata", request.MetadataHash);
        builder.AddHash("postprocessors", request.EffectivePostprocessorHash);
        builder.AddHash("project-settings", request.ProjectSettingsHash);
        builder.AddUInt32("engine-serialization", request.EngineSerializationVersion);
        builder.AddUInt32("artifact-schema", request.ArtifactSchemaVersion);
        builder.AddString("importer", descriptor.ID);
        builder.AddUInt32("importer-version", descriptor.ImporterVersion);
        builder.AddHash("implementation", descriptor.ImplementationHash);
        builder.AddTarget(request.Target, ArtifactTargetDimension::All);

        AssetImportPlan plan;
        plan.Request = request;
        plan.Importer = descriptor;
        plan.ImporterLease = MoveTemp(lease);
        plan.ImporterRegistryGeneration = registryGeneration;
        plan.StaticFingerprint = builder.Finalize();
        plan.KeyComponents = builder.GetComponents();
        plans.Add(MoveTemp(plan));
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
}
