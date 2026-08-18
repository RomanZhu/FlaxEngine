// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Int3.h"
#include "Engine/Core/Math/Int4.h"
#include "Engine/Core/Math/Vector4.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "GlobalGIInvalidation.h"

#define HDDAGI_MAX_CASCADES 8
#define HDDAGI_CASCADE_SIZE_X 128
#define HDDAGI_CASCADE_SIZE_Y 64
#define HDDAGI_CASCADE_SIZE_Z 128
#define HDDAGI_CASCADE_SIZE_Y_HIGH 128
#define HDDAGI_REGION_SIZE 8
#define HDDAGI_BLOCK_SIZE 4
#define HDDAGI_PROBES_X 17
#define HDDAGI_PROBES_Y 9
#define HDDAGI_PROBES_Z 17
#define HDDAGI_PROBES_Y_HIGH 17
#define HDDAGI_PROBE_BINS 25
#define HDDAGI_OCT_SIZE 5
#define HDDAGI_OCT_TILE_SIZE 7
#define HDDAGI_DEFAULT_HISTORY_FRAMES 6
#define HDDAGI_MAX_HISTORY_FRAMES 16
#define HDDAGI_DEFAULT_PROBE_BUDGET 1024

/// <summary>
/// GPU cascade transform and metadata structure for HDDAGI.
/// </summary>
struct HDDAGICascadeData
{
    Float3 WorldOffset = Float3::Zero;
    float CellSize = 0.0f;

    Int3 CellPosition = Int3::Zero;
    uint32 MotionAxisMask = 0;

    Int3 RegionWorldOffset = Int3::Zero;
    uint32 RegionRevisionBase = 1;

    Int3 DirtyScroll = Int3::Zero;
    uint32 DirtyRegionCount = 0;

    Float3 WorldExtent = Float3::Zero;
    float ToCell = 0.0f;
};

FORCE_INLINE Int3 GlobalGIInvalidation::GetRegionMin(const HDDAGICascadeData& cascade) const
{
    const BoundingBox box = GetCombinedBounds();
    if (box.Minimum.X > box.Maximum.X)
        return Int3::Zero;
    const Float3 minCell = (box.Minimum - cascade.WorldOffset) * cascade.ToCell;
    return Int3(
        Math::FloorToInt(minCell.X / (float)HDDAGI_REGION_SIZE),
        Math::FloorToInt(minCell.Y / (float)HDDAGI_REGION_SIZE),
        Math::FloorToInt(minCell.Z / (float)HDDAGI_REGION_SIZE)
    );
}

FORCE_INLINE Int3 GlobalGIInvalidation::GetRegionMax(const HDDAGICascadeData& cascade) const
{
    const BoundingBox box = GetCombinedBounds();
    if (box.Minimum.X > box.Maximum.X)
        return Int3::Zero;
    const Float3 maxCell = (box.Maximum - cascade.WorldOffset) * cascade.ToCell;
    return Int3(
        Math::FloorToInt(maxCell.X / (float)HDDAGI_REGION_SIZE),
        Math::FloorToInt(maxCell.Y / (float)HDDAGI_REGION_SIZE),
        Math::FloorToInt(maxCell.Z / (float)HDDAGI_REGION_SIZE)
    );
}

/// <summary>
/// GPU constant buffer layout for HDDAGI shaders.
/// </summary>
GPU_CB_STRUCT(HDDAGIConstantsData
{
    HDDAGICascadeData Cascades[HDDAGI_MAX_CASCADES];

    Int3 GridSize;
    uint32 CascadesCount;

    Int3 ProbeAxisSize;
    uint32 HistoryFrames;

    Float3 ViewPosition;
    float Energy;

    float NormalBias;
    float ReflectionBias;
    float ProbeBias;
    float OcclusionBias;

    uint32 GlobalFrame;
    uint32 ProbeUpdateBudget;
    uint32 InactiveProbeUpdateFrames;
    uint32 EnableProbeFilter;

    uint32 EnableSpecular;
    uint32 EnableOcclusion;
    float BounceFeedback;
    float IndirectLightingIntensity;

    Float4 FallbackIrradiance;
    Float3 SkyboxIntensity;
    float Padding;
});

/// <summary>
/// Process voxel record for GPU direct-light and material dispatch.
/// </summary>
struct HDDAGIProcessVoxel
{
    uint32 PackedPositionAndFlags;
    uint32 PackedAlbedoNormal;
    uint32 PackedEmission;
    uint32 PackedProbeOcclusion;
};

/// <summary>
/// Packed light data structure for GPU HDDAGI direct light injection.
/// </summary>
struct HDDAGILightData
{
    Float3 Color;
    float Energy;

    Float3 Direction;
    uint32 HasShadow;

    Float3 Position;
    float Radius;

    uint32 Type; // 0 = Directional, 1 = Point, 2 = Spot
    float SpotCos;
    float SpotInvAttenuation;
    float Padding;
};

/// <summary>
/// Scheduled probe record for GPU probe integration.
/// </summary>
struct HDDAGIProbeScheduleEntry
{
    uint32 ProbeIndex;
    uint32 Cascade;
    uint32 Priority;
    uint32 Age;
};
