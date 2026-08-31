// Copyright (c) Wojciech Figat. All rights reserved.

#include "ModelProcessor.h"

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR

#include "Engine/Content/Build/ArtifactBuildContext.h"
#include "Engine/Content/Build/PrepareAssetContext.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Assets/Animation.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"

#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/ContentImporters/CreateMaterial.h"
#include "Engine/ContentImporters/ImportModel.h"
#include "Engine/ContentImporters/ImportTexture.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Core/ScopeExit.h"
#endif

#include <algorithm>
#include <future>
#include <mutex>

namespace
{
    constexpr int32 ModelAnalysisCacheCapacity = 64;
#if COMPILE_WITH_ASSETS_IMPORTER
    constexpr int32 ModelBuildCacheCapacity = 16;
    constexpr uint64 ModelBuildCacheByteCapacity = 512ull * 1024ull * 1024ull;
#endif

    struct ModelSourceAnalysisResult
    {
        ModelSourceAnalysis Analysis;
        AssetPipelineDiagnostic Diagnostic;
        bool Failed = false;
    };

#if COMPILE_WITH_ASSETS_IMPORTER
    struct ModelBuildData
    {
        ModelData Data;
        ModelTool::Options Options;
        Array<ModelSubAssetInfo> SubAssets;
        uint64 MemoryBytes = 0;

        ~ModelBuildData();
    };

    struct ModelBuildDataResult
    {
        std::shared_ptr<ModelBuildData> Value;
        AssetPipelineDiagnostic Diagnostic;
        bool Failed = false;
    };

    struct ModelBuildCacheEntry
    {
        std::shared_future<std::shared_ptr<ModelBuildDataResult>> Future;
        uint64 MemoryBytes = 0;
    };
#endif

    struct ModelProcessorCache
    {
        std::mutex Locker;
        Dictionary<StringAnsi, std::shared_future<std::shared_ptr<ModelSourceAnalysisResult>>> Analyses;
        Array<StringAnsi> AnalysisOrder;
#if COMPILE_WITH_ASSETS_IMPORTER
        Dictionary<StringAnsi, std::shared_ptr<ModelBuildCacheEntry>> Builds;
        Array<StringAnsi> BuildOrder;
        uint64 BuildBytes = 0;
#endif
    };

    ModelProcessorCache& Cache()
    {
        static ModelProcessorCache cache;
        return cache;
    }

    void ReleaseModelMeshes(ModelData& data)
    {
        HashSet<MeshData*> released;
        for (ModelLodData& lod : data.LODs)
        {
            for (MeshData* mesh : lod.Meshes)
            {
                if (mesh && released.Add(mesh))
                    Delete(mesh);
            }
            lod.Meshes.Clear();
        }
    }

#if COMPILE_WITH_ASSETS_IMPORTER
    ModelBuildData::~ModelBuildData()
    {
        ReleaseModelMeshes(Data);
    }
#endif

    struct ScopedModelData
    {
        ModelData Data;

