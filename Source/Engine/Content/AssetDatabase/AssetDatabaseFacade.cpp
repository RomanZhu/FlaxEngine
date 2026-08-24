// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseFacade.h"
#include "AssetDatabaseSnapshot.h"
#include "AssetMeta.h"
#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/SkeletonMask.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/JsonStorageProxy.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Documents/CollisionDataDocument.h"
#include "Engine/Content/Documents/ParticleSystemDocument.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Physics/CollisionData.h"
#include "LegacyAssetMigrator.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Threading/Threading.h"
#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Graphics/Textures/TextureData.h"
#include "Engine/Render2D/SpriteAtlas.h"
#include "Engine/Content/Build/Processors/TextureProcessorSettings.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/TexturePipelineService.h"
#endif
#endif
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
#include "Engine/Content/Assets/Model.h"
#include "Engine/Content/Assets/SkinnedModel.h"
#include "Engine/Content/Build/Processors/ModelProcessorSettings.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#endif
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "Engine/Content/Build/Processors/GraphDocumentProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
#include "Engine/ContentImporters/CreateMaterialInstance.h"
#include "Engine/ContentImporters/CreateSkeletonMask.h"
#include "Engine/ContentImporters/CreateSceneAnimation.h"
#include "Engine/ContentImporters/CreateParticleSystem.h"
#include "Engine/ContentImporters/CreateCollisionData.h"
#include "Engine/ContentImporters/Types.h"
#endif
#include <algorithm>

Delegate<uint64> AssetDatabaseFacade::DatabaseChanged;

namespace
{
    CriticalSection StateLocker;
    Array<AssetPipelineDiagnostic> LastDiagnostics;
    SourceHashCache HashCache;
    bool IsBound = false;

    String SnapshotDirectory()
    {
#if USE_EDITOR
        return Globals::ProjectLibraryFolder / TEXT("AssetDatabase");
#else
        return String::Empty;
#endif
    }

    String SnapshotPath()
    {
        return SnapshotDirectory() / TEXT("index.bin");
    }

    void OnDatabaseChanged(const AssetDatabaseChangeBatch& change)
    {
        AssetDatabaseFacade::DatabaseChanged(change.Revision);
    }

    void EnsureBound()
    {
        if (!IsBound)
        {
            AssetDatabase::Get().Changed.BindUnique<OnDatabaseChanged>();
            IsBound = true;
        }
    }

    AssetDatabaseRecordInfo ToInfo(const AssetRecord& record)
    {
        AssetDatabaseRecordInfo result;
        result.ID = record.ID;
        result.SourceAssetID = record.SourceAssetID;
        result.TypeName = record.TypeName;
        result.CanonicalPath = record.CanonicalPath.Get();
        result.SourcePath = record.SourcePath.Get();
        result.MetaPath = record.MetaPath.Get();
        result.SubAssetKey = record.SubAsset.Get();
        result.ProcessorID = record.ProcessorID;
        result.MetaSemanticHash = record.MetaSemanticHash;
        result.SourceKind = record.SourceKind;
        result.Status = record.Status;
        result.Revision = record.DatabaseRevision;
        result.IsMain = record.IsMainAsset();
        return result;
    }

