// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Scripting/ScriptingType.h"

class AudioZoneVolume;

/// <summary>Backend-neutral target class for deterministic zone aggregation.</summary>
API_ENUM() enum class FLAXENGINE_API AudioZoneTargetType : uint8
{
    Snapshot = 0,
    BusVolume = 1,
    BusMute = 2,
    VCAVolume = 3,
    GlobalParameter = 4,
};

/// <summary>One sampled zone contribution.</summary>
struct FLAXENGINE_API AudioZoneContribution
{
    AudioZoneVolume* Zone = nullptr;
    float Weight = 0.0f;
    int32 Priority = 0;
};

/// <summary>
/// Deterministically combines active zone samples before writing mixer state.
/// </summary>
class FLAXENGINE_API AudioZoneMixer
{
public:
    /// <summary>Applies final snapshot weights for the supplied zone set.</summary>
    static void Apply(const Array<AudioZoneVolume*>& zones);

    /// <summary>Removes a zone from retained mixer state before the actor is destroyed.</summary>
    static void Remove(AudioZoneVolume* zone);
};