        ~ScopedModelData()
        {
            ReleaseModelMeshes(Data);
        }
    };

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, AssetPipelineDiagnosticStage stage,
        const Guid& assetID, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = stage;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = path;
        diagnostic.ProcessorId = ModelProcessorSettings::ProcessorID();
        diagnostic.Message = message;
        return true;
    }

    ModelTool::ModelType TypeFromName(const StringView& typeName)
    {
        if (typeName == SkinnedModel::TypeName)
            return ModelTool::ModelType::SkinnedModel;
        if (typeName == Animation::TypeName)
            return ModelTool::ModelType::Animation;
        return ModelTool::ModelType::Model;
    }

    uint64 EstimateData(const ModelData& data)
    {
        uint64 bytes = sizeof(ModelData);
        for (const ModelLodData& lod : data.LODs)
        {
            bytes += sizeof(ModelLodData) + static_cast<uint64>(lod.Meshes.Count()) * sizeof(MeshData*);
            for (const MeshData* mesh : lod.Meshes)
            {
                if (!mesh)
                    continue;
                bytes += sizeof(MeshData);
                bytes += static_cast<uint64>(mesh->Positions.Count()) * sizeof(Float3);
                bytes += static_cast<uint64>(mesh->Indices.Count()) * sizeof(uint32);
                bytes += static_cast<uint64>(mesh->Normals.Count() + mesh->Tangents.Count()) * sizeof(Float3);
                bytes += static_cast<uint64>(mesh->BlendIndices.Count()) * sizeof(Int4);
                bytes += static_cast<uint64>(mesh->BlendWeights.Count()) * sizeof(Float4);
            }
        }
        bytes += data.Skeleton.GetMemoryUsage();
        for (const AnimationData& animation : data.Animations)
            bytes += animation.GetMemoryUsage();
        for (const TextureEntry& texture : data.Textures)
            bytes += texture.EmbeddedData.Count();
        return bytes;
    }

    bool DeclareGltfDependencies(PrepareAssetContext& context, const Array<byte>& source, const StringView& sourcePath,
        HashSet<String>& declaredPaths, ArtifactKeyBuilder& analysisKeyBuilder, int32& analysisDependencyIndex,
        AssetPipelineDiagnostic& diagnostic)
    {
        rapidjson_flax::Document document;
        document.Parse(reinterpret_cast<const char*>(source.Get()), source.Count());
        if (document.HasParseError() || !document.IsObject())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                context.GetRecord().ID, sourcePath, TEXT("glTF source JSON is malformed."));

        const String directory = StringUtils::GetDirectoryName(sourcePath);
        auto readUris = [&](const char* collectionName) -> bool
        {
            const auto collection = document.FindMember(collectionName);
            if (collection == document.MemberEnd())
                return false;
            if (!collection->value.IsArray())
                return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, AssetPipelineDiagnosticStage::Prepare,
                    context.GetRecord().ID, sourcePath, TEXT("glTF dependency collection must be an array."));
            for (const auto& item : collection->value.GetArray())
            {
                if (!item.IsObject())
                    continue;
                const auto uri = item.FindMember("uri");
                if (uri == item.MemberEnd() || !uri->value.IsString())
                    continue;
                const StringAnsiView value(uri->value.GetString(), uri->value.GetStringLength());
                if (value.StartsWith("data:") || value.Contains("://"))
                    continue;
                String path = directory / String(value);
                FileSystem::NormalizePath(path);
                if (!declaredPaths.Add(path.ToLower()))
                    continue;
                Array<byte> bytes;
                ContentHash hash;
                AssetDependencyOrigin origin;
                origin.Path = sourcePath;
                origin.GraphNode = String(collectionName);
                if (context.ReadSourceFile(path, bytes, hash, origin, diagnostic))
                {
                    diagnostic.Message = String::Format(TEXT("Missing or unreadable glTF external dependency '{0}' declared by '{1}'."), path, sourcePath);
                    return true;
                }
                const StringAnsi prefix = StringAnsi::Format("gltf-dependency-{0}-", analysisDependencyIndex++);
                analysisKeyBuilder.AddString(prefix + "path", path.ToLower());
                analysisKeyBuilder.AddHash(prefix + "content", hash);
            }
            return false;
        };
        return readUris("buffers") || readUris("images");
    }

    const AssetProcessorOutputDescriptor* FindOutput(const AssetProcessorDescriptor& descriptor, const StringAnsiView& kind)
    {
        for (const AssetProcessorOutputDescriptor& output : descriptor.Outputs)
        {
            if (output.Kind == kind)
                return &output;
        }
        return nullptr;
    }

    bool GetCachedSourceAnalysis(const StringAnsi& key, const StringView& sourcePath, const ModelProcessorSettings& settings,
        std::shared_ptr<const ModelSourceAnalysis>& analysis, AssetPipelineDiagnostic& diagnostic)
    {
        ModelProcessorCache& cache = Cache();
        std::shared_future<std::shared_ptr<ModelSourceAnalysisResult>> future;
        std::shared_ptr<std::promise<std::shared_ptr<ModelSourceAnalysisResult>>> producer;
        {
            std::lock_guard<std::mutex> lock(cache.Locker);
            auto* existing = cache.Analyses.TryGet(key);
            if (existing)
            {
                future = *existing;
            }
            else
            {
                producer = std::make_shared<std::promise<std::shared_ptr<ModelSourceAnalysisResult>>>();
                future = producer->get_future().share();
                cache.Analyses.Add(key, future);
                cache.AnalysisOrder.Add(key);
                while (cache.AnalysisOrder.Count() > ModelAnalysisCacheCapacity)
                {
                    const StringAnsi oldest = cache.AnalysisOrder[0];
                    cache.AnalysisOrder.RemoveAtKeepOrder(0);
                    cache.Analyses.Remove(oldest);
                }
            }
        }
        if (producer)
        {
            auto result = std::make_shared<ModelSourceAnalysisResult>();
            result->Failed = ModelProcessor::AnalyzeSource(sourcePath, settings, result->Analysis, result->Diagnostic);
            producer->set_value(result);
        }
        const std::shared_ptr<ModelSourceAnalysisResult> result = future.get();
        if (result->Failed)
        {
            diagnostic = result->Diagnostic;
            return true;
        }
        analysis = std::shared_ptr<const ModelSourceAnalysis>(result, &result->Analysis);
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

#if COMPILE_WITH_ASSETS_IMPORTER
    void TrimBuildCache(ModelProcessorCache& cache, const StringAnsi& retainedKey)
    {
        while (cache.BuildOrder.Count() > ModelBuildCacheCapacity || cache.BuildBytes > ModelBuildCacheByteCapacity)
        {
            int32 removeIndex = 0;
            if (cache.BuildOrder[removeIndex] == retainedKey && cache.BuildOrder.Count() > 1)
                removeIndex = 1;
            const StringAnsi key = cache.BuildOrder[removeIndex];
            if (key == retainedKey && cache.BuildOrder.Count() == 1)
                break;
            const std::shared_ptr<ModelBuildCacheEntry>* entry = cache.Builds.TryGet(key);
            if (entry)
            {
                cache.BuildBytes = cache.BuildBytes >= (*entry)->MemoryBytes ? cache.BuildBytes - (*entry)->MemoryBytes : 0;
                cache.Builds.Remove(key);
            }
            cache.BuildOrder.RemoveAtKeepOrder(removeIndex);
        }
    }

    bool GetCachedBuildData(const StringAnsi& key, const StringView& sourcePath, ModelTool::Options options,
        std::shared_ptr<const ModelBuildData>& data, AssetPipelineDiagnostic& diagnostic)
    {
        ModelProcessorCache& cache = Cache();
        std::shared_ptr<ModelBuildCacheEntry> entry;
        std::shared_ptr<std::promise<std::shared_ptr<ModelBuildDataResult>>> producer;
        {
            std::lock_guard<std::mutex> lock(cache.Locker);
            auto* existing = cache.Builds.TryGet(key);
            if (existing)
            {
                entry = *existing;
            }
            else
            {
                producer = std::make_shared<std::promise<std::shared_ptr<ModelBuildDataResult>>>();
                entry = std::make_shared<ModelBuildCacheEntry>();
                entry->Future = producer->get_future().share();
                cache.Builds.Add(key, entry);
                cache.BuildOrder.Add(key);
                TrimBuildCache(cache, key);
            }
        }
        if (producer)
        {
            auto result = std::make_shared<ModelBuildDataResult>();
            auto value = std::make_shared<ModelBuildData>();
            String importError;
            if (ModelTool::ImportModel(String(sourcePath), value->Data, options, importError, String::Empty))
            {
                result->Failed = Fail(result->Diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                    Guid::Empty, sourcePath, importError.IsEmpty() ? TEXT("Model compatibility importer rejected the verified source.") : importError);
            }
            else
            {
                if (value->Data.PositionFormat == ModelData::PositionFormats::Automatic)
                    value->Data.PositionFormat = ModelData::PositionFormats::Float32;
                value->Options = options;
                Array<SubAssetCandidate> candidates;
                result->Failed = ModelSubAssetKeys::Enumerate(value->Data, value->SubAssets, candidates, result->Diagnostic);
                if (!result->Failed)
                {
                    value->MemoryBytes = EstimateData(value->Data);
                    result->Value = MoveTemp(value);
                }
            }
            if (result->Failed)
                result->Diagnostic.SourcePath = sourcePath;
            {
                std::lock_guard<std::mutex> lock(cache.Locker);
                const std::shared_ptr<ModelBuildCacheEntry>* current = cache.Builds.TryGet(key);
                if (current && *current == entry && result->Value)
                {
                    entry->MemoryBytes = result->Value->MemoryBytes;
                    cache.BuildBytes += entry->MemoryBytes;
                    TrimBuildCache(cache, key);
                }
            }
            producer->set_value(result);
        }
        const std::shared_ptr<ModelBuildDataResult> result = entry->Future.get();
        if (result->Failed || !result->Value)
        {
            diagnostic = result->Diagnostic;
            return true;
        }
        data = result->Value;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
#endif
}

uint64 ModelPreparedPayload::GetMemoryUsage() const
{
    uint64 result = sizeof(ModelPreparedPayload);
    result += static_cast<uint64>(RootTypeName.Length() + RootSourcePath.Length() + SelectedStableKey.Length()) * sizeof(Char);
    for (const ModelSubAssetInfo& info : SubAssets)
        result += sizeof(info) + static_cast<uint64>(info.StableKey.Length() + info.DisplayName.Length() + info.TypeName.Length()) * sizeof(Char);
    result += static_cast<uint64>(AssignedIDs.Count()) * (sizeof(Guid) + 64);
    return result;
}

AssetProcessorDescriptor ModelProcessor::CreateDescriptor()
{
    AssetProcessorDescriptor descriptor;
    descriptor.ID = ModelProcessorSettings::ProcessorID();
    descriptor.ProviderID = TEXT("flax");
    const Char* extensions[] = {
        TEXT(".obj"), TEXT(".fbx"), TEXT(".x"), TEXT(".dae"), TEXT(".gltf"), TEXT(".glb"), TEXT(".blend"),
        TEXT(".bvh"), TEXT(".ase"), TEXT(".ply"), TEXT(".dxf"), TEXT(".ifc"), TEXT(".nff"), TEXT(".smd"),
        TEXT(".vta"), TEXT(".mdl"), TEXT(".md2"), TEXT(".md3"), TEXT(".md5mesh"), TEXT(".q3o"), TEXT(".q3s"),
        TEXT(".ac"), TEXT(".stl"), TEXT(".lwo"), TEXT(".lws"), TEXT(".lxo")
    };
    for (const Char* extension : extensions)
        descriptor.SourceExtensions.Add(extension);
    descriptor.SourceKinds.Add(AssetSourceKind::ImportedSource);
    descriptor.MainOutputType = Model::TypeName;
    descriptor.SettingsSchemaVersion = ModelProcessorSettings::CurrentVersion;
    descriptor.ImplementationVersion = ImplementationVersion;
    descriptor.InterfaceVersion = 1;
    descriptor.MaxParallelismClass = "model";
    descriptor.MemoryEstimate = 512ull * 1024ull * 1024ull;
    descriptor.UpgradeSettings = &ModelProcessorSettings::Upgrade;
    descriptor.Prepare = &ModelProcessor::Prepare;
    descriptor.Build = &ModelProcessor::Build;
    AssetPipelineDiagnostic diagnostic;
    const bool defaultsFailed = ModelProcessorSettings::Defaults().ToJson(descriptor.NormalizedDefaultSettings, diagnostic);
    ASSERT(!defaultsFailed);

    auto addOutput = [&descriptor](const char* kind, const char* extension, uint32 version, ArtifactTargetDimension dimensions, const char* compatibility)
    {
        AssetProcessorOutputDescriptor output;
        output.Kind = kind;
        output.Extension = extension;
        output.FormatVersion = version;
        output.TargetDimensions = dimensions;
        output.CompatibilityTag = compatibility;
        output.IndependentlyReusable = true;
        descriptor.Outputs.Add(MoveTemp(output));
    };
    const ArtifactTargetDimension runtimeDimensions = ArtifactTargetDimension::Platform | ArtifactTargetDimension::Architecture | ArtifactTargetDimension::Graphics;
    addOutput("runtime", ".flax", RuntimeFormatVersion, runtimeDimensions, "flax-model-runtime-v1");
    addOutput("geometry", ".bin", GeometryFormatVersion, ArtifactTargetDimension::Architecture, "flax-model-geometry-v1");
    addOutput("lod", ".bin", LodFormatVersion, ArtifactTargetDimension::Architecture, "flax-model-lod-v1");
    addOutput("sdf", ".bin", SdfFormatVersion, ArtifactTargetDimension::Graphics, "flax-model-sdf-v1");
    addOutput("skeleton", ".bin", SkeletonFormatVersion, ArtifactTargetDimension::Architecture, "flax-model-skeleton-v1");
    addOutput("animation", ".flax", AnimationFormatVersion, ArtifactTargetDimension::Architecture, "flax-model-animation-v1");
    addOutput("material", ".flax", MaterialFormatVersion, runtimeDimensions, "flax-model-owned-material-v1");
    addOutput("texture", ".flax", TextureFormatVersion, runtimeDimensions, "flax-model-owned-texture-v1");
    return descriptor;
}

bool ModelProcessor::AnalyzeSource(const StringView& sourcePath, const ModelProcessorSettings& settings,
    ModelSourceAnalysis& analysis, AssetPipelineDiagnostic& diagnostic)
{
    analysis = ModelSourceAnalysis();
    ScopedModelData ownedData;
    ModelTool::Options probeOptions = settings.Import;
    probeOptions.Type = ModelTool::ModelType::Prefab;
    probeOptions.ImportTypes = ImportDataTypes::Geometry | ImportDataTypes::Skeleton | ImportDataTypes::Animations |
                               ImportDataTypes::Nodes | ImportDataTypes::Materials | ImportDataTypes::Textures;
    probeOptions.Cached = nullptr;
    String error;
    if (ModelTool::ImportData(String(sourcePath), ownedData.Data, probeOptions, error))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            Guid::Empty, sourcePath, error.IsEmpty() ? TEXT("Model structure parser rejected the source.") : error);

    if (ModelSubAssetKeys::Enumerate(ownedData.Data, analysis.SubAssets, analysis.Candidates, diagnostic))
    {
        diagnostic.SourcePath = sourcePath;
        return true;
    }
    if (!settings.Import.ImportTextures)
    {
        for (int32 i = analysis.SubAssets.Count() - 1; i >= 0; i--)
        {
            if (analysis.SubAssets[i].Kind == ModelSubAssetKind::Texture)
                analysis.SubAssets.RemoveAtKeepOrder(i);
        }
        for (int32 i = analysis.Candidates.Count() - 1; i >= 0; i--)
        {
            if (analysis.Candidates[i].TypeName == Texture::TypeName)
                analysis.Candidates.RemoveAtKeepOrder(i);
        }
    }
    for (const TextureEntry& texture : ownedData.Data.Textures)
    {
        if (settings.Import.ImportTextures && !texture.FilePath.IsEmpty() && texture.EmbeddedData.IsEmpty())
            analysis.ReferencedTexturePaths.Add(texture.FilePath);
    }
    analysis.SourceLodCount = ownedData.Data.LODs.Count();
    analysis.SourceMeshCount = ownedData.Data.LODs.HasItems() ? ownedData.Data.LODs[0].Meshes.Count() : 0;
    analysis.SourceAnimationCount = ownedData.Data.Animations.Count();
    analysis.SourceMaterialCount = ownedData.Data.Materials.Count();
    analysis.SourceSkeletonBoneCount = ownedData.Data.Skeleton.Bones.Count();
    analysis.SourceSkeletonNodeCount = ownedData.Data.Skeleton.Nodes.Count();
    analysis.EstimatedMemoryBytes = EstimateData(ownedData.Data);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

void ModelProcessor::ClearCaches()
{
    ModelProcessorCache& cache = Cache();
    std::lock_guard<std::mutex> lock(cache.Locker);
    cache.Analyses.Clear();
    cache.AnalysisOrder.Clear();
#if COMPILE_WITH_ASSETS_IMPORTER
    cache.Builds.Clear();
    cache.BuildOrder.Clear();
    cache.BuildBytes = 0;
#endif
}

bool ModelProcessor::Prepare(PrepareAssetContext& context, PreparedAsset& prepared, AssetPipelineDiagnostic& diagnostic)
{
    prepared = PreparedAsset();
    ModelProcessorSettings settings;
    if (ModelProcessorSettings::Parse(context.GetSettings(), ModelProcessorSettings::CurrentVersion, settings, diagnostic))
    {
        diagnostic.AssetGuid = context.GetRecord().ID;
        diagnostic.SourcePath = context.GetRecord().SourcePath.Get();
        return true;
    }

    Array<byte> source;
    ContentHash sourceHash;
    AssetDependencyOrigin sourceOrigin;
    sourceOrigin.Path = context.GetRecord().SourcePath.Get();
    if (context.ReadSourceFile(context.GetRecord().SourcePath.Get(), source, sourceHash, sourceOrigin, diagnostic))
        return true;
    if (context.GetCancellation().IsCancellationRequested())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, AssetPipelineDiagnosticStage::Prepare,
            context.GetRecord().ID, context.GetRecord().SourcePath.Get(), TEXT("Model preparation was cancelled after reading the root source."));

    HashSet<String> dependencyPaths;
    dependencyPaths.Add(String(context.GetRecord().SourcePath.Get()).ToLower());
    const String extension = FileSystem::GetExtension(context.GetRecord().SourcePath.Get()).ToLower();
    StringAnsi settingsJson;
    if (settings.ToJson(settingsJson, diagnostic))
        return true;
    ArtifactKeyBuilder analysisKeyBuilder(StringAnsiView("flax-model-source-analysis-v1"));
    analysisKeyBuilder.AddHash(StringAnsiView("source"), sourceHash);
    analysisKeyBuilder.AddHash(StringAnsiView("settings"), ContentHash::Compute(settingsJson.Get(), settingsJson.Length()));
    analysisKeyBuilder.AddString(StringAnsiView("extension"), extension);
    int32 analysisDependencyIndex = 0;
    if (extension == TEXT("gltf") && DeclareGltfDependencies(context, source, context.GetRecord().SourcePath.Get(), dependencyPaths,
        analysisKeyBuilder, analysisDependencyIndex, diagnostic))
        return true;

    std::shared_ptr<const ModelSourceAnalysis> analysis;
    const StringAnsi analysisKey = analysisKeyBuilder.Finalize().ToString();
    if (GetCachedSourceAnalysis(analysisKey, context.GetRecord().SourcePath.Get(), settings, analysis, diagnostic))
    {
        diagnostic.AssetGuid = context.GetRecord().ID;
        return true;
    }

    if ((dependencyPaths.Count() > 1 || analysis->ReferencedTexturePaths.HasItems()) &&
        context.DependsOnLogicalPath(sourceOrigin, diagnostic))
        return true;

    for (const String& texturePath : analysis->ReferencedTexturePaths)
    {
        String path = texturePath;
        FileSystem::NormalizePath(path);
        if (!dependencyPaths.Add(path.ToLower()))
            continue;
        Array<byte> bytes;
        ContentHash hash;
        AssetDependencyOrigin origin;
        origin.Path = context.GetRecord().SourcePath.Get();
        origin.GraphNode = TEXT("material-texture");
        if (context.ReadSourceFile(path, bytes, hash, origin, diagnostic))
        {
            diagnostic.Message = String::Format(TEXT("Model material references missing external source '{0}'."), path);
            return true;
        }
    }

    prepared.SubAssets = analysis->Candidates;

    const String parserIdentity = TEXT("flax-model-parser-v1-") + extension;
    const String lodIdentity = TEXT("flax-model-meshoptimizer-v1");
    const String sdfIdentity = TEXT("flax-model-sdf-v1");
    const String animationIdentity = TEXT("flax-model-animation-compressor-v1");
    if (context.DeclareToolchain(TEXT("model-parser"), ContentHash::Compute(*parserIdentity, parserIdentity.Length() * sizeof(Char)), sourceOrigin, diagnostic) ||
        context.DeclareToolchain(TEXT("model-lod"), ContentHash::Compute(*lodIdentity, lodIdentity.Length() * sizeof(Char)), sourceOrigin, diagnostic) ||
        context.DeclareToolchain(TEXT("model-sdf"), ContentHash::Compute(*sdfIdentity, sdfIdentity.Length() * sizeof(Char)), sourceOrigin, diagnostic) ||
        context.DeclareToolchain(TEXT("model-animation"), ContentHash::Compute(*animationIdentity, animationIdentity.Length() * sizeof(Char)), sourceOrigin, diagnostic))
        return true;

    const ModelSubAssetInfo* selected = nullptr;
    const String stableKey = context.GetRecord().SubAsset.Get();
    if (stableKey.HasChars())
    {
        selected = ModelSubAssetKeys::Find(analysis->SubAssets, stableKey);
        if (!selected || selected->TypeName != context.GetRecord().TypeName)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Prepare,
                context.GetRecord().ID, context.GetRecord().SourcePath.Get(), TEXT("Model child record no longer matches a prepared stable candidate."));
    }

    if (context.DeclareOutput(StringAnsiView("runtime"), context.GetRecord().ID, diagnostic))
        return true;
    const bool animationOutput = (selected && selected->Kind == ModelSubAssetKind::Animation) ||
        (!selected && context.GetRecord().TypeName == Animation::TypeName);
    const bool materialOutput = selected && selected->Kind == ModelSubAssetKind::Material;
    const bool textureOutput = selected && selected->Kind == ModelSubAssetKind::Texture;
    const bool hasGeometry = analysis->SourceMeshCount > 0;
    if (!animationOutput && !materialOutput && !textureOutput)
    {
        if (hasGeometry)
        {
            if (context.DeclareOutput(StringAnsiView("geometry"), context.GetRecord().ID, diagnostic) ||
                context.DeclareOutput(StringAnsiView("lod"), context.GetRecord().ID, diagnostic))
                return true;
        }
        const bool isStatic = selected ? selected->TypeName == Model::TypeName : context.GetRecord().TypeName == Model::TypeName;
        if (hasGeometry && isStatic && settings.Import.GenerateSDF && context.DeclareOutput(StringAnsiView("sdf"), context.GetRecord().ID, diagnostic))
            return true;
        const bool isSkinned = selected ? selected->TypeName == SkinnedModel::TypeName : context.GetRecord().TypeName == SkinnedModel::TypeName;
        if (isSkinned && context.DeclareOutput(StringAnsiView("skeleton"), context.GetRecord().ID, diagnostic))
            return true;
    }
    else if (animationOutput)
    {
        if (context.DeclareOutput(StringAnsiView("animation"), context.GetRecord().ID, diagnostic))
            return true;
    }
    else if (materialOutput)
    {
        if (context.DeclareOutput(StringAnsiView("material"), context.GetRecord().ID, diagnostic))
            return true;
    }
    else if (textureOutput)
    {
        if (context.DeclareOutput(StringAnsiView("texture"), context.GetRecord().ID, diagnostic))
            return true;
    }

    auto payload = std::make_shared<ModelPreparedPayload>();
    payload->Settings = settings;
    payload->RootTypeName = context.GetRecord().IsMainAsset() ? context.GetRecord().TypeName : String::Empty;
    if (payload->RootTypeName.IsEmpty())
        payload->RootTypeName = analysis->SourceSkeletonBoneCount > 0 ? SkinnedModel::TypeName : Model::TypeName;
    payload->RootSourcePath = context.GetRecord().SourcePath.Get();
    payload->SelectedStableKey = stableKey;
    payload->RootSourceHash = sourceHash;
    payload->SubAssets = analysis->SubAssets;
    payload->SourceLodCount = analysis->SourceLodCount;
    payload->SourceMeshCount = analysis->SourceMeshCount;
    payload->SourceAnimationCount = analysis->SourceAnimationCount;
    payload->SourceMaterialCount = analysis->SourceMaterialCount;
    payload->SourceSkeletonNodeCount = analysis->SourceSkeletonNodeCount;
    payload->EstimatedOutputBytes = Math::Max<uint64>(source.Count() * 3ull, 1024ull * 1024ull);
    prepared.Payload = payload;
    prepared.MemoryEstimate = Math::Max<uint64>(analysis->EstimatedMemoryBytes, payload->EstimatedOutputBytes);
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