    bool FileStatesStillMatch(const Array<AssetDatabaseFileState>& states)
    {
        Array<String> files;
        if (FileSystem::DirectoryGetFiles(files, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
            return false;
        Dictionary<String, const AssetDatabaseFileState*> byPath;
        for (const AssetDatabaseFileState& state : states)
            byPath.Add(state.Path.ToLower(), &state);
        int32 matched = 0;
        for (const String& file : files)
        {
            String normalized = file.ToLower();
            normalized.Replace(TEXT('\\'), TEXT('/'));
            if (normalized.Contains(TEXT("/cache/")) || normalized.Contains(TEXT("/output/")) || normalized.Contains(TEXT("/generated/")) ||
                normalized.Contains(TEXT("/migrationbackup/")) || normalized.Contains(TEXT("/.asset-pipeline/")) || normalized.Contains(TEXT("/.git/")))
                continue;
            const AssetDatabaseFileState* const* state = byPath.TryGet(file.ToLower());
            if (!state || !SourceHashCache::IsStateCurrent(**state))
                return false;
            matched++;
        }
        return matched == states.Count();
    }

    void SetDiagnostics(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        ScopeLock lock(StateLocker);
        LastDiagnostics = diagnostics;
    }
}

uint64 AssetDatabaseFacade::GetRevision()
{
    return AssetDatabase::Get().GetRevision();
}

Array<AssetDatabaseRecordInfo> AssetDatabaseFacade::GetRecords()
{
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    Array<AssetDatabaseRecordInfo> result;
    result.EnsureCapacity(snapshot.Records.Count());
    for (const AssetRecord& record : snapshot.Records)
        result.Add(ToInfo(record));
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetDatabaseRecordInfo& a, const AssetDatabaseRecordInfo& b)
        {
            if (a.CanonicalPath != b.CanonicalPath)
                return a.CanonicalPath < b.CanonicalPath;
            return a.SubAssetKey < b.SubAssetKey;
        });
    }
    return result;
}

Array<AssetPipelineDiagnostic> AssetDatabaseFacade::GetDiagnostics()
{
    ScopeLock lock(StateLocker);
    return LastDiagnostics;
}

bool AssetDatabaseFacade::Scan(bool strictMetadata)
{
#if USE_EDITOR
    EnsureBound();
    EnsureExistingJsonSidecars();
    AssetDatabaseScanOptions options;
    options.StrictMetadata = strictMetadata;
    options.HashCache = &HashCache;
    AssetDatabaseScanResult result;
    const bool failed = AssetDatabaseScanner::Scan(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, options, AssetDatabase::Get(), result);
    SetDiagnostics(result.Diagnostics);
    if (!failed)
    {
        const String directory = SnapshotDirectory();
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseSnapshotStore::SaveAtomic(SnapshotPath(), Globals::ProjectFolder, Globals::ProjectContentFolder, AssetDatabase::Get().GetSnapshot(), result.FileStates, diagnostic))
        {
            ScopeLock lock(StateLocker);
            LastDiagnostics.Add(diagnostic);
        }
    }
    return failed;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::LoadOrScan(bool strictMetadata)
{
    EnsureBound();
    Array<AssetDatabaseFileState> states;
    AssetPipelineDiagnostic diagnostic;
    if (!AssetDatabaseSnapshotStore::Load(SnapshotPath(), Globals::ProjectFolder, Globals::ProjectContentFolder, AssetDatabase::Get(), states, diagnostic))
    {
        HashCache.Seed(states);
        if (FileStatesStillMatch(states))
        {
            if (!strictMetadata)
            {
                SetDiagnostics(Array<AssetPipelineDiagnostic>());
                return false;
            }
        }
    }
    return Scan(strictMetadata);
}

