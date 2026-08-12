// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/Span.h"
#include "Engine/Scripting/ScriptingType.h"

class Foliage;
class Actor;

/// <summary>
/// Foliage tools for editor. Allows to spawn and modify foliage instances.
/// </summary>
API_CLASS(Static, Namespace="FlaxEditor") class FoliageTools
{
DECLARE_SCRIPTING_TYPE_NO_SPAWN(FoliageTools);

    /// <summary>
    /// Paints the foliage instances using the given foliage types selection and the brush location.
    /// </summary>
    /// <param name="foliage">The foliage actor.</param>
    /// <param name="foliageTypesIndices">The foliage types indices to use for painting.</param>
    /// <param name="brushPosition">The brush position.</param>
    /// <param name="brushRadius">The brush radius.</param>
    /// <param name="additive">True if paint using additive mode, false if remove foliage instances.</param>
    /// <param name="densityScale">The additional scale for foliage density when painting. Can be used to increase or decrease foliage density during painting.</param>
    /// <param name="placementSurface">Optional actor to restrict additive placement geometry to.</param>
    API_FUNCTION() static void Paint(Foliage* foliage, Span<int32> foliageTypesIndices, const Vector3& brushPosition, float brushRadius, bool additive, float densityScale = 1.0f, Actor* placementSurface = nullptr);

    /// <summary>
    /// Paints the foliage instances using the given foliage types selection and the brush location.
    /// </summary>
    /// <param name="foliage">The foliage actor.</param>
    /// <param name="foliageTypesIndices">The foliage types indices to use for painting.</param>
    /// <param name="brushPosition">The brush position.</param>
    /// <param name="brushRadius">The brush radius.</param>
    /// <param name="densityScale">The additional scale for foliage density when painting. Can be used to increase or decrease foliage density during painting.</param>
    /// <param name="placementSurface">Optional actor to restrict placement geometry to.</param>
    static void Paint(Foliage* foliage, Span<int32> foliageTypesIndices, const Vector3& brushPosition, float brushRadius, float densityScale = 1.0f, Actor* placementSurface = nullptr);

    /// <summary>
    /// Removes the foliage instances using the given foliage types selection and the brush location.
    /// </summary>
    /// <param name="foliage">The foliage actor.</param>
    /// <param name="foliageTypesIndices">The foliage types indices to use for painting.</param>
    /// <param name="brushPosition">The brush position.</param>
    /// <param name="brushRadius">The brush radius.</param>
    API_FUNCTION() static void Remove(Foliage* foliage, Span<int32> foliageTypesIndices, const Vector3& brushPosition, float brushRadius);
};