bool ModelProcessor::BuildOutputKey(const PreparedAsset& prepared, const ArtifactTarget& target, const StringAnsiView& outputKind,
    ArtifactKey& key, Array<ArtifactKeyComponent>& components, AssetPipelineDiagnostic& diagnostic)
{
    key = ArtifactKey();
    components.Clear();
    const auto* payload = static_cast<const ModelPreparedPayload*>(prepared.Payload.get());
    const AssetProcessorDescriptor descriptor = CreateDescriptor();
    const AssetProcessorOutputDescriptor* output = FindOutput(descriptor, outputKind);
    if (!payload || !output)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Prepare,
            prepared.AssetID, payload ? payload->RootSourcePath : StringView::Empty, TEXT("Model output key requires prepared state and a declared output kind."));

    ArtifactKeyBuilder builder(StringAnsiView("flax-model-output-v1"));
    descriptor.AppendVersionKey(builder, *output);
    builder.AddString(StringAnsiView("compatibility"), output->CompatibilityTag);
    builder.AddGuid(StringAnsiView("effective-asset"), prepared.AssetID);
    builder.AddString(StringAnsiView("output-type"), prepared.OutputType);
    builder.AddString(StringAnsiView("stable-subasset"), payload->SelectedStableKey);
    builder.AddUInt32(StringAnsiView("subasset-key-algorithm"), ModelSubAssetKeys::AlgorithmVersion);

    int32 dependencyIndex = 0;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        bool include = dependency.IsSourceDependency();
        if (dependency.Kind == AssetDependencyKind::Toolchain)
        {
            include = dependency.StableIdentity == TEXT("model-parser") ||
                ((outputKind == "lod" || outputKind == "runtime") && dependency.StableIdentity == TEXT("model-lod")) ||
                ((outputKind == "sdf" || outputKind == "runtime") && dependency.StableIdentity == TEXT("model-sdf")) ||
                ((outputKind == "animation" || (outputKind == "runtime" && prepared.OutputType == Animation::TypeName)) && dependency.StableIdentity == TEXT("model-animation"));
        }
        if (include)
            dependency.AppendKeyComponents(builder, dependencyIndex++);
    }

    auto addSection = [&](const char* name) -> bool
    {
        StringAnsi json;
        if (payload->Settings.ToSectionJson(StringAnsiView(name), json, diagnostic))
            return true;
        builder.AddHash(StringAnsi(name) + "-settings", ContentHash::Compute(json.Get(), json.Length()));
        return false;
    };
    if (outputKind == "geometry")
    {
        if (addSection("geometry") || addSection("transform"))
            return true;
    }
    else if (outputKind == "lod")
    {
        if (addSection("geometry") || addSection("transform") || addSection("lod"))
            return true;
    }
    else if (outputKind == "sdf")
    {
        if (addSection("geometry") || addSection("transform") || addSection("lod") || addSection("sdf"))
            return true;
    }
    else if (outputKind == "skeleton")
    {
        if (addSection("geometry") || addSection("transform") || addSection("lod"))
            return true;
    }
    else if (outputKind == "animation")
    {
        if (addSection("animation") || addSection("transform"))
            return true;
    }
    else if (outputKind == "material")
    {
        if (addSection("materials"))
            return true;
    }
    else
    {
        StringAnsi allSettings;
        if (payload->Settings.ToJson(allSettings, diagnostic))
            return true;
        builder.AddHash(StringAnsiView("all-settings"), ContentHash::Compute(allSettings.Get(), allSettings.Length()));
    }

    if (payload->SelectedStableKey.HasChars())
    {
        const ModelSubAssetInfo* selected = ModelSubAssetKeys::Find(payload->SubAssets, payload->SelectedStableKey);
        if (selected)
            builder.AddHash(StringAnsiView("selected-semantic"), selected->SemanticHash);
    }
    if ((outputKind == "runtime" || outputKind == "material") && payload->AssignedIDs.Count() != 0)
    {
        Array<String> assignedKeys;
        assignedKeys.EnsureCapacity(payload->AssignedIDs.Count());
        for (const auto& assigned : payload->AssignedIDs)
            assignedKeys.Add(assigned.Key);
        std::sort(assignedKeys.Get(), assignedKeys.Get() + assignedKeys.Count());
        for (int32 i = 0; i < assignedKeys.Count(); i++)
        {
            builder.AddString(StringAnsi::Format("owned-{0}-key", i), assignedKeys[i]);
            builder.AddGuid(StringAnsi::Format("owned-{0}-guid", i), payload->AssignedIDs[assignedKeys[i]]);
        }
    }
    builder.AddTarget(target, output->TargetDimensions);
    key = builder.Finalize();
    components = builder.GetComponents();
    diagnostic = AssetPipelineDiagnostic();
    return false;
}

