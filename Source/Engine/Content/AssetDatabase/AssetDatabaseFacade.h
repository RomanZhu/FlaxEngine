// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseScanner.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Scripting/ScriptingType.h"
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

/// <summary>Managed-safe immutable asset database record projection.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDatabaseRecordInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDatabaseRecordInfo);

    API_FIELD() Guid ID;
    API_FIELD() Guid SourceAssetID;
    API_FIELD() String TypeName;
    API_FIELD() String CanonicalPath;
    API_FIELD() String SourcePath;
    API_FIELD() String MetaPath;
    API_FIELD() String SubAssetKey;
    API_FIELD() String ProcessorID;
    API_FIELD() uint64 MetaSemanticHash = 0;
    API_FIELD() AssetSourceKind SourceKind = AssetSourceKind::LegacyBinary;
    API_FIELD() AssetRecordStatus Status = AssetRecordStatus::Ready;
    API_FIELD() uint64 Revision = 0;
    API_FIELD() bool IsMain = false;
};

/// <summary>Coarse managed boundary for the canonical source/sidecar database.</summary>
API_CLASS(Static) class FLAXENGINE_API AssetDatabaseFacade
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AssetDatabaseFacade);

public:
    /// <summary>Emitted once per coherent native database batch.</summary>
    API_EVENT() static Delegate<uint64> DatabaseChanged;

    API_PROPERTY() static uint64 GetRevision();
    API_FUNCTION() static Array<AssetDatabaseRecordInfo> GetRecords();
    API_FUNCTION() static Array<AssetPipelineDiagnostic> GetDiagnostics();

    /// <summary>Loads a still-current disposable snapshot or performs one full scan.</summary>
    /// <returns>True if the scan infrastructure failed. Content diagnostics remain queryable.</returns>
    API_FUNCTION() static bool LoadOrScan(bool strictMetadata = false);

    /// <summary>Forces one read-only full scan and atomically publishes its records.</summary>
    /// <returns>True if the scan infrastructure failed. Content diagnostics remain queryable.</returns>
    API_FUNCTION() static bool Scan(bool strictMetadata = false);

    /// <summary>Safely clears and recreates the configured Project Library root.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool CleanLibrary();

    /// <summary>Clones a sidecar while regenerating the root, live subasset, and tombstone GUID tree.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool CloneMetadata(const StringView& sourceMetaPath, const StringView& destinationMetaPath);

#if COMPILE_WITH_TEXTURE_TOOL
    /// <summary>Creates and registers canonical texture metadata beside an imported source image.</summary>
    /// <returns>The new asset identifier, or an invalid identifier on failure.</returns>
    API_FUNCTION() static Guid CreateTextureMetadata(const StringView& sourcePath, const TextureTool::Options& options);

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

    /// <summary>Creates a canonical small authored document plus sidecar.</summary>
    API_FUNCTION() static Guid CreateAuthoredDocument(const StringView& outputPath, const StringView& typeName);

    /// <summary>Saves an edited authored compatibility asset back into its canonical text document.</summary>
    API_FUNCTION() static bool SaveAuthoredDocument(BinaryAsset* asset, const Guid& canonicalAssetID);

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

    /// <summary>Loads tracked canonical audio import settings without writing the sidecar.</summary>
    API_FUNCTION() static bool LoadAudioMetadata(const StringView& sourcePath, API_PARAM(Out) AudioTool::Options& options);
#endif

#if COMPILE_WITH_MODEL_TOOL && USE_EDITOR
    /// <summary>Creates canonical model metadata beside an imported source and seeds subasset GUIDs from a sibling flax package when present.</summary>
    API_FUNCTION() static Guid CreateModelMetadata(const StringView& sourcePath, const ModelTool::Options& options);

    /// <summary>Creates model metadata with a root type inferred from the source contents.</summary>
    API_FUNCTION() static Guid CreateDefaultModelMetadata(const StringView& sourcePath);
#endif

    /// <summary>Loads compiled Visject surface bytes from a canonical graph document without writing.</summary>
    API_FUNCTION() static BytesContainer LoadGraphSurface(const StringView& path);

    /// <summary>Encodes Visject surface bytes into a canonical graph document and queues an exact build.</summary>
    API_FUNCTION() static bool SaveGraphSurface(const StringView& path, const BytesContainer& surface, bool allowOverwriteConflict = false, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Queues an exact graph document rebuild.</summary>
    API_FUNCTION() static bool RebuildGraph(const Guid& assetID);

    /// <summary>Creates a sidecar for an existing JSON scene/prefab, preserving the in-file GUID.</summary>
    API_FUNCTION() static Guid CreateExistingJsonMetadata(const StringView& sourcePath);

    /// <summary>Writes missing scene/prefab sidecars without changing document bytes.</summary>
    API_FUNCTION() static bool EnsureExistingJsonSidecars();

    /// <summary>Builds a read-only mixed-mode migration inventory JSON without writing Content.</summary>
    API_FUNCTION() static String GetMigrationInventoryJson();

    /// <summary>Converts one eligible legacy flax asset to its canonical source and removes the legacy binary.</summary>
    /// <returns>True on failure.</returns>
    API_FUNCTION() static bool MigrateLegacyAsset(const StringView& sourcePath);
};
