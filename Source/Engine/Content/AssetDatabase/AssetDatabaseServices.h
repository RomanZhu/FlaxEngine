// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseScanner.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Identity/AssetObjectId.h"
#include "Engine/Physics/CollisionData.h"

#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Tools/TextureTool/TextureTool.h"
class Texture;
#endif
#if COMPILE_WITH_MODEL_TOOL
#include "Engine/Tools/ModelTool/ModelTool.h"
#endif
#if COMPILE_WITH_AUDIO_TOOL && USE_EDITOR
#include "Engine/Tools/AudioTool/AudioTool.h"
#endif

class BinaryAsset;
class Asset;
class Material;
class MaterialInstance;
class SkeletonMask;

/// <summary>Controls generic asset import and refresh behavior.</summary>
API_ENUM(Attributes="Flags") enum class ImportAssetOptions : uint32
{
    Default = 0,
    ForceUpdate = 1 << 0,
    ForceSynchronousImport = 1 << 1,
    ImportRecursive = 1 << 2,
    DontDownloadFromCacheServer = 1 << 3,
    ForceUncompressedImport = 1 << 4,
};

DECLARE_ENUM_OPERATORS(ImportAssetOptions);

/// <summary>Managed-safe immutable asset database record projection.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseRecordInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseRecordInfo);

    API_FIELD() Guid ID;
    API_FIELD() Guid SourceAssetID;
    API_FIELD() int64 LocalId = 1;
    API_FIELD() String TypeName;
    API_FIELD() String CanonicalPath;
    API_FIELD() String SourcePath;
    API_FIELD() String MetaPath;
    API_FIELD() String SubAssetKey;
    API_FIELD() String DisplayName;
    API_FIELD() String ProcessorID;
    API_FIELD() uint64 MetaSemanticHash = 0;
    API_FIELD() AssetSourceKind SourceKind = AssetSourceKind::ImportedSource;
    API_FIELD() AssetRecordStatus Status = AssetRecordStatus::Ready;
    API_FIELD() uint64 Revision = 0;
    API_FIELD() bool IsMain = false;
};

/// <summary>Composite database-indexed editor asset query.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseQuery
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseQuery);

    API_FIELD() String Name;
    API_FIELD() String PathPrefix;
    API_FIELD() String TypeName;
    API_FIELD() String ImporterID;
    API_FIELD() String Label;
    API_FIELD() AssetRecordStatus Status = AssetRecordStatus::Ready;
    API_FIELD() bool HasStatus = false;
    API_FIELD() bool MainAssetsOnly = false;
    API_FIELD() AssetObjectId ReferencedAsset;
    API_FIELD() AssetObjectId UsedByAsset;
};

/// <summary>Managed-safe normalized asset dependency projection.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseDependencyInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseDependencyInfo);

    API_FIELD() AssetObjectId Owner;
    API_FIELD() String TargetID;
    API_FIELD() String Kind;
    API_FIELD() AssetObjectId TargetObject;
    API_FIELD() String SourcePath;
    API_FIELD() String ExactArtifact;
    API_FIELD() String CustomDependency;
    API_FIELD() String ContentHash;
    API_FIELD() String OriginPath;
    API_FIELD() int32 OriginLine = -1;
    API_FIELD() int32 OriginColumn = -1;
};

/// <summary>Managed-safe immutable artifact publication projection.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabasePublicationInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabasePublicationInfo);

    API_FIELD() AssetObjectId Object;
    API_FIELD() String TargetID;
    API_FIELD() String Artifact;
    API_FIELD() String ManifestHash;
    API_FIELD() String InputFingerprint;
    API_FIELD() uint64 SourceRevision = 0;
    API_FIELD() uint64 ImporterRegistryGeneration = 0;
    API_FIELD() int64 PublishedUtcTicks = 0;
    API_FIELD() bool IsLastKnownGood = false;
};

/// <summary>Managed-safe result of one immutable artifact garbage collection pass.</summary>
API_STRUCT() struct FLAXENGINE_API AssetArtifactCleanupInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetArtifactCleanupInfo);

    API_FIELD() uint64 TotalArtifactBytes = 0;
    API_FIELD() uint64 ReachableBytes = 0;
    API_FIELD() uint64 CandidateBytes = 0;
    API_FIELD() uint64 ReclaimedBytes = 0;
    API_FIELD() int32 ScannedFiles = 0;
    API_FIELD() int32 ReachableFiles = 0;
    API_FIELD() int32 LeasedFiles = 0;
    API_FIELD() int32 CandidateFiles = 0;
    API_FIELD() int32 DeletedFiles = 0;
    API_FIELD() bool BlockedByInvalidManifest = false;
    API_FIELD() String DeletedPaths;
    API_FIELD() Array<AssetPipelineDiagnostic> Diagnostics;
};