#if COMPILE_WITH_ASSETS_IMPORTER
namespace
{
    struct ModelArtifactArguments
    {
        const ModelData* Data = nullptr;
        ModelTool::Options* Options = nullptr;
    };

    CreateAssetResult CreateModelArtifact(CreateAssetContext& context)
    {
        const auto* arguments = static_cast<const ModelArtifactArguments*>(context.CustomArg);
        if (!arguments || !arguments->Data || !arguments->Options)
            return CreateAssetResult::Error;
        return ImportModel::CreateCompatibility(context, *arguments->Data, *arguments->Options);
    }

    struct ModelTextureArtifactArguments
    {
        TextureData* Data = nullptr;
        TextureTool::Options* Options = nullptr;
    };

    CreateAssetResult CreateModelTextureArtifact(CreateAssetContext& context)
    {
        const auto* arguments = static_cast<const ModelTextureArtifactArguments*>(context.CustomArg);
        if (!arguments || !arguments->Data || !arguments->Options)
            return CreateAssetResult::Error;
        return ImportTexture::CreateArtifact(context, *arguments->Data, *arguments->Options);
    }

    Guid GetTextureID(const Dictionary<int32, Guid>& assignedTextures, int32 index)
    {
        const Guid* id = assignedTextures.TryGet(index);
        return id ? *id : Guid::Empty;
    }

