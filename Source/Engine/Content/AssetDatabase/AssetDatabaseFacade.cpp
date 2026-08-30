// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseFacade.h"
#include "AssetDatabaseSnapshot.h"
#include "AssetSourceRoots.h"
#include "AssetMeta.h"
#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Assets/Material.h"
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
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Core/Collections/HashSet.h"
#if COMPILE_WITH_MATERIAL_GRAPH
#include "Engine/Tools/MaterialGenerator/Types.h"
#endif
#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Graphics/PixelFormatExtensions.h"
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
#include "Engine/Content/Build/Processors/ModelProcessor.h"
#include "Engine/Content/Build/Processors/ModelSubAssetKeys.h"
#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#endif
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "Engine/Content/Build/Processors/GraphDocumentProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
#include "Engine/Content/Build/Processors/ImportedSourceProcessor.h"
#include "Engine/ContentImporters/CreateMaterialInstance.h"
#include "Engine/ContentImporters/CreateSkeletonMask.h"
#include "Engine/ContentImporters/CreateSceneAnimation.h"
#include "Engine/ContentImporters/CreateParticleSystem.h"
#include "Engine/ContentImporters/CreateCollisionData.h"
#include "Engine/ContentImporters/Types.h"
#endif
#include <algorithm>
#include <future>
#include <vector>

Delegate<uint64> AssetDatabaseFacade::DatabaseChanged;
Delegate<Guid> AssetDatabaseFacade::ArtifactPublished;

void AssetDatabaseFacade::NotifyArtifactPublished(const Guid& assetID)
{
    ArtifactPublished(assetID);
}

namespace
{
    CriticalSection StateLocker;
    Array<AssetPipelineDiagnostic> LastDiagnostics;
    AssetDatabaseChangeInfo LastChange;
    Array<AssetDatabaseFileState> LastFileStates;
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
        {
            ScopeLock lock(StateLocker);
            LastChange = AssetDatabaseChangeInfo();
            LastChange.Revision = change.Revision;
            LastChange.Added = change.Added;
            LastChange.Removed = change.Removed;
            LastChange.Changed = change.Changed;
            LastChange.StatusChanged = change.StatusChanged;
        }
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
        Dictionary<String, const AssetDatabaseFileState*> byPath;
        for (const AssetDatabaseFileState& state : states)
            byPath.Add(state.Path.ToLower(), &state);
        int32 matched = 0;
        const auto checkRoot = [&byPath, &matched](const StringView& root)
        {
            if (!FileSystem::DirectoryExists(root))
                return false;
            Array<String> files;
            if (FileSystem::DirectoryGetFiles(files, String(root), TEXT("*"), DirectorySearchOption::AllDirectories))
                return false;
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
            return true;
        };
        if (!checkRoot(Globals::ProjectContentFolder))
            return false;
        const String engineRoot = AssetSourceRoots::GetEngineRoot();
        if (FileSystem::DirectoryExists(engineRoot) && !checkRoot(engineRoot))
            return false;
        return matched == states.Count();
    }

    void SetDiagnostics(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        ScopeLock lock(StateLocker);
        LastDiagnostics = diagnostics;
    }

    String NormalizeAbsolutePath(const StringView& path)
    {
        String result(path);
        if (result.IsEmpty())
            return result;
        FileSystem::NormalizePath(result);
        return result;
    }

    String PathKey(const StringView& path)
    {
        String result = NormalizeAbsolutePath(path);
        result.Replace(TEXT('\\'), TEXT('/'));
        return result.ToLower();
    }

    bool IsMetaPath(const StringView& path)
    {
        return path.EndsWith(TEXT(".meta"), StringSearchCase::IgnoreCase);
    }

    void AddUniquePath(const StringView& path, HashSet<String>& keys, Array<String>& expanded)
    {
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            return;
        const String key = PathKey(normalized);
        if (!keys.Add(key))
            return;
        expanded.Add(normalized);
    }

    void ExpandRefreshPath(const StringView& path, HashSet<String>& keys, Array<String>& expanded)
    {
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            return;
        if (FileSystem::DirectoryExists(normalized))
        {
            Array<String> nested;
            if (!FileSystem::DirectoryGetFiles(nested, normalized, TEXT("*"), DirectorySearchOption::AllDirectories))
            {
                for (const String& nestedPath : nested)
                    AddUniquePath(nestedPath, keys, expanded);
            }
            return;
        }
        AddUniquePath(normalized, keys, expanded);
        if (IsMetaPath(normalized))
            AddUniquePath(normalized.Left(normalized.Length() - 5), keys, expanded);
        else
            AddUniquePath(normalized + TEXT(".meta"), keys, expanded);
    }

    bool RecordPathAffected(const AssetRecord& record, const HashSet<String>& keys)
    {
        return keys.Contains(PathKey(record.SourcePath.Get())) ||
            (!record.MetaPath.Get().IsEmpty() && keys.Contains(PathKey(record.MetaPath.Get())));
    }

    bool PathIsUnder(const StringView& path, const StringView& root)
    {
        if (path.IsEmpty() || root.IsEmpty())
            return false;
        const String pathKey = PathKey(path);
        const String rootKey = PathKey(root);
        if (pathKey == rootKey)
            return true;
        return pathKey.Length() > rootKey.Length() && pathKey.StartsWith(rootKey) && pathKey[rootKey.Length()] == '/';
    }

    // Both arguments must already be PathKey-normalized.
    bool IsKeyUnderAnyRoot(const String& pathKey, const Array<String>& rootKeys)
    {
        for (const String& rootKey : rootKeys)
        {
            if (pathKey == rootKey)
                return true;
            if (pathKey.Length() > rootKey.Length() && pathKey.StartsWith(rootKey) && pathKey[rootKey.Length()] == '/')
                return true;
        }
        return false;
    }

    // A scoped refresh only knows about the paths it was given, so diagnostics for every other
    // source must survive. Unattributable entries are dropped so they cannot accumulate forever.
    void MergeScopedDiagnostics(const HashSet<String>& affectedKeys, const Array<AssetPipelineDiagnostic>& fresh)
    {
        Array<AssetPipelineDiagnostic> merged;
        {
            ScopeLock lock(StateLocker);
            merged.EnsureCapacity(LastDiagnostics.Count() + fresh.Count());
            for (const AssetPipelineDiagnostic& diagnostic : LastDiagnostics)
            {
                if (diagnostic.SourcePath.IsEmpty() || affectedKeys.Contains(PathKey(diagnostic.SourcePath)))
                    continue;
                merged.Add(diagnostic);
            }
        }
        merged.Add(fresh);
        SetDiagnostics(merged);
    }

    // Sources that arrive without a sidecar are invisible to the database until one exists. A full
    // Scan handles this through EnsureExistingJsonSidecars, so a scoped refresh must do the same.
    void EnsureScopedJsonSidecars(HashSet<String>& keys, Array<String>& expanded)
    {
        const int32 count = expanded.Count();
        for (int32 i = 0; i < count; i++)
        {
            const String path = expanded[i];
            const String extension = FileSystem::GetExtension(path).ToLower();
            if (extension != TEXT("scene") && extension != TEXT("prefab") && extension != TEXT("json"))
                continue;
            const String metaPath = path + TEXT(".meta");
            if (!FileSystem::FileExists(path) || FileSystem::FileExists(metaPath))
                continue;
            if (AssetDatabaseFacade::CreateExistingJsonMetadata(path).IsValid())
                AddUniquePath(metaPath, keys, expanded);
        }
    }

    bool SnapshotIdentityChanged(const AssetDatabaseSnapshot& previous, const Array<AssetRecord>& merged)
    {
        if (previous.Records.Count() != merged.Count())
            return true;
        Dictionary<Guid, const AssetRecord*> previousById;
        for (const AssetRecord& record : previous.Records)
            previousById.Add(record.ID, &record);
        for (const AssetRecord& record : merged)
        {
            const AssetRecord* const* previousRecord = previousById.TryGet(record.ID);
            if (!previousRecord || !(*previousRecord)->HasSameIdentityAndContent(record) || (*previousRecord)->Status != record.Status)
                return true;
        }
        return false;
    }

    bool PersistSnapshot()
    {
        const String directory = SnapshotDirectory();
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseSnapshotStore::SaveAtomic(SnapshotPath(), Globals::ProjectFolder, Globals::ProjectContentFolder,
            AssetDatabase::Get().GetSnapshot(), LastFileStates, diagnostic))
        {
            ScopeLock lock(StateLocker);
            LastDiagnostics.Add(diagnostic);
            return true;
        }
        return false;
    }

    bool RefreshPath(const StringView& path)
    {
        Array<String> paths;
        paths.Add(String(path));
        return AssetDatabaseFacade::RefreshSources(paths);
    }

    bool RefreshPath(const String& path)
    {
        return RefreshPath(StringView(path));
    }

    // Migration is driven one asset at a time, and every conversion invalidates the persisted file
    // states, so revalidating the whole snapshot per call is quadratic. Load once, then keep the
    // database current through scoped refreshes.
    bool EnsureDatabaseLoaded()
    {
        if (AssetDatabase::Get().GetRevision() != 0)
            return false;
        return AssetDatabaseFacade::LoadOrScan(false);
    }

