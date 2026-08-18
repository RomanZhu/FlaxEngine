// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Graphics/Enums.h"

/// <summary>
/// Flags that describe what changed in a global GI invalidation event.
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
/// Represents a spatial invalidation event where scene geometry, materials, or lighting changed, requiring dynamic GI updates.
/// </summary>
API_STRUCT() struct FLAXENGINE_API GlobalGIInvalidation
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(GlobalGIInvalidation);

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

    /// <summary>
    /// Identifier of the scene actor or contributor that caused the invalidation.
    /// </summary>
    API_FIELD() uint64 ContributorId;

    /// <summary>
    /// Revision number of the contributor transform / mesh state.
    /// </summary>
    API_FIELD() uint32 ContributorRevision;

    /// <summary>
    /// Contribution classification of the contributor.
    /// </summary>
    API_FIELD() HDDAGIContribution Contribution;

    GlobalGIInvalidation()
        : PreviousBounds(BoundingBox::Empty)
        , CurrentBounds(BoundingBox::Empty)
        , Flags(GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged)
        , ContributorId(0)
        , ContributorRevision(0)
        , Contribution(HDDAGIContribution::Auto)
    {
    }

    GlobalGIInvalidation(const BoundingBox& previousBounds, const BoundingBox& currentBounds, GlobalGIDirtyFlags flags = GlobalGIDirtyFlags::GeometryChanged | GlobalGIDirtyFlags::LightingChanged, uint64 contributorId = 0, uint32 revision = 0, HDDAGIContribution contribution = HDDAGIContribution::Auto)
        : PreviousBounds(previousBounds)
        , CurrentBounds(currentBounds)
        , Flags(flags)
        , ContributorId(contributorId)
        , ContributorRevision(revision)
        , Contribution(contribution)
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

    /// <summary>
    /// Expands the combined bounds by conservative voxel margin for voxelization clipping.
    /// </summary>
    FORCE_INLINE BoundingBox ExpandForVoxelization(float cellSize) const
    {
        BoundingBox box = GetCombinedBounds();
        if (box.Minimum.X <= box.Maximum.X)
        {
            box.Minimum -= Float3(cellSize);
            box.Maximum += Float3(cellSize);
        }
        return box;
    }

    /// <summary>
    /// Computes minimum 8x8x8 region coordinates in the specified cascade.
    /// </summary>
    FORCE_INLINE Int3 GetRegionMin(const struct HDDAGICascadeData& cascade) const;

    /// <summary>
    /// Computes maximum 8x8x8 region coordinates in the specified cascade.
    /// </summary>
    FORCE_INLINE Int3 GetRegionMax(const struct HDDAGICascadeData& cascade) const;
};

typedef GlobalGIInvalidation GlobalGIDirtyRegion;
