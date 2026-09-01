// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Importing/AssetImportContext.h"
#include "Engine/Content/Importing/AssetImportPlanner.h"
#include "Engine/Content/Importing/AssetPostprocessor.h"
#include "Engine/Content/Importing/AssetRefreshCoordinator.h"
#include "Engine/Content/Importing/AssetImportWorkerProtocol.h"
#include "Engine/Content/Importing/CustomDependencyRegistry.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
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
        request.SourcePath = TEXT("Content/model.foo");
        request.SourceRevision = revision;
        request.SourceHash = ContentHash::Compute("source", 6);
        request.MetadataHash = ContentHash::Compute("metadata", 8);
        return request;
    }

    AssetImportJobRequest MakeWorkerRequest(const StringView& stagingPath)
    {
        AssetImportJobRequest request;
        request.JobID = Guid::New();
        request.Capability = Guid::New();
        request.Asset = AssetGuid(Guid::New());
        request.SourceRevision = 7;
        request.SourcePath = TEXT("Content/model.foo");
        request.SourceSnapshot.Add(1);
        request.SourceSnapshot.Add(2);
        request.SourceHash = ContentHash::Compute(request.SourceSnapshot.Get(), request.SourceSnapshot.Count());
        request.MetaSnapshot.Add(3);
        request.MetaHash = ContentHash::Compute(request.MetaSnapshot.Get(), request.MetaSnapshot.Count());
        AssetImportWorkerInput input;
        input.Identity = TEXT("texture:albedo");
        input.CanonicalPath = TEXT("Content/albedo.png");
        input.Snapshot.Add(4);
        input.Hash = ContentHash::Compute(input.Snapshot.Get(), input.Snapshot.Count());
        request.AuthorizedInputs.Add(input);
        AssetImportWorkerTool tool;
        tool.Name = TEXT("Tests.Tool");
        tool.VersionHash = ContentHash::Compute("tool", 4);
        request.AllowedTools.Add(tool);
        request.Importer.ID = TEXT("Tests.Worker");
        request.Importer.ImplementationHash = ContentHash::Compute("worker", 6);
        request.Importer.ProducesMainObject = false;
        request.OutputStagingPath = stagingPath;
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
    request.SourcePath = TEXT("Content/file.FOO");
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
    const String outputStaging = Globals::TemporaryFolder / (TEXT("AssetImportContextOutput-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(outputStaging));
    SCOPE_EXIT { FileSystem::DeleteDirectory(outputStaging, true); };
    AssetImportContext context(asset, TEXT("Content/model.foo"), target, "{}", [](const StringView& path, Array<byte>& bytes, ContentHash& hash, AssetPipelineDiagnostic&)
    {
        bytes.Add(42);
        const StringAnsi narrow(path);
        hash = ContentHash::Compute(narrow.Get(), narrow.Length());
        return false;
    }, outputStaging, 3, 1);
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
    context.DependsOnLogicalPath(TEXT("path-sensitive test"));
    const int32 output = context.CreateOutput(TEXT("runtime"), "runtime", ".bin", ArtifactTargetDimension::Platform);
    REQUIRE(output >= 0);
    const byte firstOutputChunk[] = { 1, 2 };
    const byte secondOutputChunk[] = { 3 };
    REQUIRE_FALSE(context.WriteOutput(output, Span<byte>(firstOutputChunk, ARRAY_COUNT(firstOutputChunk)), diagnostic));
    REQUIRE_FALSE(context.WriteOutput(output, Span<byte>(secondOutputChunk, ARRAY_COUNT(secondOutputChunk)), diagnostic));
    REQUIRE_FALSE(context.CompleteOutput(output, diagnostic));
    AssetImportContextResult result;
    REQUIRE_FALSE(context.Complete(true, result, diagnostic));
    CHECK(result.MainObject == object);
    CHECK(result.Objects.Count() == 1);
    CHECK(result.Dependencies.Count() == 4);
    bool foundLogicalPath = false;
    for (const AssetImportDependency& dependency : result.Dependencies)
    {
        if (dependency.Kind != AssetImportDependencyKind::LogicalPath)
            continue;
        foundLogicalPath = true;
        CHECK(dependency.Identity == TEXT("Content/model.foo"));
        CHECK(dependency.Origin == TEXT("path-sensitive test"));
        CHECK_FALSE(dependency.ExpectedHash.IsZero());
    }
    CHECK(foundLogicalPath);
    REQUIRE(result.Outputs.Count() == 1);
    CHECK(result.Outputs[0].TargetDimensions == ArtifactTargetDimension::Platform);
    CHECK(result.Outputs[0].RelativePath == TEXT("runtime.bin"));
    CHECK(result.Outputs[0].Size == 3);
    const byte expectedOutput[] = { 1, 2, 3 };
    CHECK(result.Outputs[0].Hash == ContentHash::Compute(expectedOutput, ARRAY_COUNT(expectedOutput)));
    Array<byte> outputBytes;
    REQUIRE_FALSE(File::ReadAllBytes(result.Outputs[0].StagingPath, outputBytes));
    REQUIRE(outputBytes.Count() == ARRAY_COUNT(expectedOutput));
    CHECK(Platform::MemoryCompare(outputBytes.Get(), expectedOutput, ARRAY_COUNT(expectedOutput)) == 0);
}

TEST_CASE("AssetImportContext enforces output quota while streaming")
{
    const String outputStaging = Globals::TemporaryFolder / (TEXT("AssetImportContextQuota-") + Guid::New().ToString(Guid::FormatType::N));
    REQUIRE_FALSE(FileSystem::CreateDirectory(outputStaging));
    SCOPE_EXIT { FileSystem::DeleteDirectory(outputStaging, true); };
    AssetImportContext context(AssetGuid(Guid::New()), TEXT("Content/model.foo"), ArtifactTarget(), "{}",
        AssetImportReadCallback(), outputStaging, 2, 1);
    AssetPipelineDiagnostic diagnostic;
    const int32 output = context.CreateOutput(TEXT("runtime"), "runtime", ".bin");
    REQUIRE(output >= 0);
    const byte tooLarge[] = { 1, 2, 3 };
    CHECK(context.WriteOutput(output, Span<byte>(tooLarge, ARRAY_COUNT(tooLarge)), diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ResourceLimitExceeded);
    CHECK(FileSystem::GetFileSize(outputStaging / TEXT("runtime.bin")) == 0);
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

TEST_CASE("Asset import worker protocol round-trips bounded capabilities")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetImportWorkerProtocol-") + Guid::New().ToString(Guid::FormatType::N));
    const String staging = root / TEXT("Staging");
    const String requestPath = root / TEXT("request.bin");
    REQUIRE_FALSE(FileSystem::CreateDirectory(root));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const AssetImportJobRequest request = MakeWorkerRequest(staging);
    AssetPipelineDiagnostic diagnostic;
    REQUIRE_FALSE(AssetImportWorkerProtocol::SaveRequest(requestPath, request, diagnostic));
    AssetImportJobRequest loaded;
    REQUIRE_FALSE(AssetImportWorkerProtocol::LoadRequest(requestPath, loaded, diagnostic));
    CHECK(loaded.ProtocolVersion == request.ProtocolVersion);
    CHECK(loaded.JobID == request.JobID);
    CHECK(loaded.Capability == request.Capability);
    CHECK(loaded.Asset == request.Asset);
    CHECK(loaded.SourceHash == request.SourceHash);
    CHECK(loaded.MetaHash == request.MetaHash);
    REQUIRE(loaded.AuthorizedInputs.Count() == 1);
    CHECK(loaded.AuthorizedInputs[0].Identity == request.AuthorizedInputs[0].Identity);
    CHECK(loaded.AuthorizedInputs[0].Hash == request.AuthorizedInputs[0].Hash);
    REQUIRE(loaded.AllowedTools.Count() == 1);
    CHECK(loaded.AllowedTools[0].Name == request.AllowedTools[0].Name);
    CHECK(loaded.AllowedTools[0].VersionHash == request.AllowedTools[0].VersionHash);
    CHECK(loaded.Importer.ID == request.Importer.ID);
    CHECK(loaded.OutputStagingPath == request.OutputStagingPath);
    CHECK(loaded.Limits.MaximumMemoryBytes == request.Limits.MaximumMemoryBytes);
    CHECK(loaded.Limits.TimeoutMilliseconds == request.Limits.TimeoutMilliseconds);
}

TEST_CASE("Asset import worker protocol rejects mismatched capabilities and escaping outputs")
{
    const String root = Globals::TemporaryFolder / (TEXT("AssetImportWorkerValidation-") + Guid::New().ToString(Guid::FormatType::N));
    const String staging = root / TEXT("Staging");
    REQUIRE_FALSE(FileSystem::CreateDirectory(staging));
    SCOPE_EXIT { FileSystem::DeleteDirectory(root, true); };

    const AssetImportJobRequest request = MakeWorkerRequest(staging);
    AssetImportJobResult result;
    result.ProtocolVersion = request.ProtocolVersion;
    result.JobID = request.JobID;
    result.Capability = Guid::New();
    result.Status = AssetImportWorkerStatus::Succeeded;
    AssetPipelineDiagnostic diagnostic;
    CHECK(AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::SnapshotInvalid);

    result.Capability = request.Capability;
    AssetImportWorkerTool unapprovedTool;
    unapprovedTool.Name = TEXT("Tests.OtherTool");
    unapprovedTool.VersionHash = ContentHash::Compute("other", 5);
    result.ObservedToolchain.Add(unapprovedTool);
    CHECK(AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::UndeclaredInput);
    result.ObservedToolchain.Clear();

    AssetImportWorkerOutput output;
    output.Name = TEXT("runtime");
    output.Kind = "runtime";
    output.RelativePath = TEXT("../escape.bin");
    output.Hash = ContentHash::Compute("output", 6);
    output.Size = 6;
    result.Outputs.Add(output);
    CHECK(AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);

    result.Outputs.Clear();
    output.RelativePath = TEXT("runtime.bin");
    const String outputPath = staging / output.RelativePath;
    REQUIRE_FALSE(File::WriteAllBytes(outputPath, "output", 6));
    result.Outputs.Add(output);
    CHECK_FALSE(AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic));
    result.Outputs[0].Hash = ContentHash::Compute("changed", 7);
    CHECK(AssetImportWorkerProtocol::ValidateResult(request, result, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ArtifactInvalid);
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

    request.SourcePath = TEXT("Content/First/model.foo");
    requests.Add(request);
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    const ArtifactKey original = plans[0].StaticFingerprint;

    requests[0].SourcePath = TEXT("Content/Second/model.foo");
    plans.Clear();
    REQUIRE_FALSE(planner.Build(requests, plans, diagnostic));
    CHECK(plans[0].StaticFingerprint == original);

    requests[0].SourcePath = TEXT("Content/Second/renamed.foo");
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

TEST_CASE("Postprocessor ordering constraints are deterministic and fingerprinted")
{
    AssetPostprocessorRegistry postprocessors;
    AssetPostprocessorRegistration forcedLastRegistration;
    AssetPostprocessorRegistration betaRegistration;
    AssetPostprocessorRegistration forcedFirstRegistration;
    AssetPostprocessorRegistration alphaRegistration;
    AssetPipelineDiagnostic diagnostic;
    String order;

    auto makeDescriptor = [&order](const StringView& id, int32 numericOrder, const StringView& marker)
    {
        AssetPostprocessorDescriptor descriptor;
        descriptor.ID = id;
        descriptor.Order = numericOrder;
        const StringAnsi implementation(id);
        descriptor.ImplementationHash = ContentHash::Compute(implementation.Get(), implementation.Length());
        const String markerCopy(marker);
        descriptor.ProcessBatch = [&order, markerCopy](const Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)
        {
            order += markerCopy;
            return false;
        };
        return descriptor;
    };

    AssetPostprocessorDescriptor forcedLast = makeDescriptor(TEXT("Tests.ForcedLast"), -100, TEXT("L"));
    AssetPostprocessorDescriptor beta = makeDescriptor(TEXT("Tests.Beta"), 0, TEXT("b"));
    AssetPostprocessorDescriptor forcedFirst = makeDescriptor(TEXT("Tests.ForcedFirst"), 100, TEXT("F"));
    forcedFirst.RunBefore.Add(TEXT("Tests.ForcedLast"));
    AssetPostprocessorDescriptor alpha = makeDescriptor(TEXT("Tests.Alpha"), 0, TEXT("a"));
    REQUIRE_FALSE(postprocessors.Register(forcedLast, forcedLastRegistration, diagnostic));
    REQUIRE_FALSE(postprocessors.Register(beta, betaRegistration, diagnostic));
    REQUIRE_FALSE(postprocessors.Register(forcedFirst, forcedFirstRegistration, diagnostic));
    REQUIRE_FALSE(postprocessors.Register(alpha, alphaRegistration, diagnostic));

    Array<AssetImportCompletion> completed;
    bool changed = false;
    REQUIRE_FALSE(postprocessors.RunBatch(completed, changed, diagnostic));
    CHECK(order == TEXT("abFL"));

    AssetPostprocessorRegistry baselineRegistry;
    AssetPostprocessorRegistry constrainedRegistry;
    AssetPostprocessorRegistration baselineARegistration;
    AssetPostprocessorRegistration baselineBRegistration;
    AssetPostprocessorRegistration constrainedARegistration;
    AssetPostprocessorRegistration constrainedBRegistration;
    AssetPostprocessorDescriptor baselineA = makeDescriptor(TEXT("Tests.HashA"), 0, TEXT(""));
    AssetPostprocessorDescriptor baselineB = makeDescriptor(TEXT("Tests.HashB"), 0, TEXT(""));
    AssetPostprocessorDescriptor constrainedA = baselineA;
    constrainedA.RunBefore.Add(TEXT("Tests.HashB"));
    REQUIRE_FALSE(baselineRegistry.Register(baselineA, baselineARegistration, diagnostic));
    REQUIRE_FALSE(baselineRegistry.Register(baselineB, baselineBRegistration, diagnostic));
    REQUIRE_FALSE(constrainedRegistry.Register(constrainedA, constrainedARegistration, diagnostic));
    REQUIRE_FALSE(constrainedRegistry.Register(baselineB, constrainedBRegistration, diagnostic));
    CHECK(baselineRegistry.GetVersionKey() != constrainedRegistry.GetVersionKey());
}

TEST_CASE("Postprocessor ordering diagnostics reject invalid graphs before callbacks")
{
    AssetPipelineDiagnostic diagnostic;
    Array<AssetImportCompletion> completed;
    bool changed = false;
    int32 calls = 0;

    auto makeDescriptor = [&calls](const StringView& id)
    {
        AssetPostprocessorDescriptor descriptor;
        descriptor.ID = id;
        const StringAnsi implementation(id);
        descriptor.ImplementationHash = ContentHash::Compute(implementation.Get(), implementation.Length());
        descriptor.ProcessBatch = [&calls](const Array<AssetImportCompletion>&, bool&, AssetPipelineDiagnostic&)
        {
            calls++;
            return false;
        };
        return descriptor;
    };

    SECTION("unknown postprocessor")
    {
        AssetPostprocessorRegistry registry;
        AssetPostprocessorRegistration registration;
        AssetPostprocessorDescriptor descriptor = makeDescriptor(TEXT("Tests.UnknownOwner"));
        descriptor.RunBefore.Add(TEXT("Tests.Missing"));
        REQUIRE_FALSE(registry.Register(descriptor, registration, diagnostic));
        CHECK(registry.RunBatch(completed, changed, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);
        CHECK(diagnostic.Message.Contains(TEXT("Tests.UnknownOwner")));
        CHECK(diagnostic.Message.Contains(TEXT("Tests.Missing")));
        CHECK(calls == 0);
    }

    SECTION("self reference")
    {
        AssetPostprocessorRegistry registry;
        AssetPostprocessorRegistration registration;
        AssetPostprocessorDescriptor descriptor = makeDescriptor(TEXT("Tests.Self"));
        descriptor.RunAfter.Add(TEXT("Tests.Self"));
        REQUIRE_FALSE(registry.Register(descriptor, registration, diagnostic));
        CHECK(registry.RunBatch(completed, changed, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);
        CHECK(diagnostic.Message.Contains(TEXT("Tests.Self")));
        CHECK(diagnostic.Message.Contains(TEXT("itself")));
        CHECK(calls == 0);
    }

    SECTION("cycle")
    {
        AssetPostprocessorRegistry registry;
        AssetPostprocessorRegistration aRegistration;
        AssetPostprocessorRegistration bRegistration;
        AssetPostprocessorDescriptor a = makeDescriptor(TEXT("Tests.CycleA"));
        AssetPostprocessorDescriptor b = makeDescriptor(TEXT("Tests.CycleB"));
        a.RunBefore.Add(TEXT("Tests.CycleB"));
        b.RunBefore.Add(TEXT("Tests.CycleA"));
        REQUIRE_FALSE(registry.Register(a, aRegistration, diagnostic));
        REQUIRE_FALSE(registry.Register(b, bRegistration, diagnostic));
        CHECK(registry.RunBatch(completed, changed, diagnostic));
        CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);
        CHECK(diagnostic.Message.Contains(TEXT("cycle")));
        CHECK(diagnostic.Message.Contains(TEXT("Tests.CycleA")));
        CHECK(diagnostic.Message.Contains(TEXT("Tests.CycleB")));
        CHECK(calls == 0);
    }
}
