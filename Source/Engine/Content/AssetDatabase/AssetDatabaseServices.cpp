// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseServices.h"
#include "AssetSourceRoots.h"
#include "AssetMeta.h"
#include "AssetOperations.h"
#include "Engine/Content/Artifacts/ArtifactGC.h"
#include "Engine/Content/Artifacts/ArtifactResolver.h"
#include "Engine/Content/Artifacts/ArtifactStore.h"
#include "Engine/Content/Asset.h"
#include "Engine/Content/BinaryAsset.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Content/Assets/Material.h"
#include "Engine/Content/Assets/MaterialInstance.h"
#include "Engine/Content/Assets/SkeletonMask.h"
#include "Engine/Content/Assets/RawDataAsset.h"
#include "Engine/Animations/SceneAnimations/SceneAnimation.h"
#include "Engine/Content/Storage/FlaxStorage.h"
#include "Engine/Content/Storage/ContentStorageManager.h"
#include "Engine/Content/Storage/JsonStorageProxy.h"
#include "Engine/Content/Documents/GraphDocument.h"
#include "Engine/Content/Documents/CanonicalJsonWriter.h"
#include "Engine/Content/Documents/CollisionDataDocument.h"
#include "Engine/Content/Documents/MaterialInstanceDocument.h"
#include "Engine/Content/Documents/ParticleSystemDocument.h"
#include "Engine/Content/Documents/SceneAnimationDocument.h"
#include "Engine/Particles/ParticleSystem.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Serialization/JsonWriters.h"
#include "Engine/Serialization/MemoryReadStream.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Utilities/Crc.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Content/Importing/AssetImportService.h"
#include "Engine/Content/Importing/CallbackImporterPipelineService.h"
#if COMPILE_WITH_MATERIAL_GRAPH
#include "Engine/Tools/MaterialGenerator/Types.h"
#endif
#if COMPILE_WITH_TEXTURE_TOOL
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
#include "Engine/Content/Build/Processors/ModelProcessor.h"
#include "Engine/Content/Build/Processors/ModelSubAssetKeys.h"
#include "Engine/Content/AssetDatabase/SubAssetReconciler.h"
#if COMPILE_WITH_ASSETS_IMPORTER
#include "Engine/Content/Build/Processors/ModelPipelineService.h"
#endif
#endif
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
#include "Engine/Content/Build/AssetProcessorRegistry.h"
#include "Engine/Content/Build/Processors/GraphDocumentProcessor.h"
#include "Engine/Content/Build/Processors/GraphPipelineService.h"
#include "Engine/Content/Build/Processors/ImportedSourceProcessor.h"
#endif
#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

Delegate<uint64> AssetDatabaseQueryService::DatabaseChanged;

namespace
{
    std::mutex ArtifactPublicationLocker;
    HashSet<Guid> PendingArtifactPublications;
    std::mutex DatabaseEventLocker;
    Array<uint64> PendingDatabaseEvents;
    std::mutex OperationWriteDrainLocker;
    Array<AssetOperationSelfWrite> PendingOperationSelfWrites;
}

void AssetPipelineService::NotifyArtifactPublished(const Guid& assetID)
{
    std::lock_guard<std::mutex> lock(ArtifactPublicationLocker);
    PendingArtifactPublications.Add(assetID);
}

Array<Guid> AssetPipelineService::DrainArtifactPublications()
{
    std::lock_guard<std::mutex> lock(ArtifactPublicationLocker);
    Array<Guid> result;
    result.EnsureCapacity(PendingArtifactPublications.Count());
    for (auto i = PendingArtifactPublications.Begin(); i.IsNotEnd(); ++i)
        result.Add(i->Item);
    PendingArtifactPublications.Clear();
    return result;
}

void AssetDatabaseQueryService::PumpDatabaseEvents()
{
    Array<uint64> revisions;
    {
        std::lock_guard<std::mutex> lock(DatabaseEventLocker);
        revisions = MoveTemp(PendingDatabaseEvents);
        PendingDatabaseEvents.Clear();
    }
    for (const uint64 revision : revisions)
        DatabaseChanged(revision);
}

namespace
{
    CriticalSection StateLocker;
    std::recursive_mutex RefreshLocker;
    Array<AssetPipelineDiagnostic> LastDiagnostics;
    Array<AssetDatabaseFileState> LastFileStates;
    SourceHashCache HashCache;
    bool IsBound = false;

    class FacadeModificationProcessor final : public IAssetModificationProcessor
    {
    public:
        bool ValidateOperation(AssetOperationKind kind, const AssetOperationTarget& target,
            const StringView& destination, AssetPipelineDiagnostic& diagnostic) override
        {
            AssetModificationProcessorRegistry* registry = AssetImportService::GetModificationProcessorRegistry();
            if (!registry)
            {
                diagnostic = AssetPipelineDiagnostic();
                return false;
            }
            AssetModificationRequest request;
            request.Path = target.SourcePath;
            request.DestinationPath = destination;
            switch (kind)
            {
            case AssetOperationKind::Move:
            case AssetOperationKind::Rename:
            case AssetOperationKind::Restore:
                request.Kind = AssetModificationKind::Move;
                break;
            case AssetOperationKind::Trash:
            case AssetOperationKind::Delete:
                request.Kind = AssetModificationKind::Delete;
                break;
            case AssetOperationKind::ImporterSettings:
                request.Kind = AssetModificationKind::Save;
                break;
            default:
                request.Kind = AssetModificationKind::Create;
                break;
            }
            AssetModificationDecision decision;
            if (registry->Process(request, decision, diagnostic))
                return true;
            if (decision.Allowed && !decision.Handled)
                return false;
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.SourcePath = target.SourcePath;
                diagnostic.Message = decision.Message.HasChars() ? decision.Message :
                    TEXT("Asset modification was denied or handled by a registered processor.");
            }
            return true;
        }
    };

    class FacadeOperationDatabaseCallbacks final : public IAssetOperationDatabaseCallbacks
    {
    public:
        bool ClearCopiedState(const Guid&, const Guid&, AssetPipelineDiagnostic& diagnostic) override
        {
            // Copy metadata always receives fresh source and object GUIDs, so no durable rows can
            // legitimately belong to the destination before its first refresh.
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool ValidateImporterSettingsRevision(const AssetOperationTarget& target,
            const AssetImporterSettingsRevision& expected, AssetPipelineDiagnostic& diagnostic) override
        {
            const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
            SourceAssetRow source;
            if (!snapshot.IsValid() || !snapshot.TryGetSource(target.ExpectedGuid, source))
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.AssetGuid = target.ExpectedGuid;
                diagnostic.SourcePath = target.SourcePath;
                diagnostic.Message = TEXT("Importer settings source is no longer registered.");
                return true;
            }
            if (!FileSystem::AreFilePathsEquivalent(source.Path, target.SourcePath) ||
                source.LastModifiedRevision != expected.SourceRevision ||
                source.MetaSemanticHash != expected.MetaSemanticHash || source.ImporterId != expected.ImporterID ||
                source.ImporterSettingsVersion != static_cast<uint32>(expected.StoredSettingsVersion))
            {
                diagnostic = AssetPipelineDiagnostic();
                diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.AssetGuid = target.ExpectedGuid;
                diagnostic.SourcePath = source.Path;
                diagnostic.ProcessorId = source.ImporterId;
                diagnostic.Message = TEXT("Importer settings write conflicts with a newer source metadata revision.");
                return true;
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }

        bool RefreshCommitted(const Array<AssetOperationCommit>& commits,
            AssetPipelineDiagnostic& diagnostic) override
        {
            Array<String> paths;
            String projectRoot = Globals::ProjectContentFolder;
            String engineRoot = AssetSourceRoots::GetEngineRoot();
            projectRoot.Replace('\\', '/');
            engineRoot.Replace('\\', '/');
            const auto addSourcePath = [&paths, &projectRoot, &engineRoot](const StringView& path)
            {
                String normalized(path);
                normalized.Replace('\\', '/');
                if (normalized.HasChars() && (AssetPathPolicy::IsSameOrChild(normalized, projectRoot) ||
                    (engineRoot.HasChars() && AssetPathPolicy::IsSameOrChild(normalized, engineRoot))))
                    paths.Add(MoveTemp(normalized));
            };
            for (const AssetOperationCommit& commit : commits)
            {
                addSourcePath(commit.SourcePath);
                addSourcePath(commit.DestinationPath);
            }
            if (AssetPipelineService::RefreshSources(paths))
            {
                const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
                diagnostic = diagnostics.HasItems() ? diagnostics[0] : AssetPipelineDiagnostic();
                return true;
            }
            for (const AssetOperationCommit& commit : commits)
            {
                if ((commit.Kind == AssetOperationKind::Trash || commit.Kind == AssetOperationKind::Delete ||
                    commit.Kind == AssetOperationKind::Restore ||
                    commit.Kind == AssetOperationKind::ImporterSettings) ||
                    !commit.AssetGuid.IsValid())
                    continue;
                if (AssetPipelineService::BuildAsset(commit.AssetGuid,
                    commit.Kind == AssetOperationKind::ImporterSettings, false))
                {
                    const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
                    diagnostic = diagnostics.HasItems() ? diagnostics[0] : AssetPipelineDiagnostic();
                    return true;
                }
            }
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
    };

    std::unique_ptr<FacadeModificationProcessor> OperationModificationProcessor;
    std::unique_ptr<FacadeOperationDatabaseCallbacks> OperationDatabaseCallbacks;
    std::unique_ptr<AssetOperations> Operations;

    void OnDatabaseChanged(const AssetDatabaseChangeBatch& change)
    {
        std::lock_guard<std::mutex> lock(DatabaseEventLocker);
        PendingDatabaseEvents.Add(change.Revision);
    }

    void EnsureBound()
    {
        if (!IsBound)
        {
            AssetDatabase::Get().Changed.BindUnique<OnDatabaseChanged>();
            IsBound = true;
        }
    }

    Guid CurrentProjectId()
    {
        String path = Globals::ProjectFolder;
        FileSystem::NormalizePath(path);
        path = path.ToLower();
        const ContentHash hash = ContentHash::Compute(*path, path.Length() * sizeof(Char));
        return Guid(hash.Values[0], hash.Values[1], hash.Values[2], hash.Values[3]);
    }

    AssetDatabaseChangeInfo ToChangeInfo(const AssetChangeSet& change)
    {
        AssetDatabaseChangeInfo result;
        result.Revision = change.Revision;
        result.RefreshId = change.RefreshId;
        result.Pass = change.Pass;
        for (const AssetAddedChange& value : change.Added)
            result.Added.Add(value.AssetGuid);
        for (const AssetRemovedChange& value : change.Removed)
            result.Removed.Add(value.AssetGuid);
        for (const AssetMovedChange& value : change.Moved)
            result.Changed.Add(value.AssetGuid);
        for (const AssetSourceChangedChange& value : change.SourceChanged)
            result.Changed.Add(value.AssetGuid);
        for (const AssetMetadataChangedChange& value : change.MetadataChanged)
            result.Changed.Add(value.AssetGuid);
        for (const AssetObjectsChangedChange& value : change.ObjectsChanged)
            result.Changed.Add(value.AssetGuid);
        for (const AssetStatusChangedChange& value : change.StatusChanged)
            result.StatusChanged.Add(value.AssetGuid);
        return result;
    }

    AssetDatabaseRecordInfo ToInfo(const AssetRecord& record)
    {
        AssetDatabaseRecordInfo result;
        result.ID = record.ID;
        result.SourceAssetID = record.SourceAssetID;
        result.LocalId = record.LocalId;
        result.TypeName = record.TypeName;
        result.CanonicalPath = record.CanonicalPath.Get();
        result.SourcePath = record.SourcePath.Get();
        result.MetaPath = record.MetaPath.Get();
        result.SubAssetKey = record.SubAsset.Get();
        result.DisplayName = record.DisplayName.HasChars()
            ? record.DisplayName
            : record.SubAsset.Get().HasChars()
            ? String(record.SubAsset.Get())
            : String(StringUtils::GetFileNameWithoutExtension(record.SourcePath.Get()));
        result.ProcessorID = record.ProcessorID;
        result.MetaSemanticHash = record.MetaSemanticHash;
        result.SourceKind = record.SourceKind;
        result.Status = record.Status;
        result.Revision = record.DatabaseRevision;
        result.IsMain = record.IsMainAsset();
        return result;
    }

    AssetDatabaseDependencyInfo ToInfo(const SourceAssetDependencyRow& dependency)
    {
        AssetDatabaseDependencyInfo result;
        result.Owner = AssetObjectId(AssetGuid(dependency.OwnerAssetGuid), dependency.OwnerLocalFileId);
        result.TargetID = dependency.TargetId;
        switch (dependency.Kind)
        {
        case AssetDependencyKind::SourceFile: result.Kind = TEXT("SourceFile"); break;
        case AssetDependencyKind::BuildInput: result.Kind = TEXT("BuildInput"); break;
        case AssetDependencyKind::RuntimeReference: result.Kind = TEXT("RuntimeReference"); break;
        case AssetDependencyKind::Toolchain: result.Kind = TEXT("Toolchain"); break;
        case AssetDependencyKind::LogicalPath: result.Kind = TEXT("LogicalPath"); break;
        default: result.Kind = TEXT("Unknown"); break;
        }
        result.TargetObject = AssetObjectId(AssetGuid(dependency.TargetAssetGuid), dependency.TargetLocalFileId);
        result.SourcePath = dependency.SourcePath;
        result.ExactArtifact = String(dependency.ExactArtifact.ToString());
        result.CustomDependency = dependency.CustomDependency;
        result.ContentHash = String(dependency.Content.ToString());
        result.OriginPath = dependency.OriginPath;
        result.OriginLine = dependency.OriginLine;
        result.OriginColumn = dependency.OriginColumn;
        return result;
    }

    AssetDatabasePublicationInfo ToInfo(const SourceAssetPublicationRow& publication)
    {
        AssetDatabasePublicationInfo result;
        result.Object = AssetObjectId(AssetGuid(publication.AssetGuid), publication.LocalFileId);
        result.TargetID = publication.TargetId;
        result.Artifact = String(publication.Artifact.ToString());
        result.ManifestHash = String(publication.ManifestHash.ToString());
        result.InputFingerprint = String(publication.InputFingerprint.ToString());
        result.SourceRevision = publication.SourceRevision;
        result.ImporterRegistryGeneration = publication.ImporterRegistryGeneration;
        result.PublishedUtcTicks = publication.PublishedUtcTicks;
        result.IsLastKnownGood = publication.IsLastKnownGood;
        return result;
    }

    void SetDiagnostics(const Array<AssetPipelineDiagnostic>& diagnostics)
    {
        ScopeLock lock(StateLocker);
        LastDiagnostics = diagnostics;
    }

    bool ReadImporterSettingsSnapshot(const Guid& sourceID, AssetImporterSettingsSnapshot& result,
        AssetPipelineDiagnostic& diagnostic)
    {
        result = AssetImporterSettingsSnapshot();
        const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
        SourceAssetRow source;
        if (!sourceID.IsValid() || !snapshot.IsValid() || !snapshot.TryGetSource(sourceID, source) ||
            source.IsFolder || source.SourceKind != AssetSourceKind::ImportedSource)
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = sourceID;
            diagnostic.Message = TEXT("Importer settings source is not registered as an imported asset.");
            return true;
        }
        AssetMeta meta;
        if (AssetMeta::Load(source.MetaPath, meta, diagnostic))
            return true;
        StringAnsi canonicalMeta;
        if (meta.ToJson(canonicalMeta, diagnostic))
            return true;
        const uint64 semanticHash = Crc::MemCrc32(canonicalMeta.Get(), canonicalMeta.Length());
        if (source.ImporterSettingsVersion > MAX_int32 || meta.ID != sourceID ||
            meta.Processor.ID != source.ImporterId || meta.Processor.SettingsVersion < 1 ||
            static_cast<uint32>(meta.Processor.SettingsVersion) != source.ImporterSettingsVersion ||
            semanticHash != source.MetaSemanticHash)
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = sourceID;
            diagnostic.SourcePath = source.Path;
            diagnostic.ProcessorId = source.ImporterId;
            diagnostic.Message = TEXT("Importer settings metadata is newer than the durable source database revision.");
            return true;
        }
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
        AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
        AssetImporterLease importer;
        if (!registry || registry->TryAcquire(source.ImporterId, importer, diagnostic))
            return true;
        if (importer.Get().SettingsSchemaVersion > MAX_int32)
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = sourceID;
            diagnostic.SourcePath = source.Path;
            diagnostic.ProcessorId = source.ImporterId;
            diagnostic.Message = TEXT("Importer settings schema version exceeds the supported editor range.");
            return true;
        }
        result.SourceAssetID = sourceID;
        result.SourceRevision = source.LastModifiedRevision;
        result.MetaSemanticHash = source.MetaSemanticHash;
        result.ImporterID = source.ImporterId;
        result.StoredSettingsVersion = static_cast<int32>(source.ImporterSettingsVersion);
        result.SettingsSchemaVersion = static_cast<int32>(importer.Get().SettingsSchemaVersion);
        result.SettingsJson = String(meta.Processor.SettingsJson);
        diagnostic = AssetPipelineDiagnostic();
        return false;
