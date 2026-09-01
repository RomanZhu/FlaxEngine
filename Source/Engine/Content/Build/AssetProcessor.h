// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Content/Artifacts/ArtifactTarget.h"
#include "Engine/Content/AssetDatabase/AssetDependency.h"

struct PrepareAssetContext;
struct PreparedAsset;
class ArtifactBuildContext;

/// <summary>Origin and lifetime class of a processor provider.</summary>
enum class AssetProcessorProviderKind : byte
{
    Native,
    Managed,
    ThirdParty,
};

/// <summary>Thread on which one processor stage must execute.</summary>
enum class AssetProcessorThreadAffinity : byte
{
    AnyWorker,
    MainThread,
};

/// <summary>Execution isolation declared by a processor provider.</summary>
enum class AssetProcessorTrustMode : byte
{
    BuiltInTrusted,
    TrustedInProcess,
    IsolatedProcess,
};

/// <summary>One stable output kind produced by a processor.</summary>
struct FLAXENGINE_API AssetProcessorOutputDescriptor
{
    StringAnsi Kind;
    StringAnsi Extension;
    uint32 FormatVersion = 1;
    ArtifactTargetDimension TargetDimensions = ArtifactTargetDimension::None;
    StringAnsi CompatibilityTag;
    bool IndependentlyReusable = false;
};

using AssetProcessorPrepareCallback = Function<bool(PrepareAssetContext&, PreparedAsset&, AssetPipelineDiagnostic&)>;
using AssetProcessorBuildCallback = Function<bool(ArtifactBuildContext&, AssetPipelineDiagnostic&)>;

/// <summary>Engine-owned immutable processor registration model.</summary>
struct FLAXENGINE_API AssetProcessorDescriptor
{
    String ID;
    String ProviderID;
    AssetProcessorProviderKind ProviderKind = AssetProcessorProviderKind::Native;
    AssetProcessorTrustMode TrustMode = AssetProcessorTrustMode::BuiltInTrusted;
    uint32 EngineApiLevel = 1;
    uint32 SettingsSchemaVersion = 1;
    uint32 ImplementationVersion = 1;
    uint32 InterfaceVersion = 1;
    uint64 ProviderGeneration = 0;
    ContentHash ProviderSemanticIdentity;
    Array<String> SourceExtensions;
    Array<AssetSourceKind> SourceKinds;
    Array<String> DocumentTypes;
    Array<AssetProcessorOutputDescriptor> Outputs;
    String MainOutputType;
    StringAnsi NormalizedDefaultSettings = "{}\n";
    bool SupportsSubAssets = false;
    AssetProcessorThreadAffinity PrepareAffinity = AssetProcessorThreadAffinity::AnyWorker;
    AssetProcessorThreadAffinity BuildAffinity = AssetProcessorThreadAffinity::AnyWorker;
    StringAnsi MaxParallelismClass = "default";
    uint64 MemoryEstimate = 0;
    bool UsesExternalProcess = false;
    AssetProcessorPrepareCallback Prepare;
    AssetProcessorBuildCallback Build;
    AssetSemanticInterfaceExtractor ExtractSemanticInterface;
    Function<void()> CancelProviderWork;

    void AppendVersionKey(ArtifactKeyBuilder& builder, const AssetProcessorOutputDescriptor& output) const;
};
