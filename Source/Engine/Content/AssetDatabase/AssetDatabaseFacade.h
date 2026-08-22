// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetDatabaseScanner.h"
#include "Engine/Core/Types/DataContainer.h"
#include "Engine/Scripting/ScriptingType.h"

#if COMPILE_WITH_TEXTURE_TOOL
#include "Engine/Tools/TextureTool/TextureTool.h"
class Texture;
#endif
#if COMPILE_WITH_MODEL_TOOL
#include "Engine/Tools/ModelTool/ModelTool.h"
#endif

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

    /// <summary>Loads compiled Visject surface bytes from a canonical graph document without writing.</summary>
    API_FUNCTION() static BytesContainer LoadGraphSurface(const StringView& path);

    /// <summary>Encodes Visject surface bytes into a canonical graph document and queues an exact build.</summary>
    API_FUNCTION() static bool SaveGraphSurface(const StringView& path, const BytesContainer& surface, bool allowOverwriteConflict = false, const StringView& propertiesJson = StringView::Empty);

    /// <summary>Queues an exact graph document rebuild.</summary>
    API_FUNCTION() static bool RebuildGraph(const Guid& assetID);

    /// <summary>Builds a read-only mixed-mode migration inventory JSON without writing Content.</summary>
    API_FUNCTION() static String GetMigrationInventoryJson();
};
