// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Importing/AssetImportContext.h"
#include "Engine/Content/Importing/AssetImportPlanner.h"
#include "Engine/Content/Importing/AssetPostprocessor.h"
#include "Engine/Content/Importing/AssetRefreshCoordinator.h"
#include "Engine/Content/Importing/CustomDependencyRegistry.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    AssetImporterDescriptor MakeImporter(const StringView& id, const StringView& extension, int32 priority = 0)
    {
        AssetImporterDescriptor descriptor;
        descriptor.ID = id;
        descriptor.Extensions.Add(extension);
        descriptor.Priority = priority;
        const StringAnsi implementation(id);
        descriptor.ImplementationHash = ContentHash::Compute(implementation.Get(), implementation.Length());
        descriptor.Import = [](AssetImportContext&, AssetPipelineDiagnostic&) { return false; };
        return descriptor;
    }

    AssetImportPlanRequest MakeRequest(const AssetGuid& asset, uint64 revision)
    {
        AssetImportPlanRequest request;
        request.Asset = asset;
        request.SourcePath = TEXT("Assets/model.foo");
        request.SourceRevision = revision;
        request.SourceHash = ContentHash::Compute("source", 6);
        request.MetadataHash = ContentHash::Compute("metadata", 8);
        return request;
    }
}

TEST_CASE("AssetImporterRegistry selection is deterministic and supports explicit overrides")
{
    AssetImporterRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    AssetImporterRegistration lower;
    AssetImporterRegistration higher;
    REQUIRE_FALSE(registry.Register(MakeImporter(TEXT("Tests.Lower"), TEXT(".foo"), 10), lower, diagnostic));
    REQUIRE_FALSE(registry.Register(MakeImporter(TEXT("Tests.Higher"), TEXT(".foo"), 20), higher, diagnostic));

    AssetImporterSelectionRequest request;
    request.SourcePath = TEXT("Assets/file.FOO");
    AssetImporterLease selected;
    REQUIRE_FALSE(registry.Resolve(request, selected, diagnostic));
    CHECK(selected.Get().ID == TEXT("Tests.Higher"));
    selected.Reset();

    request.ExplicitImporterID = TEXT("Tests.Lower");
    REQUIRE_FALSE(registry.Resolve(request, selected, diagnostic));
    CHECK(selected.Get().ID == TEXT("Tests.Lower"));
    selected.Reset();
}

TEST_CASE("AssetImportContext records controlled reads dependencies and declared objects")
{
    const AssetGuid asset(Guid::New());
    ArtifactTarget target;
    AssetImportContext context(asset, TEXT("Assets/model.foo"), target, "{}", [](const StringView& path, Array<byte>& bytes, ContentHash& hash, AssetPipelineDiagnostic&)
    {
        bytes.Add(42);
        const StringAnsi narrow(path);
        hash = ContentHash::Compute(narrow.Get(), narrow.Length());
        return false;
    });
    Array<byte> bytes;
    ContentHash hash;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(context.ReadSource(bytes, hash, diagnostic));
    const int32 object = context.AddObjectToAsset(TEXT("main:object"), TEXT("FlaxEngine.Model"));
    REQUIRE(object >= 0);
    REQUIRE_FALSE(context.SetMainObject(object, diagnostic));
    context.DependsOnCustomDependency(TEXT("render-pipeline"), ContentHash::Compute("rp", 2));
    const ArtifactKey exactArtifact(ContentHash::Compute("exact", 5));
    context.DependsOnArtifact(exactArtifact);
    const int32 output = context.CreateOutput(TEXT("runtime"), "runtime", ".bin", ArtifactTargetDimension::Platform);
    REQUIRE(output >= 0);
    AssetImportContextResult result;
    REQUIRE_FALSE(context.Complete(true, result, diagnostic));
    CHECK(result.MainObject == object);
    CHECK(result.Objects.Count() == 1);
    CHECK(result.Dependencies.Count() == 3);
    REQUIRE(result.Outputs.Count() == 1);
    CHECK(result.Outputs[0].TargetDimensions == ArtifactTargetDimension::Platform);
}

