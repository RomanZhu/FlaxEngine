// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include <ThirdParty/catch2/catch.hpp>
#include <thread>

namespace
{
    bool PrepareNoop(PrepareAssetContext&, PreparedAsset&, AssetPipelineDiagnostic&)
    {
        return false;
    }

    bool BuildNoop(ArtifactBuildContext&, AssetPipelineDiagnostic&)
    {
        return false;
    }

    AssetProcessorDescriptor MakeProcessor(const StringView& id, const StringView& extension)
    {
        AssetProcessorDescriptor descriptor;
        descriptor.ID = id;
        descriptor.ProviderID = TEXT("tests");
        descriptor.SourceExtensions.Add(extension);
        descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
        descriptor.MainOutputType = TEXT("FlaxEngine.RawDataAsset");
        descriptor.Prepare = &PrepareNoop;
        descriptor.Build = &BuildNoop;
        AssetProcessorOutputDescriptor output;
        output.Kind = "runtime";
        output.Extension = ".flax";
        output.FormatVersion = 1;
        descriptor.Outputs.Add(output);
        return descriptor;
    }
}

TEST_CASE("AssetProcessorRegistry shares native and managed contracts")
{
    AssetProcessorRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    AssetProcessorRegistration nativeRegistration;
    AssetProcessorDescriptor native = MakeProcessor(TEXT("Tests.Native"), TEXT(".native"));
    native.NormalizedDefaultSettings = "{\"quality\":1}";
    REQUIRE_FALSE(registry.Register(native, nativeRegistration, diagnostic));
    REQUIRE(nativeRegistration.IsValid());

    AssetProcessorDescriptor stored;
    REQUIRE(registry.TryGetDescriptor(TEXT("Tests.Native"), stored));
    CHECK(stored.ProviderKind == AssetProcessorProviderKind::Native);
    CHECK(stored.NormalizedDefaultSettings == "{\n  \"quality\": 1\n}\n");

    AssetProcessorRegistration managedRegistration;
    AssetProcessorDescriptor managed = MakeProcessor(TEXT("Tests.Managed"), TEXT(".managed"));
    REQUIRE_FALSE(registry.RegisterManaged(managed, managedRegistration, diagnostic));
    REQUIRE(registry.TryGetDescriptor(TEXT("Tests.Managed"), stored));
    CHECK(stored.ProviderKind == AssetProcessorProviderKind::Managed);

    AssetProcessorRegistration duplicate;
    CHECK(registry.Register(MakeProcessor(TEXT("Tests.Native"), TEXT(".other")), duplicate, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::InvalidSettingsCombination);

    AssetProcessorLease missing;
    CHECK(registry.TryAcquire(TEXT("Tests.Missing"), AssetProcessorInvocationStage::Prepare, missing, diagnostic));
    CHECK(diagnostic.Code == AssetPipelineDiagnosticCode::ProcessorMissing);
}

TEST_CASE("AssetProcessorRegistry rejects ambiguous claims and unsafe trust")
{
    AssetProcessorRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    AssetProcessorRegistration first;
    REQUIRE_FALSE(registry.Register(MakeProcessor(TEXT("Tests.First"), TEXT(".shared")), first, diagnostic));
    AssetProcessorRegistration ambiguous;
    CHECK(registry.Register(MakeProcessor(TEXT("Tests.Second"), TEXT(".SHARED")), ambiguous, diagnostic));

    registry.SetTrustPolicy(true);
    AssetProcessorDescriptor plugin = MakeProcessor(TEXT("Tests.Plugin"), TEXT(".plugin"));
    plugin.ProviderKind = AssetProcessorProviderKind::ThirdParty;
    plugin.ProviderID = TEXT("plugin.vendor.package");
    plugin.ProviderSemanticIdentity = ContentHash::Compute("plugin-v1", 9);
    plugin.TrustMode = AssetProcessorTrustMode::TrustedInProcess;
    AssetProcessorRegistration pluginRegistration;
    CHECK(registry.Register(plugin, pluginRegistration, diagnostic));
    plugin.TrustMode = AssetProcessorTrustMode::IsolatedProcess;
    REQUIRE_FALSE(registry.Register(plugin, pluginRegistration, diagnostic));
}

TEST_CASE("AssetProcessorRegistry pins generations through revocation")
{
    AssetProcessorRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    bool cancelled = false;
    AssetProcessorDescriptor descriptor = MakeProcessor(TEXT("Tests.Reload"), TEXT(".reload"));
    descriptor.CancelProviderWork = [&cancelled]() { cancelled = true; };
    AssetProcessorRegistration registration;
    REQUIRE_FALSE(registry.Register(descriptor, registration, diagnostic));
    const uint64 firstGeneration = registration.GetGeneration();

    AssetProcessorLease lease;
    REQUIRE_FALSE(registry.TryAcquire(TEXT("Tests.Reload"), AssetProcessorInvocationStage::Build, lease, diagnostic));
    CHECK(lease.Get().ProviderGeneration == firstGeneration);
    CHECK(registry.Unregister(TEXT("Tests.Reload"), firstGeneration, false, diagnostic));
    CHECK(cancelled);
    AssetProcessorLease blocked;
    CHECK(registry.TryAcquire(TEXT("Tests.Reload"), AssetProcessorInvocationStage::Build, blocked, diagnostic));
    lease.Reset();
    REQUIRE_FALSE(registry.Unregister(TEXT("Tests.Reload"), firstGeneration, true, diagnostic));

    AssetProcessorRegistration replacement;
    REQUIRE_FALSE(registry.Register(descriptor, replacement, diagnostic));
    CHECK(replacement.GetGeneration() > firstGeneration);
}

TEST_CASE("AssetProcessorRegistry enforces callback affinity")
{
    AssetProcessorRegistry registry;
    AssetPipelineDiagnostic diagnostic;
    AssetProcessorDescriptor descriptor = MakeProcessor(TEXT("Tests.MainThread"), TEXT(".mainthread"));
    descriptor.PrepareAffinity = AssetProcessorThreadAffinity::MainThread;
    AssetProcessorRegistration registration;
    REQUIRE_FALSE(registry.Register(descriptor, registration, diagnostic));

    bool failed = false;
    AssetPipelineDiagnostic workerDiagnostic;
    std::thread worker([&]()
    {
        AssetProcessorLease lease;
        failed = registry.TryAcquire(TEXT("Tests.MainThread"), AssetProcessorInvocationStage::Prepare, lease, workerDiagnostic);
    });
    worker.join();
    CHECK(failed);
    CHECK(workerDiagnostic.Code == AssetPipelineDiagnosticCode::BuildFailed);
}

TEST_CASE("Processor version contracts enter artifact keys")
{
    AssetProcessorDescriptor descriptor = MakeProcessor(TEXT("Tests.Versioned"), TEXT(".versioned"));
    ArtifactKeyBuilder firstBuilder;
    descriptor.AppendVersionKey(firstBuilder, descriptor.Outputs[0]);
    const ArtifactKey first = firstBuilder.Finalize();
    descriptor.ImplementationVersion++;
    ArtifactKeyBuilder implementationBuilder;
    descriptor.AppendVersionKey(implementationBuilder, descriptor.Outputs[0]);
    CHECK(implementationBuilder.Finalize() != first);
    descriptor.ImplementationVersion--;
    descriptor.Outputs[0].FormatVersion++;
    ArtifactKeyBuilder outputBuilder;
    descriptor.AppendVersionKey(outputBuilder, descriptor.Outputs[0]);
    CHECK(outputBuilder.Finalize() != first);
}
