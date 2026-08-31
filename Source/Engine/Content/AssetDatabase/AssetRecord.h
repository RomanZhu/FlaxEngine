// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetPath.h"
#include "Identity/AssetObjectId.h"
#include "Engine/Content/AssetInfo.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>Kind of canonical input represented by an asset record.</summary>
API_ENUM() enum class AssetSourceKind : byte
{
    ImportedSource,
    TextDocument,
    ExistingJson,
    LegacyBinary,
    Folder,
};

/// <summary>Current registration/build state of an asset record.</summary>
API_ENUM() enum class AssetRecordStatus : byte
{
    Ready,
    Stale,
    Building,
    Failed,
    MissingSource,
    MissingDependency,
    DuplicateGuid,
    UnsupportedProcessor,
    MetaUpgradeRequired,
    DocumentUpgradeRequired,
    SubAssetReconciliationRequired,
    MissingMeta,
    MalformedMeta,
    OrphanMeta,
    PathCollision,
};

/// <summary>Canonical identity record. Generated artifact paths are intentionally absent.</summary>
struct FLAXENGINE_API AssetRecord
{
    Guid ID;
    Guid SourceAssetID;
    int64 LocalId = 1;
    String TypeName;
    CanonicalAssetPath CanonicalPath;
    SourceFilePath SourcePath;
    MetaFilePath MetaPath;
    SubAssetKey SubAsset;
    String ProcessorID;
    String PortabilityKey;
    uint64 MetaSemanticHash = 0;
    Array<String> Labels;
    Array<AssetObjectId> BuildInputDependencies;
    Array<AssetObjectId> RuntimeReferences;
    AssetSourceKind SourceKind = AssetSourceKind::LegacyBinary;
    AssetRecordStatus Status = AssetRecordStatus::Ready;
    uint64 DatabaseRevision = 0;

    static AssetRecord FromLegacy(const AssetInfo& info);
    AssetInfo ToAssetInfo() const;
    bool IsMainAsset() const;
    bool HasSameIdentityAndContent(const AssetRecord& other) const;
};
