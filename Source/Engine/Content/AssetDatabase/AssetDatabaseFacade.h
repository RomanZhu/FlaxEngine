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

/// <summary>Controls generic asset import and refresh behavior.</summary>
API_ENUM(Attributes="Flags") enum class ImportAssetOptions : uint32
{
    Default = 0,
    ForceUpdate = 1 << 0,
    ForceSynchronousImport = 1 << 1,
    ImportRecursive = 1 << 2,
    DontDownloadFromCacheServer = 1 << 3,
    ForceUncompressedImport = 1 << 4,
    /// <summary>Runs isolated imports twice and blocks publication when outputs or dependencies differ.</summary>
    VerifyDeterminism = 1 << 5,
};

DECLARE_ENUM_OPERATORS(ImportAssetOptions);

/// <summary>Managed-safe immutable asset database record projection.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseRecordInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseRecordInfo);

    /// <summary>Compatibility alias for BackingAssetID. Never serialize this as project object identity.</summary>
    API_FIELD() Guid ID;
    /// <summary>Canonical persistent identity.</summary>
    API_FIELD() AssetObjectId ObjectID;
    /// <summary>Deterministic engine runtime/cache address.</summary>
    API_FIELD() Guid BackingAssetID;
    API_FIELD() Guid SourceAssetID;
    API_FIELD() int64 LocalId = 1;
    API_FIELD() String TypeName;
    API_FIELD() String CanonicalPath;
    API_FIELD() String SourcePath;
    API_FIELD() String MetaPath;
    API_FIELD() String SubAssetKey;
    API_FIELD() String ProcessorID;
    API_FIELD() uint64 MetaSemanticHash = 0;
    /// <summary>Sorted labels separated by newline; labels cannot contain control characters.</summary>
    API_FIELD() String LabelsSerialized;
    API_FIELD() AssetSourceKind SourceKind = AssetSourceKind::LegacyBinary;
    API_FIELD() AssetRecordStatus Status = AssetRecordStatus::Ready;
    API_FIELD() uint64 Revision = 0;
    API_FIELD() bool IsMain = false;
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

/// <summary>Native-owned projection of one adjacent metadata importer block.</summary>
API_STRUCT() struct FLAXENGINE_API AssetImporterMetaInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetImporterMetaInfo);

    API_FIELD() Guid SourceAssetID;
    API_FIELD() uint64 Revision = 0;
    API_FIELD() String ImporterID;
    API_FIELD() int32 SettingsSchemaVersion = 1;
    API_FIELD() String SettingsJson;
    API_FIELD() String ExternalObjectsJson;
    API_FIELD() String UserData;
    API_FIELD() String AssetBundleName;
    API_FIELD() String AssetBundleVariant;
};

/// <summary>Managed-safe result from the native journaled source mutation owner.</summary>
API_STRUCT() struct FLAXENGINE_API AssetMutationResultInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetMutationResultInfo);

    API_FIELD() bool Succeeded = false;
    API_FIELD() bool RequiresRecovery = false;
    API_FIELD() Guid TransactionID;
    API_FIELD() Guid AssetID;
    API_FIELD() String SourcePath;
    API_FIELD() String DestinationPath;
    API_FIELD() String RecoveryPath;
    API_FIELD() String Message;
};

/// <summary>Coarse managed boundary for the canonical source/sidecar database.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetDatabaseFacade
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetDatabaseFacade);

