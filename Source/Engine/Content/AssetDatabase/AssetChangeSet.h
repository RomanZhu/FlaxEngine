// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetRecord.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"

struct FLAXENGINE_API AssetAddedChange
{
    Guid AssetGuid = Guid::Empty;
    String Path;
};

struct FLAXENGINE_API AssetRemovedChange
{
    Guid AssetGuid = Guid::Empty;
    String PreviousPath;
};

struct FLAXENGINE_API AssetMovedChange
{
    Guid AssetGuid = Guid::Empty;
    String PreviousPath;
    String Path;
};

struct FLAXENGINE_API AssetSourceChangedChange
{
    Guid AssetGuid = Guid::Empty;
    ContentHash PreviousHash;
    ContentHash Hash;
};

struct FLAXENGINE_API AssetMetadataChangedChange
{
    Guid AssetGuid = Guid::Empty;
    ContentHash PreviousHash;
    ContentHash Hash;
};

struct FLAXENGINE_API AssetImportedChange
{
    Guid AssetGuid = Guid::Empty;
    int64 LocalFileId = 0;
    String TargetId;
    ArtifactKey Artifact;
};

struct FLAXENGINE_API AssetObjectsChangedChange
{
    Guid AssetGuid = Guid::Empty;
    Array<int64> LocalFileIds;
};

struct FLAXENGINE_API AssetStatusChangedChange
{
    Guid AssetGuid = Guid::Empty;
    AssetRecordStatus Previous = AssetRecordStatus::Ready;
    AssetRecordStatus Status = AssetRecordStatus::Ready;
};

struct FLAXENGINE_API AssetDiagnosticsChangedChange
{
    Guid AssetGuid = Guid::Empty;
    uint32 ActiveCount = 0;
};

/// <summary>One committed, ordered and replayable database change set.</summary>
struct FLAXENGINE_API AssetChangeSet
{
    uint64 Revision = 0;
    Guid RefreshId = Guid::Empty;
    uint32 Pass = 0;
    Array<AssetAddedChange> Added;
    Array<AssetRemovedChange> Removed;
    Array<AssetMovedChange> Moved;
    Array<AssetSourceChangedChange> SourceChanged;
    Array<AssetMetadataChangedChange> MetadataChanged;
    Array<AssetImportedChange> Imported;
    Array<AssetObjectsChangedChange> ObjectsChanged;
    Array<AssetStatusChangedChange> StatusChanged;
    Array<AssetDiagnosticsChangedChange> DiagnosticsChanged;

    bool IsEmpty() const;
    void Serialize(Array<byte>& output) const;
    static bool Deserialize(const byte* data, uint32 length, AssetChangeSet& output);
};