/// <summary>Last published asset-database identity change, for scoped editor tree refresh.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseChangeInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseChangeInfo);

    API_FIELD() uint64 Revision = 0;
    API_FIELD() Array<Guid> Added;
    API_FIELD() Array<Guid> Removed;
    API_FIELD() Array<Guid> Changed;
    API_FIELD() Array<Guid> StatusChanged;
};

/// <summary>Read-only source database queries and ordered change delivery.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetDatabaseQueryService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetDatabaseQueryService);
public:
    API_EVENT() static Delegate<uint64> DatabaseChanged;
    API_FUNCTION() static void PumpDatabaseEvents();
    API_PROPERTY() static uint64 GetRevision();
    API_FUNCTION() static Array<AssetDatabaseRecordInfo> GetRecords();
    API_FUNCTION() static Array<AssetDatabaseRecordInfo> QueryRecords(const AssetDatabaseQuery& query);
    API_FUNCTION() static bool TryGetRecord(const AssetObjectId& objectID, API_PARAM(Out) AssetDatabaseRecordInfo& result);
    API_FUNCTION() static bool TryGetMainRecordAtPath(const StringView& path, API_PARAM(Out) AssetDatabaseRecordInfo& result);
    API_FUNCTION() static Array<String> GetLabels(const Guid& sourceID);
    API_FUNCTION() static Array<AssetDatabaseDependencyInfo> GetDependencies(const AssetObjectId& objectID);
    API_FUNCTION() static Array<AssetDatabaseDependencyInfo> GetReferencers(const AssetObjectId& objectID);
    API_FUNCTION() static Array<AssetDatabasePublicationInfo> GetPublications(const AssetObjectId& objectID);
    API_FUNCTION() static Array<AssetPipelineDiagnostic> GetDiagnostics();
    API_FUNCTION() static AssetDatabaseChangeInfo GetLastChange();
    API_FUNCTION() static Array<AssetDatabaseChangeInfo> GetChangesAfter(uint64 revision, API_PARAM(Out) bool& requiresSnapshot);
    API_FUNCTION() static Guid AssetPathToGUID(const StringView& path);
    API_FUNCTION() static String GUIDToAssetPath(const Guid& assetID);
    API_FUNCTION() static Array<String> GetAllAssetPaths();
    API_FUNCTION() static bool TryGetAssetObjectId(Asset* asset, API_PARAM(Out) AssetObjectId& result);
    API_FUNCTION() static Guid GetBackingAssetID(const AssetObjectId& objectID);
    API_FUNCTION() static String GetCanonicalSourcePath(const Guid& assetID);
    API_FUNCTION() static Asset* LoadAssetPreview(const AssetObjectId& objectID);
    API_FUNCTION() static Guid GetPublishedArtifactCacheID(const Guid& assetID, const StringView& outputKind);
};

/// <summary>Coordinates source refresh, importer execution, and artifact publication.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetPipelineService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetPipelineService);
public:
    static void NotifyArtifactPublished(const Guid& assetID);
    API_FUNCTION() static Array<Guid> DrainArtifactPublications();
    API_FUNCTION() static bool Initialize();
    API_FUNCTION() static bool Shutdown();
    API_FUNCTION() static bool LoadOrScan(bool strictMetadata = false);
    API_FUNCTION() static bool Scan(bool strictMetadata = false);
    API_FUNCTION() static bool RefreshSources(const Array<String>& paths);
    API_FUNCTION() static bool ImportAsset(const StringView& path, ImportAssetOptions options = ImportAssetOptions::Default);
    API_FUNCTION() static bool Refresh(ImportAssetOptions options = ImportAssetOptions::Default);
    API_FUNCTION() static bool BuildAsset(const Guid& assetID, bool force = false, bool synchronous = false);
    API_FUNCTION() static bool RebuildAsset(const Guid& assetID, bool synchronous = false);
    API_FUNCTION() static bool IsArtifactCurrent(const Guid& assetID);
    API_FUNCTION() static String GetBuildStatus(const Guid& assetID);
    API_FUNCTION() static AssetPipelineDiagnostic GetBuildDiagnostic(const Guid& assetID);
    API_FUNCTION() static bool RegisterCustomDependency(const StringView& name, const StringView& contentHash, const StringView& provider = StringView::Empty);
    API_FUNCTION() static bool UnregisterCustomDependency(const StringView& name);
    API_FUNCTION() static String GetCustomDependencyHash(const StringView& name);
    API_FUNCTION() static bool CleanLibrary();
    API_FUNCTION() static bool CleanUnusedArtifacts(API_PARAM(Out) AssetArtifactCleanupInfo& result);
};