    void FillMaterialOptions(const MaterialSlotEntry& source, const Dictionary<int32, Guid>& assignedTextures,
        CreateMaterial::Options& destination)
    {
        destination.Diffuse.Color = source.Diffuse.Color;
        destination.Diffuse.Texture = GetTextureID(assignedTextures, source.Diffuse.TextureIndex);
        destination.Diffuse.HasAlphaMask = source.Diffuse.HasAlphaMask;
        destination.Emissive.Color = source.Emissive.Color;
        destination.Emissive.Texture = GetTextureID(assignedTextures, source.Emissive.TextureIndex);
        destination.Opacity.Value = source.Opacity.Value;
        destination.Opacity.Texture = GetTextureID(assignedTextures, source.Opacity.TextureIndex);
        destination.Roughness.Value = source.Roughness.Value;
        destination.Roughness.Channel = source.Roughness.Channel;
        destination.Roughness.Texture = GetTextureID(assignedTextures, source.Roughness.TextureIndex);
        destination.Metalness.Value = source.Metalness.Value;
        destination.Metalness.Channel = source.Metalness.Channel;
        destination.Metalness.Texture = GetTextureID(assignedTextures, source.Metalness.TextureIndex);
        destination.Normals.Texture = GetTextureID(assignedTextures, source.Normals.TextureIndex);
        if (source.TwoSided || source.Diffuse.HasAlphaMask)
            destination.Info.CullMode = CullMode::TwoSided;
        if (source.Wireframe)
            destination.Info.FeaturesFlags |= MaterialFeaturesFlags::Wireframe;
        if (!Math::IsOne(source.Opacity.Value) || source.Opacity.TextureIndex != -1)
            destination.Info.BlendMode = MaterialBlendMode::Transparent;
    }

