// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Generation-safe handle representing an active audio event or snapshot instance in the audio event backend.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioEventHandle
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioEventHandle);

    /// <summary>
    /// The slot index in the backend instance pool.
    /// </summary>
    API_FIELD() uint32 Index = 0;

    /// <summary>
    /// The generation counter of the slot used to detect stale references.
    /// </summary>
    API_FIELD() uint32 Generation = 0;

    AudioEventHandle() = default;

    constexpr AudioEventHandle(uint32 index, uint32 generation)
        : Index(index)
        , Generation(generation)
    {
    }

    FORCE_INLINE bool IsValid() const
    {
        return Generation != 0;
    }

    FORCE_INLINE explicit operator bool() const
    {
        return IsValid();
    }

    FORCE_INLINE bool operator==(const AudioEventHandle& other) const
    {
        return Index == other.Index && Generation == other.Generation;
    }

    FORCE_INLINE bool operator!=(const AudioEventHandle& other) const
    {
        return Index != other.Index || Generation != other.Generation;
    }

    FORCE_INLINE uint64 Raw() const
    {
        return ((uint64)Generation << 32) | (uint64)Index;
    }

    static FORCE_INLINE AudioEventHandle FromRaw(uint64 raw)
    {
        return AudioEventHandle((uint32)(raw & 0xFFFFFFFF), (uint32)(raw >> 32));
    }
};

template<>
struct TIsPODType<AudioEventHandle>
{
    enum { Value = true };
};

inline uint32 GetHash(const AudioEventHandle& key)
{
    return GetHash(key.Raw());
}