bool AssetDatabaseFacade::CleanLibrary()
{
#if USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    const bool failed = ArtifactStore::CleanEntireLibrary(diagnostic);
    Array<AssetPipelineDiagnostic> diagnostics;
    if (diagnostic.Code != AssetPipelineDiagnosticCode::None)
        diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
    return failed;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath)
{
    AssetMeta source;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(sourceMetaPath, source, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    const AssetMeta clone = source.CloneWithNewIdentities();
    if (AssetMeta::SaveAtomic(destinationMetaPath, clone, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    return false;
}

#if COMPILE_WITH_TEXTURE_TOOL
Guid AssetDatabaseFacade::CreateTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Texture metadata already exists.") : TEXT("Texture source does not exist.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
    if (!options.IsAtlas)
    {
        TextureData sourceData;
        if (!TextureTool::ImportTexture(sourcePath, sourceData) && sourceData.GetArraySize() == 6)
            meta.AssetType = CubeTexture::TypeName;
    }
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TextureProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) || AssetMeta::SaveAtomic(metaPath, meta, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    if (Scan(false))
        return Guid::Empty;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (TexturePipelineService::RequestBuild(meta.ID, false, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#endif
    return meta.ID;
}

bool AssetDatabaseFacade::LoadTextureMetadata(const StringView& sourcePath, TextureTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    TextureProcessorSettings settings;
    if (TextureProcessorSettings::Parse(meta.Processor.SettingsJson, meta.Processor.SettingsVersion, settings, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    options = settings.ToImportOptions(StringAnsiView("windows"));
    return false;
}

bool AssetDatabaseFacade::ApplyTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(metaPath, meta, diagnostic))
        goto Failed;
    {
        const TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic) || settings.ToJson(meta.Processor.SettingsJson, diagnostic))
            goto Failed;
        meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
        if (!options.IsAtlas)
        {
            TextureData sourceData;
            if (!TextureTool::ImportTexture(sourcePath, sourceData) && sourceData.GetArraySize() == 6)
                meta.AssetType = CubeTexture::TypeName;
        }
        meta.Processor.ID = TextureProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    }
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || Scan(false))
        goto Failed;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (TexturePipelineService::RequestBuild(meta.ID, false, diagnostic))
        goto Failed;
#endif
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::RebuildTexture(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!TexturePipelineService::RequestBuild(assetID, true, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::BuildTexture(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!TexturePipelineService::RequestBuild(assetID, false, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

String AssetDatabaseFacade::GetTextureBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    switch (TexturePipelineService::GetStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: break;
    }
#endif
    return TEXT("NotBuilt");
}

AssetPipelineDiagnostic AssetDatabaseFacade::GetTextureBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER
    TexturePipelineService::GetStatus(assetID, diagnostic);
#endif
    return diagnostic;
}

Texture* AssetDatabaseFacade::LoadTextureThumbnail(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    if (!ArtifactResolver::Get().IsConfigured())
        return nullptr;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = TexturePipelineService::GetHostTarget();
    request.OutputKind = "thumbnail";
    request.RequiredCompatibility = "flax-texture-thumbnail-v1";
    request.Policy = ArtifactResolvePolicy::NoBuild;
    ResolvedArtifact artifact;
    AssetPipelineDiagnostic diagnostic;
    if (ArtifactResolver::Get().Resolve(request, artifact, diagnostic))
        return nullptr;
    const ArtifactLease lease = ArtifactLease::Acquire(artifact.StoragePath.Get());
    return Texture::FromFile(artifact.StoragePath.Get(), false);
#else
    return nullptr;
#endif
}
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
bool AssetDatabaseFacade::LoadModelMetadata(const StringView& sourcePath, ModelTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
        goto Failed;
    {
        ModelProcessorSettings settings;
        if (meta.Processor.ID != ModelProcessorSettings::ProcessorID())
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.ProcessorId = meta.Processor.ID;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Model metadata is not owned by the Flax.Model processor.");
            goto Failed;
        }
        if (ModelProcessorSettings::Parse(meta.Processor.SettingsJson, meta.Processor.SettingsVersion, settings, diagnostic))
            goto Failed;
        options = settings.Import;
    }
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::ApplyModelMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    if (AssetMeta::Load(metaPath, meta, diagnostic))
        goto Failed;
    {
        const ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic) || settings.ToJson(meta.Processor.SettingsJson, diagnostic))
            goto Failed;
        meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel || options.Type == ModelTool::ModelType::Animation
            ? SkinnedModel::TypeName
            : Model::TypeName;
        meta.Processor.ID = ModelProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    }
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || Scan(false))
        goto Failed;
#if COMPILE_WITH_ASSETS_IMPORTER
    if (ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
        goto Failed;
#endif
    return false;

Failed:
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    return true;
}

bool AssetDatabaseFacade::ReconcileModel(const Guid& rootAssetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<SubAssetReconcileChange> changes;
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::ReconcileMetadata(rootAssetID, changes, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::BuildModel(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::RequestBuild(assetID, false, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

bool AssetDatabaseFacade::RebuildModel(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    if (!ModelPipelineService::RequestBuild(assetID, true, diagnostic))
        return false;
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(diagnostic);
    SetDiagnostics(diagnostics);
#endif
    return true;
}

String AssetDatabaseFacade::GetModelBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    switch (ModelPipelineService::GetStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: break;
    }
#endif
    return TEXT("NotBuilt");
}

AssetPipelineDiagnostic AssetDatabaseFacade::GetModelBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER
    ModelPipelineService::GetStatus(assetID, diagnostic);
    if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
    {
        const Array<AssetPipelineDiagnostic> diagnostics = GetDiagnostics();
        if (diagnostics.HasItems())
            diagnostic = diagnostics[0];
    }
#endif
    return diagnostic;
}
#endif

Guid AssetDatabaseFacade::CreateGraphDocument(const StringView& outputPath, const StringView& typeName, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!GraphDocumentCodec::IsSupportedType(typeName) || outputPath.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Graph document type or path is invalid.");
        return fail();
    }
    GraphDocument document;
    StringAnsi json;
    if (GraphDocumentCodec::CreateStarter(typeName, document, diagnostic))
        return fail();
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic) ||
        GraphDocumentCodec::SaveAtomic(outputPath, json, diagnostic))
        return fail();

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = TEXT("Flax.GraphDocument");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(String(outputPath) + TEXT(".meta"), meta, diagnostic) || Scan(false))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
        return fail();
