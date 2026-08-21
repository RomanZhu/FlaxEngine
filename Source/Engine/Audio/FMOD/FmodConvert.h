// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/Events/AudioEventTypes.h"
#include <fmod.hpp>
#include <fmod_studio.hpp>
#include <fmod_errors.h>

/// <summary>
/// Helper utilities for converting between Flax and FMOD data types and coordinates.
/// </summary>
class FLAXENGINE_API FmodConvert
{
public:
    /// <summary>Flax world units (centimeters) per one meter used by FMOD.</summary>
    static constexpr float FlaxUnitsPerMeter = 100.0f;
    /// <summary>Meters per one Flax world unit.</summary>
    static constexpr float MetersPerFlaxUnit = 1.0f / FlaxUnitsPerMeter;

    /// <summary>Converts a Flax world position in centimeters to FMOD meters.</summary>
    static FORCE_INLINE FMOD_VECTOR ToFmodPositionMeters(const Vector3& v)
    {
        FMOD_VECTOR fv;
        fv.x = (float)v.X * MetersPerFlaxUnit;
        fv.y = (float)v.Y * MetersPerFlaxUnit;
        fv.z = (float)v.Z * MetersPerFlaxUnit;
        return fv;
    }

    /// <summary>Converts a Flax velocity in centimeters per second to FMOD meters per second.</summary>
    static FORCE_INLINE FMOD_VECTOR ToFmodVelocityMetersPerSecond(const Vector3& v)
    {
        return ToFmodPositionMeters(v);
    }

    /// <summary>Converts an orientation vector without applying any distance scale.</summary>
    static FORCE_INLINE FMOD_VECTOR ToFmodDirection(const Vector3& v)
    {
        FMOD_VECTOR fv;
        fv.x = (float)v.X;
        fv.y = (float)v.Y;
        fv.z = (float)v.Z;
        return fv;
    }

    static FORCE_INLINE Vector3 FromFmodPositionCentimeters(const FMOD_VECTOR& fv)
    {
        return Vector3(fv.x, fv.y, fv.z) * FlaxUnitsPerMeter;
    }

    static FORCE_INLINE Vector3 FromFmodVelocityCentimetersPerSecond(const FMOD_VECTOR& fv)
    {
        return FromFmodPositionCentimeters(fv);
    }

    static FORCE_INLINE Vector3 FromFmodDirection(const FMOD_VECTOR& fv)
    {
        return Vector3(fv.x, fv.y, fv.z);
    }

    static FORCE_INLINE FMOD_3D_ATTRIBUTES ToFmodAttributes(const Audio3DAttributes& attrs)
    {
        Vector3 position = attrs.Position;
        Vector3 velocity = attrs.Velocity;
        Vector3 forward = attrs.Forward;
        Vector3 up = attrs.Up;
        if (position.IsNanOrInfinity())
            position = Vector3::Zero;
        if (velocity.IsNanOrInfinity())
            velocity = Vector3::Zero;
        if (forward.IsNanOrInfinity() || forward.LengthSquared() < ZeroTolerance)
            forward = Vector3::Forward;
        else
            forward.Normalize();
        if (up.IsNanOrInfinity() || up.LengthSquared() < ZeroTolerance)
            up = Vector3::Up;
        else
            up.Normalize();
        if (Math::Abs(Vector3::Dot(forward, up)) > 0.999f)
            up = Math::Abs(Vector3::Dot(forward, Vector3::Up)) < 0.999f ? Vector3::Up : Vector3::Right;

        FMOD_3D_ATTRIBUTES fa;
        fa.position = ToFmodPositionMeters(position);
        fa.velocity = ToFmodVelocityMetersPerSecond(velocity);
        fa.forward = ToFmodDirection(forward);
        fa.up = ToFmodDirection(up);
        return fa;
    }

    static FORCE_INLINE Audio3DAttributes FromFmodAttributes(const FMOD_3D_ATTRIBUTES& attrs)
    {
        Audio3DAttributes result;
        result.Position = FromFmodPositionCentimeters(attrs.position);
        result.Velocity = FromFmodVelocityCentimetersPerSecond(attrs.velocity);
        result.Forward = FromFmodDirection(attrs.forward);
        result.Up = FromFmodDirection(attrs.up);
        return result;
    }

    static FMOD_GUID ToFmodGuid(const Guid& guid);
    static Guid FromFmodGuid(const FMOD_GUID& guid);

    static bool CheckResult(FMOD_RESULT result, const char* operation);
};

#endif
