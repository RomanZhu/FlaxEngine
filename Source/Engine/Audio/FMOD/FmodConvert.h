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
    static FORCE_INLINE FMOD_VECTOR ToFmodVector(const Vector3& v)
    {
        FMOD_VECTOR fv;
        fv.x = (float)v.X;
        fv.y = (float)v.Y;
        fv.z = (float)v.Z;
        return fv;
    }

    static FORCE_INLINE Vector3 FromFmodVector(const FMOD_VECTOR& fv)
    {
        return Vector3(fv.x, fv.y, fv.z);
    }

    static FORCE_INLINE FMOD_3D_ATTRIBUTES ToFmodAttributes(const Audio3DAttributes& attrs)
    {
        FMOD_3D_ATTRIBUTES fa;
        fa.position = ToFmodVector(attrs.Position);
        fa.velocity = ToFmodVector(attrs.Velocity);
        fa.forward = ToFmodVector(attrs.Forward);
        fa.up = ToFmodVector(attrs.Up);
        return fa;
    }

    static FMOD_GUID ToFmodGuid(const Guid& guid);
    static Guid FromFmodGuid(const FMOD_GUID& guid);

    static bool CheckResult(FMOD_RESULT result, const char* operation);
};

#endif