#endif
    return meta.ID;
}

Guid AssetDatabaseFacade::CreateAuthoredDocument(const StringView& outputPath, const StringView& typeName)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    CreateAssetFunction callback;
    if (typeName == TEXT("FlaxEngine.MaterialInstance"))
        callback.Bind(&CreateMaterialInstance::Create);
    else if (typeName == TEXT("FlaxEngine.SkeletonMask"))
        callback.Bind(&CreateSkeletonMask::Create);
    else if (typeName == TEXT("FlaxEngine.SceneAnimation"))
        callback.Bind(&CreateSceneAnimation::Create);
    else if (typeName == TEXT("FlaxEngine.ParticleSystem"))
        callback.Bind(&CreateParticleSystem::Create);
    else if (typeName == TEXT("FlaxEngine.CollisionData"))
        callback.Bind(&CreateCollisionData::Create);
    if (!callback.IsBinded() || outputPath.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Authored document type or path is invalid.");
        return fail();
    }
    const Guid id = Guid::New();
    const String temporaryFolder = Globals::ProjectLibraryFolder / TEXT("Temp/AuthoredCreates");
    if (FileSystem::CreateDirectory(temporaryFolder))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::LibraryCreationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.Message = TEXT("Cannot create the temporary authored-document folder.");
        return fail();
    }
    const String tempPath = temporaryFolder / id.ToString(Guid::FormatType::N) + TEXT(".flax");
    CreateAssetContext importerContext(StringView::Empty, tempPath, id, nullptr, true, typeName);
    if (importerContext.Run(callback) != CreateAssetResult::Ok)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Authored starter flax could not be created.");
        FileSystem::DeleteFile(tempPath);
        return fail();
    }
    if (LegacyAssetMigrator::ConvertFlax(tempPath, outputPath, id, typeName, diagnostic))
    {
        FileSystem::DeleteFile(tempPath);
        return fail();
    }
    FileSystem::DeleteFile(tempPath);
    if (Scan(false))
        return fail();
    if (GraphPipelineService::RequestBuild(id, true, diagnostic))
        return fail();
    return id;
#else
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.Message = TEXT("Authored documents require the editor importer.");
    return fail();
#endif
}