    void FillTextureOptions(const TextureEntry& source, TextureTool::Options& destination)
    {
        destination.sRGB = source.sRGB;
        switch (source.Type)
        {
        case TextureEntry::TypeHint::ColorRGB:
            destination.Type = TextureFormatType::ColorRGB;
            break;
        case TextureEntry::TypeHint::ColorRGBA:
            destination.Type = TextureFormatType::ColorRGBA;
            break;
        case TextureEntry::TypeHint::Normals:
            destination.Type = TextureFormatType::NormalMap;
            destination.sRGB = false;
            break;
        }
        ImportTexture::NormalizeOptions(destination);
    }

    bool CopyMeshGroup(const ModelData& source, const StringView& name, const Dictionary<int32, Guid>& assignedMaterials, ModelData& destination)
    {
        bool retained = false;
        destination.MinScreenSize = source.MinScreenSize;
        destination.Textures = source.Textures;
        destination.Skeleton = source.Skeleton;
        destination.Nodes = source.Nodes;
        destination.Animations = source.Animations;
        destination.PositionFormat = source.PositionFormat;
        destination.TexCoordFormat = source.TexCoordFormat;
        Dictionary<int32, int32> remap;
        for (const ModelLodData& sourceLod : source.LODs)
        {
            ModelLodData& destinationLod = destination.LODs.AddOne();
            destinationLod.ScreenSize = sourceLod.ScreenSize;
            for (const MeshData* sourceMesh : sourceLod.Meshes)
            {
                if (!sourceMesh || sourceMesh->Name != name)
                    continue;
                retained = true;
                auto* mesh = New<MeshData>(*sourceMesh);
                int32* mapped = remap.TryGet(sourceMesh->MaterialSlotIndex);
                if (!mapped)
                {
                    const int32 oldIndex = sourceMesh->MaterialSlotIndex;
                    const int32 newIndex = destination.Materials.Count();
                    if (oldIndex < 0 || oldIndex >= source.Materials.Count())
                    {
                        Delete(mesh);
                        return true;
                    }
                    MaterialSlotEntry material = source.Materials[oldIndex];
                    const Guid* assigned = assignedMaterials.TryGet(oldIndex);
                    if (assigned)
                        material.AssetID = *assigned;
                    destination.Materials.Add(MoveTemp(material));
                    remap.Add(oldIndex, newIndex);
                    mesh->MaterialSlotIndex = newIndex;
                }
                else
                {
                    mesh->MaterialSlotIndex = *mapped;
                }
                destinationLod.Meshes.Add(mesh);
            }
        }
        return !retained;
    }

    bool WriteOutput(ArtifactBuildContext& context, const StringAnsiView& kind, const StringView& filename,
        const void* bytes, int32 length, AssetPipelineDiagnostic& diagnostic)
    {
        ArtifactWriter writer;
        return context.OpenOutput(kind, writer, diagnostic) || writer.WriteFile(filename, bytes, length, diagnostic);
    }

    bool LoadChunk(FlaxStorage* storage, AssetInitData& initData, int32 chunkIndex, AssetPipelineDiagnostic& diagnostic,
        const PreparedAsset& prepared, const StringView& path)
    {
        if (chunkIndex < 0 || chunkIndex >= ASSET_FILE_DATA_CHUNKS || !initData.Header.Chunks[chunkIndex] ||
            storage->LoadAssetChunk(initData.Header.Chunks[chunkIndex]))
        {
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, path, String::Format(TEXT("Model compatibility output is missing required chunk {0}."), chunkIndex));
        }
        return false;
    }

    const ModelSubAssetInfo* ResolveBuiltSelection(const Array<ModelSubAssetInfo>& infos, const ModelSubAssetInfo& selected)
    {
        const ModelSubAssetInfo* result = ModelSubAssetKeys::Find(infos, selected.StableKey);
        if (result)
            return result;
        for (const ModelSubAssetInfo& info : infos)
        {
            if (info.Kind == selected.Kind && info.SourceIndex == selected.SourceIndex && info.SemanticHash == selected.SemanticHash)
                return &info;
        }
        return nullptr;
    }

}
#endif