/// <summary>Transactional source and sidecar mutations.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetOperationService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetOperationService);
public:
    API_FUNCTION() static Array<String> DrainSelfWrites();
    API_FUNCTION() static bool SetLabels(const Guid& sourceID, const Array<String>& labels);
    API_FUNCTION() static bool MoveAsset(const StringView& sourcePath, const StringView& destinationPath);
    API_FUNCTION() static bool CopyAsset(const StringView& sourcePath, const StringView& destinationPath, API_PARAM(Out) Guid& copiedGuid);
    API_FUNCTION() static bool DeleteAsset(const StringView& sourcePath);
    API_FUNCTION() static void StartEditing();
    API_FUNCTION() static bool StopEditing();
    API_FUNCTION() static bool CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath);
    API_FUNCTION() static Guid CreateImportedSourceMetadata(const StringView& sourcePath, const StringView& typeName, const StringView& processorId);
#if USE_EDITOR
    /// <summary>Imports one external source through a journaled source-plus-meta transaction.</summary>
    API_FUNCTION() static bool ImportAsset(const StringView& externalSource, const StringView& destination);
    API_FUNCTION() static Array<Guid> StageDefaultMetadataBatch(const Array<String>& sourcePaths, const Array<String>& stagingPaths);
    API_FUNCTION() static bool PublishDefaultMetadataBatch(const Array<Guid>& assetIDs, const Array<String>& sourcePaths);
#endif
};

#if COMPILE_WITH_TEXTURE_TOOL
/// <summary>Texture importer settings and presentation adapter.</summary>
API_CLASS(Static) class FLAXENGINE_API TextureImporterService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(TextureImporterService);
public:
    API_FUNCTION() static Guid CreateMetadata(const StringView& sourcePath, const TextureTool::Options& options);
    API_FUNCTION() static bool LoadMetadata(const StringView& sourcePath, API_PARAM(Out) TextureTool::Options& options);
    API_FUNCTION() static bool ApplyMetadata(const StringView& sourcePath, const TextureTool::Options& options);
    API_FUNCTION() static Texture* LoadThumbnail(const Guid& assetID);
};
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
/// <summary>Model importer settings and sub-object reconciliation.</summary>
API_CLASS(Static) class FLAXENGINE_API ModelImporterService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(ModelImporterService);
public:
    API_FUNCTION() static bool LoadMetadata(const StringView& sourcePath, API_PARAM(Out) ModelTool::Options& options);
    API_FUNCTION() static bool ApplyMetadata(const StringView& sourcePath, const ModelTool::Options& options);
    API_FUNCTION() static bool ReconcileSubAssets(const Guid& rootAssetID);
    API_FUNCTION() static Guid CreateMetadata(const StringView& sourcePath, const ModelTool::Options& options);
    API_FUNCTION() static Guid CreateDefaultMetadata(const StringView& sourcePath);
};
#endif

#if COMPILE_WITH_AUDIO_TOOL && USE_EDITOR
/// <summary>Audio importer settings.</summary>
API_CLASS(Static) class FLAXENGINE_API AudioImporterService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AudioImporterService);
public:
    API_FUNCTION() static Guid CreateMetadata(const StringView& sourcePath, const AudioTool::Options& options);
    API_FUNCTION() static bool LoadMetadata(const StringView& sourcePath, API_PARAM(Out) AudioTool::Options& options);
};
#endif

/// <summary>Canonical non-graph authored source-document operations.</summary>
API_CLASS(Static) class FLAXENGINE_API AuthoredAssetDocumentService
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AuthoredAssetDocumentService);
public:
    API_FUNCTION() static Guid Create(const StringView& outputPath, const StringView& typeName);
    API_FUNCTION() static bool Save(BinaryAsset* asset, const Guid& sourceAssetID);
    API_FUNCTION() static bool SaveMaterialInstance(MaterialInstance* asset, const Guid& sourceAssetID);
    API_FUNCTION() static bool SaveSkeletonMask(SkeletonMask* asset, const Guid& sourceAssetID);
    API_FUNCTION() static bool SaveMaterial(Material* asset, const Guid& sourceAssetID);
    API_FUNCTION() static BytesContainer LoadSceneAnimationTimeline(const StringView& path);
    API_FUNCTION() static bool SaveSceneAnimationTimeline(const StringView& path, const BytesContainer& timeline);
    API_FUNCTION() static BytesContainer LoadParticleSystemTimeline(const StringView& path);
    API_FUNCTION() static bool SaveParticleSystemTimeline(const StringView& path, const BytesContainer& timeline);
    static bool SaveCollisionData(const StringView& path, CollisionDataType type, const Guid& model, int32 modelLodIndex,
        uint32 materialSlotsMask, ConvexMeshGenerationFlags convexFlags, int32 convexVertexLimit);
    static bool LoadCollisionData(const StringView& path, CollisionData::SerializedOptions& options);
    API_FUNCTION() static Guid CreateMetadata(const StringView& sourcePath);
    API_FUNCTION() static bool EnsureSidecars();
};