#else
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = sourceID;
        diagnostic.SourcePath = source.Path;
        diagnostic.ProcessorId = source.ImporterId;
        diagnostic.Message = TEXT("Importer settings are unavailable without the editor importer registry.");
        return true;
#endif
    }

    bool EnsureOperations(AssetPipelineDiagnostic& diagnostic)
    {
#if USE_EDITOR
        if (Operations)
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
        if (AssetImportService::EnsureInitialized(diagnostic))
            return true;
        auto modificationProcessor = std::make_unique<FacadeModificationProcessor>();
        auto databaseCallbacks = std::make_unique<FacadeOperationDatabaseCallbacks>();
        auto operations = std::make_unique<AssetOperations>(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, *modificationProcessor, *databaseCallbacks);
        if (operations->Initialize(diagnostic))
            return true;
        Array<AssetPipelineDiagnostic> recoveryDiagnostics;
        if (operations->RecoverIncompleteTransactions(recoveryDiagnostics))
        {
            diagnostic = recoveryDiagnostics.HasItems() ? recoveryDiagnostics[0] : AssetPipelineDiagnostic();
            SetDiagnostics(recoveryDiagnostics);
            return true;
        }
        OperationModificationProcessor = std::move(modificationProcessor);
        OperationDatabaseCallbacks = std::move(databaseCallbacks);
        Operations = std::move(operations);
        diagnostic = AssetPipelineDiagnostic();
        return false;
#else
        diagnostic = AssetPipelineDiagnostic();
        return false;
#endif
    }

    String NormalizeAbsolutePath(const StringView& path)
    {
        String result(path);
        if (result.IsEmpty())
            return result;
        FileSystem::NormalizePath(result);
        return result;
    }

    String ResolveFacadeAssetPath(const StringView& path)
    {
        String result(path);
        result.Replace('\\', '/');
        const StringView enginePrefix(TEXT("EngineContent/"));
        if (result.StartsWith(enginePrefix, StringSearchCase::IgnoreCase))
            result = AssetSourceRoots::GetEngineRoot() / result.Substring(enginePrefix.Length());
        else if (FileSystem::IsRelative(result))
            result = Globals::ProjectFolder / result;
        FileSystem::NormalizePath(result);
        return result;
    }

    String ToLogicalAssetPath(const StringView& path)
    {
        const String engineRoot = AssetSourceRoots::GetEngineRoot();
        String result;
        if (AssetPathPolicy::IsSameOrChild(path, engineRoot))
        {
            result = String(TEXT("EngineContent/")) + FileSystem::ConvertAbsolutePathToRelative(engineRoot, path);
        }
        else if (AssetPathPolicy::IsSameOrChild(path, Globals::ProjectContentFolder))
        {
            result = FileSystem::ConvertAbsolutePathToRelative(Globals::ProjectFolder, path);
        }
        else
        {
            result = path;
        }
        result.Replace('\\', '/');
        return result;
    }

    bool IsFacadeAssetPath(const StringView& path)
    {
        return AssetPathPolicy::IsSameOrChild(path, Globals::ProjectContentFolder) ||
            AssetPathPolicy::IsSameOrChild(path, AssetSourceRoots::GetEngineRoot());
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

    bool IsV3MetadataExcluded(const StringView& path)
    {
        String normalized(path);
        FileSystem::NormalizePath(normalized);
        normalized = normalized.ToLower();
        String root(Globals::ProjectContentFolder);
        FileSystem::NormalizePath(root);
        root = root.ToLower();
        String relative;
        if (normalized == root)
            relative = String::Empty;
        else if (normalized.StartsWith(root))
            relative = normalized.Substring(root.Length());
        else
            relative = normalized;
        relative = TEXT("/") + relative + TEXT("/");
        const Char* excluded[] =
        {
            TEXT("/library/"), TEXT("/cache/"), TEXT("/output/"), TEXT("/generated/"),
            TEXT("/migrationbackup/"), TEXT("/.asset-pipeline/"), TEXT("/.git/")
        };
        for (const Char* segment : excluded)
        {
            if (relative.Contains(segment))
                return true;
        }
        return false;
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
    // Scan handles this through EnsureJsonDocumentSidecars, so a scoped refresh must do the same.
    void EnsureScopedJsonSidecars(HashSet<String>& keys, Array<String>& expanded)
    {
        const int32 count = expanded.Count();
        for (int32 i = 0; i < count; i++)
        {
            const String path = expanded[i];
            const String extension = FileSystem::GetExtension(path).ToLower();
            if (extension != TEXT("scene") && extension != TEXT("prefab") && extension != TEXT("json") && extension != TEXT("settings"))
                continue;
            const String metaPath = path + TEXT(".meta");
            if (!FileSystem::FileExists(path) || FileSystem::FileExists(metaPath))
                continue;
            if (AuthoredAssetDocumentService::CreateMetadata(path).IsValid())
                AddUniquePath(metaPath, keys, expanded);
        }
    }

    bool RefreshPath(const StringView& path)
    {
        Array<String> paths;
        paths.Add(String(path));
        return AssetPipelineService::RefreshSources(paths);
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
        return AssetPipelineService::LoadOrScan(false);
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

    bool TryGetTypedAuthoredDocumentMetadata(const StringView& extension, String& typeName, String& processorID);
    void ConfigureTypedAuthoredDocumentMetadata(AssetMeta& metadata, const StringView& typeName, const StringView& processorID);
    bool ValidateTypedAuthoredDocumentType(const StringView& sourcePath, const StringView& expectedType,
        AssetPipelineDiagnostic& diagnostic);

    bool TryResolveCallbackImporter(const StringView& sourcePath, AssetImporterDescriptor& descriptor)
    {
        AssetPipelineDiagnostic ignored;
        if (AssetImportService::EnsureInitialized(ignored))
            return false;
        AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
        if (!registry)
            return false;
        AssetImporterSelectionRequest selection;
        selection.SourcePath = sourcePath;
        AssetImporterLease lease;
        if (registry->Resolve(selection, lease, ignored) || !lease.Get().ProcessSafe)
            return false;
        if (lease.Get().ProviderKind != AssetProcessorProviderKind::Managed &&
            (lease.Get().ProviderKind != AssetProcessorProviderKind::Native || lease.Get().WorkerExecutable.IsEmpty()))
            return false;
        descriptor = lease.Get();
        return true;
    }

    void ConfigureCallbackMetadata(AssetMeta& meta, const AssetImporterDescriptor& descriptor)
    {
        meta.AssetType = RawDataAsset::TypeName;
        meta.SourceKind = AssetSourceKind::ImportedSource;
        meta.Processor.ID = descriptor.ID;
        meta.Processor.SettingsVersion = descriptor.SettingsSchemaVersion;
        meta.Processor.SettingsJson = "{}\n";
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

        String authoredType;
        String authoredProcessor;
        if (TryGetTypedAuthoredDocumentMetadata(extension, authoredType, authoredProcessor))
        {
            if (ValidateTypedAuthoredDocumentType(work.SourcePath, authoredType, work.Diagnostic))
                return true;
            ConfigureTypedAuthoredDocumentMetadata(meta, authoredType, authoredProcessor);
            work.BuildKind = CanonicalBatchBuildKind::Imported;
            return false;
        }

        AssetImporterDescriptor callbackImporter;
        if (TryResolveCallbackImporter(work.SourcePath, callbackImporter))
        {
            ConfigureCallbackMetadata(meta, callbackImporter);
            work.BuildKind = CanonicalBatchBuildKind::Imported;
            return false;
        }

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
            ModelProcessor::PrimeAnalysisCache(work.SourcePath, settings, analysis);
            meta.AssetType = options.Type == ModelTool::ModelType::SkinnedModel ? SkinnedModel::TypeName : Model::TypeName;
            meta.Processor.ID = ModelProcessorSettings::ProcessorID();
            meta.Processor.SettingsVersion = ModelProcessorSettings::CurrentVersion;
            if (settings.ToJson(meta.Processor.SettingsJson, work.Diagnostic))
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
        else if (extension == TEXT("txt"))
        {
            meta.AssetType = RawDataAsset::TypeName;
            meta.SourceKind = AssetSourceKind::TextDocument;
            meta.Processor.ID = TEXT("Flax.Text");
        }
        else if (extension == TEXT("ies"))
        {
            meta.AssetType = TEXT("FlaxEngine.IESProfile");
            meta.Processor.ID = TEXT("Flax.IES");
        }
        else
        {
            meta.AssetType = RawDataAsset::TypeName;
            meta.Processor.ID = TEXT("Flax.Binary");
        }
        meta.Processor.SettingsVersion = 1;
        meta.Processor.SettingsJson = "{}\n";
        work.BuildKind = CanonicalBatchBuildKind::Imported;
        return false;
    }

    bool HasDefaultCanonicalImporter(const StringView& sourcePath)
    {
        const String extension = FileSystem::GetExtension(sourcePath).ToLower();
        return extension == TEXT("png") || extension == TEXT("tga") || extension == TEXT("exr") ||
            extension == TEXT("bmp") || extension == TEXT("gif") || extension == TEXT("tiff") || extension == TEXT("tif") ||
            extension == TEXT("jpeg") || extension == TEXT("jpg") || extension == TEXT("dds") || extension == TEXT("hdr") ||
            extension == TEXT("raw") || extension == TEXT("obj") || extension == TEXT("fbx") || extension == TEXT("x") ||
            extension == TEXT("dae") || extension == TEXT("gltf") || extension == TEXT("glb") || extension == TEXT("blend") ||
            extension == TEXT("bvh") || extension == TEXT("ase") || extension == TEXT("ply") || extension == TEXT("dxf") ||
            extension == TEXT("ifc") || extension == TEXT("nff") || extension == TEXT("smd") || extension == TEXT("vta") ||
            extension == TEXT("mdl") || extension == TEXT("md2") || extension == TEXT("md3") || extension == TEXT("md5mesh") ||
            extension == TEXT("q3o") || extension == TEXT("q3s") || extension == TEXT("ac") || extension == TEXT("stl") ||
            extension == TEXT("lwo") || extension == TEXT("lws") || extension == TEXT("lxo") || extension == TEXT("wav") ||
            extension == TEXT("mp3") || extension == TEXT("ogg") || extension == TEXT("ttf") || extension == TEXT("otf") ||
            extension == TEXT("shader") || extension == TEXT("mp4") || extension == TEXT("webm") || extension == TEXT("mov") ||
            extension == TEXT("mkv") || extension == TEXT("txt") || extension == TEXT("ies");
    }

    void ConfigureSettingsMetadata(AssetMeta& metadata)
    {
        metadata.AssetType = JsonAsset::TypeName;
        metadata.SourceKind = AssetSourceKind::TextDocument;
        metadata.Processor.ID = TEXT("Flax.Settings");
        metadata.Processor.SettingsVersion = 1;
        metadata.Processor.SettingsJson = "{}\n";
    }

    bool IsAuthoredJsonExtension(const StringView& extension)
    {
        return JsonStorageProxy::IsValidExtension(extension);
    }

    void ConfigureJsonDocumentMetadata(AssetMeta& metadata, const StringView& typeName)
    {
        metadata.AssetType = typeName;
        metadata.SourceKind = AssetSourceKind::TextDocument;
        metadata.Processor.ID = TEXT("Flax.JsonDocument");
        metadata.Processor.SettingsVersion = 1;
        metadata.Processor.SettingsJson = "{}\n";
    }

    bool TryGetTypedAuthoredDocumentMetadata(const StringView& extension, String& typeName, String& processorID)
    {
        if (!GraphDocumentCodec::TypeForExtension(extension, typeName))
        {
            processorID = TEXT("Flax.GraphDocument");
            return true;
        }
        if (extension == TEXT("materialinstance"))
        {
            typeName = TEXT("FlaxEngine.MaterialInstance");
            processorID = TEXT("Flax.MaterialInstance");
        }
        else if (extension == TEXT("skeletonmask"))
        {
            typeName = TEXT("FlaxEngine.SkeletonMask");
            processorID = TEXT("Flax.SkeletonMask");
        }
        else if (extension == TEXT("sceneanimation"))
        {
            typeName = TEXT("FlaxEngine.SceneAnimation");
            processorID = TEXT("Flax.SceneAnimation");
        }
        else if (extension == TEXT("particlesystem"))
        {
            typeName = TEXT("FlaxEngine.ParticleSystem");
            processorID = TEXT("Flax.ParticleSystem");
        }
        else if (extension == TEXT("collisiondata"))
        {
            typeName = TEXT("FlaxEngine.CollisionData");
            processorID = TEXT("Flax.CollisionData");
        }
        else
        {
            typeName.Clear();
            processorID.Clear();
            return false;
        }
        return true;
    }

    void ConfigureTypedAuthoredDocumentMetadata(AssetMeta& metadata, const StringView& typeName, const StringView& processorID)
    {
        metadata.AssetType = typeName;
        metadata.SourceKind = AssetSourceKind::TextDocument;
        metadata.Processor.ID = processorID;
        metadata.Processor.SettingsVersion = 1;
        metadata.Processor.SettingsJson = "{}\n";
    }

    bool GetJsonSourceDocumentType(const StringView& sourcePath, String& dataType, AssetPipelineDiagnostic& diagnostic)
    {
        dataType.Clear();
        Array<byte> bytes;
        rapidjson_flax::Document json;
        if (File::ReadAllBytes(sourcePath, bytes))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Cannot read authored JSON source document.");
            return true;
        }
        json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
        if (json.HasParseError() || !json.IsObject())
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Authored JSON source document is malformed.");
            return true;
        }
        const String extension = FileSystem::GetExtension(sourcePath).ToLower();
        const char* versionName = "documentVersion";
        const char* payloadName = "data";
        if (extension == TEXT("scene"))
        {
            dataType = TEXT("FlaxEngine.SceneAsset");
            versionName = "sceneVersion";
            payloadName = "objects";
        }
        else if (extension == TEXT("prefab"))
        {
            dataType = TEXT("FlaxEngine.Prefab");
            versionName = "prefabVersion";
            payloadName = "objects";
        }
        else
        {
            const auto typeMember = json.FindMember("type");
            if (typeMember != json.MemberEnd() && typeMember->value.IsString())
                dataType = String(StringAnsiView(typeMember->value.GetString(), typeMember->value.GetStringLength()));
            if (extension == TEXT("settings"))
                versionName = "settingsVersion";
        }
        const auto versionMember = json.FindMember(versionName);
        const auto payloadMember = json.FindMember(payloadName);
        const bool isSceneOrPrefab = extension == TEXT("scene") || extension == TEXT("prefab");
        if (dataType.IsEmpty() || versionMember == json.MemberEnd() || !versionMember->value.IsUint() || versionMember->value.GetUint() < 1 ||
            payloadMember == json.MemberEnd() || (isSceneOrPrefab ? !payloadMember->value.IsArray() :
                (!payloadMember->value.IsObject() && !payloadMember->value.IsArray())))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Authored JSON source document does not match its extension schema.");
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool ValidateTypedAuthoredDocumentType(const StringView& sourcePath, const StringView& expectedType,
        AssetPipelineDiagnostic& diagnostic)
    {
        Array<byte> bytes;
        rapidjson_flax::Document json;
        if (File::ReadAllBytes(sourcePath, bytes))
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Cannot read typed authored source document.");
            return true;
        }
        json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
        if (json.HasParseError() || !json.IsObject())
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Typed authored source type does not match its extension.");
            return true;
        }
        const auto type = json.FindMember("type");
        if (type == json.MemberEnd() || !type->value.IsString() ||
            String(StringAnsiView(type->value.GetString(), type->value.GetStringLength())) != expectedType)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = sourcePath;
            diagnostic.Message = TEXT("Typed authored source type does not match its extension.");
            return true;
        }
        return false;
    }

    bool EnsureDefaultCanonicalMetadata(const StringView& sourcePath, AssetPipelineDiagnostic& diagnostic)
    {
        const String metaPath = String(sourcePath) + TEXT(".meta");
        const bool isFolder = FileSystem::DirectoryExists(sourcePath);
        const bool isFile = FileSystem::FileExists(sourcePath);
        const String extension = isFile ? FileSystem::GetExtension(sourcePath).ToLower() : String::Empty;
        if (!isFile && !isFolder)
            return false;
        if (FileSystem::FileExists(metaPath))
        {
            AssetMeta metadata;
            if (AssetMeta::Load(metaPath, metadata, diagnostic))
                return true;
            String authoredType;
            String authoredProcessor;
            if (TryGetTypedAuthoredDocumentMetadata(extension, authoredType, authoredProcessor))
            {
                if (ValidateTypedAuthoredDocumentType(sourcePath, authoredType, diagnostic))
                {
                    diagnostic.AssetGuid = metadata.ID;
                    return true;
                }
                const bool exactMetadata = metadata.AssetType == authoredType &&
                    metadata.SourceKind == AssetSourceKind::TextDocument && metadata.Processor.ID == authoredProcessor &&
                    metadata.Processor.SettingsVersion == 1 && metadata.Processor.SettingsJson == StringAnsiView("{}\n");
                const bool regressionDamagedMetadata = metadata.AssetType == RawDataAsset::TypeName &&
                    metadata.SourceKind == AssetSourceKind::ImportedSource &&
                    metadata.Processor.ID == TEXT("Flax.Binary") && metadata.Processor.SettingsVersion == 1 &&
                    metadata.Processor.SettingsJson == StringAnsiView("{}\n");
                if (regressionDamagedMetadata)
                {
                    ConfigureTypedAuthoredDocumentMetadata(metadata, authoredType, authoredProcessor);
                    return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
                }
                if (!exactMetadata)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
                    diagnostic.SourcePath = sourcePath;
                    diagnostic.AssetGuid = metadata.ID;
                    diagnostic.Message = TEXT("Typed authored metadata does not match its extension and declared source type.");
                    return true;
                }
                return false;
            }
            AssetImporterDescriptor callbackImporter;
            if (metadata.Processor.ID == TEXT("Flax.Binary") && TryResolveCallbackImporter(sourcePath, callbackImporter))
            {
                ConfigureCallbackMetadata(metadata, callbackImporter);
                return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
            }
            const bool isSettings = extension == TEXT("settings");
            const bool isJsonDocument = !isSettings && IsAuthoredJsonExtension(extension);
            if (isSettings || isJsonDocument)
            {
                String dataType;
                if (GetJsonSourceDocumentType(sourcePath, dataType, diagnostic))
                    return true;
                if ((!isSettings && metadata.AssetType != dataType) || (isSettings && metadata.AssetType != JsonAsset::TypeName))
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
                    diagnostic.SourcePath = sourcePath;
                    diagnostic.AssetGuid = metadata.ID;
                    diagnostic.Message = TEXT("Authored JSON source type does not match its metadata type.");
                    return true;
                }
            }
            const bool settingsUpgrade = isSettings &&
                (metadata.AssetType != JsonAsset::TypeName || metadata.SourceKind != AssetSourceKind::TextDocument ||
                 metadata.Processor.ID != TEXT("Flax.Settings") || metadata.Processor.SettingsVersion != 1 ||
                 metadata.Processor.SettingsJson != StringAnsiView("{}\n"));
            const bool jsonDocumentUpgrade = isJsonDocument &&
                (metadata.SourceKind != AssetSourceKind::TextDocument || metadata.Processor.ID != TEXT("Flax.JsonDocument") ||
                 metadata.Processor.SettingsVersion != 1 || metadata.Processor.SettingsJson != StringAnsiView("{}\n"));
            if (!settingsUpgrade && !jsonDocumentUpgrade)
                return false;
            if (isSettings)
                ConfigureSettingsMetadata(metadata);
            else if (isJsonDocument)
            {
                ConfigureJsonDocumentMetadata(metadata, metadata.AssetType);
            }
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        AssetMeta metadata;
        metadata.ID = Guid::New();
        metadata.FolderAsset = isFolder;
        if (isFolder)
        {
            metadata.AssetType = TEXT("FlaxEngine.Folder");
            metadata.SourceKind = AssetSourceKind::Folder;
            metadata.Processor.ID = TEXT("Flax.Folder");
            metadata.Processor.SettingsVersion = 1;
            metadata.Processor.SettingsJson = "{}\n";
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        if (extension == TEXT("flax"))
            return false;
        String authoredType;
        String authoredProcessor;
        if (TryGetTypedAuthoredDocumentMetadata(extension, authoredType, authoredProcessor))
        {
            if (ValidateTypedAuthoredDocumentType(sourcePath, authoredType, diagnostic))
                return true;
            ConfigureTypedAuthoredDocumentMetadata(metadata, authoredType, authoredProcessor);
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        if (IsAuthoredJsonExtension(extension))
        {
            metadata.ID = Guid::New();
            if (GetJsonSourceDocumentType(sourcePath, metadata.AssetType, diagnostic))
            {
                return true;
            }
            if (extension == TEXT("settings"))
                ConfigureSettingsMetadata(metadata);
            else
            {
                ConfigureJsonDocumentMetadata(metadata, metadata.AssetType);
            }
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        AssetImporterDescriptor callbackImporter;
        if (TryResolveCallbackImporter(sourcePath, callbackImporter))
        {
            ConfigureCallbackMetadata(metadata, callbackImporter);
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        if (!HasDefaultCanonicalImporter(sourcePath))
        {
            metadata.AssetType = RawDataAsset::TypeName;
            metadata.SourceKind = AssetSourceKind::ImportedSource;
            metadata.Processor.ID = TEXT("Flax.Binary");
            metadata.Processor.SettingsVersion = 1;
            metadata.Processor.SettingsJson = "{}\n";
            return AssetMeta::SaveAtomic(metaPath, metadata, diagnostic);
        }
        CanonicalBatchWork work;
        work.SourcePath = sourcePath;
        work.StagingPath = metaPath;
        if (PrepareDefaultCanonicalMetadata(work) || AssetMeta::SaveAtomic(metaPath, work.Meta, work.Diagnostic))
        {
            diagnostic = work.Diagnostic;
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }

    bool CollectSourceDirectories(const StringView& root, Array<String>& result)
    {
        Array<String> pending;
        pending.Add(String(root));
        for (int32 index = 0; index < pending.Count(); index++)
        {
            Array<String> children;
            if (FileSystem::GetChildDirectories(children, pending[index]))
                return true;
            for (String& child : children)
            {
                result.Add(child);
                pending.Add(MoveTemp(child));
            }
        }
        return false;
    }

    void EnsureV3MetadataForRoot(const StringView& root, Array<AssetPipelineDiagnostic>& diagnostics)
    {
        Array<String> sources;
        if (FileSystem::DirectoryGetFiles(sources, String(root), TEXT("*"), DirectorySearchOption::AllDirectories) ||
            CollectSourceDirectories(root, sources))
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
            diagnostic.SourcePath = root;
            diagnostic.Message = TEXT("Cannot enumerate the writable source root for metadata reconciliation.");
            diagnostics.Add(MoveTemp(diagnostic));
            return;
        }
        if (sources.Count() > 1)
            std::sort(sources.Get(), sources.Get() + sources.Count());
        for (const String& source : sources)
        {
            if (IsMetaPath(source) || IsV3MetadataExcluded(source))
                continue;
            AssetPipelineDiagnostic diagnostic;
            if (EnsureDefaultCanonicalMetadata(source, diagnostic))
                diagnostics.Add(MoveTemp(diagnostic));
        }
    }

    bool ReconcileFullScan(bool strictMetadata, const Guid& refreshId, uint32 pass,
        AssetPipelineDiagnostic& diagnostic)
    {
        Array<AssetPipelineDiagnostic> metadataDiagnostics;
        EnsureV3MetadataForRoot(Globals::ProjectContentFolder, metadataDiagnostics);
        AssetDatabaseScanOptions options;
        options.StrictMetadata = strictMetadata;
        options.HashCache = &HashCache;
        AssetDatabaseScanResult result;
        Array<AssetRecord> records;
        const AssetDatabaseSnapshot previous = AssetDatabase::Get().GetSnapshot();
        bool failed = AssetDatabaseScanner::Collect(Globals::ProjectFolder, Globals::ProjectContentFolder,
            Globals::ProjectLibraryFolder, options, previous, records, result);
        const String engineRoot = AssetSourceRoots::GetEngineRoot();
        if (!failed && FileSystem::DirectoryExists(engineRoot))
        {
            AssetDatabaseScanResult engineResult;
            Array<AssetRecord> engineRecords;
            failed = AssetDatabaseScanner::Collect(Globals::StartupFolder, engineRoot, Globals::ProjectLibraryFolder,
                options, previous, engineRecords, engineResult);
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
        result.Diagnostics.Add(metadataDiagnostics);
        if (!failed)
        {
            AssetPipelineDiagnostic publishDiagnostic;
            failed = AssetDatabase::Get().ReconcileScanRows(records, result.Diagnostics, result.FileStates,
                publishDiagnostic, refreshId, pass);
            if (failed)
                result.Diagnostics.Add(publishDiagnostic);
            else
                result.Revision = AssetDatabase::Get().GetRevision();
        }
        SetDiagnostics(result.Diagnostics);
        if (!failed)
            LastFileStates = result.FileStates;
        if (failed)
        {
            diagnostic = result.Diagnostics.HasItems() ? result.Diagnostics.Last() : AssetPipelineDiagnostic();
            if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
            {
                diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
                diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
                diagnostic.Message = TEXT("Asset database full scan failed without a diagnostic.");
            }
            return true;
        }
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
#endif

#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    enum class GenericBuildRequestResult : byte
    {
        Unsupported,
        Queued,
        Failed,
    };

    enum class GenericRefreshScanMode : byte
    {
        None,
        ScanOnly,
        ScanAndBuildAll,
    };

    bool WaitForGenericBuild(const Guid& assetID, const AssetImporterBuildStatus& getStatus,
        AssetPipelineDiagnostic& diagnostic)
    {
        for (;;)
        {
            const AssetBuildJobStatus status = getStatus(assetID, diagnostic);
            if (status == AssetBuildJobStatus::Succeeded)
                return false;
            if (status == AssetBuildJobStatus::Failed || status == AssetBuildJobStatus::Cancelled || status == AssetBuildJobStatus::Invalid)
            {
                if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
                {
                    diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
                    diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
                    diagnostic.AssetGuid = assetID;
                    diagnostic.Message = TEXT("Synchronous asset import did not complete successfully.");
                }
                return true;
            }
            Platform::Sleep(1);
        }
    }

    GenericBuildRequestResult RequestGenericBuild(const AssetRecord& record, bool force, bool synchronous,
        const Guid& refreshId, uint32 pass, AssetBuildJobPriority priority, AssetPipelineDiagnostic& diagnostic)
    {
        AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
        AssetImporterLease importer;
        if (!registry || registry->TryAcquire(record.ProcessorID, importer, diagnostic))
            return GenericBuildRequestResult::Unsupported;
        const AssetImporterDescriptor& descriptor = importer.Get();
        if ((!descriptor.RequestBuild.IsBinded() && !descriptor.RequestBuildWithPriority.IsBinded()) ||
            !descriptor.GetBuildStatus.IsBinded())
        {
            diagnostic = AssetPipelineDiagnostic();
            return GenericBuildRequestResult::Unsupported;
        }
        bool failed;
        if (descriptor.RequestBuildWithPriority.IsBinded())
            failed = descriptor.RequestBuildWithPriority(record.ID, force, priority, refreshId, pass, diagnostic);
        else if (priority != AssetBuildJobPriority::Normal && CallbackImporterPipelineService::OwnsProcessor(record.ProcessorID))
            failed = CallbackImporterPipelineService::RequestBuild(record.ID, force, diagnostic, nullptr, refreshId, pass, priority);
        else
            failed = descriptor.RequestBuild(record.ID, force, refreshId, pass, diagnostic);
        if (failed ||
            (synchronous && WaitForGenericBuild(record.ID, descriptor.GetBuildStatus, diagnostic)))
            return GenericBuildRequestResult::Failed;
        return GenericBuildRequestResult::Queued;
    }

    bool SupportsGenericBuild(const AssetRecord& record)
    {
        AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
        AssetImporterLease importer;
        AssetPipelineDiagnostic diagnostic;
        return registry && !registry->TryAcquire(record.ProcessorID, importer, diagnostic) &&
            (importer.Get().RequestBuild.IsBinded() || importer.Get().RequestBuildWithPriority.IsBinded()) &&
            importer.Get().GetBuildStatus.IsBinded();
    }

    bool RunGenericBuildRefresh(const Array<AssetRecord>& selected, bool force, bool synchronous,
        AssetRefreshReason reason, AssetPipelineDiagnostic& diagnostic,
        GenericRefreshScanMode scanMode = GenericRefreshScanMode::None, bool strictMetadata = false,
        AssetBuildJobPriority priority = AssetBuildJobPriority::Normal)
    {
        if (selected.IsEmpty() && scanMode == GenericRefreshScanMode::None)
        {
            diagnostic = AssetPipelineDiagnostic();
            return false;
        }
        if (AssetImportService::EnsureInitialized(diagnostic))
            return true;
        AssetRefreshCoordinator* coordinator = AssetImportService::GetRefreshCoordinator();
        if (!coordinator)
        {
            diagnostic = AssetPipelineDiagnostic();
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
            diagnostic.Message = TEXT("Asset refresh coordinator is unavailable.");
            return true;
        }

        Array<Guid> selectedIds;
        Array<AssetImporterDescriptor> importerDescriptors;
        AssetImportService::GetImporterRegistry()->GetDescriptors(importerDescriptors);
        selectedIds.EnsureCapacity(selected.Count());
        for (const AssetRecord& record : selected)
        {
            if (!record.IsMainAsset() || record.Status != AssetRecordStatus::Ready)
                continue;
            bool importerAvailable = false;
            for (const AssetImporterDescriptor& descriptor : importerDescriptors)
            {
                if (descriptor.ID == record.ProcessorID &&
                    (descriptor.RequestBuild.IsBinded() || descriptor.RequestBuildWithPriority.IsBinded()) &&
                    descriptor.GetBuildStatus.IsBinded())
                {
                    importerAvailable = true;
                    break;
                }
            }
            if (importerAvailable)
                selectedIds.Add(record.ID);
        }

        AssetRefreshCallbacks callbacks;
        bool scanPending = scanMode != GenericRefreshScanMode::None;
        const uint64 startingRevision = AssetDatabase::Get().GetRevision();
        callbacks.Session = [startingRevision, reason](const AssetRefreshResult& refresh,
            AssetRefreshRunState state, const AssetPipelineDiagnostic&, AssetPipelineDiagnostic& localDiagnostic)
        {
            SourceRefreshSessionRow session;
            if (state == AssetRefreshRunState::Started)
            {
                session.RefreshId = refresh.RefreshId;
                session.StartingRevision = startingRevision;
                session.Reason = reason == AssetRefreshReason::Filesystem ? TEXT("Filesystem") : TEXT("Explicit");
                session.Status = TEXT("Running");
                session.StartedUtcTicks = DateTime::NowUTC().Ticks;
            }
            else
            {
                const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
                if (!snapshot.TryGetRefreshSession(refresh.RefreshId, session))
                {
                    localDiagnostic = AssetPipelineDiagnostic();
                    localDiagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
                    localDiagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
                    localDiagnostic.Message = TEXT("Cannot complete a refresh session that has no durable start row.");
                    return true;
                }
                session.EndingRevision = snapshot.GetRevision();
                session.IterationCount = refresh.Iterations;
                session.Status = state == AssetRefreshRunState::Succeeded ? TEXT("Completed") : TEXT("Failed");
                session.CompletedUtcTicks = DateTime::NowUTC().Ticks;
            }
            return AssetDatabase::Get().RecordRefreshSession(session, refresh.Pass, localDiagnostic);
        };
        callbacks.Reconcile = [&selectedIds, &importerDescriptors, &scanPending, force, scanMode, strictMetadata](
            const AssetRefreshIterationContext& context, Array<AssetImportPlanRequest>& requests,
            bool&, AssetPipelineDiagnostic& localDiagnostic)
        {
            if (scanPending)
            {
                if (ReconcileFullScan(strictMetadata, context.RefreshId, context.Pass, localDiagnostic))
                    return true;
                scanPending = false;
                if (scanMode == GenericRefreshScanMode::ScanAndBuildAll)
                {
                    selectedIds.Clear();
                    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
                    for (const AssetRecord& record : snapshot.Records)
                    {
                        if (!record.IsMainAsset() || record.Status != AssetRecordStatus::Ready)
                            continue;
                        for (const AssetImporterDescriptor& descriptor : importerDescriptors)
                        {
                            if (descriptor.ID == record.ProcessorID &&
                                (descriptor.RequestBuild.IsBinded() || descriptor.RequestBuildWithPriority.IsBinded()) &&
                                descriptor.GetBuildStatus.IsBinded())
                            {
                                selectedIds.Add(record.ID);
                                break;
                            }
                        }
                    }
                }
            }
            requests.Clear();
            for (const Guid& id : selectedIds)
            {
                AssetRecord record;
                if (!AssetDatabase::Get().TryGetRecord(id, record) || record.Status != AssetRecordStatus::Ready ||
                    !record.IsMainAsset() || !FileSystem::FileExists(record.SourcePath.Get()))
                    continue;
                ContentHash sourceHash;
                SourceHashFileState fileState;
                if (HashCache.HashFile(record.SourcePath.Get(), sourceHash, fileState, localDiagnostic))
                    return true;
                AssetImportPlanRequest request;
                request.Asset = AssetGuid(record.ID);
                request.SourcePath = record.SourcePath.Get();
                request.ExplicitImporterID = record.ProcessorID;
                request.Reason = context.Reasons == AssetRefreshReason::Filesystem ? TEXT("filesystem-refresh") : TEXT("explicit-refresh");
                if (ArtifactResolver::Get().IsConfigured())
                    request.Target = ArtifactResolver::Get().GetDefaultTarget();
                request.SourceRevision = record.DatabaseRevision;
                request.SourceHash = sourceHash;
                request.MetadataHash = ContentHash::Compute(&record.MetaSemanticHash, sizeof(record.MetaSemanticHash));
                request.Force = force;
                requests.Add(MoveTemp(request));
            }
            localDiagnostic = AssetPipelineDiagnostic();
            return false;
        };
        callbacks.Execute = [force, synchronous, priority](const AssetRefreshIterationContext&, const Array<AssetImportPlan>& plans,
            Array<AssetImportCompletion>& completed, bool&, AssetPipelineDiagnostic& localDiagnostic)
        {
            for (const AssetImportPlan& plan : plans)
            {
                AssetRecord record;
                if (!AssetDatabase::Get().TryGetRecord(plan.Request.Asset.Value, record))
                    continue;
                const GenericBuildRequestResult request = RequestGenericBuild(record, force, synchronous,
                    plan.Request.RefreshId, plan.Request.Pass, priority, localDiagnostic);
                if (request == GenericBuildRequestResult::Failed)
                    return true;
                if (synchronous && request == GenericBuildRequestResult::Queued)
                {
                    AssetImportCompletion completion;
                    completion.Asset = plan.Request.Asset;
                    completion.SourcePath = plan.Request.SourcePath;
                    completion.Artifact = plan.StaticFingerprint;
                    completion.Succeeded = true;
                    completed.Add(MoveTemp(completion));
                }
            }
            localDiagnostic = AssetPipelineDiagnostic();
            return false;
        };
        AssetRefreshResult result;
        if ((selectedIds.HasItems() || scanMode != GenericRefreshScanMode::None) &&
            coordinator->Refresh(reason, callbacks, result, diagnostic))
            return true;
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
#endif

}

uint64 AssetDatabaseQueryService::GetRevision()
{
    return AssetDatabase::Get().GetRevision();
}

bool AssetPipelineService::Initialize()
{
#if USE_EDITOR
    EnsureBound();
    if (AssetDatabase::Get().IsOpen())
    {
        AssetPipelineDiagnostic diagnostic;
        if (EnsureOperations(diagnostic))
        {
            SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
            return true;
        }
        return false;
    }
    AssetPipelineDiagnostic diagnostic;
    if (AssetDatabase::Get().Open(Globals::ProjectLibraryFolder, CurrentProjectId(), diagnostic))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    if (EnsureOperations(diagnostic))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        AssetDatabase::Get().Close();
        return true;
    }
    Array<AssetPipelineDiagnostic> diagnostics;
    const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
    if (snapshot.IsValid())
    {
        for (const SourceAssetDiagnosticRow& row : snapshot.GetState().Diagnostics)
        {
            if (row.IsActive)
                diagnostics.Add(row.Diagnostic);
        }
    }
    SetDiagnostics(diagnostics);
    return false;
#else
    return true;
#endif
}

bool AssetPipelineService::Shutdown()
{
    Operations.reset();
    OperationDatabaseCallbacks.reset();
    OperationModificationProcessor.reset();
    {
        std::lock_guard<std::mutex> lock(DatabaseEventLocker);
        PendingDatabaseEvents.Clear();
    }
    {
        std::lock_guard<std::mutex> lock(OperationWriteDrainLocker);
        PendingOperationSelfWrites.Clear();
    }
    AssetPipelineDiagnostic diagnostic;
    const bool failed = AssetDatabase::Get().Close(&diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
}

Array<String> AssetOperationService::DrainSelfWrites()
{
    Array<String> result;
    if (!Operations)
        return result;
    Array<AssetOperationSelfWrite> writes;
    Operations->DrainSelfWrites(writes);
    std::lock_guard<std::mutex> lock(OperationWriteDrainLocker);
    PendingOperationSelfWrites.Add(writes);
    constexpr int32 MaxWritesPerDrain = 16;
    result.EnsureCapacity(MaxWritesPerDrain);
    while (result.Count() < MaxWritesPerDrain && PendingOperationSelfWrites.HasItems())
    {
        AssetOperationSelfWrite write = MoveTemp(PendingOperationSelfWrites.Last());
        PendingOperationSelfWrites.RemoveLast();
        if (IsFacadeAssetPath(write.Path))
            result.Add(MoveTemp(write.Path));
    }
    return result;
}

bool AssetOperationService::RegisterSelfWrite(const StringView& path, const ContentHash& content)
{
#if USE_EDITOR
    if (path.IsEmpty() || content.IsZero() || AssetPipelineService::Initialize() || !Operations)
        return true;
    const String resolved = ResolveFacadeAssetPath(path);
    if (!IsFacadeAssetPath(resolved))
        return true;
    Operations->RegisterSelfWrite(resolved, content);
    return false;
#else
    return true;
#endif
}

#if USE_EDITOR
bool AssetOperationService::ImportAsset(const StringView& externalSource, const StringView& destination)
{
    if (AssetPipelineService::Initialize())
        return true;
    CanonicalBatchWork work;
    work.SourcePath = NormalizeAbsolutePath(externalSource);
    const String destinationPath = ResolveFacadeAssetPath(destination);
    work.StagingPath = destinationPath + TEXT(".meta");
    if (PrepareDefaultCanonicalMetadata(work))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ work.Diagnostic }));
        return true;
    }
    AssetPipelineDiagnostic diagnostic;
    if (!Operations || Operations->ImportAsset(work.SourcePath, destinationPath, work.Meta, diagnostic))
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Configuration;
            diagnostic.SourcePath = work.SourcePath;
            diagnostic.Message = TEXT("Asset operation service is unavailable.");
        }
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    return false;
}
#endif