TEST_CASE("Process-safe native callback importers require an external worker")
{
    AssetImporterRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    AssetImporterRegistration registration;
    AssetImporterDescriptor descriptor = MakeImporter(TEXT("Tests.NativeWorker"), TEXT(".native"));
    descriptor.ProviderKind = AssetProcessorProviderKind::Native;
    descriptor.ProcessSafe = true;
    CHECK(registry.Register(descriptor, registration, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);

    descriptor.WorkerExecutable = TEXT("Binaries/NativeImportWorker.exe");
    REQUIRE_FALSE(registry.Register(descriptor, registration, diagnostic));
}

TEST_CASE("AssetImportPlanner coalesces revisions and pins importer lifetime")
{
    AssetImporterRegistry registry;
    AssetImporterRegistration registration;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(registry.Register(MakeImporter(TEXT("Tests.Foo"), TEXT(".foo")), registration, diagnostic));
    AssetImportPlanner planner(registry);
    Array<AssetImportPlanRequest> requests;
    const AssetGuid asset(Guid::New());
    requests.Add(MakeRequest(asset, 1));
    requests.Add(MakeRequest(asset, 2));
    Array<AssetImportPlan> plans;
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    REQUIRE(plans.Count() == 1);
    CHECK(plans[0].Request.SourceRevision == 2);
    CHECK(plans[0].ImporterLease != nullptr);
    plans.Clear();
}

TEST_CASE("AssetImportPlanner fingerprints source basename but not parent path")
{
    AssetImporterRegistry registry;
    AssetImporterRegistration registration;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(registry.Register(MakeImporter(TEXT("Tests.PathSemantics"), TEXT(".foo")), registration, diagnostic));
    AssetImportPlanner planner(registry);
    AssetImportPlanRequest request = MakeRequest(AssetGuid(Guid::New()), 1);
    Array<AssetImportPlanRequest> requests;
    Array<AssetImportPlan> plans;

    request.SourcePath = TEXT("Assets/First/model.foo");
    requests.Add(request);
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    const ArtifactKey original = plans[0].StaticFingerprint;

    requests[0].SourcePath = TEXT("Assets/Second/model.foo");
    plans.Clear();
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    CHECK(plans[0].StaticFingerprint == original);

    requests[0].SourcePath = TEXT("Assets/Second/renamed.foo");
    plans.Clear();
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    CHECK(plans[0].StaticFingerprint != original);
}

TEST_CASE("AssetRefreshCoordinator restarts on importer generation and reaches a fixed point")
{
    AssetImporterRegistry importers;
    AssetImporterRegistration first;
    AssetImporterRegistration second;
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(importers.Register(MakeImporter(TEXT("Tests.First"), TEXT(".foo"), 1), first, diagnostic));
    AssetImportPlanner planner(importers);
    AssetPostprocessorRegistry postprocessors;
    AssetRefreshCoordinator refresh(importers, planner, postprocessors, 4);
    const AssetGuid asset(Guid::New());

    AssetRefreshCallbacks callbacks;
    callbacks.Reconcile = [&](const AssetRefreshIterationContext& context, Array<AssetImportPlanRequest>& requests, bool&, AssetPipelineDiagnostic& localDiagnostic)
    {
        if (context.Iteration == 1)
        {
            if (importers.Register(MakeImporter(TEXT("Tests.Second"), TEXT(".foo"), 2), second, localDiagnostic))
                return true;
        }
        requests.Add(MakeRequest(asset, context.Iteration));
        return false;
    };
    callbacks.Execute = [](const AssetRefreshIterationContext&, const Array<AssetImportPlan>& plans, Array<AssetImportCompletion>& completed, bool&, AssetPipelineDiagnostic&)
    {
        for (const AssetImportPlan& plan : plans)
        {
            AssetImportCompletion completion;
            completion.Asset = plan.Request.Asset;
            completion.SourcePath = plan.Request.SourcePath;
            completion.Artifact = plan.StaticFingerprint;
            completion.Succeeded = true;
            completed.Add(MoveTemp(completion));
        }
        return false;
    };
    AssetRefreshResult result;
    REQUIRE_FALSE(refresh.Refresh(AssetRefreshReason::Explicit, callbacks, result, diagnostic));
    CHECK(result.Iterations == 2);
    REQUIRE(result.Completed.Count() == 1);
}

TEST_CASE("AssetRefreshCoordinator reports non-converging refresh")
{
    AssetImporterRegistry importers;
    AssetImportPlanner planner(importers);
    AssetPostprocessorRegistry postprocessors;
    AssetRefreshCoordinator refresh(importers, planner, postprocessors, 2);
    AssetRefreshCallbacks callbacks;
    callbacks.Reconcile = [](const AssetRefreshIterationContext&, Array<AssetImportPlanRequest>&, bool& changed, AssetPipelineDiagnostic&)
    {
        changed = true;
        return false;
    };
    callbacks.Execute = [](const AssetRefreshIterationContext&, const Array<AssetImportPlan>&, Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)
    {
        return false;
    };
    AssetRefreshResult result;
    AssetPipelineDiagnostic diagnostic;
    CHECK(refresh.Refresh(AssetRefreshReason::Explicit, callbacks, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::BuildCycle);
    CHECK(result.Iterations == 2);
}

TEST_CASE("Postprocessor batches and custom dependencies are deterministic")
{
    AssetPostprocessorRegistry postprocessors;
    AssetPostprocessorRegistration late;
    AssetPostprocessorRegistration early;
    AssetPipelineDiagnostic diagnostic;
    String order;
    AssetPostprocessorDescriptor lateDescriptor;
    lateDescriptor.ID = TEXT("Tests.Late");
    lateDescriptor.Order = 20;
    lateDescriptor.ImplementationHash = ContentHash::Compute("late", 4);
    lateDescriptor.ProcessBatch = [&order](const Array<AssetImportCompletion>&, bool& changed, AssetPipelineDiagnostic&)
    {
        order += TEXT("L");
        changed = true;
        return false;
    };
    AssetPostprocessorDescriptor earlyDescriptor;
    earlyDescriptor.ID = TEXT("Tests.Early");
    earlyDescriptor.Order = 10;
    earlyDescriptor.ImplementationHash = ContentHash::Compute("early", 5);
    earlyDescriptor.ProcessBatch = [&order](const Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)
    {
        order += TEXT("E");
        return false;
    };
    REQUIRE_FALSE(postprocessors.Register(lateDescriptor, late, diagnostic));
    REQUIRE_FALSE(postprocessors.Register(earlyDescriptor, early, diagnostic));
    Array<AssetImportCompletion> completed;
    bool changed = false;
    REQUIRE_FALSE(postprocessors.RunBatch(completed, changed, diagnostic));
    CHECK(order == TEXT("EL"));
    CHECK(changed);

    CustomDependencyRegistry dependencies;
    const ContentHash value = ContentHash::Compute("value", 5);
    bool dependencyChanged = false;
    REQUIRE_FALSE(dependencies.Register(TEXT("render-pipeline"), value, dependencyChanged, diagnostic));
    CHECK(dependencyChanged);
    REQUIRE_FALSE(dependencies.Register(TEXT("render-pipeline"), value, dependencyChanged, diagnostic));
    CHECK_FALSE(dependencyChanged);
    Array<String> names;
    dependencies.ConsumeChanges(names);
    REQUIRE(names.Count() == 1);
    CHECK(names[0] == TEXT("render-pipeline"));
}
