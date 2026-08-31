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
#include "Engine/Graphics/Materials/MaterialShader.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/ShadersCompilation/ShadersCompilation.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Utilities/Encryption.h"

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

    bool LoadChunk(FlaxStorage* storage, AssetInitData& initData, int32 chunkIndex, AssetPipelineDiagnostic& diagnostic,
        const PreparedAsset& prepared, const StringView& path)
    {
        if (chunkIndex < 0 || chunkIndex >= ASSET_FILE_DATA_CHUNKS || !initData.Header.Chunks[chunkIndex] ||
            storage->LoadAssetChunk(initData.Header.Chunks[chunkIndex]))
        {
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, String::Format(TEXT("Graph material output is missing required chunk {0}."), chunkIndex));
        }
        return false;
    }

    bool HasTargetFeature(const ArtifactTarget& target, const StringAnsiView& feature)
    {
        for (const StringAnsi& value : target.FeatureFlags)
        {
            if (value == feature)
                return true;
        }
        return false;
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
    descriptor.SourceExtensions.Add(TEXT(".particleemitter"));
    descriptor.SourceExtensions.Add(TEXT(".material"));
    descriptor.SourceKinds.Add(AssetSourceKind::TextDocument);
    descriptor.DocumentTypes.Add(MaterialFunction::TypeName);
    descriptor.DocumentTypes.Add(AnimationGraphFunction::TypeName);
    descriptor.DocumentTypes.Add(AnimationGraph::TypeName);
    descriptor.DocumentTypes.Add(VisualScript::TypeName);
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.BehaviorTree"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.ParticleEmitterFunction"));
    descriptor.DocumentTypes.Add(TEXT("FlaxEngine.ParticleEmitter"));
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
    runtime.TargetDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture |
        ArtifactTargetDimension::Graphics | ArtifactTargetDimension::ShaderCompiler |
        ArtifactTargetDimension::Role | ArtifactTargetDimension::FeatureFlags;
    runtime.CompatibilityTag = "flax-graph-document-v1";
    runtime.IndependentlyReusable = true;
    descriptor.Outputs.Add(runtime);
    return descriptor;
}

bool GraphDocumentProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    context.SetSourceSerializerVersion(RuntimeFormatVersion);
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
            const bool failed = dependency.ObjectID.IsValid()
                ? context.DeclareBuildInput(dependency.StableIdentity, dependency.ObjectID, dependency.ExactArtifact, semantic, declared, diagnostic)
                : context.DeclareBuildInput(dependency.StableIdentity, dependency.AssetID, dependency.ExactArtifact, semantic, declared, diagnostic);
            if (failed)
                return true;
        }
        else if (dependency.Kind == AssetDependencyKind::RuntimeReference)
        {
            const bool failed = dependency.ObjectID.IsValid()
                ? context.DeclareRuntimeReference(dependency.StableIdentity, dependency.ObjectID, declared, diagnostic)
                : context.DeclareRuntimeReference(dependency.StableIdentity, dependency.AssetID, declared, diagnostic);
            if (failed)
                return true;
        }
    }
    if (context.DeclareOutput(StringAnsiView("runtime"), record.ID, diagnostic))
        return true;
    const char compilerIdentity[] = "flax-visject-compatibility-compiler-v2";
    if (context.DeclareToolchain(TEXT("graph-compiler"), ContentHash::Compute(compilerIdentity, ARRAY_COUNT(compilerIdentity) - 1), origin, diagnostic))
        return true;
    if (record.TypeName == Material::TypeName)
    {
        const String materialCompilerIdentity = String::Format(TEXT("flax-material-shader-v{0}-graph-{1}-processor-{2}"),
            GPU_SHADER_CACHE_VERSION, MATERIAL_GRAPH_VERSION, GraphDocumentProcessor::ImplementationVersion);
        if (context.DeclareToolchain(TEXT("material-shader-compiler"),
            ContentHash::Compute(*materialCompilerIdentity, materialCompilerIdentity.Length() * sizeof(Char)), origin, diagnostic))
            return true;
    }

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

