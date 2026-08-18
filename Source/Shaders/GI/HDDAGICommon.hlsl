// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __HDDAGI_COMMON__
#define __HDDAGI_COMMON__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/Octahedral.hlsl"

#define HDDAGI_MAX_CASCADES 8
#define HDDAGI_CASCADE_SIZE_X 128
#define HDDAGI_CASCADE_SIZE_Y 64
#define HDDAGI_CASCADE_SIZE_Z 128
#define HDDAGI_REGION_SIZE 8
#define HDDAGI_BLOCK_SIZE 4
#define HDDAGI_PROBES_X 17
#define HDDAGI_PROBES_Y 9
#define HDDAGI_PROBES_Z 17
#define HDDAGI_PROBE_BINS 25
#define HDDAGI_OCT_SIZE 5
#define HDDAGI_OCT_TILE_SIZE 7
#define HDDAGI_MAX_HISTORY_FRAMES 16

#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (1 << (FP_SHIFT - 1))

struct HDDAGICascadeData
{
    float3 WorldOffset;
    float CellSize;

    int3 CellPosition;
    uint MotionAxisMask;

    int3 RegionWorldOffset;
    uint RegionRevisionBase;

    int3 DirtyScroll;
    uint DirtyRegionCount;

    float3 WorldExtent;
    float ToCell;
};

struct HDDAGIData
{
    HDDAGICascadeData Cascades[HDDAGI_MAX_CASCADES];

    int3 GridSize;
    uint CascadesCount;

    int3 ProbeAxisSize;
    uint HistoryFrames;

    float3 ViewPosition;
    float Energy;

    float NormalBias;
    float ReflectionBias;
    float ProbeBias;
    float OcclusionBias;

    uint GlobalFrame;
    uint ProbeUpdateBudget;
    uint InactiveProbeUpdateFrames;
    uint EnableProbeFilter;

    uint EnableSpecular;
    uint EnableOcclusion;
    float BounceFeedback;
    float IndirectLightingIntensity;

    float4 FallbackIrradiance;
    float3 SkyboxIntensity;
    float Padding;
};

struct HDDAGILightData
{
    float3 Color;
    float Energy;

    float3 Direction;
    uint HasShadow;

    float3 Position;
    float Radius;

    uint Type; // 0 = Directional, 1 = Point, 2 = Spot
    float SpotCos;
    float SpotInvAttenuation;
    float Padding;
};

struct HDDAGIProbeScheduleEntry
{
    uint ProbeIndex;
    uint Cascade;
    uint Priority;
    uint Age;
};

// Modulo helper that always returns positive result
int3 PositiveModulo(int3 val, int3 modVal)
{
    int3 r = val % modVal;
    return r + (r < 0 ? modVal : 0);
}

int PositiveModulo(int val, int modVal)
{
    int r = val % modVal;
    return r + (r < 0 ? modVal : 0);
}

// Coordinate conversions
int3 WorldToCascadeCell(HDDAGICascadeData cascade, float3 worldPos)
{
    float3 localPos = worldPos - cascade.WorldOffset;
    return (int3)floor(localPos * cascade.ToCell);
}

float3 CascadeCellToWorld(HDDAGICascadeData cascade, int3 cell)
{
    return ((float3)cell + 0.5f) * cascade.CellSize + cascade.WorldOffset;
}

bool IsInsideCascade(int3 cell, int3 gridSize)
{
    return all(cell >= 0) && all(cell < gridSize);
}

// Octahedral 5x5 bin direction calculation
float3 GetHDDAGIBinDirection(int2 binCoord)
{
    float2 uv = ((float2(binCoord) + 0.5f) / (float)HDDAGI_OCT_SIZE) * 2.0f - 1.0f;
    return GetOctahedralDirection(uv);
}

// 2D texture coordinate mappings
int2 GetHDDAGIProbeBinCoord(int3 probeCoord, int2 binCoord)
{
    return int2(probeCoord.x + probeCoord.z * HDDAGI_PROBES_X, probeCoord.y) * HDDAGI_OCT_SIZE + binCoord;
}

int2 GetHDDAGIProbeHistoryCoord(int3 probeCoord, uint slot, int2 binCoord)
{
    return int2(probeCoord.x + probeCoord.z * HDDAGI_PROBES_X, probeCoord.y * HDDAGI_MAX_HISTORY_FRAMES + slot) * HDDAGI_OCT_SIZE + binCoord;
}