Array<AssetDatabaseRecordInfo> AssetDatabaseQueryService::GetRecords()
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

Array<AssetDatabaseRecordInfo> AssetDatabaseQueryService::QueryRecords(const AssetDatabaseQuery& query)
{
    Array<AssetDatabaseRecordInfo> result;
    if (EnsureDatabaseLoaded())
        return result;
    AssetRecordQuery nativeQuery;
    nativeQuery.Name = query.Name;
    nativeQuery.PathPrefix = query.PathPrefix;
    nativeQuery.TypeName = query.TypeName;
    nativeQuery.ProcessorId = query.ImporterID;
    nativeQuery.Label = query.Label;
    nativeQuery.Status = query.Status;
    nativeQuery.HasStatus = query.HasStatus;
    nativeQuery.MainAssetsOnly = query.MainAssetsOnly;
    nativeQuery.ReferencedAsset = query.ReferencedAsset;
    nativeQuery.UsedByAsset = query.UsedByAsset;
    Array<AssetRecord> records;
    AssetDatabase::Get().QueryRecords(nativeQuery, records);
    result.EnsureCapacity(records.Count());
    for (const AssetRecord& record : records)
        result.Add(ToInfo(record));
    return result;
}

bool AssetDatabaseQueryService::TryGetRecord(const AssetObjectId& objectID, AssetDatabaseRecordInfo& result)
{
    result = AssetDatabaseRecordInfo();
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return false;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(objectID, record))
        return false;
    result = ToInfo(record);
    return true;
}