namespace
{
    bool CompileMaterialRuntime(const StringView& path, const PreparedAsset& prepared, const ArtifactTarget& target,
        AssetPipelineDiagnostic& diagnostic)
    {
        if (prepared.OutputType != Material::TypeName || target.Role != StringAnsiView("Runtime"))
            return false;
#if COMPILE_WITH_SHADER_COMPILER
        AssetInitData sourceData;
        auto storage = ContentStorageManager::GetStorage(path, true);
        if (!storage || storage->LoadAssetHeader(prepared.AssetID, sourceData) ||
            sourceData.CustomData.Length() != sizeof(ShaderStorage::Header))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, TEXT("Graph material header could not be opened for target shader compilation."));
        if (LoadChunk(storage.Get(), sourceData, SHADER_FILE_CHUNK_SOURCE, diagnostic, prepared, path))
            return true;
        if (sourceData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS] &&
            storage->LoadAssetChunk(sourceData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS]))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, TEXT("Graph material parameters could not be loaded for runtime publication."));

        AssetInitData runtimeData;
        runtimeData.SerializedVersion = sourceData.SerializedVersion;
        runtimeData.Header.ID = sourceData.Header.ID;
        runtimeData.Header.TypeName = sourceData.Header.TypeName;
        runtimeData.CustomData.Copy(sourceData.CustomData);
        if (sourceData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS])
            runtimeData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS] = sourceData.Header.Chunks[SHADER_FILE_CHUNK_MATERIAL_PARAMS]->Clone();

        BytesContainer source;
        source.Copy(sourceData.Header.Chunks[SHADER_FILE_CHUNK_SOURCE]->Data);
        uint32 sourceLength = source.Length();
        Encryption::DecryptBytes(source.Get(), sourceLength);
        source.Get()[sourceLength - 1] = 0;
        while (sourceLength > 2 && source.Get()[sourceLength - 1] == 0)
            sourceLength--;

        const auto& shaderHeader = *reinterpret_cast<const ShaderStorage::Header*>(sourceData.CustomData.Get());
        ShaderCompilationOptions options;
        options.TargetName = prepared.AssetName.HasChars() ? prepared.AssetName : prepared.AssetID.ToString(Guid::FormatType::N);
        options.TargetID = prepared.AssetID;
        options.Source = reinterpret_cast<const char*>(source.Get());
        options.SourceLength = sourceLength;
        options.NoOptimize = HasTargetFeature(target, StringAnsiView("shader-no-optimize"));
        options.GenerateDebugData = HasTargetFeature(target, StringAnsiView("shader-debug-data"));
        options.TreatWarningsAsErrors = false;
        MemoryWriteStream cacheStream(32 * 1024);
        int32 compiledProfiles = 0;
        auto compileProfile = [&](ShaderProfile profile, int32 chunkIndex, const char* platformDefine, PlatformType platform = (PlatformType)0) -> bool
        {
            cacheStream.SetPosition(0);
            options.Profile = profile;
            options.Platform = platform;
            options.Output = &cacheStream;
            options.Macros.Clear();
            ShaderMacro& macro = options.Macros.AddOne();
            macro.Name = platformDefine;
            macro.Definition = nullptr;
            Material::SetupCompilationOptions(options, shaderHeader.Material.Info);
            if (ShadersCompilation::Compile(options))
                return true;
            auto* chunk = New<FlaxChunk>();
            chunk->Data.Copy(cacheStream.GetHandle(), cacheStream.GetPosition());
            runtimeData.Header.Chunks[chunkIndex] = chunk;
            compiledProfiles++;
            return false;
        };

        const bool dx12 = HasTargetFeature(target, StringAnsiView("shader-dx12"));
        const bool dx11 = HasTargetFeature(target, StringAnsiView("shader-dx11"));
        const bool dx10 = HasTargetFeature(target, StringAnsiView("shader-dx10"));
        const bool vulkan = HasTargetFeature(target, StringAnsiView("shader-vulkan"));
        const bool console = HasTargetFeature(target, StringAnsiView("shader-console"));
        const bool webGpu = HasTargetFeature(target, StringAnsiView("shader-webgpu"));
        const char* platformDefine = target.Platform == StringAnsiView("UWP") ? "PLATFORM_UWP" :
            target.Platform == StringAnsiView("Linux") ? "PLATFORM_LINUX" :
            target.Platform == StringAnsiView("Android") ? "PLATFORM_ANDROID" :
            target.Platform == StringAnsiView("Mac") ? "PLATFORM_MAC" :
            target.Platform == StringAnsiView("iOS") ? "PLATFORM_IOS" :
            target.Platform == StringAnsiView("Switch") ? "PLATFORM_SWITCH" : "PLATFORM_WINDOWS";
        if ((dx12 && compileProfile(ShaderProfile::DirectX_SM6, SHADER_FILE_CHUNK_INTERNAL_D3D_SM6_CACHE, platformDefine,
                target.Platform == StringAnsiView("XboxScarlett") ? PlatformType::XboxScarlett : (PlatformType)0)) ||
            (dx11 && compileProfile(ShaderProfile::DirectX_SM5, SHADER_FILE_CHUNK_INTERNAL_D3D_SM5_CACHE, platformDefine)) ||
            (dx10 && compileProfile(ShaderProfile::DirectX_SM4, SHADER_FILE_CHUNK_INTERNAL_D3D_SM4_CACHE, platformDefine)) ||
            (vulkan && compileProfile(ShaderProfile::Vulkan_SM5, SHADER_FILE_CHUNK_INTERNAL_VULKAN_SM5_CACHE, platformDefine)) ||
            (console && target.Platform == StringAnsiView("PS4") && compileProfile(ShaderProfile::PS4, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE, "PLATFORM_PS4")) ||
            (console && target.Platform == StringAnsiView("PS5") && compileProfile(ShaderProfile::PS5, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE, "PLATFORM_PS5")) ||
            (webGpu && compileProfile(ShaderProfile::WebGPU, SHADER_FILE_CHUNK_INTERNAL_GENERIC_CACHE, "PLATFORM_WEB")))
        {
            runtimeData.Header.DeleteChunks();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, TEXT("Graph material shader compilation failed for the runtime target."));
        }
        if (compiledProfiles == 0)
        {
            runtimeData.Header.DeleteChunks();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, TEXT("Runtime target declares no material shader profiles."));
        }

        storage = nullptr;
        ContentStorageManager::EnsureAccess(path);
        const bool saveFailed = FlaxStorage::Create(path, runtimeData, true);
        runtimeData.Header.DeleteChunks();
        if (saveFailed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, TEXT("Target-compiled graph material artifact could not be saved."));
        diagnostic = AssetPipelineDiagnostic();
        return false;
#else
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, path, TEXT("Runtime graph material publication requires the shader compiler."));
#endif
    }
}

bool GraphDocumentProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const AssetDependency* sourceDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceAsset)
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
    if (CompileMaterialRuntime(scratchPath, prepared, context.GetTarget(), diagnostic))
        return true;
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