public:
    /// <summary>Emitted once per coherent native database batch.</summary>
    API_EVENT() static Delegate<uint64> DatabaseChanged;

    /// <summary>Emitted on the main thread after a generated artifact has been published and hot-swapped.</summary>
    API_EVENT() static Delegate<Guid> ArtifactPublished;

    /// <summary>Notifies editor consumers that an exact generated artifact is ready.</summary>
    static void NotifyArtifactPublished(const Guid& assetID);

    API_PROPERTY() static uint64 GetRevision();
    API_PROPERTY() static int32 GetDesiredWorkerCount();
    API_PROPERTY() static void SetDesiredWorkerCount(int32 value);
    static int32 GetConfiguredMemoryLimitMegabytes();
    API_FUNCTION() static Array<AssetDatabaseRecordInfo> GetRecords();
    API_FUNCTION() static Array<AssetPipelineDiagnostic> GetDiagnostics();
    API_FUNCTION() static AssetDatabaseChangeInfo GetLastChange();

    /// <summary>Returns the source GUID at a logical or absolute canonical asset path.</summary>
    API_FUNCTION() static Guid AssetPathToGUID(const StringView& path);

    /// <summary>Returns the canonical logical path for a live source file GUID.</summary>
    API_FUNCTION() static String GUIDToAssetPath(const Guid& assetID);

    /// <summary>Returns all live main-asset paths in deterministic canonical order.</summary>
    API_FUNCTION() static Array<String> GetAllAssetPaths();

    /// <summary>Gets deterministic direct or recursive build-input dependency source paths.</summary>
    API_FUNCTION() static Array<String> GetDependencies(const Guid& assetID, bool recursive = false);

    /// <summary>Gets a stable 128-bit projection of the committed dependency closure.</summary>
    API_FUNCTION() static Guid GetDependencyHash(const Guid& assetID);

    /// <summary>Atomically replaces source labels when the caller's record revision is current.</summary>
    API_FUNCTION() static bool SetLabels(const Guid& assetID, const Array<String>& labels, uint64 expectedRevision = 0);

    /// <summary>Reads importer metadata through the native metadata authority.</summary>
    API_FUNCTION() static AssetImporterMetaInfo GetImporterMetadata(const Guid& assetID);

    /// <summary>Atomically applies a complete importer metadata proxy with optimistic revision checking.</summary>
    API_FUNCTION() static bool ApplyImporterMetadata(const Guid& assetID, uint64 expectedRevision, const StringView& importerID,
        int32 settingsSchemaVersion, const StringView& settingsJson, const StringView& externalObjectsJson,
        const StringView& userData, const StringView& assetBundleName, const StringView& assetBundleVariant);
    API_FUNCTION() static bool ResetImporterMetadataToDefault(const Guid& assetID, uint64 expectedRevision);
    API_FUNCTION() static bool ForceReserializeMetadata(const Array<String>& paths);

    API_FUNCTION() static bool RegisterCustomDependency(const StringView& name, const Guid& hash);
    API_FUNCTION() static bool UnregisterCustomDependencyPrefix(const StringView& prefix);

    API_FUNCTION() static AssetMutationResultInfo ValidateAssetMove(const StringView& sourcePath, const StringView& destinationPath);
    API_FUNCTION() static AssetMutationResultInfo MoveAssetPair(const StringView& sourcePath, const StringView& destinationPath);
    API_FUNCTION() static AssetMutationResultInfo CopyAssetPair(const StringView& sourcePath, const StringView& destinationPath);
    API_FUNCTION() static AssetMutationResultInfo DeleteAssetPairToRecovery(const StringView& sourcePath);
    API_FUNCTION() static AssetMutationResultInfo CreateAssetFolder(const StringView& path);
    API_FUNCTION() static AssetMutationResultInfo PublishExternalSource(const StringView& externalSourcePath,
        const StringView& destinationPath, const StringView& typeName, const StringView& processorId,
        bool replaceExisting = false);
    API_FUNCTION() static AssetMutationResultInfo RegisterCanonicalSource(const StringView& sourcePath,
        bool replaceExistingMetadata = false);
    API_FUNCTION() static AssetMutationResultInfo RecoverAssetPair(const StringView& recoveryPath, const StringView& destinationPath);

    /// <summary>Resolves a loaded main asset or subasset to its persistent source identity.</summary>
    API_FUNCTION() static bool TryGetAssetObjectId(Asset* asset, API_PARAM(Out) AssetObjectId& result);

    /// <summary>Resolves a persistent asset object identity to the current runtime backing asset GUID.</summary>
    API_FUNCTION() static Guid GetBackingAssetID(const AssetObjectId& objectID);

    /// <summary>Returns the canonical source path for an asset identifier, or an empty string when it is not registered.</summary>
    API_FUNCTION() static String GetCanonicalSourcePath(const Guid& assetID);

    /// <summary>Loads an asset for passive editor presentation without scheduling source or dependency builds.</summary>
    API_FUNCTION() static Asset* LoadAssetPreview(const Guid& assetID);

    /// <summary>Gets a stable cache version derived from the currently published artifact key.</summary>
    API_FUNCTION() static Guid GetPublishedArtifactCacheID(const Guid& assetID, const StringView& outputKind);

    /// <summary>Returns true when a compatible published output exists for the asset.</summary>
    API_FUNCTION() static bool HasPublishedArtifact(const Guid& assetID, const StringView& outputKind);

    /// <summary>Loads a still-current disposable snapshot or performs one full scan.</summary>
    /// <returns>True if the scan infrastructure failed. Content diagnostics remain queryable.</returns>
    API_FUNCTION() static bool LoadOrScan(bool strictMetadata = false);

    /// <summary>Forces one read-only full scan and atomically publishes its records.</summary>
    /// <returns>True if the scan infrastructure failed. Content diagnostics remain queryable.</returns>
    API_FUNCTION() static bool Scan(bool strictMetadata = false);

    /// <summary>Reindexes an explicit set of source or sidecar paths without enumerating the Content tree.</summary>
    /// <returns>True if indexing failed. Content diagnostics remain queryable.</returns>
    API_FUNCTION() static bool RefreshSources(const Array<String>& paths);

    /// <summary>Reconciles and builds one canonical source or a recursive source folder.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool ImportAsset(const StringView& path, ImportAssetOptions options = ImportAssetOptions::Default);

    /// <summary>Reconciles all mounted sources and queues their supported current builds.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool Refresh(ImportAssetOptions options = ImportAssetOptions::Default);

    /// <summary>Safely clears and recreates the configured Project Library root.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool CleanLibrary();

    /// <summary>Clones a sidecar with a new file GUID while preserving file-relative local IDs.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath);