bool AssetDatabaseFacade::SaveAuthoredDocument(BinaryAsset* asset, const Guid& canonicalAssetID)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    AssetRecord record;
    if (!asset || !canonicalAssetID.IsValid() || !AssetDatabase::Get().TryGetRecord(canonicalAssetID, record) ||
        record.SourceKind != AssetSourceKind::TextDocument ||
        (record.ProcessorID != TEXT("Flax.MaterialInstance") && record.ProcessorID != TEXT("Flax.SkeletonMask") &&
            record.ProcessorID != TEXT("Flax.SceneAnimation") && record.ProcessorID != TEXT("Flax.ParticleSystem")))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("The edited asset is not backed by a canonical authored document.");
        return fail();
    }

    const String temporaryFolder = Globals::ProjectLibraryFolder / TEXT("Temp/AuthoredSaves");
    if (FileSystem::CreateDirectory(temporaryFolder))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::LibraryCreationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("Cannot create the temporary authored-document save folder.");
        return fail();
    }
    const String token = Guid::New().ToString(Guid::FormatType::N);
    const String temporaryFlax = temporaryFolder / token + TEXT(".flax");
    const String stagedDocument = String(record.SourcePath.Get()) + TEXT(".stage-") + token;
    const String stagedMeta = stagedDocument + TEXT(".meta");
    SCOPE_EXIT
    {
        ContentStorageManager::EnsureAccess(temporaryFlax);
        FileSystem::DeleteFile(temporaryFlax);
        FileSystem::DeleteFile(stagedDocument);
        FileSystem::DeleteFile(stagedMeta);
    };

    bool saveFailed = true;
    if (record.TypeName == MaterialInstance::TypeName)
    {
        auto* typed = ScriptingObject::Cast<MaterialInstance>(asset);
        saveFailed = !typed || typed->Save(temporaryFlax);
    }
    else if (record.TypeName == SkeletonMask::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SkeletonMask>(asset);
        saveFailed = !typed || typed->Save(temporaryFlax);
    }
    else if (record.TypeName == SceneAnimation::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SceneAnimation>(asset);
        if (typed)
        {
            const BytesContainer& timeline = typed->LoadTimeline();
            FlaxChunk chunk;
            chunk.Data.Copy(timeline.Get(), timeline.Length());
            AssetInitData data;
            data.Header.ID = canonicalAssetID;
            data.Header.TypeName = SceneAnimation::TypeName;
            data.SerializedVersion = SceneAnimation::SerializedVersion;
            data.Header.Chunks[0] = &chunk;
            saveFailed = FlaxStorage::Create(temporaryFlax, data);
        }
    }
    else if (record.TypeName == ParticleSystem::TypeName)
    {
        auto* typed = ScriptingObject::Cast<ParticleSystem>(asset);
        if (typed)
        {
            const BytesContainer timeline = typed->LoadTimeline();
            FlaxChunk chunk;
            chunk.Data.Copy(timeline.Get(), timeline.Length());
            AssetInitData data;
            data.Header.ID = canonicalAssetID;
            data.Header.TypeName = ParticleSystem::TypeName;
            data.SerializedVersion = ParticleSystem::SerializedVersion;
            data.Header.Chunks[0] = &chunk;
            saveFailed = FlaxStorage::Create(temporaryFlax, data);
        }
    }
    if (saveFailed)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("The edited asset could not be serialized for canonical document conversion.");
        return fail();
    }
    if (LegacyAssetMigrator::ConvertFlax(temporaryFlax, stagedDocument, canonicalAssetID, record.TypeName, diagnostic))
        return fail();
    if (FileSystem::FileExists(record.SourcePath.Get()) && FileSystem::IsReadOnly(record.SourcePath.Get()))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The canonical authored document is read-only.");
        return fail();
    }
    if (FileSystem::MoveFile(record.SourcePath.Get(), stagedDocument, true))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("Cannot atomically replace the canonical authored document.");
        return fail();
    }
    if (Scan(false) || GraphPipelineService::RequestBuild(canonicalAssetID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveCollisionDataDocument(const StringView& path, CollisionDataType type, const Guid& model, int32 modelLodIndex,
    uint32 materialSlotsMask, ConvexMeshGenerationFlags convexFlags, int32 convexVertexLimit)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    CollisionData::SerializedOptions options;
    Platform::MemoryClear(&options, sizeof(options));
    options.Type = type;
    options.Model = model;
    options.ModelLodIndex = modelLodIndex;
    options.MaterialSlotsMask = materialSlotsMask;
    options.ConvexFlags = convexFlags;
    options.ConvexVertexLimit = convexVertexLimit;
    rapidjson_flax::Document json;
    String error;
    if (CollisionDataDocument::DecodeLegacy(options, json, error))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        return fail();
    }
    StringAnsi text;
    CanonicalJsonError jsonError;
    Array<StringAnsi> order;
    order.Add("documentVersion");
    order.Add("type");
    order.Add("collisionType");
    order.Add("sourceModel");
    order.Add("modelLodIndex");
    order.Add("materialSlotsMask");
    order.Add("convexFlags");
    order.Add("convexVertexLimit");
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveAtomic(path, text, diagnostic) || Scan(false))
        return fail();
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.CollisionData"))
        return fail();
    if (GraphPipelineService::RequestBuild(meta.ID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveParticleSystemTimeline(const StringView& path, const BytesContainer& timeline)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    rapidjson_flax::Document json;
    String error;
    if (ParticleSystemDocument::DecodeLegacy(Span<byte>(timeline.Get(), timeline.Length()), json, error))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        return fail();
    }
    StringAnsi text;
    CanonicalJsonError jsonError;
    Array<StringAnsi> order;
    order.Add("documentVersion");
    order.Add("type");
    order.Add("framesPerSecond");
    order.Add("durationFrames");
    order.Add("tracks");
    order.Add("parameterOverrides");
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveAtomic(path, text, diagnostic) || Scan(false))
        return fail();
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.ParticleSystem"))
        return fail();
    return GraphPipelineService::RequestBuild(meta.ID, false, diagnostic) ? fail() : false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::LoadCollisionDataDocument(const StringView& path, CollisionData::SerializedOptions& options)
{
    Array<byte> bytes;
    if (File::ReadAllBytes(path, bytes))
        return true;
    rapidjson_flax::Document json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    String error;
    return json.HasParseError() || CollisionDataDocument::Parse(json, options, error);
}