bool AssetDatabaseQueryService::TryGetMainRecordAtPath(const StringView& path, AssetDatabaseRecordInfo& result)
{
    result = AssetDatabaseRecordInfo();
    if (path.IsEmpty() || EnsureDatabaseLoaded())
        return false;
    String key = ResolveFacadeAssetPath(path).ToLower();
    key.Replace(TEXT('\\'), TEXT('/'));
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetMainRecordByPath(key, record))
        return false;
    result = ToInfo(record);
    return true;
}

Array<String> AssetDatabaseQueryService::GetLabels(const Guid& sourceID)
{
    Array<String> result;
    if (sourceID.IsValid() && !EnsureDatabaseLoaded())
        AssetDatabase::Get().GetLabels(sourceID, result);
    return result;
}

bool AssetOperationService::SetLabels(const Guid& sourceID, const Array<String>& labels)
{
    if (EnsureDatabaseLoaded())
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = AssetDatabase::Get().SetLabels(sourceID, labels, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
}

bool AssetPipelineService::RegisterCustomDependency(const StringView& name, const StringView& contentHash, const StringView& provider)
{
    if (EnsureDatabaseLoaded())
        return true;
    ContentHash hash;
    AssetPipelineDiagnostic diagnostic;
    if (ContentHash::Parse(contentHash, hash))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.Message = TEXT("Custom dependency hashes must be canonical SHA-256 values.");
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    const bool failed = AssetDatabase::Get().RegisterCustomDependency(name, hash, provider, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
}

bool AssetPipelineService::UnregisterCustomDependency(const StringView& name)
{
    if (EnsureDatabaseLoaded())
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = AssetDatabase::Get().UnregisterCustomDependency(name, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
}

String AssetPipelineService::GetCustomDependencyHash(const StringView& name)
{
    if (EnsureDatabaseLoaded())
        return String::Empty;
    ContentHash hash;
    return AssetDatabase::Get().TryGetCustomDependencyHash(name, hash) ? String(hash.ToString()) : String::Empty;
}

Array<AssetDatabaseDependencyInfo> AssetDatabaseQueryService::GetDependencies(const AssetObjectId& objectID)
{
    Array<AssetDatabaseDependencyInfo> result;
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return result;
    const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
    if (!snapshot.IsValid())
        return result;
    for (const SourceAssetDependencyRow& dependency : snapshot.GetState().Dependencies)
    {
        if (dependency.OwnerAssetGuid == objectID.Asset.Value && dependency.OwnerLocalFileId == objectID.LocalId)
            result.Add(ToInfo(dependency));
    }
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetDatabaseDependencyInfo& a, const AssetDatabaseDependencyInfo& b)
        {
            if (a.Kind != b.Kind)
                return a.Kind < b.Kind;
            if (a.TargetID != b.TargetID)
                return a.TargetID < b.TargetID;
            if (a.TargetObject.Asset.Value != b.TargetObject.Asset.Value)
                return a.TargetObject.Asset.ToString() < b.TargetObject.Asset.ToString();
            return a.TargetObject.LocalId < b.TargetObject.LocalId;
        });
    }
    return result;
}