#if USE_EDITOR
    enum class CanonicalBatchBuildKind : byte
    {
        None,
        Texture,
        Model,
        Imported,
    };

    struct CanonicalBatchWork
    {
        String SourcePath;
        String StagingPath;
        AssetMeta Meta;
        AssetPipelineDiagnostic Diagnostic;
        CanonicalBatchBuildKind BuildKind = CanonicalBatchBuildKind::None;
        bool Failed = false;
    };

    bool FailCanonicalBatchWork(CanonicalBatchWork& work, AssetPipelineDiagnosticCode code, const StringView& message)
    {
        work.Diagnostic = AssetPipelineDiagnostic();
        work.Diagnostic.Code = code;
        work.Diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        work.Diagnostic.SourcePath = work.SourcePath;
        work.Diagnostic.Message = message;
        return true;
    }

    bool PrepareDefaultCanonicalMetadata(CanonicalBatchWork& work)
    {
        if (!FileSystem::FileExists(work.SourcePath))
            return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::SourceMissing, TEXT("Canonical source does not exist."));
        if (FileSystem::FileExists(work.StagingPath))
            return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::PathCollision, TEXT("Canonical metadata staging path already exists."));

        const String extension = FileSystem::GetExtension(work.SourcePath).ToLower();
        AssetMeta& meta = work.Meta;
        meta.ID = Guid::New();
        meta.SourceKind = AssetSourceKind::ImportedSource;

