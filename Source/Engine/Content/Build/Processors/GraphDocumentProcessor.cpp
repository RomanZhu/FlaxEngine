// Copyright (c) Wojciech Figat. All rights reserved.

#include "GraphDocumentProcessor.h"

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Assets/AnimationGraph.h"
#include "Engine/Content/Assets/AnimationGraphFunction.h"
#include "Engine/Content/Assets/MaterialFunction.h"
#include "Engine/Content/Assets/VisualScript.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = GraphDocumentProcessor::ProcessorID();
        diagnostic.Message = message;
        return true;
    }
}

const String& GraphDocumentProcessor::ProcessorID()
{
    static const String value(TEXT("Flax.GraphDocument"));
    return value;
}

AssetProcessorDescriptor GraphDocumentProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    descriptor.SourceExtensions.Add(TEXT(".materialfunction"));
    descriptor.SourceExtensions.Add(TEXT(".animgraphfunction"));
    descriptor.SourceExtensions.Add(TEXT(".animgraph"));
    descriptor.SourceExtensions.Add(TEXT(".visualscript"));
    descriptor.SourceExtensions.Add(TEXT(".behaviortree"));
    descriptor.SourceExtensions.Add(TEXT(".particlefunction"));
    descriptor.SourceExtensions.Add(TEXT(".material"));
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.DocumentTypes.Add(MaterialFunction::TypeName);
    descriptor.DocumentTypes.Add(AnimationGraphFunction::TypeName);
    descriptor.DocumentTypes.Add(AnimationGraph::TypeName);
    descriptor.DocumentTypes.Add(VisualScript::TypeName);
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.BehaviorTree"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.ParticleEmitterFunction"));
    descriptor.DocumentTypes.Add(Material::TypeName);
    descriptor.MainOutputType = MaterialFunction::TypeName;
    descriptor.SettingsSchemaVersion = 1;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "graph-document";
    descriptor.MemoryEstimate = 128ull * 1024ull * 1024ull;
    descriptor.Prepare = &GraphDocumentProcessor::Prepare;
    descriptor.Build = &GraphDocumentProcessor::Build;
    descriptor.ExtractSemanticInterface = &GraphDocumentProcessor::ExtractSemanticInterface;

    AssetProcessorOutputDescriptor runtime;
    runtime.Kind = "runtime";
    runtime.Extension = ".flax";
    runtime.FormatVersion = RuntimeFormatVersion;
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture;
    runtime.CompatibilityTag = "flax-graph-document-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool GraphDocumentProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    const AssetRecord& record = context.GetRecord();
    if (!GraphDocumentCodec::IsSupportedType(record.TypeName))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Graph document metadata declares an unsupported runtime type."));

    Array<byte> bytes;
    ContentHash sourceHash;
    AssetDependencyOrigin origin;
    origin.Path = record.SourcePath.Get();
    if (context.ReadSourceFile(record.SourcePath.Get(), bytes, sourceHash, origin, diagnostic))
        return true;
    const StringAnsiView source(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    if (codec.DecodeGraph(source, snapshot, diagnostic))
    {
        diagnostic.AssetGuid = record.ID;
        diagnostic.SourcePath = record.SourcePath.Get();
        return true;
    }
    if (snapshot.TypeName != record.TypeName)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Graph document type does not match its metadata sidecar."));
    for (const AssetDependency& dependency : snapshot.Dependencies)
    {
        AssetDependencyOrigin declared = dependency.Origin;
        declared.Path = record.SourcePath.Get();
        if (dependency.Kind == AssetDependencyKind::BuildInput)
        {
            AssetSemanticInterface semantic;
            semantic.Hash = dependency.SemanticInterface;
            semantic.Version = dependency.InterfaceVersion;
            if (context.DeclareBuildInput(dependency.StableIdentity, dependency.AssetID, dependency.ExactArtifact, semantic, declared, diagnostic))
                return true;
        }
        else if (dependency.Kind == AssetDependencyKind::RuntimeReference)
        {
            if (context.DeclareRuntimeReference(dependency.StableIdentity, dependency.AssetID, declared, diagnostic))
                return true;
        }
    }
    if (context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    const char compilerIdentity[] = "flax-visject-compatibility-compiler-v2";
    if (context.DeclareToolchain(TEXT("graph-compiler"), ContentHash::Compute(compilerIdentity, ARRAY_COUNT(compilerIdentity) - 1), origin, diagnostic))
        return true;

    auto payload = std::make_shared<GraphDocumentPreparedPayload>();
    payload->SemanticHash = snapshot.SemanticHash;
    payload->FunctionInterfaceHash = snapshot.FunctionInterfaceHash;
    payload->SurfaceBytes = snapshot.CompatibilitySurface.Count();
    payload->NodeCount = snapshot.Document.Nodes.Count();
    prepared.Payload = payload;
    prepared.MemoryEstimate = sizeof(GraphDocumentPreparedPayload) + snapshot.CompatibilitySurface.Count() + snapshot.CanonicalText.Length();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const GraphDocumentPreparedPayload*>(prepared.Payload.get());
    const AssetProcessorDescriptor descriptor = CreateDescriptor();
    if (!payload || outputKind != StringAnsiView("runtime"))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, StringView::Empty, TEXT("Graph output key requires prepared graph state and the runtime output."));
    const AssetProcessorOutputDescriptor& output = descriptor.Outputs[0];
    ArtifactKeyBuilder builder(StringAnsiView("flax-graph-document-output-v2"));
    descriptor.AppendVersionKey(builder, output);
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddHash(StringAnsiView("semantic-graph"), payload->SemanticHash);
    builder.AddHash(StringAnsiView("function-interface"), payload->FunctionInterfaceHash);
    builder.AddUInt32(StringAnsiView("node-count"), payload->NodeCount);
    for (int32 i = 0; i < prepared.Dependencies.Count(); i++)
    {
        if (prepared.Dependencies[i].AffectsBuildKey())
            prepared.Dependencies[i].AppendKeyComponents(builder, i);
    }
    builder.AddTarget(target, output.TargetDimensions);
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentProcessor::ExtractSemanticInterface(const AssetRecord& record, AssetSemanticInterface& result, AssetPipelineDiagnostic& diagnostic)
{
    result = AssetSemanticInterface();
    Array<byte> bytes;
    if (File::ReadAllBytes(record.SourcePath.Get(), bytes))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, AssetPipelineDiagnosticStage::Prepare,
            record.ID, record.SourcePath.Get(), TEXT("Graph document is missing."));
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    if (codec.DecodeGraph(StringAnsiView(reinterpret_cast<const char*>(bytes.Get()), bytes.Count()), snapshot, diagnostic))
        return true;
    result.Version = 1;
    result.Hash = snapshot.FunctionInterfaceHash.IsZero() ? snapshot.SemanticHash : snapshot.FunctionInterfaceHash;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool GraphDocumentProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceFile)
        {
            sourceDependency = &dependency;
            break;
        }
    }
    if (!sourceDependency)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, StringView::Empty, TEXT("Graph build has no declared source capability."));
    Array<byte> sourceBytes;
    ContentHash sourceHash;
    if (context.ReadInput(sourceDependency->StableIdentity, sourceBytes, sourceHash, diagnostic))
        return true;
    GraphDocumentCodec codec;
    GraphDocumentSnapshot snapshot;
    if (codec.DecodeGraph(StringAnsiView(reinterpret_cast<const char*>(sourceBytes.Get()), sourceBytes.Count()), snapshot, diagnostic))
        return true;
    if (snapshot.TypeName != prepared.OutputType)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourceDependency->StableIdentity, TEXT("Graph document type changed after preparation."));

    String scratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), scratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(scratchPath);
        FileSystem::DeleteFile(scratchPath);
    };
    if (GraphDocumentCodec::WriteCompatibilityAsset(scratchPath, prepared.AssetID, prepared.OutputType, snapshot.CompatibilitySurface, snapshot.Document.PropertiesJson, diagnostic, true))
    {
        diagnostic.AssetGuid = prepared.AssetID;
        diagnostic.SourcePath = sourceDependency->StableIdentity;
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        return true;
    }
    Array<byte> artifact;
    if (File::ReadAllBytes(scratchPath, artifact))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, scratchPath, TEXT("Graph compatibility artifact is unreadable."));
    ArtifactWriter writer;
    if (context.OpenOutput(StringAnsiView("runtime"), writer, diagnostic) ||
        writer.WriteFile(TEXT("graph.flax"), artifact.Get(), artifact.Count(), diagnostic))
        return true;
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#endif