Array<AssetDatabaseDependencyInfo> AssetDatabaseQueryService::GetReferencers(const AssetObjectId& objectID)
{
    Array<AssetDatabaseDependencyInfo> result;
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return result;
    const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
    if (!snapshot.IsValid())
        return result;
    for (const SourceAssetDependencyRow& dependency : snapshot.GetState().Dependencies)
    {
        if (dependency.TargetAssetGuid == objectID.Asset.Value && dependency.TargetLocalFileId == objectID.LocalId)
            result.Add(ToInfo(dependency));
    }
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetDatabaseDependencyInfo& a, const AssetDatabaseDependencyInfo& b)
        {
            if (a.Owner.Asset.Value != b.Owner.Asset.Value)
                return a.Owner.Asset.ToString() < b.Owner.Asset.ToString();
            if (a.Owner.LocalId != b.Owner.LocalId)
                return a.Owner.LocalId < b.Owner.LocalId;
            if (a.Kind != b.Kind)
                return a.Kind < b.Kind;
            return a.TargetID < b.TargetID;
        });
    }
    return result;
}

Array<AssetDatabasePublicationInfo> AssetDatabaseQueryService::GetPublications(const AssetObjectId& objectID)
{
    Array<AssetDatabasePublicationInfo> result;
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return result;
    const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
    if (!snapshot.IsValid())
        return result;
    for (const SourceAssetPublicationRow& publication : snapshot.GetState().Publications)
    {
        if (publication.AssetGuid == objectID.Asset.Value && publication.LocalFileId == objectID.LocalId)
            result.Add(ToInfo(publication));
    }
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetDatabasePublicationInfo& a, const AssetDatabasePublicationInfo& b)
        {
            return a.TargetID < b.TargetID;
        });
    }
    return result;
}

Array<AssetPipelineDiagnostic> AssetDatabaseQueryService::GetDiagnostics()
{
    ScopeLock lock(StateLocker);
    return LastDiagnostics;
}

AssetDatabaseChangeInfo AssetDatabaseQueryService::GetLastChange()
{
    const uint64 revision = GetRevision();
    bool requiresSnapshot;
    Array<AssetDatabaseChangeInfo> changes = GetChangesAfter(revision ? revision - 1 : 0, requiresSnapshot);
    return changes.HasItems() ? changes.Last() : AssetDatabaseChangeInfo();
}

Array<AssetDatabaseChangeInfo> AssetDatabaseQueryService::GetChangesAfter(uint64 revision, bool& requiresSnapshot)
{
    Array<AssetDatabaseChangeInfo> result;
    Array<AssetChangeSet> changes;
    AssetPipelineDiagnostic diagnostic;
    requiresSnapshot = false;
    if (AssetDatabase::Get().ReadChangesAfter(revision, changes, requiresSnapshot, diagnostic))
        return result;
    result.EnsureCapacity(changes.Count());
    for (const AssetChangeSet& change : changes)
        result.Add(ToChangeInfo(change));
    return result;
}

Guid AssetDatabaseQueryService::AssetPathToGUID(const StringView& path)
{
    if (path.IsEmpty() || EnsureDatabaseLoaded())
        return Guid::Empty;
    String key = ResolveFacadeAssetPath(path).ToLower();
    key.Replace(TEXT('\\'), TEXT('/'));
    AssetRecord record;
    return AssetDatabase::Get().TryGetMainRecordByPath(key, record) ? record.SourceAssetID : Guid::Empty;
}

String AssetDatabaseQueryService::GUIDToAssetPath(const Guid& assetID)
{
    if (!assetID.IsValid() || EnsureDatabaseLoaded())
        return String::Empty;
    AssetRecord record;
    return AssetDatabase::Get().TryGetRecord(assetID, record)
        ? ToLogicalAssetPath(record.CanonicalPath.Get())
        : String::Empty;
}

Array<String> AssetDatabaseQueryService::GetAllAssetPaths()
{
    Array<String> result;
    if (EnsureDatabaseLoaded())
        return result;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset())
            result.Add(ToLogicalAssetPath(record.CanonicalPath.Get()));
    }
    if (result.Count() > 1)
        std::sort(result.Get(), result.Get() + result.Count());
    return result;
}

bool AssetDatabaseQueryService::TryGetAssetObjectId(Asset* asset, AssetObjectId& result)
{
    result = AssetObjectId();
    if (!asset || EnsureDatabaseLoaded())
        return false;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(asset->GetID(), record))
        return false;
    result = AssetObjectId(AssetGuid(record.SourceAssetID), record.LocalId);
    return true;
}

Guid AssetDatabaseQueryService::GetBackingAssetID(const AssetObjectId& objectID)
{
    if (!objectID.IsValid() || EnsureDatabaseLoaded())
        return Guid::Empty;
    AssetRecord record;
    return AssetDatabase::Get().TryGetRecord(objectID, record) ? record.ID : Guid::Empty;
}

String AssetDatabaseQueryService::GetCanonicalSourcePath(const Guid& assetID)
{
    AssetRecord record;
    return assetID.IsValid() && AssetDatabase::Get().TryGetRecord(assetID, record)
        ? String(record.SourcePath.Get())
        : String::Empty;
}

Asset* AssetDatabaseQueryService::LoadAssetPreview(const AssetObjectId& objectID)
{
#if USE_EDITOR
    return Content::LoadAsyncPreview(objectID, Asset::TypeInitializer);
#else
    return nullptr;
#endif
}

bool AssetPipelineService::Scan(bool strictMetadata)
{
#if USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    EnsureBound();
    if (Initialize())
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    Array<AssetRecord> selected;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = RunGenericBuildRefresh(selected, false, false, AssetRefreshReason::Filesystem, diagnostic,
        GenericRefreshScanMode::ScanOnly, strictMetadata);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    AssetPipelineDiagnostic diagnostic;
    return ReconcileFullScan(strictMetadata, Guid::Empty, 0, diagnostic);
#endif
#else
    return true;
#endif
}

bool AssetPipelineService::LoadOrScan(bool strictMetadata)
{
    EnsureBound();
    if (Initialize())
        return true;
    return Scan(strictMetadata);
}

bool AssetPipelineService::RefreshSources(const Array<String>& paths, bool ensureDefaultMetadata)
{
    AssetPipelineDiagnostic diagnostic;
    return RefreshSources(paths, ensureDefaultMetadata, diagnostic);
}