int2 GetHDDAGIProbeTileCoord(int3 probeCoord, int2 localTileCoord)
{
    return int2(probeCoord.x + probeCoord.z * HDDAGI_PROBES_X, probeCoord.y) * HDDAGI_OCT_TILE_SIZE + localTileCoord;
}

// Wrap border coordinates for 7x7 octahedral map (1px border around 5x5 interior)
int2 WrapHDDAGIOctBorder(int2 octCoord)
{
    int2 wrapped = octCoord - 1; // map 1..5 to 0..4
    if (wrapped.x < 0)
    {
        wrapped.x = 0;
        wrapped.y = (HDDAGI_OCT_SIZE - 1) - wrapped.y;
    }
    else if (wrapped.x >= HDDAGI_OCT_SIZE)
    {
        wrapped.x = HDDAGI_OCT_SIZE - 1;
        wrapped.y = (HDDAGI_OCT_SIZE - 1) - wrapped.y;
    }
    if (wrapped.y < 0)
    {
        wrapped.y = 0;
        wrapped.x = (HDDAGI_OCT_SIZE - 1) - wrapped.x;
    }
    else if (wrapped.y >= HDDAGI_OCT_SIZE)
    {
        wrapped.y = HDDAGI_OCT_SIZE - 1;
        wrapped.x = (HDDAGI_OCT_SIZE - 1) - wrapped.x;
    }
    return wrapped;
}

// Ray Hit Cache packing
uint PackRayHitCache(int3 hitCell, uint cascade, uint exitAxis, bool hit, bool valid)
{
    uint packed = 0;
    packed |= (uint(hitCell.x) & 0xFFu);
    packed |= (uint(hitCell.y) & 0xFFu) << 8;
    packed |= (uint(hitCell.z) & 0xFFu) << 16;
    packed |= (cascade & 0x07u) << 24;
    packed |= (exitAxis & 0x03u) << 27;
    if (hit) packed |= (1u << 30);
    if (valid) packed |= (1u << 31);
    return packed;
}

void UnpackRayHitCache(uint packed, out int3 hitCell, out uint cascade, out uint exitAxis, out bool hit, out bool valid)
{
    hitCell.x = int(packed & 0xFFu);
    hitCell.y = int((packed >> 8) & 0xFFu);
    hitCell.z = int((packed >> 16) & 0xFFu);
    cascade = (packed >> 24) & 0x07u;
    exitAxis = (packed >> 27) & 0x03u;
    hit = (packed & (1u << 30)) != 0;
    valid = (packed & (1u << 31)) != 0;
}

// RGBE packing for compact history storage
uint PackRGBE(float3 rgb)
{
    float maxComponent = max(max(rgb.r, rgb.g), rgb.b);
    if (maxComponent < 1e-6f)
        return 0;

    int exp;
    float scale = frexp(maxComponent, exp);
    scale = scale * 256.0f / maxComponent;

    uint r = (uint)clamp(rgb.r * scale, 0.0f, 255.0f);
    uint g = (uint)clamp(rgb.g * scale, 0.0f, 255.0f);
    uint b = (uint)clamp(rgb.b * scale, 0.0f, 255.0f);
    uint e = (uint)clamp(exp + 128, 0, 255);

    return r | (g << 8) | (b << 16) | (e << 24);
}

float3 UnpackRGBE(uint rgbe)
{
    if (rgbe == 0)
        return float3(0, 0, 0);

    int exp = int((rgbe >> 24) & 0xFF) - 128;
    float scale = ldexp(1.0f / 256.0f, exp);

    float r = float(rgbe & 0xFF) * scale;
    float g = float((rgbe >> 8) & 0xFF) * scale;
    float b = float((rgbe >> 16) & 0xFF) * scale;

    return float3(r, g, b);
}

// Hit structure for fixed-point HDDA traversal
struct HDDAGIHit
{
    bool Hit;
    uint Cascade;
    int3 Cell;
    int3 HitFace;
    int3 Region;
    float Distance;
    uint ExitAxis;
};

HDDAGIHit HDDAGIMiss()
{
    HDDAGIHit hit;
    hit.Hit = false;
    hit.Cascade = 0;
    hit.Cell = int3(0, 0, 0);
    hit.HitFace = int3(0, 0, 0);
    hit.Region = int3(0, 0, 0);
    hit.Distance = 1e6f;
    hit.ExitAxis = 3;
    return hit;
}

#endif