#if COMPILE_WITH_TEXTURE_TOOL
        const bool isTexture = extension == TEXT("png") || extension == TEXT("tga") || extension == TEXT("exr") ||
            extension == TEXT("bmp") || extension == TEXT("gif") || extension == TEXT("tiff") || extension == TEXT("tif") ||
            extension == TEXT("jpeg") || extension == TEXT("jpg") || extension == TEXT("dds") || extension == TEXT("hdr") ||
            extension == TEXT("raw");
        if (isTexture)
        {
            TextureTool::Options options;
            TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
            if (settings.Validate(work.Diagnostic))
                return true;
            meta.AssetType = options.IsAtlas ? SpriteAtlas::TypeName : Texture::TypeName;
            if (!options.IsAtlas)
            {
                TextureData sourceData;
                if (!TextureTool::ImportTexture(work.SourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
                    meta.AssetType = CubeTexture::TypeName;
            }
            meta.Processor.ID = TextureProcessorSettings::ProcessorID();
            meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
            work.BuildKind = CanonicalBatchBuildKind::Texture;
            return settings.ToJson(meta.Processor.SettingsJson, work.Diagnostic);
        }
#endif

#if COMPILE_WITH_MODEL_TOOL
        const bool isModel = extension == TEXT("obj") || extension == TEXT("fbx") || extension == TEXT("x") ||
            extension == TEXT("dae") || extension == TEXT("gltf") || extension == TEXT("glb") || extension == TEXT("blend") ||
            extension == TEXT("bvh") || extension == TEXT("ase") || extension == TEXT("ply") || extension == TEXT("dxf") ||
            extension == TEXT("ifc") || extension == TEXT("nff") || extension == TEXT("smd") || extension == TEXT("vta") ||
            extension == TEXT("mdl") || extension == TEXT("md2") || extension == TEXT("md3") || extension == TEXT("md5mesh") ||
            extension == TEXT("q3o") || extension == TEXT("q3s") || extension == TEXT("ac") || extension == TEXT("stl") ||
            extension == TEXT("lwo") || extension == TEXT("lws") || extension == TEXT("lxo");
        if (isModel)
        {
            ModelTool::Options options;
            ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
            if (settings.Validate(work.Diagnostic))
                return true;
            ModelSourceAnalysis analysis;
            if (ModelProcessor::AnalyzeSource(work.SourcePath, settings, analysis, work.Diagnostic))
                return true;
            options.Type = analysis.SourceSkeletonBoneCount > 0 || analysis.SourceAnimationCount > 0
                ? ModelTool::ModelType::SkinnedModel
                : ModelTool::ModelType::Model;
            settings = ModelProcessorSettings::FromLegacyOptions(options);
            meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel ? SkinnedModel::TypeName : Model::TypeName;
            meta.Processor.ID = ModelProcessorSettings::ProcessorID();
            meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
            if (settings.ToJson(meta.Processor.SettingsJson, work.Diagnostic))
                return true;
            const String flaxSibling = String(StringUtils::GetDirectoryName(work.SourcePath)) /
                String(StringUtils::GetFileNameWithoutExtension(work.SourcePath)) + TEXT(".flax");
            if (FileSystem::FileExists(flaxSibling) && LegacyAssetMigrator::SeedModelSubAssets(flaxSibling, meta, work.Diagnostic))
                return true;
            SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, analysis.Candidates, true);
            if (reconciliation.RequiresUserReconciliation)
            {
                work.Diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : work.Diagnostic;
                work.Diagnostic.AssetGuid = meta.ID;
                work.Diagnostic.SourcePath = work.SourcePath;
                return true;
            }
            meta.SubAssets = MoveTemp(reconciliation.Resolved);
            work.BuildKind = CanonicalBatchBuildKind::Model;
            return false;
        }
#endif

#if COMPILE_WITH_AUDIO_TOOL
        if (extension == TEXT("wav") || extension == TEXT("mp3") || extension == TEXT("ogg"))
        {
            rapidjson_flax::StringBuffer settingsBuffer;
            CompactJsonWriter settingsWriter(settingsBuffer);
            settingsWriter.StartObject();
            AudioTool::Options options;
            options.Serialize(settingsWriter, nullptr);
            settingsWriter.EndObject();
            meta.AssetType = TEXT("FlaxEngine.AudioClip");
            meta.Processor.ID = TEXT("Flax.Audio");
            meta.Processor.SettingsVersion = 1;
            meta.Processor.SettingsJson = StringAnsi(settingsBuffer.GetString(), static_cast<int32>(settingsBuffer.GetSize()));
            work.BuildKind = CanonicalBatchBuildKind::Imported;
            return false;
        }
#endif

        if (extension == TEXT("ttf") || extension == TEXT("otf"))
        {
            meta.AssetType = TEXT("FlaxEngine.FontAsset");
            meta.Processor.ID = TEXT("Flax.Font");
        }
        else if (extension == TEXT("shader"))
        {
            meta.AssetType = TEXT("FlaxEngine.Shader");
            meta.Processor.ID = TEXT("Flax.ShaderSource");
        }
        else if (extension == TEXT("mp4") || extension == TEXT("webm") || extension == TEXT("mov") || extension == TEXT("mkv"))
        {
            meta.AssetType = TEXT("FlaxEngine.Video");
            meta.Processor.ID = TEXT("Flax.Video");
        }
        else
        {
            return FailCanonicalBatchWork(work, AssetPipelineDiagnosticCode::ProcessorMissing, TEXT("No default canonical processor supports this source extension."));
        }
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        work.BuildKind = CanonicalBatchBuildKind::Imported;
        return false;
    }