bool AssetPipelineService::RefreshSources(const Array<String>& paths, bool ensureDefaultMetadata,
    AssetPipelineDiagnostic& failureDiagnostic)
{
#if USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    EnsureBound();
    failureDiagnostic = AssetPipelineDiagnostic();
    const auto setFailureDiagnostic = [&paths, &failureDiagnostic](const Array<AssetPipelineDiagnostic>& candidates,
        const StringView& fallback)
    {
        for (int32 i = candidates.Count() - 1; i >= 0; i--)
        {
            for (const String& path : paths)
            {
                if (FileSystem::AreFilePathsEquivalent(candidates[i].SourcePath, path))
                {
                    failureDiagnostic = candidates[i];
                    return;
                }
            }
        }
        if (candidates.HasItems())
        {
            failureDiagnostic = candidates.Last();
            return;
        }
        failureDiagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        failureDiagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        if (paths.HasItems())
            failureDiagnostic.SourcePath = paths[0];
        failureDiagnostic.Message = fallback;
    };
    if (Initialize())
    {
        setFailureDiagnostic(AssetDatabaseQueryService::GetDiagnostics(), TEXT("Asset database initialization failed during source refresh."));
        return true;
    }
    if (paths.IsEmpty())
        return false;

    Array<AssetPipelineDiagnostic> metadataDiagnostics;
    if (ensureDefaultMetadata)
    {
        for (const String& requestedPath : paths)
        {
            String sourcePath = NormalizeAbsolutePath(requestedPath);
            if (IsMetaPath(sourcePath))
                sourcePath = sourcePath.Left(sourcePath.Length() - 5);
            if (IsV3MetadataExcluded(sourcePath) || !AssetPathPolicy::IsSameOrChild(sourcePath, Globals::ProjectContentFolder))
                continue;
            if (FileSystem::DirectoryExists(sourcePath))
                EnsureV3MetadataForRoot(sourcePath, metadataDiagnostics);
            AssetPipelineDiagnostic diagnostic;
            if (EnsureDefaultCanonicalMetadata(sourcePath, diagnostic))
                metadataDiagnostics.Add(MoveTemp(diagnostic));
        }
    }

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
        if (FileSystem::DirectoryExists(normalized))
        {
            AddUniquePath(normalized, affectedKeys, expanded);
            AddUniquePath(normalized + TEXT(".meta"), affectedKeys, expanded);
            Array<String> directories;
            if (!CollectSourceDirectories(normalized, directories))
            {
                for (const String& directory : directories)
                {
                    AddUniquePath(directory, affectedKeys, expanded);
                    AddUniquePath(directory + TEXT(".meta"), affectedKeys, expanded);
                }
            }
        }
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
        if (!FileSystem::FileExists(path) && !FileSystem::DirectoryExists(path))
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
    {
        setFailureDiagnostic(result.Diagnostics, TEXT("Project source collection failed during targeted refresh."));
        return true;
    }
    if (engineFiles.Count())
    {
        AssetDatabaseScanResult engineResult;
        Array<AssetRecord> engineRecords;
        if (AssetDatabaseScanner::CollectFromFiles(Globals::StartupFolder, engineRoot, Globals::ProjectLibraryFolder,
            engineFiles, options, previous, engineRecords, engineResult))
        {
            setFailureDiagnostic(engineResult.Diagnostics, TEXT("Engine source collection failed during targeted refresh."));
            return true;
        }
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

    Array<AssetPipelineDiagnostic> diagnostics;
    diagnostics.Add(result.Diagnostics);
    diagnostics.Add(metadataDiagnostics);
    MergeScopedDiagnostics(affectedKeys, diagnostics);
    diagnostics = AssetDatabaseQueryService::GetDiagnostics();

    Array<AssetDatabaseFileState> nextStates;
    nextStates.EnsureCapacity(LastFileStates.Count() + result.FileStates.Count());
    HashSet<String> nextStateKeys;
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
        nextStateKeys.Add(key);
    }
    const AssetDatabaseReadSnapshot durable = AssetDatabase::Get().GetDurableSnapshot();
    if (durable.IsValid())
    {
        for (const SourceAssetRow& source : durable.GetState().Sources)
        {
            const String key = PathKey(source.Path);
            if (source.SourceHash.IsZero() || affectedKeys.Contains(key) || nextStateKeys.Contains(key))
                continue;
            AssetDatabaseFileState state;
            state.Path = source.Path;
            state.Size = source.SourceSize;
            state.LastWriteTicks = source.SourceMtimeHint;
            state.CachedContentHash = source.SourceHash;
            nextStates.Add(MoveTemp(state));
            nextStateKeys.Add(key);
        }
    }
    nextStates.Add(result.FileStates);

    AssetPipelineDiagnostic publishDiagnostic;
    if (AssetDatabase::Get().ReconcileScanRows(merged, diagnostics, nextStates, publishDiagnostic))
    {
        failureDiagnostic = publishDiagnostic;
        diagnostics.Add(MoveTemp(publishDiagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }

    LastFileStates = MoveTemp(nextStates);
#if COMPILE_WITH_ASSETS_IMPORTER
    if (AssetRefreshCoordinator* coordinator = AssetImportService::GetRefreshCoordinator())
        coordinator->RequestRefresh(AssetRefreshReason::Filesystem);
#endif
    failureDiagnostic = AssetPipelineDiagnostic();
    return false;
#else
    failureDiagnostic = AssetPipelineDiagnostic();
    failureDiagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
    failureDiagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
    failureDiagnostic.Message = TEXT("Targeted source refresh requires editor support.");
    return true;
#endif
}

bool AssetPipelineService::ImportAsset(const StringView& path, ImportAssetOptions options)
{
#if USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    if (path.IsEmpty())
        return true;
    String resolved = ResolveFacadeAssetPath(path);
    if (IsMetaPath(resolved))
        resolved = resolved.Left(resolved.Length() - 5);
    if (!IsFacadeAssetPath(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::PathCollision;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Asset import path must be under Content or EngineContent.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    if (!FileSystem::FileExists(resolved) && !FileSystem::DirectoryExists(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Asset import source does not exist.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }
    const bool isDirectory = FileSystem::DirectoryExists(resolved);
    if (isDirectory && !EnumHasAnyFlags(options, ImportAssetOptions::ImportRecursive))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Folder import requires ImportRecursive.");
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(MoveTemp(diagnostic));
        SetDiagnostics(diagnostics);
        return true;
    }

    Array<String> sources;
    if (isDirectory)
    {
        if (FileSystem::DirectoryGetFiles(sources, resolved, TEXT("*"), DirectorySearchOption::AllDirectories))
            return true;
        if (sources.Count() > 1)
            std::sort(sources.Get(), sources.Get() + sources.Count());
    }
    else if (FileSystem::FileExists(resolved))
    {
        sources.Add(resolved);
    }

    Array<AssetPipelineDiagnostic> preparationDiagnostics;
    for (const String& source : sources)
    {
        if (IsMetaPath(source) || IsV3MetadataExcluded(source) || !AssetPathPolicy::IsSameOrChild(source, Globals::ProjectContentFolder))
            continue;
        AssetPipelineDiagnostic diagnostic;
        if (EnsureDefaultCanonicalMetadata(source, diagnostic))
            preparationDiagnostics.Add(MoveTemp(diagnostic));
    }
    if (preparationDiagnostics.HasItems())
    {
        SetDiagnostics(preparationDiagnostics);
        return true;
    }

    Array<String> refreshPaths;
    refreshPaths.Add(resolved);
    if (RefreshSources(refreshPaths))
        return true;
#if COMPILE_WITH_ASSETS_IMPORTER
    const bool force = EnumHasAnyFlags(options, ImportAssetOptions::ForceUpdate) ||
        EnumHasAnyFlags(options, ImportAssetOptions::ForceUncompressedImport);
    const bool synchronous = EnumHasAnyFlags(options, ImportAssetOptions::ForceSynchronousImport);
    bool matched = false;
    Array<AssetRecord> selected;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (!record.IsMainAsset())
            continue;
        const bool pathMatches = isDirectory
            ? PathIsUnder(record.SourcePath.Get(), resolved)
            : FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved);
        if (!pathMatches)
            continue;
        matched = true;
        if (record.Status == AssetRecordStatus::UnsupportedProcessor)
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = record.ID;
            diagnostic.SourcePath = record.SourcePath.Get();
            diagnostic.Message = TEXT("Imported source has no registered asset processor.");
            SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
            return true;
        }
        if (SupportsGenericBuild(record))
            selected.Add(record);
    }
    HashSet<Guid> selectedIds;
    for (const AssetRecord& record : selected)
        selectedIds.Add(record.ID);
    for (int32 index = 0; index < selected.Count(); index++)
    {
        Array<AssetRecord> dependants;
        AssetDatabase::Get().GetBuildDependants(AssetObjectId::Main(AssetGuid(selected[index].SourceAssetID)), dependants);
        for (const AssetRecord& dependant : dependants)
        {
            if (dependant.IsMainAsset() && SupportsGenericBuild(dependant) && selectedIds.Add(dependant.ID))
                selected.Add(dependant);
        }
    }
    AssetPipelineDiagnostic buildDiagnostic;
    if (RunGenericBuildRefresh(selected, force, synchronous, AssetRefreshReason::Explicit, buildDiagnostic))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ buildDiagnostic }));
        return true;
    }
    if (!matched && FileSystem::FileExists(resolved))
    {
        AssetPipelineDiagnostic diagnostic;
        diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Imported source has no registered asset processor.");
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
#endif
    return false;
#else
    return true;
#endif
}

bool AssetPipelineService::Refresh(ImportAssetOptions options)
{
#if USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    EnsureBound();
    if (Initialize())
        return true;
    Array<String> sources;
    if (FileSystem::DirectoryGetFiles(sources, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
        return true;
    Array<AssetPipelineDiagnostic> preparationDiagnostics;
    for (const String& source : sources)
    {
        if (IsMetaPath(source) || IsV3MetadataExcluded(source))
            continue;
        AssetPipelineDiagnostic diagnostic;
        if (EnsureDefaultCanonicalMetadata(source, diagnostic))
            preparationDiagnostics.Add(MoveTemp(diagnostic));
    }
    if (preparationDiagnostics.HasItems())
    {
        SetDiagnostics(preparationDiagnostics);
        return true;
    }
#if COMPILE_WITH_ASSETS_IMPORTER
    const bool force = EnumHasAnyFlags(options, ImportAssetOptions::ForceUpdate) ||
        EnumHasAnyFlags(options, ImportAssetOptions::ForceUncompressedImport);
    const bool synchronous = EnumHasAnyFlags(options, ImportAssetOptions::ForceSynchronousImport);
    Array<AssetRecord> selected;
    AssetPipelineDiagnostic buildDiagnostic;
    if (RunGenericBuildRefresh(selected, force, synchronous, AssetRefreshReason::Explicit, buildDiagnostic,
        GenericRefreshScanMode::ScanAndBuildAll, false))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ buildDiagnostic }));
        return true;
    }
#else
    AssetPipelineDiagnostic scanDiagnostic;
    if (ReconcileFullScan(false, Guid::Empty, 0, scanDiagnostic))
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ scanDiagnostic }));
        return true;
    }
#endif
    return false;
#else
    return true;
#endif
}