bool ModelProcessor::Build(ArtifactBuildContext& context, AssetPipelineDiagnostic& diagnostic)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    diagnostic = AssetPipelineDiagnostic();
    const PreparedAsset& prepared = context.GetPreparedAsset();
    const auto* payload = static_cast<const ModelPreparedPayload*>(prepared.Payload.get());
    if (!payload)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, StringView::Empty, TEXT("Model prepared payload is missing."));
    if (context.GetCancellation().IsCancellationRequested())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, payload->RootSourcePath, TEXT("Model build was cancelled before source parsing."));

    const AssetDependency* rootDependency = nullptr;
    for (const AssetDependency& dependency : prepared.Dependencies)
    {
        if (dependency.Kind == AssetDependencyKind::SourceAsset && dependency.Content == payload->RootSourceHash)
        {
            rootDependency = &dependency;
            break;
        }
    }
    if (!rootDependency)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, payload->RootSourcePath, TEXT("Prepared model root source capability is missing."));
    String sourcePath;
    if (context.TryGetInputPath(rootDependency->StableIdentity, sourcePath, diagnostic))
        return true;

    const ModelSubAssetInfo* selected = payload->SelectedStableKey.HasChars()
        ? ModelSubAssetKeys::Find(payload->SubAssets, payload->SelectedStableKey)
        : nullptr;
    if (payload->SelectedStableKey.HasChars() && !selected)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourcePath, TEXT("Selected model subasset disappeared before build."));

    ModelTool::Options options = payload->Settings.Import;
    options.Cached = nullptr;
    options.SplitObjects = false;
    options.ObjectIndex = -1;
    options.CollisionMeshesPrefix.Clear();
    options.CollisionMeshesPostfix.Clear();
    if (selected && selected->Kind == ModelSubAssetKind::Animation)
        options.Type = ModelTool::ModelType::Animation;
    else if (selected && (selected->Kind == ModelSubAssetKind::Material || selected->Kind == ModelSubAssetKind::Texture))
        options.Type = ModelTool::ModelType::Prefab;
    else
        options.Type = TypeFromName(selected ? selected->TypeName : prepared.OutputType);
    if (selected && (selected->Kind == ModelSubAssetKind::Animation || selected->Kind == ModelSubAssetKind::Material ||
        selected->Kind == ModelSubAssetKind::Texture))
    {
        options.GenerateLODs = false;
        options.GenerateSDF = false;
    }
    if (selected && selected->Kind == ModelSubAssetKind::Mesh)
        options.MergeMeshes = false;

    ScopedModelData ownedData;
    ScopedModelData selectedData;
    std::shared_ptr<const ModelBuildData> sharedData;
    const ModelData* buildData = nullptr;
    const Array<ModelSubAssetInfo>* builtInfos = nullptr;
    Array<ModelSubAssetInfo> ownedInfos;
    if (selected)
    {
        ArtifactKeyBuilder sharedKeyBuilder(StringAnsiView("flax-model-shared-build-data-v1"));
        sharedKeyBuilder.AddHash(StringAnsiView("settings"), prepared.SettingsHash);
        sharedKeyBuilder.AddUInt32(StringAnsiView("type"), static_cast<uint32>(options.Type));
        sharedKeyBuilder.AddUInt32(StringAnsiView("kind"), static_cast<uint32>(selected->Kind));
        int32 dependencyIndex = 0;
        for (const AssetDependency& dependency : prepared.Dependencies)
        {
            if (!dependency.IsSourceDependency())
                continue;
            const StringAnsi prefix = StringAnsi::Format("source-{0}-", dependencyIndex++);
            sharedKeyBuilder.AddString(prefix + "identity", dependency.StableIdentity);
            sharedKeyBuilder.AddHash(prefix + "content", dependency.Content);
        }
        if (GetCachedBuildData(sharedKeyBuilder.Finalize().ToString(), sourcePath, options, sharedData, diagnostic))
        {
            diagnostic.AssetGuid = prepared.AssetID;
            return true;
        }
        options = sharedData->Options;
        buildData = &sharedData->Data;
        builtInfos = &sharedData->SubAssets;
    }
    else
    {
        String importError;
        if (ModelTool::ImportModel(sourcePath, ownedData.Data, options, importError, String::Empty))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, sourcePath, importError.IsEmpty() ? TEXT("Model compatibility importer rejected the verified source.") : importError);
        // Canonical artifacts cannot consult mutable editor Build Settings during serialization.
        // Resolve Automatic to the stable full-precision representation for every build target.
        if (ownedData.Data.PositionFormat == ModelData::PositionFormats::Automatic)
            ownedData.Data.PositionFormat = ModelData::PositionFormats::Float32;
        Array<SubAssetCandidate> builtCandidates;
        if (ModelSubAssetKeys::Enumerate(ownedData.Data, ownedInfos, builtCandidates, diagnostic))
            return true;
        buildData = &ownedData.Data;
        builtInfos = &ownedInfos;
    }
    if (context.GetCancellation().IsCancellationRequested())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildCancelled, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, sourcePath, TEXT("Model build was cancelled after source processing."));

    Dictionary<int32, Guid> assignedMaterials;
    Dictionary<int32, Guid> assignedTextures;
    for (const ModelSubAssetInfo& info : *builtInfos)
    {
        const Guid* assigned = payload->AssignedIDs.TryGet(info.StableKey);
        if (!assigned)
            continue;
        if (info.Kind == ModelSubAssetKind::Material && info.SourceIndex >= 0 && info.SourceIndex < buildData->Materials.Count())
        {
            if (selected)
                assignedMaterials[info.SourceIndex] = *assigned;
            else
                ownedData.Data.Materials[info.SourceIndex].AssetID = *assigned;
        }
        else if (info.Kind == ModelSubAssetKind::Texture && info.SourceIndex >= 0 && info.SourceIndex < buildData->Textures.Count())
        {
            assignedTextures[info.SourceIndex] = *assigned;
            if (!selected)
                ownedData.Data.Textures[info.SourceIndex].AssetID = *assigned;
        }
    }

    if (selected && selected->Kind == ModelSubAssetKind::Mesh)
    {
        if (CopyMeshGroup(*buildData, selected->DisplayName, assignedMaterials, selectedData.Data))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, sourcePath, TEXT("Stable mesh group could not be selected after source processing."));
        buildData = &selectedData.Data;
    }
    if (selected && selected->Kind == ModelSubAssetKind::Animation)
    {
        const ModelSubAssetInfo* rebuilt = ResolveBuiltSelection(*builtInfos, *selected);
        if (!rebuilt || rebuilt->SourceIndex < 0 || rebuilt->SourceIndex >= buildData->Animations.Count())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, sourcePath, TEXT("Stable animation selection became ambiguous after processing."));
        options.ObjectIndex = rebuilt->SourceIndex;
    }

    String runtimeScratchPath;
    if (context.CreateScratchFilePath(TEXT(".flax"), runtimeScratchPath, diagnostic))
        return true;
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(runtimeScratchPath);
        FileSystem::DeleteFile(runtimeScratchPath);
    };

    CreateAssetResult createResult;
    if (selected && selected->Kind == ModelSubAssetKind::Material)
    {
        const ModelSubAssetInfo* rebuilt = ResolveBuiltSelection(*builtInfos, *selected);
        if (!rebuilt || rebuilt->SourceIndex < 0 || rebuilt->SourceIndex >= buildData->Materials.Count())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, sourcePath, TEXT("Stable material selection became ambiguous after processing."));
        CreateMaterial::Options materialOptions;
        FillMaterialOptions(buildData->Materials[rebuilt->SourceIndex], assignedTextures, materialOptions);
        CreateAssetContext importerContext(sourcePath, runtimeScratchPath, prepared.AssetID, &materialOptions, true, Material::TypeName);
        CreateAssetFunction callback = &CreateMaterial::Create;
        createResult = importerContext.Run(callback);
    }
    else if (selected && selected->Kind == ModelSubAssetKind::Texture)
    {
        const ModelSubAssetInfo* rebuilt = ResolveBuiltSelection(*builtInfos, *selected);
        if (!rebuilt || rebuilt->SourceIndex < 0 || rebuilt->SourceIndex >= buildData->Textures.Count())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::SubAssetReconcileRequired, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, sourcePath, TEXT("Stable texture selection became ambiguous after processing."));
        const TextureEntry& texture = buildData->Textures[rebuilt->SourceIndex];
        String textureSourcePath;
        bool ownsTextureSource = false;
        if (texture.EmbeddedData.HasItems())
        {
            String extension = FileSystem::GetExtension(texture.FilePath);
            extension = extension.IsEmpty() ? TEXT(".png") : TEXT(".") + extension;
            if (context.CreateScratchFilePath(extension, textureSourcePath, diagnostic) ||
                File::WriteAllBytes(textureSourcePath, texture.EmbeddedData.Get(), texture.EmbeddedData.Count()))
                return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                    prepared.AssetID, texture.FilePath, TEXT("Embedded model texture could not be staged for decoding."));
            ownsTextureSource = true;
        }
        else
            return Fail(diagnostic, AssetPipelineDiagnosticCode::UndeclaredInput, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, texture.FilePath, TEXT("Model-owned texture has no embedded source payload."));
        SCOPE_EXIT
        {
            if (ownsTextureSource)
                FileSystem::DeleteFile(textureSourcePath);
        };
        TextureTool::Options textureOptions;
        FillTextureOptions(texture, textureOptions);
        TextureData sourceTextureData;
        if (TextureTool::ImportTexture(textureSourcePath, sourceTextureData, false))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, texture.FilePath, TEXT("Model-owned texture source decode failed."));
        textureOptions.GenerateMipMaps = textureOptions.GenerateMipMaps && sourceTextureData.Width > 0 && sourceTextureData.Height > 0 &&
            Math::IsPowerOfTwo(sourceTextureData.Width) && Math::IsPowerOfTwo(sourceTextureData.Height);
        TextureData textureData;
        String textureError;
        const bool decodeFailed = TextureTool::ImportTexture(textureSourcePath, textureData, textureOptions, textureError, false);
        if (decodeFailed)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::BuildFailed, AssetPipelineDiagnosticStage::Build,
                prepared.AssetID, texture.FilePath, textureError.IsEmpty() ? TEXT("Model-owned texture decode failed.") : textureError);
        ModelTextureArtifactArguments arguments;
        arguments.Data = &textureData;
        arguments.Options = &textureOptions;
        CreateAssetContext importerContext(texture.FilePath, runtimeScratchPath, prepared.AssetID, &arguments, true, Texture::TypeName);
        createResult = importerContext.Run(&CreateModelTextureArtifact);
    }
    else
    {
        ModelArtifactArguments arguments;
        arguments.Data = buildData;
        arguments.Options = &options;
        CreateAssetContext importerContext(sourcePath, runtimeScratchPath, prepared.AssetID, &arguments, true, prepared.OutputType);
        CreateAssetFunction callback = &CreateModelArtifact;
        createResult = importerContext.Run(callback);
    }
    if (createResult != CreateAssetResult::Ok)
    {
        diagnostic.Related.Add(::ToString(createResult));
        return Fail(diagnostic, createResult == CreateAssetResult::Abort ? AssetPipelineDiagnosticCode::BuildCancelled : AssetPipelineDiagnosticCode::BuildFailed,
            AssetPipelineDiagnosticStage::Build, prepared.AssetID, sourcePath, TEXT("Model compatibility artifact creation failed in job staging."));
    }

    Array<byte> runtimeBytes;
    if (File::ReadAllBytes(runtimeScratchPath, runtimeBytes) || runtimeBytes.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, runtimeScratchPath, TEXT("Model compatibility artifact is unreadable or empty."));
    if (WriteOutput(context, StringAnsiView("runtime"), TEXT("asset.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic))
        return true;

    if (prepared.OutputType == Material::TypeName)
        return WriteOutput(context, StringAnsiView("material"), TEXT("material.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic);
    if (prepared.OutputType == Texture::TypeName)
        return WriteOutput(context, StringAnsiView("texture"), TEXT("texture.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic);
    if (prepared.OutputType == Animation::TypeName)
        return WriteOutput(context, StringAnsiView("animation"), TEXT("animation.flax"), runtimeBytes.Get(), runtimeBytes.Count(), diagnostic);

    auto storage = ContentStorageManager::GetStorage(runtimeScratchPath, true);
    AssetInitData initData;
    if (!storage || storage->LoadAssetHeader(prepared.AssetID, initData))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactInvalid, AssetPipelineDiagnosticStage::Build,
            prepared.AssetID, runtimeScratchPath, TEXT("Model compatibility artifact header could not be reopened."));

    if (payload->SourceMeshCount > 0)
    {
        const int32 geometryChunkIndex = MODEL_LOD_TO_CHUNK_INDEX(0);
        if (LoadChunk(storage.Get(), initData, geometryChunkIndex, diagnostic, prepared, runtimeScratchPath))
            return true;
        FlaxChunk* geometryChunk = initData.Header.Chunks[geometryChunkIndex];
        if (WriteOutput(context, StringAnsiView("geometry"), TEXT("geometry.bin"), geometryChunk->Data.Get(), geometryChunk->Data.Length(), diagnostic))
            return true;

        MemoryWriteStream lodStream(1024);
        lodStream.WriteInt32(buildData->LODs.Count());
        for (int32 lodIndex = 0; lodIndex < buildData->LODs.Count(); lodIndex++)
        {
            const int32 chunkIndex = MODEL_LOD_TO_CHUNK_INDEX(lodIndex);
            if (LoadChunk(storage.Get(), initData, chunkIndex, diagnostic, prepared, runtimeScratchPath))
                return true;
            const FlaxChunk* chunk = initData.Header.Chunks[chunkIndex];
            lodStream.WriteInt32(chunk->Data.Length());
            lodStream.WriteBytes(chunk->Data.Get(), chunk->Data.Length());
        }
        if (WriteOutput(context, StringAnsiView("lod"), TEXT("lod.bin"), lodStream.GetHandle(), static_cast<int32>(lodStream.GetPosition()), diagnostic))
            return true;

        if (options.Type == ModelTool::ModelType::Model && options.GenerateSDF)
        {
            if (LoadChunk(storage.Get(), initData, 15, diagnostic, prepared, runtimeScratchPath))
                return true;
            const FlaxChunk* sdfChunk = initData.Header.Chunks[15];
            if (WriteOutput(context, StringAnsiView("sdf"), TEXT("sdf.bin"), sdfChunk->Data.Get(), sdfChunk->Data.Length(), diagnostic))
                return true;
        }
    }
    if (options.Type == ModelTool::ModelType::SkinnedModel)
    {
        if (LoadChunk(storage.Get(), initData, 0, diagnostic, prepared, runtimeScratchPath))
            return true;
        const FlaxChunk* skeletonChunk = initData.Header.Chunks[0];
        if (WriteOutput(context, StringAnsiView("skeleton"), TEXT("skeleton.bin"), skeletonChunk->Data.Get(), skeletonChunk->Data.Length(), diagnostic))
            return true;
    }
    diagnostic = AssetPipelineDiagnostic();
    return false;
#else
    const PreparedAsset& prepared = context.GetPreparedAsset();
    return Fail(diagnostic, AssetPipelineDiagnosticCode::ProcessorMissing, AssetPipelineDiagnosticStage::Build,
        prepared.AssetID, StringView::Empty, TEXT("Model compatibility build requires the editor asset importer module."));
#endif
}

#endif