#endif

    bool StageImportedFiles(const StringView& legacyPath, const StringView& extractedPath, const StringView& destinationPath,
        const StringView& backupPath, const AssetMeta& meta, AssetPipelineDiagnostic& diagnostic)
    {
        const String destinationMeta = String(destinationPath) + TEXT(".meta");
        if (!FileSystem::FileExists(legacyPath) || !FileSystem::FileExists(extractedPath) ||
            FileSystem::FileExists(destinationPath) || FileSystem::FileExists(destinationMeta) || FileSystem::FileExists(backupPath))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Imported migration paths are missing or collide with existing files.");
            return true;
        }
        const String destinationFolder(StringUtils::GetDirectoryName(destinationPath));
        const String backupFolder(StringUtils::GetDirectoryName(backupPath));
        if ((!FileSystem::DirectoryExists(destinationFolder) && FileSystem::CreateDirectory(destinationFolder)) ||
            (!FileSystem::DirectoryExists(backupFolder) && FileSystem::CreateDirectory(backupFolder)) ||
            FileSystem::CopyFile(destinationPath, extractedPath) || AssetMeta::SaveAtomic(destinationMeta, meta, diagnostic))
        {
            FileSystem::DeleteFile(destinationMeta);
            FileSystem::DeleteFile(destinationPath);
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
                diagnostic.SourcePath = destinationPath;
                diagnostic.Message = TEXT("Canonical imported source staging failed.");
            }
            return true;
        }
        ContentStorageManager::EnsureAccess(legacyPath);
        if (FileSystem::MoveFile(backupPath, legacyPath, false))
        {
            FileSystem::DeleteFile(destinationMeta);
            FileSystem::DeleteFile(destinationPath);
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceBusy;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Legacy imported asset could not be moved into reversible staging.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
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

AssetDatabaseChangeInfo AssetDatabaseFacade::GetLastChange()
{
    ScopeLock lock(StateLocker);
    return LastChange;
}

String AssetDatabaseFacade::GetCanonicalSourcePath(const Guid& assetID)
{
    AssetRecord record;
    return assetID.IsValid() && AssetDatabase::Get().TryGetRecord(assetID, record)
        ? String(record.SourcePath.Get())
        : String::Empty;
}

Asset* AssetDatabaseFacade::LoadAssetPreview(const Guid& assetID)
{
#if USE_EDITOR
    return Content::LoadAsyncPreview(assetID, Asset::TypeInitializer);
#else
    return nullptr;
#endif
}

Guid AssetDatabaseFacade::GetPublishedArtifactCacheID(const Guid& assetID, const StringView& outputKind)
{
#if USE_EDITOR
    ArtifactResolver& resolver = ArtifactResolver::Get();
    if (!resolver.IsConfigured() || !assetID.IsValid() || outputKind.IsEmpty())
        return Guid::Empty;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = resolver.GetDefaultTarget();
    request.OutputKind = StringAnsi(outputKind);
    request.Policy = ArtifactResolvePolicy::PublishedOnly;
    ResolvedArtifact artifact;
    AssetPipelineDiagnostic diagnostic;
    ArtifactKey key;
    if (resolver.Resolve(request, artifact, diagnostic) || ArtifactKey::Parse(artifact.Key, key))
        return Guid::Empty;
    return Guid(key.Digest.Values[0], key.Digest.Values[1], key.Digest.Values[2], key.Digest.Values[3]);
#else
    return Guid::Empty;
#endif
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
    Array<AssetRecord> records;
    const AssetDatabaseSnapshot previous = AssetDatabase::Get().GetSnapshot();
    bool failed = AssetDatabaseScanner::Collect(Globals::ProjectFolder, Globals::ProjectContentFolder, Globals::ProjectLibraryFolder, options, previous, records, result);
    const String engineRoot = AssetSourceRoots::GetEngineRoot();
    if (!failed && FileSystem::DirectoryExists(engineRoot))
    {
        AssetDatabaseScanResult engineResult;
        Array<AssetRecord> engineRecords;
        failed = AssetDatabaseScanner::Collect(Globals::StartupFolder, engineRoot, Globals::ProjectLibraryFolder, options, previous, engineRecords, engineResult);
        for (AssetRecord& record : engineRecords)
        {
            record.PortabilityKey = String(TEXT("engine/")) + record.PortabilityKey;
            records.Add(MoveTemp(record));
        }
        result.FilesExamined += engineResult.FilesExamined;
        result.SidecarsParsed += engineResult.SidecarsParsed;
        result.Cancelled |= engineResult.Cancelled;
        result.Diagnostics.Add(engineResult.Diagnostics);
        result.FileStates.Add(engineResult.FileStates);
    }
    if (!failed)
    {
        AssetPipelineDiagnostic publishDiagnostic;
        failed = AssetDatabase::Get().PublishFullSnapshot(records, publishDiagnostic);
        if (failed)
            result.Diagnostics.Add(MoveTemp(publishDiagnostic));
        else
            result.Revision = AssetDatabase::Get().GetRevision();
    }
    SetDiagnostics(result.Diagnostics);
    if (!failed)
    {
        LastFileStates = result.FileStates;
        const String directory = SnapshotDirectory();
        if (!FileSystem::DirectoryExists(directory))
            FileSystem::CreateDirectory(directory);
        AssetPipelineDiagnostic diagnostic;
        if (AssetDatabaseSnapshotStore::SaveAtomic(SnapshotPath(), Globals::ProjectFolder, Globals::ProjectContentFolder, AssetDatabase::Get().GetSnapshot(), LastFileStates, diagnostic))
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
        LastFileStates = states;
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

bool AssetDatabaseFacade::RefreshSources(const Array<String>& paths)
{
#if USE_EDITOR
    EnsureBound();
    if (paths.IsEmpty())
        return false;

    const AssetDatabaseSnapshot previous = AssetDatabase::Get().GetSnapshot();
    HashSet<String> affectedKeys;
    Array<String> expanded;
    Array<String> refreshedRootKeys;
    for (const String& path : paths)
    {
        ExpandRefreshPath(path, affectedKeys, expanded);
        const String normalized = NormalizeAbsolutePath(path);
        if (normalized.IsEmpty())
            continue;
        refreshedRootKeys.Add(PathKey(normalized));
        if (FileSystem::FileExists(normalized) && !FileSystem::DirectoryExists(normalized))
            continue;
        for (const AssetRecord& record : previous.Records)
        {
            if (!PathIsUnder(record.SourcePath.Get(), normalized) &&
                (record.MetaPath.Get().IsEmpty() || !PathIsUnder(record.MetaPath.Get(), normalized)))
                continue;
            AddUniquePath(record.SourcePath.Get(), affectedKeys, expanded);
            if (!record.MetaPath.Get().IsEmpty())
                AddUniquePath(record.MetaPath.Get(), affectedKeys, expanded);
        }
    }

    // A conflict status is a statement about the whole database, so a record can never stop
    // reporting one unless it is re-collected. Conflicts are normally absent, which makes pulling
    // every one of them into each refresh cheap.
    for (const AssetRecord& record : previous.Records)
    {
        if (record.Status != AssetRecordStatus::PathCollision && record.Status != AssetRecordStatus::DuplicateGuid)
            continue;
        AddUniquePath(record.SourcePath.Get(), affectedKeys, expanded);
        if (!record.MetaPath.Get().IsEmpty())
            AddUniquePath(record.MetaPath.Get(), affectedKeys, expanded);
    }

    EnsureScopedJsonSidecars(affectedKeys, expanded);

    Array<String> projectFiles;
    Array<String> engineFiles;
    const String engineRoot = AssetSourceRoots::GetEngineRoot();
    for (const String& path : expanded)
    {
        if (!FileSystem::FileExists(path))
            continue;
        if (engineRoot.HasChars() && AssetPathPolicy::IsSameOrChild(path, engineRoot))
            engineFiles.Add(path);
        else
            projectFiles.Add(path);
    }
    AssetDatabaseScanOptions options;
    options.HashCache = &HashCache;
    // Matches a full editor Scan, so a source that shows up without a sidecar still reports
    // MissingMeta and can be picked up by the metadata registration queue.
    options.StrictMetadata = true;
    AssetDatabaseScanResult result;
    Array<AssetRecord> collected;
    if (projectFiles.Count() && AssetDatabaseScanner::CollectFromFiles(Globals::ProjectFolder, Globals::ProjectContentFolder,
        Globals::ProjectLibraryFolder, projectFiles, options, previous, collected, result))
        return true;
    if (engineFiles.Count())
    {
        AssetDatabaseScanResult engineResult;
        Array<AssetRecord> engineRecords;
        if (AssetDatabaseScanner::CollectFromFiles(Globals::StartupFolder, engineRoot, Globals::ProjectLibraryFolder,
            engineFiles, options, previous, engineRecords, engineResult))
            return true;
        for (AssetRecord& record : engineRecords)
        {
            record.PortabilityKey = String(TEXT("engine/")) + record.PortabilityKey;
            collected.Add(MoveTemp(record));
        }
        result.FilesExamined += engineResult.FilesExamined;
        result.SidecarsParsed += engineResult.SidecarsParsed;
        result.Diagnostics.Add(engineResult.Diagnostics);
        result.FileStates.Add(engineResult.FileStates);
    }

    Array<AssetRecord> merged;
    merged.EnsureCapacity(previous.Records.Count() + collected.Count());
    for (const AssetRecord& record : previous.Records)
    {
        if (!RecordPathAffected(record, affectedKeys))
            merged.Add(record);
    }
    merged.Add(collected);

    const bool identityChanged = SnapshotIdentityChanged(previous, merged);
    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(result.Diagnostics);
    bool publishFailed = false;
    if (identityChanged)
    {
        AssetPipelineDiagnostic publishDiagnostic;
        if (AssetDatabase::Get().PublishFullSnapshot(merged, publishDiagnostic))
        {
            diagnostics.Add(MoveTemp(publishDiagnostic));
            publishFailed = true;
        }
    }
    MergeScopedDiagnostics(affectedKeys, diagnostics);
    if (publishFailed)
        return true;

    Array<AssetDatabaseFileState> nextStates;
    nextStates.EnsureCapacity(LastFileStates.Count() + result.FileStates.Count());
    for (const AssetDatabaseFileState& state : LastFileStates)
    {
        const String key = PathKey(state.Path);
        if (affectedKeys.Contains(key))
            continue;
        // Files that vanished under a refreshed root, such as a deleted directory, would otherwise
        // be carried forever and keep invalidating the persisted snapshot. Existence is probed only
        // for those, so an ordinary single-file refresh stays free of extra syscalls here.
        if (IsKeyUnderAnyRoot(key, refreshedRootKeys) && !FileSystem::FileExists(state.Path))
            continue;
        nextStates.Add(state);
    }
    nextStates.Add(result.FileStates);
    LastFileStates = MoveTemp(nextStates);
    PersistSnapshot();
    return false;
#else
    return true;
#endif
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

#if USE_EDITOR
Array<Guid> AssetDatabaseFacade::StageDefaultCanonicalMetadataBatch(const Array<String>& sourcePaths, const Array<String>& stagingPaths)
{
    Array<Guid> result;
    result.Resize(sourcePaths.Count());
    Array<AssetPipelineDiagnostic> diagnostics;
    if (sourcePaths.Count() != stagingPaths.Count())
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.Message = TEXT("Canonical metadata batch source and staging path counts do not match.");
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return result;
    }

    Array<CanonicalBatchWork> work;
    work.Resize(sourcePaths.Count());
    for (int32 i = 0; i < work.Count(); i++)
    {
        work[i].SourcePath = sourcePaths[i];
        work[i].StagingPath = stagingPaths[i];
    }

    constexpr int32 MaxPreparationConcurrency = 4;
    for (int32 begin = 0; begin < work.Count(); begin += MaxPreparationConcurrency)
    {
        const int32 end = Math::Min(begin + MaxPreparationConcurrency, work.Count());
        std::vector<std::future<void>> tasks;
        tasks.reserve(end - begin);
        for (int32 i = begin; i < end; i++)
        {
            tasks.emplace_back(std::async(std::launch::async, [&work, i]
            {
                work[i].Failed = PrepareDefaultCanonicalMetadata(work[i]);
                if (work[i].Failed)
                {
                    if (work[i].Diagnostic.SourcePath.IsEmpty())
                        work[i].Diagnostic.SourcePath = work[i].SourcePath;
                    if (work[i].Diagnostic.AssetGuid == Guid::Empty)
                        work[i].Diagnostic.AssetGuid = work[i].Meta.ID;
                }
            }));
        }
        for (std::future<void>& task : tasks)
            task.get();
    }

    for (int32 i = 0; i < work.Count(); i++)
    {
        CanonicalBatchWork& item = work[i];
        if (!item.Failed && AssetMeta::SaveAtomic(item.StagingPath, item.Meta, item.Diagnostic))
            item.Failed = true;
        if (item.Failed)
        {
            diagnostics.Add(item.Diagnostic);
            continue;
        }
        result[i] = item.Meta.ID;
    }
    SetDiagnostics(diagnostics);
    return result;
}

bool AssetDatabaseFacade::PublishDefaultCanonicalMetadataBatch(const Array<Guid>& assetIDs, const Array<String>& sourcePaths)
{
    if (sourcePaths.Count() ? RefreshSources(sourcePaths) : Scan(false))
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<AssetPipelineDiagnostic> diagnostics;
    bool failed = false;
    for (const Guid& assetID : assetIDs)
    {
        AssetRecord record;
        AssetPipelineDiagnostic diagnostic;
        if (!assetID.IsValid() || !AssetDatabase::Get().TryGetRecord(assetID, record))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.AssetGuid = assetID;
            diagnostic.Message = TEXT("Staged canonical metadata was not published into the asset database.");
            diagnostics.Add(MoveTemp(diagnostic));
            failed = true;
            continue;
        }

#if COMPILE_WITH_TEXTURE_TOOL
        if (record.ProcessorID == TextureProcessorSettings::ProcessorID())
            failed = TexturePipelineService::RequestBuild(assetID, false, diagnostic);
        else
#endif
#if COMPILE_WITH_MODEL_TOOL
        if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
            failed = ModelPipelineService::RequestBuild(assetID, false, diagnostic);
        else
#endif
        if (ImportedSourceProcessor::Owns(record.ProcessorID) || record.ProcessorID == GraphDocumentProcessor::ProcessorID())
            failed = GraphPipelineService::RequestBuild(assetID, true, diagnostic);
        else
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = assetID;
            diagnostic.SourcePath = record.SourcePath.Get();
            diagnostic.ProcessorId = record.ProcessorID;
            diagnostic.Message = TEXT("Published canonical metadata has no supported build pipeline.");
            failed = true;
        }
        if (failed)
            diagnostics.Add(MoveTemp(diagnostic));
    }
    if (diagnostics.HasItems())
        SetDiagnostics(diagnostics);
    return diagnostics.HasItems();
#else
    return false;
#endif
}
#endif

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
        if (!TextureTool::ImportTexture(sourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
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

    if (RefreshPath(sourcePath))
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

Guid AssetDatabaseFacade::StageLegacyTextureMigration(const StringView& legacyPath, const StringView& extractedPath,
    const StringView& destinationPath, const StringView& backupPath, const TextureTool::Options& options)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    FlaxStorage::Entry root;
    {
        FlaxStorageReference storage = ContentStorageManager::GetStorage(legacyPath, true);
        if (!storage || storage->GetEntriesCount() < 1)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
            diagnostic.SourcePath = legacyPath;
            diagnostic.Message = TEXT("Legacy texture header could not be read.");
            return fail();
        }
        storage->GetEntry(0, root);
    }
    TextureProcessorSettings settings = TextureProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
        return fail();
    AssetMeta meta;
    meta.ID = root.ID;
    meta.AssetType = root.TypeName;
    meta.SourceKind = AssetSourceKind::ImportedSource;
    meta.Processor.ID = TextureProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) ||
        StageImportedFiles(legacyPath, extractedPath, destinationPath, backupPath, meta, diagnostic))
        return fail();
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
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
            if (!TextureTool::ImportTexture(sourcePath, sourceData, false) && sourceData.GetArraySize() == 6)
                meta.AssetType = CubeTexture::TypeName;
        }
        meta.Processor.ID = TextureProcessorSettings::ProcessorID();
        meta.Processor.SettingsVersion = TextureProcessorSettings::CurrentVersion;
    }
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || RefreshPath(sourcePath))
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
    AssetPipelineDiagnostic diagnostic;
    const AssetBuildJobStatus thumbnailStatus = TexturePipelineService::GetThumbnailStatus(assetID, diagnostic);
    if (thumbnailStatus == AssetBuildJobStatus::Queued || thumbnailStatus == AssetBuildJobStatus::Building ||
        thumbnailStatus == AssetBuildJobStatus::Publishing || thumbnailStatus == AssetBuildJobStatus::Failed ||
        thumbnailStatus == AssetBuildJobStatus::Cancelled)
        return nullptr;
    ArtifactRequest request;
    request.AssetID = assetID;
    request.Target = TexturePipelineService::GetHostTarget();
    request.OutputKind = "thumbnail";
    request.RequiredCompatibility = "flax-texture-thumbnail-v2";
    request.Policy = ArtifactResolvePolicy::NoBuild;
    ResolvedArtifact artifact;
    if (ArtifactResolver::Get().Resolve(request, artifact, diagnostic))
    {
        TexturePipelineService::RequestThumbnailBuild(assetID, diagnostic);
        return nullptr;
    }
    const ArtifactLease lease = ArtifactLease::Acquire(artifact.StoragePath.Get());
    TextureData textureData;
    if (TextureTool::ImportTexture(artifact.StoragePath.Get(), textureData, false))
        return nullptr;
    if (PixelFormatExtensions::IsSRGB(textureData.Format))
        textureData.Format = PixelFormatExtensions::ToNonsRGB(textureData.Format);
    auto* texture = Content::CreateVirtualAsset<Texture>();
    auto* initData = New<TextureBase::InitData>();
    initData->FromTextureData(textureData, false);
    if (texture->Init(initData))
    {
        texture->DeleteObject();
        return nullptr;
    }
    return texture;
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
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || RefreshPath(sourcePath))
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
    if (GraphDocumentCodec::CreateStarter(typeName, document, diagnostic))
        return fail();
    Array<byte> surface;
    if (GraphDocumentCompiler::CompileDocument(document, surface, diagnostic))
        return fail();

    BytesContainer data;
    data.Link(ToSpan(surface));
    return CreateGraphDocumentFromSurface(outputPath, typeName, data, propertiesJson);
}

