// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetGuid.h"
#include "Engine/Platform/StringUtils.h"

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
    API_FIELD() AssetGuid Asset;

    /// <summary>
    /// The stable object identifier within the asset file. Zero is invalid.
    /// </summary>
    API_FIELD() int64 LocalId = 0;

    AssetObjectId()
    {
    }

    AssetObjectId(const AssetGuid& guid, LocalFileId localId)
        : Asset(guid)
        , LocalId(localId)
    {
    }

    FORCE_INLINE bool IsValid() const
    {
        return Asset.IsValid() && LocalId != 0;
    }

    FORCE_INLINE bool IsNull() const
    {
        return !Asset.IsValid() || LocalId == 0;
    }

    FORCE_INLINE bool IsMainObject() const
    {
        return Asset.IsValid() && LocalId == 1;
    }

    static FORCE_INLINE AssetObjectId Main(const AssetGuid& asset)
    {
        return AssetObjectId(asset, 1);
    }

    String ToString() const
    {
        return String::Format(TEXT("{0}:{1}"), Asset.Value, LocalId);
    }

    /// <summary>Parses the canonical guid:fileId representation.</summary>
    /// <returns>True on failure.</returns>
    static bool Parse(const StringView& text, AssetObjectId& value)
    {
        const int32 separator = text.Find(':');
        Guid guid;
        int64 localId;
        if (separator <= 0 || separator + 1 >= text.Length() ||
            Guid::Parse(text.Left(separator), guid) ||
            StringUtils::Parse(text.Get() + separator + 1, text.Length() - separator - 1, &localId) ||
            !guid.IsValid() || localId == 0)
        {
            value = AssetObjectId();
            return true;
        }
        value = AssetObjectId(AssetGuid(guid), localId);
        return false;
    }

    /// <summary>
    /// Produces the transitional storage/runtime key used while legacy binary containers are being removed.
    /// This derived value can collide and must never be used as authority for exact object identity.
    /// </summary>
    Guid ToRuntimeObjectGuid() const
    {
        if (IsMainObject())
            return Asset.Value;
        const uint64 local = static_cast<uint64>(LocalId);
        const uint32 low = static_cast<uint32>(local);
        const uint32 high = static_cast<uint32>(local >> 32);
        Guid result(Asset.Value.A ^ low, Asset.Value.B ^ high,
                    Asset.Value.C ^ ((low << 13) | (low >> 19)),
                    Asset.Value.D ^ ((high << 7) | (high >> 25)) ^ 0x9e3779b9u);
        if (!result.IsValid())
            result.D = 1;
        return result;
    }

    FORCE_INLINE explicit operator bool() const
    {
        return IsValid();
    }

    FORCE_INLINE bool operator==(const AssetObjectId& other) const
    {
        return Asset == other.Asset && LocalId == other.LocalId;
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
    return GetHash(key.Asset) ^ static_cast<uint32>(localId) ^ static_cast<uint32>(localId >> 32);
}

DEFINE_DEFAULT_FORMATTING_VIA_TO_STRING(AssetObjectId);