bool AssetOperationService::MoveAsset(const StringView& sourcePath, const StringView& destinationPath)
{
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    const String resolved = ResolveFacadeAssetPath(sourcePath);
    AssetOperationTarget target;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset() && FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved))
        {
            target.SourcePath = record.SourcePath.Get();
            target.ExpectedGuid = record.SourceAssetID;
            break;
        }
    }
    AssetPipelineDiagnostic diagnostic;
    if (!target.ExpectedGuid.IsValid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Canonical move source is not registered as a main source asset.");
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    const bool failed = Operations->MoveAsset(target, ResolveFacadeAssetPath(destinationPath), diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::CopyAsset(const StringView& sourcePath, const StringView& destinationPath, Guid& copiedGuid)
{
    copiedGuid = Guid::Empty;
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    const String resolved = ResolveFacadeAssetPath(sourcePath);
    AssetOperationTarget target;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset() && FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved))
        {
            target.SourcePath = record.SourcePath.Get();
            target.ExpectedGuid = record.SourceAssetID;
            break;
        }
    }
    AssetPipelineDiagnostic diagnostic;
    if (!target.ExpectedGuid.IsValid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Canonical copy source is not registered as a main source asset.");
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    const bool failed = Operations->CopyAsset(target, ResolveFacadeAssetPath(destinationPath), copiedGuid, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::CopyAssets(const Array<AssetCopyEntryRequest>& entries, Array<Guid>& copiedGuids)
{
    copiedGuids.Clear();
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations || entries.IsEmpty())
        return true;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    Array<AssetCopyEntryRequest> resolvedEntries;
    resolvedEntries.EnsureCapacity(entries.Count());
    for (const AssetCopyEntryRequest& entry : entries)
    {
        AssetCopyEntryRequest resolvedEntry = entry;
        resolvedEntry.SourcePath = ResolveFacadeAssetPath(entry.SourcePath);
        resolvedEntry.DestinationPath = ResolveFacadeAssetPath(entry.DestinationPath);
        if (entry.Kind != AssetCopyEntryKind::CanonicalAsset)
        {
            if (entry.ExpectedAssetGuid.IsValid())
            {
                AssetPipelineDiagnostic diagnostic;
                diagnostic.Code = AssetPipelineDiagnosticCode::InvalidSettingsCombination;
                diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
                diagnostic.SourcePath = resolvedEntry.SourcePath;
                diagnostic.Message = TEXT("Only canonical copy entries may specify an expected asset GUID.");
                SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
                return true;
            }
            resolvedEntries.Add(MoveTemp(resolvedEntry));
            continue;
        }
        bool found = false;
        for (const AssetRecord& record : snapshot.Records)
        {
            if (record.IsMainAsset() &&
                FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolvedEntry.SourcePath) &&
                (!entry.ExpectedAssetGuid.IsValid() || entry.ExpectedAssetGuid == record.SourceAssetID))
            {
                resolvedEntry.SourcePath = record.SourcePath.Get();
                resolvedEntry.ExpectedAssetGuid = record.SourceAssetID;
                found = true;
                break;
            }
        }
        if (!found)
        {
            AssetPipelineDiagnostic diagnostic;
            diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.SourcePath = resolvedEntry.SourcePath;
            diagnostic.Message = TEXT("Canonical copy batch source is not registered as a main source asset.");
            SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
            return true;
        }
        resolvedEntries.Add(MoveTemp(resolvedEntry));
    }
    AssetPipelineDiagnostic diagnostic;
    const bool failed = Operations->CopyAssets(resolvedEntries, copiedGuids, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::DeleteAsset(const StringView& sourcePath)
{
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    const String resolved = ResolveFacadeAssetPath(sourcePath);
    AssetOperationTarget target;
    const AssetDatabaseSnapshot snapshot = AssetDatabase::Get().GetSnapshot();
    for (const AssetRecord& record : snapshot.Records)
    {
        if (record.IsMainAsset() && FileSystem::AreFilePathsEqual(record.SourcePath.Get(), resolved))
        {
            target.SourcePath = record.SourcePath.Get();
            target.ExpectedGuid = record.SourceAssetID;
            break;
        }
    }
    AssetPipelineDiagnostic diagnostic;
    if (!target.ExpectedGuid.IsValid())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = resolved;
        diagnostic.Message = TEXT("Canonical delete source is not registered as a main source asset.");
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    }
    AssetTrashRecord trash;
    const bool failed = Operations->DeleteAsset(target, trash, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::TrashEntries(const Array<AssetTrashEntryRequest>& entries, AssetTrashBatch& trash)
{
    trash = AssetTrashBatch();
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = Operations->TrashEntries(entries, trash, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::RestoreEntries(const AssetTrashBatch& trash)
{
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = Operations->RestoreEntries(trash, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::DiscardTrash(const AssetTrashBatch& trash)
{
#if USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = Operations->DiscardTrash(trash, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetOperationService::GetImporterSettings(const Guid& sourceAssetID, AssetImporterSettingsSnapshot& result)
{
    result = AssetImporterSettingsSnapshot();
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (AssetPipelineService::Initialize())
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = ReadImporterSettingsSnapshot(sourceAssetID, result, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    AssetPipelineDiagnostic diagnostic;
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.AssetGuid = sourceAssetID;
    diagnostic.Message = TEXT("Importer settings are unavailable without the editor importer registry.");
    SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return true;
#endif
}

AssetImporterSettingsSaveResult AssetOperationService::SaveImporterSettingsAndReimportDetailed(
    const AssetImporterSettingsSnapshot& expected, const StringView& settingsJson)
{
    AssetImporterSettingsSaveResult result;
    auto fail = [&result](AssetImporterSettingsWriteOutcome outcome, const AssetPipelineDiagnostic& diagnostic)
    {
        result.WriteOutcome = outcome;
        result.Diagnostic = diagnostic;
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return result;
    };
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (AssetPipelineService::Initialize() || !Operations)
    {
        const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
        AssetPipelineDiagnostic diagnostic = diagnostics.HasItems() ? diagnostics.Last() : AssetPipelineDiagnostic();
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = expected.SourceAssetID;
            diagnostic.Message = TEXT("Asset operations are unavailable for importer settings.");
        }
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    AssetPipelineDiagnostic diagnostic;
    if (Operations->IsAssetEditing())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = expected.SourceAssetID;
        diagnostic.Message = TEXT("Importer settings cannot be saved inside an asset editing batch.");
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    const AssetDatabaseReadSnapshot snapshot = AssetDatabase::Get().GetDurableSnapshot();
    SourceAssetRow source;
    if (!snapshot.IsValid() || !snapshot.TryGetSource(expected.SourceAssetID, source) || source.IsFolder ||
        source.SourceKind != AssetSourceKind::ImportedSource)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = expected.SourceAssetID;
        diagnostic.Message = TEXT("Importer settings source is no longer registered as an imported asset.");
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    if (expected.SourceRevision == 0 || expected.ImporterID.IsEmpty() || expected.StoredSettingsVersion < 1 ||
        expected.SettingsSchemaVersion < 1)
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = expected.SourceAssetID;
        diagnostic.SourcePath = source.Path;
        diagnostic.Message = TEXT("Importer settings revision is invalid.");
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    if (source.LastModifiedRevision != expected.SourceRevision || source.MetaSemanticHash != expected.MetaSemanticHash ||
        source.ImporterId != expected.ImporterID ||
        source.ImporterSettingsVersion != static_cast<uint32>(expected.StoredSettingsVersion))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = expected.SourceAssetID;
        diagnostic.SourcePath = source.Path;
        diagnostic.ProcessorId = source.ImporterId;
        diagnostic.Message = TEXT("Importer settings write conflicts with a newer source metadata revision.");
        AssetPipelineDiagnostic ignored;
        ReadImporterSettingsSnapshot(expected.SourceAssetID, result.Current, ignored);
        return fail(AssetImporterSettingsWriteOutcome::Conflict, diagnostic);
    }

    AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
    AssetImporterLease importer;
    if (!registry || registry->TryAcquire(expected.ImporterID, importer, diagnostic))
    {
        if (diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
            diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
            diagnostic.AssetGuid = expected.SourceAssetID;
            diagnostic.SourcePath = source.Path;
            diagnostic.ProcessorId = expected.ImporterID;
            diagnostic.Message = TEXT("Importer settings processor is not registered.");
        }
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    if (importer.Get().SettingsSchemaVersion > MAX_int32 ||
        expected.SettingsSchemaVersion != static_cast<int32>(importer.Get().SettingsSchemaVersion))
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::PrepareInvalidated;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = expected.SourceAssetID;
        diagnostic.SourcePath = source.Path;
        diagnostic.ProcessorId = expected.ImporterID;
        diagnostic.Message = TEXT("Importer settings schema changed after the editor revision was captured.");
        AssetPipelineDiagnostic ignored;
        ReadImporterSettingsSnapshot(expected.SourceAssetID, result.Current, ignored);
        return fail(AssetImporterSettingsWriteOutcome::Conflict, diagnostic);
    }

    AssetOperationTarget target;
    target.SourcePath = source.Path;
    target.ExpectedGuid = expected.SourceAssetID;
    AssetImporterSettingsRevision revision;
    revision.SourceRevision = expected.SourceRevision;
    revision.MetaSemanticHash = expected.MetaSemanticHash;
    revision.ImporterID = expected.ImporterID;
    revision.StoredSettingsVersion = expected.StoredSettingsVersion;
    const StringAnsi settings(settingsJson);
    bool wasChanged = false;
    bool wasConflict = false;
    const bool failed = Operations->WriteImporterSettings(target, revision, expected.SettingsSchemaVersion,
        StringAnsiView(settings), diagnostic, AssetMetaWriteFailurePoint::None, &wasChanged, &wasConflict);
    importer.Reset();
    if (failed)
    {
        AssetPipelineDiagnostic ignored;
        ReadImporterSettingsSnapshot(expected.SourceAssetID, result.Current, ignored);
        return fail(wasConflict ? AssetImporterSettingsWriteOutcome::Conflict : AssetImporterSettingsWriteOutcome::Failed,
            diagnostic);
    }
    if (ReadImporterSettingsSnapshot(expected.SourceAssetID, result.Current, diagnostic))
    {
        if (wasChanged)
        {
            result.WriteOutcome = AssetImporterSettingsWriteOutcome::Committed;
            result.ReimportOutcome = AssetImporterSettingsReimportOutcome::Blocked;
            result.Diagnostic = diagnostic;
            SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
            return result;
        }
        return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
    }
    result.WriteOutcome = wasChanged
        ? AssetImporterSettingsWriteOutcome::Committed
        : AssetImporterSettingsWriteOutcome::Unchanged;
    if (!wasChanged)
        return result;
    if (AssetPipelineService::BuildAsset(expected.SourceAssetID, true, false))
    {
        result.ReimportOutcome = AssetImporterSettingsReimportOutcome::Failed;
        result.Diagnostic = AssetPipelineService::GetBuildDiagnostic(expected.SourceAssetID);
        if (result.Diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            const Array<AssetPipelineDiagnostic> diagnostics = AssetDatabaseQueryService::GetDiagnostics();
            if (diagnostics.HasItems())
                result.Diagnostic = diagnostics.Last();
        }
        if (result.Diagnostic.Code == AssetPipelineDiagnosticCode::None)
        {
            result.Diagnostic.Code = AssetPipelineDiagnosticCode::BuildFailed;
            result.Diagnostic.Stage = AssetPipelineDiagnosticStage::Build;
            result.Diagnostic.AssetGuid = expected.SourceAssetID;
            result.Diagnostic.Message = TEXT("Importer settings were committed, but reimport could not be queued.");
        }
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ result.Diagnostic }));
        return result;
    }
    result.ReimportOutcome = AssetImporterSettingsReimportOutcome::Queued;
    return result;
#else
    AssetPipelineDiagnostic diagnostic;
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.AssetGuid = expected.SourceAssetID;
    diagnostic.Message = TEXT("Importer settings are unavailable without the editor importer registry.");
    return fail(AssetImporterSettingsWriteOutcome::Failed, diagnostic);
#endif
}

bool AssetOperationService::SaveImporterSettingsAndReimport(const AssetImporterSettingsSnapshot& expected,
    const StringView& settingsJson, AssetImporterSettingsSnapshot& current)
{
    const AssetImporterSettingsSaveResult result = SaveImporterSettingsAndReimportDetailed(expected, settingsJson);
    current = result.Current;
    return (result.WriteOutcome != AssetImporterSettingsWriteOutcome::Committed &&
            result.WriteOutcome != AssetImporterSettingsWriteOutcome::Unchanged) ||
        result.ReimportOutcome == AssetImporterSettingsReimportOutcome::Failed ||
        result.ReimportOutcome == AssetImporterSettingsReimportOutcome::Blocked;
}

void AssetOperationService::StartEditing()
{
#if USE_EDITOR
    if (!AssetPipelineService::Initialize() && Operations)
        Operations->StartAssetEditing();
#endif
}

bool AssetOperationService::StopEditing()
{
#if USE_EDITOR
    if (!Operations)
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = Operations->StopAssetEditing(diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetPipelineService::BuildAsset(const Guid& assetID, bool force, bool synchronous)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    if (Initialize())
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset())
        return false;
    Array<AssetRecord> selected;
    selected.Add(record);
    AssetPipelineDiagnostic diagnostic;
    const bool failed = RunGenericBuildRefresh(selected, force, synchronous, AssetRefreshReason::Explicit, diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetPipelineService::BuildAssetForeground(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    std::lock_guard<std::recursive_mutex> refreshLock(RefreshLocker);
    if (Initialize())
        return true;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset())
        return false;
    AssetPipelineDiagnostic diagnostic;
    const GenericBuildRequestResult request = RequestGenericBuild(record, false, false, Guid::Empty, 0,
        AssetBuildJobPriority::Foreground, diagnostic);
    const bool failed = request == GenericBuildRequestResult::Failed;
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetPipelineService::RebuildAsset(const Guid& assetID, bool synchronous)
{
    return BuildAsset(assetID, true, synchronous);
}

String AssetPipelineService::GetBuildStatus(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetRecord record;
    AssetPipelineDiagnostic diagnostic;
    AssetImporterLease importer;
    AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !registry ||
        registry->TryAcquire(record.ProcessorID, importer, diagnostic) ||
        !importer.Get().GetBuildStatus.IsBinded())
        return TEXT("NotBuilt");
    switch (importer.Get().GetBuildStatus(assetID, diagnostic))
    {
    case AssetBuildJobStatus::Queued: return TEXT("Queued");
    case AssetBuildJobStatus::Building: return TEXT("Building");
    case AssetBuildJobStatus::Publishing: return TEXT("Publishing");
    case AssetBuildJobStatus::Succeeded: return TEXT("ReadyExact");
    case AssetBuildJobStatus::Failed: return TEXT("Failed");
    case AssetBuildJobStatus::Cancelled: return TEXT("Cancelled");
    default: return TEXT("NotBuilt");
    }
#else
    return TEXT("NotBuilt");
#endif
}

AssetPipelineDiagnostic AssetPipelineService::GetBuildDiagnostic(const Guid& assetID)
{
    AssetPipelineDiagnostic diagnostic;
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    AssetRecord record;
    AssetImporterLease importer;
    AssetImporterRegistry* registry = AssetImportService::GetImporterRegistry();
    if (AssetDatabase::Get().TryGetRecord(assetID, record) && registry &&
        !registry->TryAcquire(record.ProcessorID, importer, diagnostic) &&
        importer.Get().GetBuildStatus.IsBinded())
        importer.Get().GetBuildStatus(assetID, diagnostic);
#endif
    return diagnostic;
}

bool AssetPipelineService::IsArtifactCurrent(const Guid& assetID)
{
#if COMPILE_WITH_ASSETS_IMPORTER && USE_EDITOR
    if (!ArtifactResolver::Get().IsConfigured())
        return false;
    AssetRecord record;
    if (!AssetDatabase::Get().TryGetRecord(assetID, record) || !record.IsMainAsset() || record.Status != AssetRecordStatus::Ready)
        return false;

    Array<AssetRecord> records;
    records.Add(record);
#if COMPILE_WITH_MODEL_TOOL
    if (record.ProcessorID == ModelProcessorSettings::ProcessorID())
    {
        Array<AssetRecord> children;
        AssetDatabase::Get().GetSubAssets(record.SourceAssetID, children);
        records.Add(children);
    }
#endif
    for (const AssetRecord& current : records)
    {
        ArtifactRequest request;
        request.Object = AssetObjectId(AssetGuid(current.SourceAssetID), current.LocalId);
        request.Target = ArtifactResolver::Get().GetDefaultTarget();
        request.OutputKind = "runtime";
        request.Policy = ArtifactResolvePolicy::NoBuild;
#if COMPILE_WITH_TEXTURE_TOOL
        if (current.ProcessorID == TextureProcessorSettings::ProcessorID())
            request.RequiredCompatibility = "flax-texture-v4";
#endif
#if COMPILE_WITH_MODEL_TOOL
        if (current.ProcessorID == ModelProcessorSettings::ProcessorID())
            request.RequiredCompatibility = "flax-model-runtime-v1";
#endif
        AssetProcessorDescriptor processorDescriptor;
        if (request.RequiredCompatibility.IsEmpty() &&
            AssetProcessorRegistry::Get().TryGetDescriptor(current.ProcessorID, processorDescriptor) && processorDescriptor.Outputs.Count())
            request.RequiredCompatibility = processorDescriptor.Outputs[0].CompatibilityTag;
        ResolvedArtifact artifact;
        AssetPipelineDiagnostic diagnostic;
        if (ArtifactResolver::Get().Resolve(request, artifact, diagnostic) || !artifact.IsExact)
            return false;
    }
    return true;
#else
    return false;
#endif
}

bool AssetPipelineService::CleanLibrary()
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

bool AssetPipelineService::CheckpointDatabase()
{
#if USE_EDITOR
    if (Initialize())
        return true;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = AssetDatabase::Get().Checkpoint(diagnostic);
    if (failed)
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
    return failed;
#else
    return true;
#endif
}

bool AssetPipelineService::CleanUnusedArtifacts(AssetArtifactCleanupInfo& result)
{
#if USE_EDITOR
    ArtifactGCOptions options;
    options.GracePeriod = TimeSpan::Zero();
    options.MaximumDeletes = MAX_int32;
    options.MaximumDeleteBytes = MAX_uint64;
    ArtifactGCResult gcResult;
    AssetPipelineDiagnostic diagnostic;
    const bool failed = ArtifactGC::Run(Globals::ProjectLibraryFolder, options, gcResult, diagnostic);
    result.TotalArtifactBytes = gcResult.TotalArtifactBytes;
    result.ReachableBytes = gcResult.ReachableBytes;
    result.CandidateBytes = gcResult.CandidateBytes;
    result.ReclaimedBytes = gcResult.ReclaimedBytes;
    result.ScannedFiles = gcResult.ScannedFiles;
    result.ReachableFiles = gcResult.ReachableFiles;
    result.LeasedFiles = gcResult.LeasedFiles;
    result.CandidateFiles = gcResult.CandidateFiles;
    result.DeletedFiles = gcResult.DeletedFiles;
    result.BlockedByInvalidManifest = gcResult.BlockedByInvalidManifest;
    for (const String& path : gcResult.DeletedPaths)
    {
        if (result.DeletedPaths.HasChars())
            result.DeletedPaths += '\n';
        result.DeletedPaths += path;
    }
    result.Diagnostics = MoveTemp(gcResult.Diagnostics);
    if (diagnostic.Code != AssetPipelineDiagnosticCode::None)
        result.Diagnostics.Add(diagnostic);
    SetDiagnostics(result.Diagnostics);
    return failed;
#else
    result = AssetArtifactCleanupInfo();
    return true;
#endif
}

bool AssetOperationService::CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath)
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
Array<Guid> AssetOperationService::StageDefaultMetadataBatch(const Array<String>& sourcePaths, const Array<String>& stagingPaths)
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

bool AssetOperationService::PublishDefaultMetadataBatch(const Array<Guid>& assetIDs, const Array<String>& sourcePaths)
{
    if (sourcePaths.Count() ? AssetPipelineService::RefreshSources(sourcePaths) : AssetPipelineService::Scan(false))
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
        if (GraphPipelineService::OwnsProcessor(record.ProcessorID))
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
Guid TextureImporterService::CreateMetadata(const StringView& sourcePath, const TextureTool::Options& options)
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

bool TextureImporterService::LoadMetadata(const StringView& sourcePath, TextureTool::Options& options)
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

bool TextureImporterService::ApplyMetadata(const StringView& sourcePath, const TextureTool::Options& options)
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

#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
bool ModelImporterService::LoadMetadata(const StringView& sourcePath, ModelTool::Options& options)
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

bool ModelImporterService::ApplyMetadata(const StringView& sourcePath, const ModelTool::Options& options)
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

bool ModelImporterService::ReconcileSubAssets(const Guid& rootAssetID)
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

#endif

Guid AuthoredAssetDocumentService::Create(const StringView& outputPath, const StringView& typeName)
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
    StringAnsi source;
    String processorID;
    const String extension = FileSystem::GetExtension(outputPath).ToLower();
    if (typeName == TEXT("FlaxEngine.MaterialInstance") && extension == TEXT("materialinstance"))
    {
        processorID = TEXT("Flax.MaterialInstance");
        source = "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.MaterialInstance\",\n  \"baseMaterial\": { \"$type\": \"AssetReference\", \"guid\": \"00000000000000000000000000000000\" },\n  \"overrides\": {}\n}\n";
    }
    else if (typeName == TEXT("FlaxEngine.SkeletonMask") && extension == TEXT("skeletonmask"))
    {
        processorID = TEXT("Flax.SkeletonMask");
        source = "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.SkeletonMask\",\n  \"skeleton\": \"00000000000000000000000000000000\",\n  \"maskedNodes\": []\n}\n";
    }
    else if (typeName == TEXT("FlaxEngine.SceneAnimation") && extension == TEXT("sceneanimation"))
    {
        processorID = TEXT("Flax.SceneAnimation");
        source = "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.SceneAnimation\",\n  \"framesPerSecond\": 30.0,\n  \"durationFrames\": 0,\n  \"tracks\": []\n}\n";
    }
    else if (typeName == TEXT("FlaxEngine.ParticleSystem") && extension == TEXT("particlesystem"))
    {
        processorID = TEXT("Flax.ParticleSystem");
        source = "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.ParticleSystem\",\n  \"framesPerSecond\": 60.0,\n  \"durationFrames\": 0,\n  \"tracks\": [],\n  \"parameterOverrides\": []\n}\n";
    }
    else if (typeName == TEXT("FlaxEngine.CollisionData") && extension == TEXT("collisiondata"))
    {
        processorID = TEXT("Flax.CollisionData");
        source = "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.CollisionData\",\n  \"collisionType\": \"None\",\n  \"sourceModel\": null,\n  \"modelLodIndex\": 0,\n  \"materialSlotsMask\": 4294967295,\n  \"convexFlags\": [],\n  \"convexVertexLimit\": 255\n}\n";
    }
    else
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = outputPath;
        diagnostic.Message = TEXT("Authored document type does not match its required source extension.");
        return fail();
    }
    if (outputPath.IsEmpty() || FileSystem::FileExists(outputPath) || FileSystem::FileExists(String(outputPath) + TEXT(".meta")) ||
        GraphDocumentCodec::SaveJsonAtomic(outputPath, source, diagnostic))
        return fail();
    AssetMeta meta;
    meta.ID = Guid::New();
    meta.AssetType = typeName;
    meta.SourceKind = AssetSourceKind::TextDocument;
    meta.Processor.ID = processorID;
    meta.Processor.SettingsVersion = 1;
    meta.Processor.SettingsJson = "{}\n";
    if (AssetMeta::SaveAtomic(String(outputPath) + TEXT(".meta"), meta, diagnostic))
    {
        FileSystem::DeleteFile(outputPath);
        return fail();
    }
    if (RefreshPath(outputPath) || GraphPipelineService::RequestBuildAndWait(meta.ID, true, diagnostic))
        return fail();
    return meta.ID;
#else
    diagnostic.Code = AssetPipelineDiagnosticCode::ProcessorMissing;
    diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
    diagnostic.Message = TEXT("Authored documents require the editor importer.");
    return fail();
#endif
}
bool AuthoredAssetDocumentService::Save(BinaryAsset* asset, const Guid& canonicalAssetID)
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

    rapidjson_flax::Document sourceJson;
    String conversionError;
    Array<StringAnsi> sourceOrder;
    if (record.TypeName == MaterialInstance::TypeName)
    {
        auto* typed = ScriptingObject::Cast<MaterialInstance>(asset);
        if (!typed)
            conversionError = TEXT("Material instance editor state is unavailable.");
        else
        {
            MemoryWriteStream sourceStream(256);
            const MaterialBase* baseMaterial = typed->GetBaseMaterial();
            sourceStream.Write(baseMaterial ? baseMaterial->GetPersistentObjectId().Asset.Value : Guid::Empty);
            typed->Params.Save(&sourceStream);
            if (MaterialInstanceDocument::DecodeLegacy(ToSpan(sourceStream), sourceJson, conversionError))
                sourceJson.SetNull();
        }
        sourceOrder.Add("documentVersion");
        sourceOrder.Add("type");
        sourceOrder.Add("baseMaterial");
        sourceOrder.Add("overrides");
    }
    else if (record.TypeName == SkeletonMask::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SkeletonMask>(asset);
        if (!typed)
            conversionError = TEXT("Skeleton mask editor state is unavailable.");
        else
        {
            sourceJson.SetObject();
            auto& allocator = sourceJson.GetAllocator();
            sourceJson.AddMember("documentVersion", 1, allocator);
            sourceJson.AddMember("type", rapidjson_flax::Value("FlaxEngine.SkeletonMask", allocator), allocator);
            const StringAnsi skeletonText = StringAnsi(typed->Skeleton.GetID().Asset.Value.ToString(Guid::FormatType::N)).ToLower();
            sourceJson.AddMember("skeleton", rapidjson_flax::Value(skeletonText.Get(), skeletonText.Length(), allocator), allocator);
            rapidjson_flax::Value nodes(rapidjson::kArrayType);
            for (const String& name : typed->GetMaskedNodes())
            {
                const StringAnsi text(name);
                nodes.PushBack(rapidjson_flax::Value(text.Get(), text.Length(), allocator), allocator);
            }
            sourceJson.AddMember("maskedNodes", nodes, allocator);
        }
        sourceOrder.Add("documentVersion");
        sourceOrder.Add("type");
        sourceOrder.Add("skeleton");
        sourceOrder.Add("maskedNodes");
    }
    else if (record.TypeName == SceneAnimation::TypeName)
    {
        auto* typed = ScriptingObject::Cast<SceneAnimation>(asset);
        if (!typed || SceneAnimationDocument::DecodeLegacy(
            Span<byte>(typed->LoadTimeline().Get(), typed->LoadTimeline().Length()), sourceJson, conversionError))
            sourceJson.SetNull();
        sourceOrder.Add("documentVersion");
        sourceOrder.Add("type");
        sourceOrder.Add("framesPerSecond");
        sourceOrder.Add("durationFrames");
        sourceOrder.Add("tracks");
    }
    else if (record.TypeName == ParticleSystem::TypeName)
    {
        auto* typed = ScriptingObject::Cast<ParticleSystem>(asset);
        const BytesContainer timeline = typed ? typed->LoadTimeline() : BytesContainer();
        if (!typed || ParticleSystemDocument::DecodeLegacy(
            Span<byte>(timeline.Get(), timeline.Length()), sourceJson, conversionError))
            sourceJson.SetNull();
        sourceOrder.Add("documentVersion");
        sourceOrder.Add("type");
        sourceOrder.Add("framesPerSecond");
        sourceOrder.Add("durationFrames");
        sourceOrder.Add("tracks");
        sourceOrder.Add("parameterOverrides");
    }
    if (sourceJson.IsNull())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.AssetGuid = canonicalAssetID;
        diagnostic.Message = conversionError.IsEmpty() ? TEXT("Authored source conversion failed.") : conversionError;
        return fail();
    }
    StringAnsi sourceText;
    CanonicalJsonError jsonError;
    if (CanonicalJsonWriter::Write(sourceJson, sourceText, jsonError, &sourceOrder) ||
        GraphDocumentCodec::SaveJsonAtomic(record.SourcePath.Get(), sourceText, diagnostic))
        return fail();
    if (GraphPipelineService::RequestBuild(canonicalAssetID, false, diagnostic))
        return fail();
    return false;
#else
    return true;
#endif
}

bool AuthoredAssetDocumentService::SaveMaterialInstance(MaterialInstance* asset, const Guid& sourceAssetID)
{
    return Save(asset, sourceAssetID);
}

bool AuthoredAssetDocumentService::SaveSkeletonMask(SkeletonMask* asset, const Guid& sourceAssetID)
{
    return Save(asset, sourceAssetID);
}

bool AuthoredAssetDocumentService::SaveMaterial(Material* asset, const Guid& canonicalAssetID)
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
    if (AssetDocumentService::SaveGraphSource(record.SourcePath.Get(), surface, StringView::Empty))
        return true;
    return GraphPipelineService::RequestBuild(canonicalAssetID, false, diagnostic) ? fail() : false;
#else
    return true;
#endif
}

bool AuthoredAssetDocumentService::SaveCollisionData(const StringView& path, CollisionDataType type, const Guid& model, int32 modelLodIndex,
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
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveJsonAtomic(path, text, diagnostic))
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

bool AuthoredAssetDocumentService::SaveParticleSystemTimeline(const StringView& path, const BytesContainer& timeline)
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
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) || GraphDocumentCodec::SaveJsonAtomic(path, text, diagnostic))
        return fail();
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.ParticleSystem"))
        return fail();
    return GraphPipelineService::RequestBuild(meta.ID, false, diagnostic) ? fail() : false;