Guid AssetDatabaseFacade::CreateGraphDocumentFromSurface(const StringView& outputPath, const StringView& typeName, const BytesContainer& surface, const StringView& propertiesJson)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    if (!GraphDocumentCodec::IsSupportedType(typeName) || outputPath.IsEmpty() || !surface.IsValid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.Message = TEXT("Graph document type, path, or surface is invalid.");
        return fail();
    }
    GraphDocument document;
    StringAnsi json;
    if (GraphDocumentCodec::FromSurface(typeName, surface, document, diagnostic))
        return fail();
    if (propertiesJson.HasChars())
        document.PropertiesJson = StringAnsi(String(propertiesJson));
    if (GraphDocumentCodec::ToCanonicalJson(document, json, diagnostic) || GraphDocumentCodec::SaveAtomic(outputPath, json, diagnostic))
        return fail();

    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = TEXT("Flax.GraphDocument");
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(String(outputPath) + TEXT(".meta"), meta, diagnostic) || RefreshPath(outputPath))
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
    if (RefreshPath(outputPath))
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
    if (RefreshPath(record.SourcePath.Get()) || GraphPipelineService::RequestBuild(canonicalAssetID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AssetDatabaseFacade::SaveMaterialDocument(Material* asset, const Guid& canonicalAssetID)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER && COMPILE_WITH_MATERIAL_GRAPH
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
        record.SourceKind != AssetSourceKind::TextDocument || record.ProcessorID != TEXT("Flax.GraphDocument") ||
        record.TypeName != Material::TypeName)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = TEXT("The edited material is not backed by a canonical graph document.");
        return fail();
    }

    const BytesContainer existingSurface = asset->LoadSurface(true);
    if (existingSurface.IsInvalid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be loaded.");
        return fail();
    }

    MaterialGraph graph;
    MemoryReadStream readStream(existingSurface);
    if (graph.Load(&readStream, true))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be decoded.");
        return fail();
    }
    for (const MaterialParameter& materialParameter : asset->Params)
    {
        GraphParameter* graphParameter = graph.GetParameter(materialParameter.GetParameterID());
        if (!graphParameter)
            continue;

        graphParameter->Value = materialParameter.GetValue();
        if (graphParameter->Value.Type == VariantType::Object)
            graphParameter->Value = graphParameter->Value.AsObject ? graphParameter->Value.AsObject->GetID() : Guid::Empty;
        else if (graphParameter->Value.Type == VariantType::Asset)
            graphParameter->Value = graphParameter->Value.AsObject ? graphParameter->Value.AsObject->GetID() : Guid::Empty;
    }

    MemoryWriteStream writeStream(existingSurface.Length());
    if (graph.Save(&writeStream, true))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.SourcePath = record.SourcePath.Get();
        diagnostic.Message = TEXT("The edited material graph could not be encoded.");
        return fail();
    }
    BytesContainer surface;
    surface.Link(ToSpan(writeStream));
    return SaveGraphSurface(record.SourcePath.Get(), surface);
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
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveJsonAtomic(path, text, diagnostic) || RefreshPath(path))
        return fail();
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.CollisionData"))
        return fail();
    if (GraphPipelineService::RequestBuildAndWait(meta.ID, false, diagnostic))
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
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveJsonAtomic(path, text, diagnostic) || RefreshPath(path))
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
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || RefreshPath(sourcePath))
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
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || RefreshPath(sourcePath))
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
namespace
{
    Guid CreateModelMetadataInternal(const StringView& sourcePath, const ModelTool::Options& requestedOptions, bool inferRootType)
    {
        AssetPipelineDiagnostic diagnostic;
        auto fail = [&diagnostic]()
        {
            Array<AssetPipelineDiagnostic> diagnostics;
            diagnostics.Add(diagnostic);
            SetDiagnostics(diagnostics);
            return Guid::Empty;
        };
        const String metaPath = String(sourcePath) + TEXT(".meta");
        if (!FileSystem::FileExists(sourcePath) || FileSystem::FileExists(metaPath))
        {
            diagnostic.Code = FileSystem::FileExists(metaPath) ? AssetPipelineDiagnosticCode::PathCollision : AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = FileSystem::FileExists(metaPath) ? TEXT("Model metadata already exists.") : TEXT("Model source does not exist.");
            return fail();
        }

        ModelTool::Options options = requestedOptions;
        ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
        if (settings.Validate(diagnostic))
            return fail();
        ModelSourceAnalysis analysis;
        if (ModelProcessor::AnalyzeSource(sourcePath, settings, analysis, diagnostic))
            return fail();
        if (inferRootType)
        {
            options.Type = analysis.SourceSkeletonBoneCount > 0 || analysis.SourceAnimationCount > 0
                ? ModelTool::ModelType::SkinnedModel
                : ModelTool::ModelType::Model;
            settings = ModelProcessorSettings::FromLegacyOptions(options);
        }

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

        SubAssetReconcileResult reconciliation = SubAssetReconciler::Reconcile(meta, analysis.Candidates, true);
        if (reconciliation.RequiresUserReconciliation)
        {
            diagnostic = reconciliation.Diagnostics.HasItems() ? reconciliation.Diagnostics[0] : diagnostic;
            diagnostic.AssetGuid = meta.ID;
            diagnostic.SourcePath = sourcePath;
            return fail();
        }
        meta.SubAssets = MoveTemp(reconciliation.Resolved);
        if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic) || RefreshPath(sourcePath))
            return fail();
