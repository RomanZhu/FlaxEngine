// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioOcclusionMaterial.h"
#include "Engine/Physics/PhysicalMaterial.h"
#include "Engine/Core/Math/Math.h"

float AudioOcclusionMaterial::ResolveTransmission(const PhysicalMaterial* material)
{
    return material ? Math::Saturate(material->AudioTransmission) : 0.0f;
}

float AudioOcclusionMaterial::ResolveLowFrequencyTransmission(const PhysicalMaterial* material)
{
    return material ? Math::Saturate(material->AudioLowFrequencyTransmission) : 0.0f;
}
