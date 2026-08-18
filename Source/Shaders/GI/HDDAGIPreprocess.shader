// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"

META_CB_BEGIN(0, Data0)
HDDAGIData HDDAGI;
uint CascadeIndex;
uint LayerMode; // 0 = Static, 1 = Dynamic, 2 = Combined
uint DirtyRegionCount;
uint Padding0;
META_CB_END

// Occupancy textures
Texture3D<uint> StaticNormalScratch : register(t0);
Texture3D<uint> DynamicNormalScratch : register(t1);
Texture3D<uint> RegionBitsSRV : register(t2);
Texture3D<uint> ProbeProcessFrameSRV : register(t3);
Texture3D<uint> ProbeGeometryProximitySRV : register(t4);
Texture3D<uint> ProbeCameraVisibilitySRV : register(t5);

RWTexture3D<uint> BlockBitsOut : register(u0);
RWTexture3D<uint> RegionBitsOut : register(u1);
RWTexture3D<uint> RegionVersionsOut : register(u2);

RWTexture3D<uint> ProbeGeometryProximityOut : register(u0);
RWTexture3D<uint> ProbeCameraVisibilityOut : register(u1);
RWTexture3D<uint> ProbeNeighbourVisibilityOut : register(u2);

RWStructuredBuffer<HDDAGIProbeScheduleEntry> ProbePriorityListOut : register(u0);

groupshared uint s_SolidCount;
groupshared uint s_BlockBits[8][2];

// One threadgroup handles one 8x8x8 region
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 8)]
void CS_BuildHierarchy(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    if (groupThreadId.x == 0 && groupThreadId.y == 0 && groupThreadId.z == 0)
    {
        s_SolidCount = 0;
        for (int b = 0; b < 8; b++)
        {
            s_BlockBits[b][0] = 0;
            s_BlockBits[b][1] = 0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    int3 voxelCoord = int3(dispatchThreadId);
    if (all(voxelCoord < HDDAGI.GridSize))
    {
        uint solidBit = (StaticNormalScratch[voxelCoord] != 0) ? 1u : 0u;
        if (LayerMode == 1) // Dynamic
            solidBit = (DynamicNormalScratch[voxelCoord] != 0) ? 1u : 0u;
        else if (LayerMode == 2) // Combined
            solidBit = ((StaticNormalScratch[voxelCoord] | DynamicNormalScratch[voxelCoord]) != 0) ? 1u : 0u;

        if (solidBit != 0)
        {
            InterlockedAdd(s_SolidCount, 1u);

            // Determine which 4x4x4 block inside the 8x8x8 region (0..7)
            uint blockIdx = (groupThreadId.z / 4) * 4 + (groupThreadId.y / 4) * 2 + (groupThreadId.x / 4);
            uint localBitIdx = (groupThreadId.z % 4) * 16 + (groupThreadId.y % 4) * 4 + (groupThreadId.x % 4);

            if (localBitIdx < 32)
                InterlockedOr(s_BlockBits[blockIdx][0], 1u << localBitIdx);
            else
                InterlockedOr(s_BlockBits[blockIdx][1], 1u << (localBitIdx - 32));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Write out block masks and region mask
    if (groupThreadId.x < 2 && groupThreadId.y < 2 && groupThreadId.z < 2)
    {
        uint blockIdx = groupThreadId.z * 4 + groupThreadId.y * 2 + groupThreadId.x;
        int3 blockCoord = int3(groupId) * 2 + int3(groupThreadId);
        BlockBitsOut[int3(blockCoord.x * 2 + 0, blockCoord.y, blockCoord.z)] = s_BlockBits[blockIdx][0];
        BlockBitsOut[int3(blockCoord.x * 2 + 1, blockCoord.y, blockCoord.z)] = s_BlockBits[blockIdx][1];
    }

    if (groupThreadId.x == 0 && groupThreadId.y == 0 && groupThreadId.z == 0)
    {
        int3 regionCoord = int3(groupId);
        RegionBitsOut[regionCoord] = s_SolidCount > 0 ? 1u : 0u;
        RegionVersionsOut[regionCoord] = RegionVersionsOut[regionCoord] + 1u;
    }
}

// Proximity and visibility metadata
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 1)]
void CS_UpdateProbeMetadata(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int3 probeCoord = int3(dispatchThreadId);
    if (any(probeCoord >= HDDAGI.ProbeAxisSize))
        return;

    // Check 3x3x3 region proximity
    int3 baseRegion = probeCoord;
    uint nearGeometry = 0;
    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int3 r = baseRegion + int3(dx, dy, dz);
                if (all(r >= 0) && all(r < HDDAGI.GridSize / HDDAGI_REGION_SIZE))
                {
                    if (RegionBitsSRV[r] != 0)
                        nearGeometry = 1;
                }
            }
        }
    }
    ProbeGeometryProximityOut[probeCoord] = nearGeometry;

    // Probe camera visibility check
    HDDAGICascadeData cascade = HDDAGI.Cascades[CascadeIndex];
    float3 probeWorldPos = CascadeCellToWorld(cascade, probeCoord * HDDAGI_REGION_SIZE);
    float distToCamera = length(probeWorldPos - HDDAGI.ViewPosition);
    uint isVisible = (distToCamera < cascade.WorldExtent.x * 1.5f) ? 1u : 0u;
    ProbeCameraVisibilityOut[probeCoord] = isVisible;
    ProbeNeighbourVisibilityOut[probeCoord] = 0xFFFFFFFFu;
}

// Compact probe priority list
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(64, 1, 1)]
void CS_BuildProbePriorityList(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIdx = dispatchThreadId.x;
    uint totalProbes = HDDAGI.ProbeAxisSize.x * HDDAGI.ProbeAxisSize.y * HDDAGI.ProbeAxisSize.z;
    if (probeIdx >= totalProbes)
        return;

    int3 probeCoord;
    probeCoord.x = probeIdx % HDDAGI.ProbeAxisSize.x;
    probeCoord.y = (probeIdx / HDDAGI.ProbeAxisSize.x) % HDDAGI.ProbeAxisSize.y;
    probeCoord.z = probeIdx / (HDDAGI.ProbeAxisSize.x * HDDAGI.ProbeAxisSize.y);

    uint processFrame = ProbeProcessFrameSRV[probeCoord];
    uint forcedCount = (processFrame >> 28) & 0x0Fu;
    uint nearGeometry = ProbeGeometryProximitySRV[probeCoord];
    uint isVisible = ProbeCameraVisibilitySRV[probeCoord];

    // Determine priority
    uint priority = 7; // lowest
    if (forcedCount > 0)
        priority = 0;
    else if (isVisible != 0 && nearGeometry != 0)
        priority = 2;
    else if (isVisible != 0)
        priority = 3;
    else if (nearGeometry != 0)
        priority = 4;
    else if (((probeCoord.x ^ probeCoord.y ^ probeCoord.z ^ HDDAGI.GlobalFrame) % max(HDDAGI.InactiveProbeUpdateFrames, 1u)) == 0)
        priority = 6;

    if (priority <= 6)
    {
        HDDAGIProbeScheduleEntry entry;
        entry.ProbeIndex = probeIdx;
        entry.Cascade = CascadeIndex;
        entry.Priority = priority;
        entry.Age = processFrame & 0x0FFFFFFFu;

        // Add to priority list buffer
        ProbePriorityListOut[probeIdx] = entry;
    }
}
