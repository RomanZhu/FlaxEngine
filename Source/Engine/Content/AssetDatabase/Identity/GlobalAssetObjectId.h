// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetObjectId.h"

/// <summary>Persistent object namespace used by assets, scenes, prefabs, and built-in content.</summary>
API_ENUM() enum class GlobalObjectKind : byte
{
    ImportedAssetObject,
    SceneObject,
    PrefabObject,
    BuiltinObject,
};

/// <summary>Identifies a persistent object and optional prefab instance context.</summary>
API_STRUCT() struct FLAXENGINE_API GlobalAssetObjectId
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(GlobalAssetObjectId);

    API_FIELD() GlobalObjectKind Kind = GlobalObjectKind::ImportedAssetObject;
    API_FIELD() AssetGuid SourceAsset;
    API_FIELD() int64 LocalFileId = 0;
    API_FIELD() int64 PrefabInstanceFileId = 0;

    FORCE_INLINE bool IsValid() const
    {
        return SourceAsset.IsValid() && LocalFileId != 0;
    }

    FORCE_INLINE bool operator==(const GlobalAssetObjectId& other) const
    {
        return Kind == other.Kind && SourceAsset == other.SourceAsset && LocalFileId == other.LocalFileId &&
               PrefabInstanceFileId == other.PrefabInstanceFileId;
    }

    FORCE_INLINE bool operator!=(const GlobalAssetObjectId& other) const
    {
        return !(*this == other);
    }
};

template<>
struct TIsPODType<GlobalAssetObjectId>
{
    enum { Value = true };
};

inline uint32 GetHash(const GlobalAssetObjectId& key)
{
    const uint64 localId = static_cast<uint64>(key.LocalFileId);
    const uint64 instanceId = static_cast<uint64>(key.PrefabInstanceFileId);
    return GetHash(key.SourceAsset) ^ static_cast<uint32>(key.Kind) ^ static_cast<uint32>(localId) ^
           static_cast<uint32>(localId >> 32) ^ static_cast<uint32>(instanceId) ^ static_cast<uint32>(instanceId >> 32);
}
