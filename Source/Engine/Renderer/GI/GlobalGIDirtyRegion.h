// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Types/BaseTypes.h"

/// <summary>
/// Flags that describe what changed in a global GI dirty region.
/// </summary>
API_ENUM(Attributes="Flags") enum class GlobalGIDirtyFlags : uint32
{
    /// <summary>
    /// No change flags.
    /// </summary>
    None = 0,

    /// <summary>
    /// Geometry changed (transform moved, mesh changed, actor added/removed/enabled/disabled).
    /// </summary>
    GeometryChanged = 1 << 0,

    /// <summary>
    /// Surface material or texture changed.
    /// </summary>
    SurfaceChanged = 1 << 1,

    /// <summary>
    /// Direct or indirect lighting changed (light moved/modified, occluder moved).
    /// </summary>
    LightingChanged = 1 << 2,

    /// <summary>
    /// Surface emission changed.
    /// </summary>
    EmissionChanged = 1 << 3,
};
DECLARE_ENUM_OPERATORS(GlobalGIDirtyFlags);

/// <summary>
/// Represents a spatial region where scene geometry, materials, or lighting changed, requiring dynamic GI updates.
/// </summary>
API_STRUCT() struct FLAXENGINE_API GlobalGIDirtyRegion
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(GlobalGIDirtyRegion);

    /// <summary>
    /// The bounding box before the change in world space.
    /// </summary>
    API_FIELD() BoundingBox PreviousBounds;

    /// <summary>
    /// The bounding box after the change in world space.
    /// </summary>
    API_FIELD() BoundingBox CurrentBounds;

    /// <summary>
    /// The change flags.
    /// </summary>
    API_FIELD() GlobalGIDirtyFlags Flags;

    GlobalGIDirtyRegion()
        : PreviousBounds(BoundingBox::Empty)
        , CurrentBounds(BoundingBox::Empty)
        , Flags(GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged)
    {
    }

    GlobalGIDirtyRegion(const BoundingBox& previousBounds, const BoundingBox& currentBounds, GlobalGIDirtyFlags flags = GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged)
        : PreviousBounds(previousBounds)
        , CurrentBounds(currentBounds)
        , Flags(flags)
    {
    }

    /// <summary>
    /// Gets the combined bounds covering both previous and current bounding boxes.
    /// </summary>
    FORCE_INLINE BoundingBox GetCombinedBounds() const
    {
        if (PreviousBounds.Minimum.X > PreviousBounds.Maximum.X)
            return CurrentBounds;
        if (CurrentBounds.Minimum.X > CurrentBounds.Maximum.X)
            return PreviousBounds;
        BoundingBox result;
        BoundingBox::Merge(PreviousBounds, CurrentBounds, result);
        return result;
    }
};