#if COMPILE_WITH_ASSETS_IMPORTER
        if (ModelPipelineService::RequestBuild(meta.ID, false, diagnostic))
            return fail();
#endif
        return meta.ID;
    }
}

Guid AssetDatabaseFacade::CreateDefaultModelMetadata(const StringView& sourcePath)
{
    ModelTool::Options options;
    return CreateModelMetadataInternal(sourcePath, options, true);
}

Guid AssetDatabaseFacade::CreateModelMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    return CreateModelMetadataInternal(sourcePath, options, false);
}

Guid AssetDatabaseFacade::StageLegacyModelMigration(const StringView& legacyPath, const StringView& extractedPath,
    const StringView& destinationPath, const StringView& backupPath, const ModelTool::Options& options)
{
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    ModelProcessorSettings settings = ModelProcessorSettings::FromLegacyOptions(options);
    if (settings.Validate(diagnostic))
        return fail();
    AssetMeta meta;
    meta.Processor.ID = ModelProcessorSettings::ProcessorID();
    meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
    if (settings.ToJson(meta.Processor.SettingsJson, diagnostic) || LegacyAssetMigrator::SeedModelSubAssets(legacyPath, meta, diagnostic))
        return fail();

    ModelTool::Options probeOptions = options;
    probeOptions.Type = ModelTool::ModelType::Prefab;
    probeOptions.ImportTypes = ImportDataTypes::Geometry | ImportDataTypes::Skeleton | ImportDataTypes::Animations |
                               ImportDataTypes::Nodes | ImportDataTypes::Materials | ImportDataTypes::Textures;
    ModelData sourceData;
    String importError;
    if (ModelTool::ImportData(String(extractedPath), sourceData, probeOptions, importError))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = extractedPath;
        diagnostic.Message = importError.IsEmpty() ? TEXT("Extracted GLB could not be parsed.") : importError;
        return fail();
    }
    Array<ModelSubAssetInfo> infos;
    Array<SubAssetCandidate> candidates;
    if (ModelSubAssetKeys::Enumerate(sourceData, infos, candidates, diagnostic))
        return fail();
    const Dictionary<String, SubAssetMeta> legacyMappings = meta.SubAssets;
    Dictionary<String, SubAssetMeta> mapped;
    HashSet<Guid> used;
    for (const SubAssetCandidate& candidate : candidates)
    {
        const SubAssetMeta* selected = nullptr;
        for (const auto& existing : legacyMappings)
        {
            if (!used.Contains(existing.Value.ID) && existing.Value.TypeName == candidate.TypeName && existing.Value.DisplayName == candidate.DisplayName)
            {
                selected = &existing.Value;
                break;
            }
        }
        if (!selected)
        {
            for (const auto& existing : legacyMappings)
            {
                if (!used.Contains(existing.Value.ID) && existing.Value.TypeName == candidate.TypeName)
                {
                    selected = &existing.Value;
                    break;
                }
            }
        }
        if (selected)
        {
            SubAssetMeta value = *selected;
            value.DisplayName = candidate.DisplayName;
            value.Removed = false;
            mapped.Add(candidate.StableKey, MoveTemp(value));
            used.Add(selected->ID);
        }
    }
    for (const auto& existing : legacyMappings)
    {
        if (used.Contains(existing.Value.ID))
            continue;
        SubAssetMeta tombstone = existing.Value;
        tombstone.Removed = true;
        mapped.Add(TEXT("legacy:") + tombstone.ID.ToString(Guid::FormatType::N), MoveTemp(tombstone));
    }
    meta.SubAssets = MoveTemp(mapped);
    if (StageImportedFiles(legacyPath, extractedPath, destinationPath, backupPath, meta, diagnostic))
        return fail();
    SetDiagnostics(Array<AssetPipelineDiagnostic>());
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
    if (GraphDocumentCodec::SaveAtomic(path, json, diagnostic, previous.IsZero() ? nullptr : &previous) || RefreshPath(path))
        return fail();
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetMeta meta;
    if (!AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) && GraphPipelineService::RequestBuild(meta.ID, false, diagnostic))
        return fail();
