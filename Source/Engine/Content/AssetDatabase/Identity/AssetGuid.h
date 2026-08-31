// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/StringView.h"

/// <summary>
/// Identifies one persistent source asset. This is deliberately distinct from runtime object identity.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AssetGuid
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetGuid);

    /// <summary>The immutable source identifier stored in the adjacent metadata file.</summary>
    API_FIELD() Guid Value;

    AssetGuid()
        : Value(Guid::Empty)
    {
    }

    explicit AssetGuid(const Guid& value)
        : Value(value)
    {
    }

    FORCE_INLINE bool IsValid() const
    {
        return Value.IsValid();
    }

    FORCE_INLINE String ToString() const
    {
        return Value.ToString(Guid::FormatType::N);
    }

    /// <summary>Parses the canonical 32-character source identifier.</summary>
    /// <returns>True on failure.</returns>
    static FORCE_INLINE bool Parse(const StringView& text, AssetGuid& value)
    {
        Guid guid;
        if (Guid::Parse(text, guid))
        {
            value = AssetGuid();
            return true;
        }
        value = AssetGuid(guid);
        return false;
    }

    FORCE_INLINE explicit operator bool() const
    {
        return IsValid();
    }

    FORCE_INLINE bool operator==(const AssetGuid& other) const
    {
        return Value == other.Value;
    }

    FORCE_INLINE bool operator!=(const AssetGuid& other) const
    {
        return Value != other.Value;
    }
};

template<>
struct TIsPODType<AssetGuid>
{
    enum { Value = true };
};

inline uint32 GetHash(const AssetGuid& key)
{
    return GetHash(key.Value);
}

DEFINE_DEFAULT_FORMATTING_VIA_TO_STRING(AssetGuid);