Guid AssetDatabaseFacade::CreateImportedSourceMetadata(const StringView& sourcePath, const StringView& typeName, const StringView& processorId)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath) || typeName.IsEmpty() || processorId.IsEmpty())
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Imported source or processor identity is invalid, or metadata already exists.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = processorId;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || Scan(false))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }
#endif
    return meta.ID;
}

#if COMPILE_WITH_AUDIO_TOOL && USE_EDITOR
Guid AssetDatabaseFacade::CreateAudioMetadata(const StringView& sourcePath, const AudioTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Audio metadata already exists.") : TEXT("Audio source does not exist.");
        return fail();
    }

    rapidjson_flax::StringBuffer settingsBuffer;
    CompactJsonWriter settingsWriter(settingsBuffer);
    settingsWriter.StartObject();
    AudioTool::Options serializedOptions = options;
    serializedOptions.Serialize(settingsWriter, nullptr);
    settingsWriter.EndObject();

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = TEXT("FlaxEngine.AudioClip");
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TEXT("Flax.Audio");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = StringAnsi(settingsBuffer.GetString(), static_cast<int32>(settingsBuffer.GetSize()));
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || Scan(false))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER
    if (GraphPipelineService::RequestBuild(meta.ID, true, diagnostic))
        return fail();
#endif
    return meta.ID;
}

bool AssetDatabaseFacade::LoadAudioMetadata(const StringView& sourcePath, AudioTool::Options& options)
{
    AssetMeta meta;
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (AssetMeta::Load(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
        return fail();
    if (meta.Processor.ID != TEXT("Flax.Audio") || meta.Processor.SettingsVersion != 1)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = meta.Processor.ID;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Audio metadata is not owned by the supported Flax.Audio processor version.");
        return fail();
    }
    rapidjson_flax::Document settings;
    settings.Parse(meta.Processor.SettingsJson.Get(), meta.Processor.SettingsJson.Length());
    if (settings.HasParseError() || !settings.IsObject())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.ProcessorId = meta.Processor.ID;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Audio processor settings are malformed.");
        return fail();
    }
    options.Deserialize(settings, nullptr);
    return false;
}
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
Guid AssetDatabaseFacade::CreateDefaultModelMetadata(const StringView& sourcePath)
{
    ModelTool::Options probeOptions;
    probeOptions.Type = ModelTool::ModelType::Prefab;
    probeOptions.ImportTypes = ImportDataTypes::Geometry | ImportDataTypes::Skeleton | ImportDataTypes::Animations |
                               ImportDataTypes::Nodes | ImportDataTypes::Materials | ImportDataTypes::Textures;
    ModelData data;
    String error;
    if (ModelTool::ImportData(String(sourcePath), data, probeOptions, error))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = error.IsEmpty() ? TEXT("Model structure parser rejected the source.") : error;
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    }

    ModelTool::Options options;
    options.Type = data.Skeleton.Bones.HasItems() || data.Animations.HasItems()
        ? ModelTool::ModelType::SkinnedModel
        : ModelTool::ModelType::Model;
    return CreateModelMetadata(sourcePath, options);
}