#endif
    return false;
}

bool AssetDatabaseFacade::BuildGraph(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (GraphPipelineService::RequestBuild(assetID, false, diagnostic))
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

String AssetDatabaseFacade::GetGraphBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    switch (GraphPipelineService::GetStatus(assetID, diagnostic))
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

AssetPipelineDiagnostic AssetDatabaseFacade::GetGraphBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    GraphPipelineService::GetStatus(assetID, diagnostic);
#endif
    return diagnostic;
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
    if (sourcePath.IsEmpty() || EnsureDatabaseLoaded())
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

    // The database is loaded once per migration run, so this conversion has to be folded back in
    // here or the next asset would see a stale snapshot.
    Array<String> refreshPaths;
    refreshPaths.Add(record.SourcePath.Get());
    refreshPaths.Add(destination);
    refreshPaths.Add(destinationMeta);
    if (RefreshSources(refreshPaths))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.AssetGuid = assetID;
        diagnostic.SourcePath = destination;
        diagnostic.Message = TEXT("The asset database could not be updated after migration.");
        return fail();
    }
    return false;
}

bool AssetDatabaseFacade::FinalizeLegacyImportedMigration(const StringView& backupPath)
{
    if (backupPath.IsEmpty() || !FileSystem::FileExists(backupPath))
        return true;
    ContentStorageManager::EnsureAccess(backupPath);
    return FileSystem::DeleteFile(backupPath);
}

bool AssetDatabaseFacade::RollbackLegacyImportedMigration(const StringView& legacyPath, const StringView& destinationPath, const StringView& backupPath)
{
    AssetPipelineDiagnostic diagnostic;
    const String metaPath = String(destinationPath) + TEXT(".meta");
    ContentStorageManager::EnsureAccess(destinationPath);
    ContentStorageManager::EnsureAccess(backupPath);
    Array<String> refreshPaths;
    refreshPaths.Add(metaPath);
    refreshPaths.Add(String(destinationPath));
    refreshPaths.Add(String(legacyPath));
    refreshPaths.Add(String(backupPath));
    const bool failed = FileSystem::DeleteFile(metaPath) || FileSystem::DeleteFile(destinationPath) ||
                        FileSystem::MoveFile(legacyPath, backupPath, false) || RefreshSources(refreshPaths);
    if (failed)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::MigrationFailed;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Migration;
        diagnostic.SourcePath = legacyPath;
        diagnostic.Message = TEXT("Staged imported migration rollback failed.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
    }
    else
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>());
    }
    return failed;
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