#if USE_EDITOR
    /// <summary>Prepares default canonical metadata concurrently at caller-owned staging paths without publishing the database.</summary>
    /// <returns>Asset identifiers aligned with the input paths; invalid identifiers report per-source preparation failures.</returns>
    API_FUNCTION() static Array<Guid> StageDefaultCanonicalMetadataBatch(const Array<String>& sourcePaths, const Array<String>& stagingPaths);

    /// <summary>Publishes one staged canonical metadata batch and queues the corresponding exact builds.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool PublishDefaultCanonicalMetadataBatch(const Array<Guid>& assetIDs, const Array<String>& sourcePaths);
#endif

#if COMPILE_WITH_TEXTURE_TOOL
    /// <summary>Creates and registers canonical texture metadata beside an imported source image.</summary>
    /// <returns>The new asset identifier, or an invalid identifier on failure.</returns>
    API_FUNCTION() static Guid CreateTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options);
    API_FUNCTION() static AssetMutationResultInfo PublishExternalTexture(const StringView& externalSourcePath,
        const StringView& destinationPath, const TextureTool::Options& options, bool replaceExisting = false);

    /// <summary>Pre-cutover only: stages a GUID-preserving legacy texture extraction without publishing a database scan.</summary>
    API_FUNCTION() static Guid StageLegacyTextureMigration(const StringView& legacyPath, const StringView& extractedPath,
        const StringView& destinationPath, const StringView& backupPath, const TextureTool::Options& options);

    /// <summary>Loads tracked texture settings without writing the sidecar.</summary>
    API_FUNCTION() static bool LoadTextureMetadata(const StringView& sourcePath, API_PARAM(Out) TextureTool::Options& options);

    /// <summary>Atomically applies tracked texture settings and queues an exact host build.</summary>
    API_FUNCTION() static bool ApplyTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options);

    /// <summary>Queues an exact build without changing tracked settings.</summary>
    API_FUNCTION() static bool RebuildTexture(const Guid& assetID);

    /// <summary>Queues the current exact texture build, sharing duplicate watcher requests.</summary>
    API_FUNCTION() static bool BuildTexture(const Guid& assetID);

    /// <summary>Returns the latest texture build state.</summary>
    API_FUNCTION() static String GetTextureBuildStatus(const Guid& assetID);

    /// <summary>Returns the latest texture build diagnostic.</summary>
    API_FUNCTION() static AssetPipelineDiagnostic GetTextureBuildDiagnostic(const Guid& assetID);

    /// <summary>Loads the exact Library thumbnail into a virtual texture, or returns null when unavailable.</summary>
    API_FUNCTION() static Texture* LoadTextureThumbnail(const Guid& assetID);