Guid AssetDatabaseFacade::CreateModelMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    const String metaPath = String(sourcePath) + TEXT(".meta");
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
    {
        diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Model metadata already exists.") : TEXT("Model source does not exist.");
        return fail();
    }
    ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
        return fail();
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel || options.Type == ModelTool::ModelType::Animation
        ? SkinnedModel::TypeName
        : Model::TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = ModelProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic))
        return fail();
    const String flaxSibling = String(StringUtils::GetDirectoryName(sourcePath)) / String(StringUtils::GetFileNameWithoutExtension(sourcePath)) + TEXT(".flax");
    if (FileSystem::FileExists(flaxSibling) && LegacyAssetMigrator::SeedModelSubAssets(flaxSibling, meta, diagnostic))
        return fail();
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || Scan(false))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<SubAssetReconcileChange> changes;
    if (ModelPipelineService::ReconcileMetadata(meta.ID, changes, diagnostic))
        return fail();
    if (ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
        return fail();
#endif
    return meta.ID;
}
#endif

BytesContainer AssetDatabaseFacade::LoadGraphSurface(const StringView& path)
{
    BytesContainer result;
    AssetPipelineDiagnostic diagnostic;
    GraphDocumentSession session;
    if (session.Open(path, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return result;
    }
    Array<byte> surface;
    if (GraphDocumentCompiler::CompileDocument(session.Document, surface, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return result;
    }
    result.Copy(ToSpan(surface));
    return result;
}

bool AssetDatabaseFacade::SaveGraphSurface(const StringView& path, const BytesContainer& surface, bool allowOverwriteConflict, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    String typeName;
    const String extension = FileSystem::GetExtension(path);
    if (GraphDocumentCodec::TypeForExtension(extension, typeName))
    {
        AssetMeta meta;
        if (!AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic))
            typeName = meta.AssetType;
    }
    GraphDocument document;
    if (GraphDocumentCodec::FromSurface(typeName, surface, document, diagnostic))
        return fail();
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    else if (FileSystem::FileExists(path))
    {
        GraphDocumentSession session;
        AssetPipelineDiagnostic ignored;
        if (!session.Open(path, ignored))
            document.PropertiesJson = session.Document.PropertiesJson;
    }
    StringAnsi json;
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic))
        return fail();
    ContentHash previous;
    if (!allowOverwriteConflict && FileSystem::FileExists(path))
    {
        Array<byte> existing;
        if (!File::ReadAllBytes(path, existing))
            previous = ContentHash::Compute(existing.Get(), existing.Count());
    }
    if (GraphDocumentCodec::SaveAtomic(path, json, diagnostic, previous.IsZero() ? nullptr : &previous) || Scan(false))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetMeta meta;
    if (!AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) && GraphPipelineService::RequestBuild(meta.ID, false, diagnostic))
        return fail();
#endif
    return false;
}

bool AssetDatabaseFacade::RebuildGraph(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(assetID, true, diagnostic))
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    }
    return false;
#else
    return true;
#endif
}

