// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/String.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Core/ISerializable.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Query pattern used by an emitter's acoustic obstruction test.
/// </summary>
API_ENUM() enum class AudioOcclusionMode : uint8
{
    SingleRay = 0,
    MultipleRays = 1,
};

/// <summary>
/// Per-emitter, backend-neutral acoustic obstruction settings.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioOcclusionSettings : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioOcclusionSettings);

    API_FIELD() bool Enabled = false;
    API_FIELD() AudioOcclusionMode Mode = AudioOcclusionMode::SingleRay;
    API_FIELD() String Parameter = TEXT("Occlusion");
    API_FIELD() uint8 Rays = 1;
    API_FIELD() float Radius = 15.0f;
    API_FIELD() uint32 CollisionMask = MAX_uint32;
    API_FIELD() float Attack = 12.0f;
    API_FIELD() float Release = 6.0f;
    API_FIELD() float NearInterval = 0.05f;
    API_FIELD() float FarInterval = 0.25f;
    API_FIELD() float MaxDistance = 5000.0f;

    FORCE_INLINE void Sanitize()
    {
        Rays = (uint8)Math::Clamp((int32)Rays, 1, 16);
        Radius = Math::Max(0.0f, Radius);
        Attack = Math::Max(0.01f, Attack);
        Release = Math::Max(0.01f, Release);
        NearInterval = Math::Max(0.0f, NearInterval);
        FarInterval = Math::Max(NearInterval, FarInterval);
        MaxDistance = Math::Max(0.0f, MaxDistance);
    }
};
