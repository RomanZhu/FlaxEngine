// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"

using AssetGuid = Guid;
using LocalFileId = int64;

/// <summary>
/// Identifies one object within an asset file by its file GUID and stable local file ID.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AssetObjectId
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetObjectId);

    /// <summary>
    /// The asset file identifier.
    /// </summary>
    API_FIELD() Guid Guid;

    /// <summary>
    /// The stable object identifier within the asset file. Zero is invalid.
    /// </summary>
    API_FIELD() int64 LocalId = 0;

    AssetObjectId()
        : Guid(AssetGuid::Empty)
    {
    }

    AssetObjectId(const AssetGuid& guid, LocalFileId localId)
        : Guid(guid)
        , LocalId(localId)
    {
    }

    FORCE_INLINE bool IsValid() const
    {
        return Guid.IsValid() && LocalId != 0;
    }

    FORCE_INLINE explicit operator bool() const
    {
        return IsValid();
    }

    FORCE_INLINE bool operator==(const AssetObjectId& other) const
    {
        return Guid == other.Guid && LocalId == other.LocalId;
    }

    FORCE_INLINE bool operator!=(const AssetObjectId& other) const
    {
        return !(*this == other);
    }
};

template<>
struct TIsPODType<AssetObjectId>
{
    enum { Value = true };
};

inline uint32 GetHash(const AssetObjectId& key)
{
    const uint64 localId = static_cast<uint64>(key.LocalId);
    return GetHash(key.Guid) ^ static_cast<uint32>(localId) ^ static_cast<uint32>(localId >> 32);
}