#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
    /// <summary>Loads tracked canonical model import settings without writing the sidecar.</summary>
    API_FUNCTION() static bool LoadModelMetadata(const StringView& sourcePath, API_PARAM(Out) ModelTool::Options& options);

    /// <summary>Atomically applies tracked canonical model settings and queues an exact host build.</summary>
    API_FUNCTION() static bool ApplyModelMetadata(const StringView& sourcePath, const ModelTool::Options& options);

    /// <summary>Explicitly reconciles model-owned stable child GUID mappings into the root sidecar.</summary>
    API_FUNCTION() static bool ReconcileModel(const Guid& rootAssetID);

    /// <summary>Queues the current exact model or model-owned child build.</summary>
    API_FUNCTION() static bool BuildModel(const Guid& assetID);

    /// <summary>Queues a forced exact model or model-owned child build.</summary>
    API_FUNCTION() static bool RebuildModel(const Guid& assetID);

    API_FUNCTION() static String GetModelBuildStatus(const Guid& assetID);
    API_FUNCTION() static AssetPipelineDiagnostic GetModelBuildDiagnostic(const Guid& assetID);
#endif

    /// <summary>Creates a canonical graph document plus sidecar and queues its first exact build.</summary>
    API_FUNCTION() static Guid CreateGraphDocument(const StringView& outputPath, const StringView& typeName, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Creates a canonical graph document from an existing Visject surface and queues its first exact build.</summary>
    API_FUNCTION() static Guid CreateGraphDocumentFromSurface(const StringView& outputPath, const StringView& typeName, const BytesContainer& surface, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Creates a canonical small authored document plus sidecar.</summary>
    API_FUNCTION() static Guid CreateAuthoredDocument(const StringView& outputPath, const StringView& typeName);

    /// <summary>Saves an edited authored compatibility asset back into its canonical text document.</summary>
    API_FUNCTION() static bool SaveAuthoredDocument(BinaryAsset* asset, const Guid& canonicalAssetID);

    /// <summary>Saves edited material parameter defaults back into the canonical graph document.</summary>
    API_FUNCTION() static bool SaveMaterialDocument(Material* asset, const Guid& canonicalAssetID);

    /// <summary>Writes particle-system timeline bytes back into the canonical text document.</summary>
    API_FUNCTION() static bool SaveParticleSystemTimeline(const StringView& path, const BytesContainer& timeline);

    /// <summary>Writes a collision recipe document and queues recooking into Library.</summary>
    static bool SaveCollisionDataDocument(const StringView& path, CollisionDataType type, const Guid& model, int32 modelLodIndex,
        uint32 materialSlotsMask, ConvexMeshGenerationFlags convexFlags, int32 convexVertexLimit);

    /// <summary>Reads collision cooking options from a canonical recipe document.</summary>
    static bool LoadCollisionDataDocument(const StringView& path, CollisionData::SerializedOptions& options);

    /// <summary>Creates canonical imported-source metadata beside an audio, font, shader, or video file.</summary>
    API_FUNCTION() static Guid CreateImportedSourceMetadata(const StringView& sourcePath, const StringView& typeName, const StringView& processorId);

#if COMPILE_WITH_AUDIO_TOOL && USE_EDITOR
    /// <summary>Creates canonical audio metadata with the selected import settings.</summary>
    API_FUNCTION() static Guid CreateAudioMetadata(const StringView& sourcePath, const AudioTool::Options& options);
    API_FUNCTION() static AssetMutationResultInfo PublishExternalAudio(const StringView& externalSourcePath,
        const StringView& destinationPath, const AudioTool::Options& options, bool replaceExisting = false);

    /// <summary>Loads tracked canonical audio import settings without writing the sidecar.</summary>
    API_FUNCTION() static bool LoadAudioMetadata(const StringView& sourcePath, API_PARAM(Out) AudioTool::Options& options);
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
    /// <summary>Creates canonical model metadata beside an imported source and seeds subasset GUIDs from a sibling flax package when present.</summary>
    API_FUNCTION() static Guid CreateModelMetadata(const StringView& sourcePath, const ModelTool::Options& options);
    API_FUNCTION() static AssetMutationResultInfo PublishExternalModel(const StringView& externalSourcePath,
        const StringView& destinationPath, const ModelTool::Options& options, bool replaceExisting = false);

    /// <summary>Pre-cutover only: stages a GUID-preserving legacy model extraction without publishing a database scan.</summary>
    API_FUNCTION() static Guid StageLegacyModelMigration(const StringView& legacyPath, const StringView& extractedPath,
        const StringView& destinationPath, const StringView& backupPath, const ModelTool::Options& options);

    /// <summary>Creates model metadata with a root type inferred from the source contents.</summary>
    API_FUNCTION() static Guid CreateDefaultModelMetadata(const StringView& sourcePath);
#endif

    /// <summary>Loads compiled Visject surface bytes from a canonical graph document without writing.</summary>
    API_FUNCTION() static BytesContainer LoadGraphSurface(const StringView& path);

    /// <summary>Encodes Visject surface bytes into a canonical graph document and queues an exact build.</summary>
    API_FUNCTION() static bool SaveGraphSurface(const StringView& path, const BytesContainer& surface, bool allowOverwriteConflict = false, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Queues the current exact graph, authored-document, or imported-source build.</summary>
    API_FUNCTION() static bool BuildGraph(const Guid& assetID);

    /// <summary>Queues an exact graph document rebuild.</summary>
    API_FUNCTION() static bool RebuildGraph(const Guid& assetID);

    API_FUNCTION() static String GetGraphBuildStatus(const Guid& assetID);
    API_FUNCTION() static AssetPipelineDiagnostic GetGraphBuildDiagnostic(const Guid& assetID);

    /// <summary>Creates a sidecar for an existing JSON scene/prefab, preserving the in-file GUID.</summary>
    API_FUNCTION() static Guid CreateExistingJsonMetadata(const StringView& sourcePath);

    /// <summary>Creates or replaces an existing-JSON source through the journaled source/metadata mutation owner.</summary>
    /// <returns>True on failure.</returns>
    static bool SaveExistingJsonSource(const StringView& sourcePath, const StringAnsiView& sourceContents,
        const Guid& sourceID, const StringView& typeName);

    /// <summary>Writes missing scene/prefab sidecars without changing document bytes.</summary>
    API_FUNCTION() static bool EnsureExistingJsonSidecars();

    /// <summary>Builds a read-only mixed-mode migration inventory JSON without writing Content.</summary>
    API_FUNCTION() static String GetMigrationInventoryJson();

    /// <summary>Pre-cutover only: converts one eligible legacy flax asset to its canonical source and removes the legacy binary.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool MigrateLegacyAsset(const StringView& sourcePath);

    /// <summary>Pre-cutover only: deletes a staged legacy backup after its canonical replacement has been verified.</summary>
    API_FUNCTION() static bool FinalizeLegacyImportedMigration(const StringView& backupPath);

    /// <summary>Pre-cutover only: restores a staged imported migration after validation fails.</summary>
    API_FUNCTION() static bool RollbackLegacyImportedMigration(const StringView& legacyPath, const StringView& destinationPath, const StringView& backupPath);
};
