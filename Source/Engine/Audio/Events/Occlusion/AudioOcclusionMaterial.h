// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

class PhysicalMaterial;

/// <summary>
/// Resolves generic physical-material acoustic transmission coefficients.
/// </summary>
class FLAXENGINE_API AudioOcclusionMaterial
{
public:
    static float ResolveTransmission(const PhysicalMaterial* material);
    static float ResolveLowFrequencyTransmission(const PhysicalMaterial* material);
};
