// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Vector3.h"

class PhysicalMaterial;

/// <summary>Physics context normalized for surface interaction audio.</summary>
API_STRUCT() struct FLAXENGINE_API AudioImpactContext
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioImpactContext);

    API_FIELD() Vector3 Point = Vector3::Zero;
    API_FIELD() Vector3 Normal = Vector3::Up;
    API_FIELD() Vector3 RelativeVelocity = Vector3::Zero;
    API_FIELD() float Impulse = 0.0f;
    API_FIELD() float RelativeSpeed = 0.0f;
    API_FIELD() float NormalSpeed = 0.0f;
    API_FIELD() PhysicalMaterial* MaterialA = nullptr;
    API_FIELD() PhysicalMaterial* MaterialB = nullptr;
};

FORCE_INLINE float ComputeImpactAngle(const AudioImpactContext& context)
{
    const float speed = context.RelativeSpeed > 0.001f ? context.RelativeSpeed : (float)context.RelativeVelocity.Length();
    return speed > 0.001f ? (float)Math::Saturate(Math::Abs(Vector3::Dot(context.RelativeVelocity / speed, context.Normal))) : 0.0f;
}