#else
    return true;
#endif
}

BytesContainer AuthoredAssetDocumentService::LoadParticleSystemTimeline(const StringView& path)
{
    BytesContainer result;
#if USE_EDITOR
    Array<byte> bytes;
    rapidjson_flax::Document json;
    String error;
    AssetPipelineDiagnostic diagnostic;
    if (File::ReadAllBytes(path, bytes))
        error = TEXT("Cannot read particle-system source document.");
    else
        json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    Array<byte> timeline;
    if (error.IsEmpty() && (json.HasParseError() || !json.IsObject()))
        error = TEXT("Particle-system source document is malformed.");
    if (error.IsEmpty() && ParticleSystemDocument::Compile(json, timeline, nullptr, error))
        timeline.Clear();
    if (!error.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return result;
    }
    result.Copy(timeline.Get(), timeline.Count());
#endif
    return result;
}

bool AuthoredAssetDocumentService::SaveSceneAnimationTimeline(const StringView& path, const BytesContainer& timeline)
{
#if USE_EDITOR && COMPILE_WITH_ASSETS_IMPORTER
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return true;
    };
    rapidjson_flax::Document json;
    String error;
    if (SceneAnimationDocument::DecodeLegacy(Span<byte>(timeline.Get(), timeline.Length()), json, error))
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
    if (CanonicalJsonWriter::Write(json, text, jsonError, &order) ||
        GraphDocumentCodec::SaveJsonAtomic(path, text, diagnostic))
        return fail();
    AssetMeta meta;
    if (AssetMeta::Load(String(path) + TEXT(".meta"), meta, diagnostic) || meta.Processor.ID != TEXT("Flax.SceneAnimation"))
        return fail();
    return GraphPipelineService::RequestBuild(meta.ID, false, diagnostic) ? fail() : false;
#else
    return true;
#endif
}

BytesContainer AuthoredAssetDocumentService::LoadSceneAnimationTimeline(const StringView& path)
{
    BytesContainer result;
#if USE_EDITOR
    Array<byte> bytes;
    rapidjson_flax::Document json;
    String error;
    AssetPipelineDiagnostic diagnostic;
    if (File::ReadAllBytes(path, bytes))
        error = TEXT("Cannot read scene-animation source document.");
    else
        json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    Array<byte> timeline;
    if (error.IsEmpty() && (json.HasParseError() || !json.IsObject()))
        error = TEXT("Scene-animation source document is malformed.");
    if (error.IsEmpty() && SceneAnimationDocument::Compile(json, timeline, nullptr, error))
        timeline.Clear();
    if (!error.IsEmpty())
    {
        diagnostic.Code = AssetPipelineDiagnosticCode::InvalidMeta;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Prepare;
        diagnostic.SourcePath = path;
        diagnostic.Message = error;
        SetDiagnostics(Array<AssetPipelineDiagnostic>({ diagnostic }));
        return result;
    }
    result.Copy(timeline.Get(), timeline.Count());
#endif
    return result;
}

bool AuthoredAssetDocumentService::LoadCollisionData(const StringView& path, CollisionData::SerializedOptions& options)
{
    Array<byte> bytes;
    if (File::ReadAllBytes(path, bytes))
        return true;
    rapidjson_flax::Document json;
    json.Parse(reinterpret_cast<const char*>(bytes.Get()), bytes.Count());
    String error;
    return json.HasParseError() || CollisionDataDocument::Parse(json, options, error);
}

Guid AssetOperationService::CreateImportedSourceMetadata(const StringView& sourcePath, const StringView& typeName, const StringView& processorId)
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
    meta.SourceKind = processorId == TEXT("Flax.Text") ? AssetSourceKind::TextDocument : AssetSourceKind::ImportedSource;
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
Guid AudioImporterService::CreateMetadata(const StringView& sourcePath, const AudioTool::Options& options)
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

bool AudioImporterService::LoadMetadata(const StringView& sourcePath, AudioTool::Options& options)
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
        ModelProcessor::PrimeAnalysisCache(sourcePath, settings, analysis);

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

Guid ModelImporterService::CreateDefaultMetadata(const StringView& sourcePath)
{
    ModelTool::Options options;
    return CreateModelMetadataInternal(sourcePath, options, true);
}

Guid ModelImporterService::CreateMetadata(const StringView& sourcePath, const ModelTool::Options& options)
{
    return CreateModelMetadataInternal(sourcePath, options, false);
}

#endif

Guid AuthoredAssetDocumentService::CreateMetadata(const StringView& sourcePath)
{
#if USE_EDITOR
    AssetPipelineDiagnostic diagnostic;
    auto fail = [&diagnostic]()
    {
        Array<AssetPipelineDiagnostic> diagnostics;
        diagnostics.Add(diagnostic);
        SetDiagnostics(diagnostics);
        return Guid::Empty;
    };
    String typeName;
    if (GetJsonSourceDocumentType(sourcePath, typeName, diagnostic))
    {
        return fail();
    }
    AssetMeta meta;
    const String metaPath = String(sourcePath) + TEXT(".meta");
    if (FileSystem::FileExists(metaPath))
    {
        if (AssetMeta::Load(metaPath, meta, diagnostic))
            return fail();
    }
    else
    {
        meta.ID = Guid::New();
    }
    if (FileSystem::GetExtension(sourcePath).ToLower() == TEXT("settings"))
        ConfigureSettingsMetadata(meta);
    else
    {
        ConfigureJsonDocumentMetadata(meta, typeName);
    }
    if (AssetMeta::SaveAtomic(metaPath, meta, diagnostic))
        return fail();
    return meta.ID;
#else
    return Guid::Empty;
#endif
}

bool AuthoredAssetDocumentService::EnsureSidecars()
{
#if USE_EDITOR
    Array<String> files;
    if (FileSystem::DirectoryGetFiles(files, Globals::ProjectContentFolder, TEXT("*"), DirectorySearchOption::AllDirectories))
        return true;
    bool failed = false;
    for (const String& path : files)
    {
        const String extension = FileSystem::GetExtension(path).ToLower();
        if (extension != TEXT("scene") && extension != TEXT("prefab") && extension != TEXT("json") && extension != TEXT("settings"))
            continue;
        const String metaPath = path + TEXT(".meta");
        if (FileSystem::FileExists(metaPath))
            continue;
        if (!CreateMetadata(path).IsValid())
            failed = true;
    }
    return failed;
#else
    return true;
#endif
}