String AssetDatabaseFacade::GetMigrationInventoryJson()
{
    LoadOrScan(false);
    Array<MigrationInventoryEntry> entries;
    MigrationInventory::Build(AssetDatabase::Get().GetSnapshot().Records, entries);
    StringAnsi json;
    AssetPipelineDiagnostic diagnostic;
    if (MigrationInventory::WriteCanonicalJson(entries, json, diagnostic))
        return String::Empty;
    return String(json);
}

bool AssetDatabaseFacade::MigrateLegacyAsset(const StringView& sourcePath)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return true;
    };
    if (sourcePath.IsEmpty() || LoadOrScan(false))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("Legacy migration requires a source path and database snapshot.");
        return fail();
    }

    AssetRecord record;
    bool found = false;
    for (const AssetRecord& candidate : AssetDatabase::Get().GetSnapshot().Records)
    {
        if (candidate.IsMainAsset() && FileSystem::AreFilePathsEqual(candidate.SourcePath.Get(), sourcePath))
        {
            record = candidate;
            found = true;
            break;
        }
    }
    if (!found)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("The requested root asset is not registered.");
        return fail();
    }
    const Guid assetID = record.ID;
    String reason, destination;
    if (MigrationInventory::Classify(record, reason, destination) != MigrationEligibility::ReadyToMigrate)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = reason;
        return fail();
    }
    const String destinationMeta = destination + TEXT(".meta");
    if (FileSystem::FileExists(destination) || FileSystem::FileExists(destinationMeta))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The canonical migration destination already exists.");
        return fail();
    }
    if (FileSystem::IsReadOnly(record.SourcePath.Get()))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The legacy asset is read-only.");
        return fail();
    }

    const String token = Guid::New().ToString(Guid::FormatType::N);
    const String stagedDocument = destination + TEXT(".migration-") + token;
    const String stagedMeta = stagedDocument + TEXT(".meta");
    SCOPE_EXIT
    {
        FileSystem::DeleteFile(stagedDocument);
        FileSystem::DeleteFile(stagedMeta);
    };
    if (LegacyAssetMigrator::ConvertFlax(record.SourcePath.Get(), stagedDocument, assetID, record.TypeName, diagnostic))
        return fail();
    if (FileSystem::MoveFile(destination, stagedDocument, false))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The canonical migration document could not be committed.");
        return fail();
    }
    if (FileSystem::MoveFile(destinationMeta, stagedMeta, false))
    {
        FileSystem::MoveFile(stagedDocument, destination, false);
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destinationMeta;
        diagnostic.Message = TEXT("The canonical migration sidecar could not be committed.");
        return fail();
    }

    ContentStorageManager::EnsureAccess(record.SourcePath.Get());
    if (FileSystem::DeleteFile(record.SourcePath.Get()))
    {
        FileSystem::DeleteFile(destinationMeta);
        FileSystem::DeleteFile(destination);
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The legacy binary could not be removed; canonical outputs were rolled back.");
        return fail();
    }
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
    return false;
}

Guid AssetDatabaseFacade::CreateExistingJsonMetadata(const StringView& sourcePath)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    Guid id;
    String typeName;
    if (!JsonStorageProxy::GetAssetInfo(sourcePath, id, typeName) || !id.IsValid() || typeName.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = sourcePath;
        diagnostic.Message = TEXT("JSON asset is missing a valid ID and TypeName header.");
        return fail();
    }
    AssetMeta meta;
    meta.ID = id;
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::ExistingJson;
    meta.Processor.ID = TEXT("Flax.ExistingJson");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(String(sourcePath) + TEXT(".meta"), meta, diagnostic))
        return fail();
    return meta.ID;
}

bool AssetDatabaseFacade::EnsureExistingJsonSidecars()
{
#if USE_EDITOR
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
        return true;
    bool failed = false;
    for (const String& path : files)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        if (extension != TEXT("scene") && extension != TEXT("prefab") && extension != TEXT("json"))
            continue;
        const String metaPath = path + TEXT(".meta");
        if (FileSystem::FileExists(metaPath))
            continue;
        if (!CreateExistingJsonMetadata(path).IsValid())
            failed = true;
    }
    return failed;
#else
    return true;
#endif
}
