// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseFacade.h"
#include "AssetDatabaseSnapshot.h"
#include "AssetMeta.h"
#include "MigrationInventory.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Threading/Threading.h"
#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Content/Artifacts/ArtifactLease.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Build/Processors/TextureProcessorSettings.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/TexturePipelineService.h"
#endif
#endif
#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
#include "Engine/Content/Build/Processors/ModelProcessorSettings.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#endif
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "Engine/Content/Build/Processors/GraphDocumentProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
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
            SetDiagnostics(Array<AssetPipelineDiagnostic>());
            return false;
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
    meta.AssetType = TEXT("FlaxEngine.Texture");
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
