// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"

META_CB_BEGIN(0, Data0)
HDDAGIData HDDAGI;
uint CascadeIndex;
uint ScheduledProbeCount;
float2 Padding0;
META_CB_END

StructuredBuffer<HDDAGIProbeScheduleEntry> ProbeSchedule : register(t0);
Texture3D<uint> CombinedBlockBits : register(t1);
Texture3D<uint> CombinedRegionBits : register(t2);
Texture3D<uint> RegionVersions : register(t3);
Texture3D<float4> VoxelRadiance : register(t4);
TextureCube Skybox : register(t5);

RWTexture2D<float4> ProbeSpecularOut : register(u0);
RWTexture2D<float4> ProbeDiffuseOut : register(u1);
RWTexture2D<uint> ProbeHistory : register(u2);
RWTexture2D<float4> ProbeRunningSum : register(u3);

groupshared float3 s_BinSamples[25];
groupshared float3 s_ConvolvedDiffuse[25];

// Fixed-point HDDA Traversal function
HDDAGIHit TraceHDDA(float3 worldOrigin, float3 worldDir, uint startCascade)
{
    for (uint cascade = startCascade; cascade < HDDAGI.CascadesCount; cascade++)
    {
        HDDAGICascadeData cData = HDDAGI.Cascades[cascade];
        int3 currentCell = WorldToCascadeCell(cData, worldOrigin);

        if (!IsInsideCascade(currentCell, HDDAGI.GridSize))
            continue;

        float3 rayDir = normalize(worldDir);
        int3 stepDir = int3(rayDir.x >= 0 ? 1 : -1, rayDir.y >= 0 ? 1 : -1, rayDir.z >= 0 ? 1 : -1);

        float3 invDir = 1.0f / max(abs(rayDir), float3(1e-5f, 1e-5f, 1e-5f));
        float3 deltaT = invDir * cData.CellSize;

        float3 cellCenter = CascadeCellToWorld(cData, currentCell);
        float3 cellMin = cellCenter - (cData.CellSize * 0.5f);
        float3 cellMax = cellCenter + (cData.CellSize * 0.5f);

        float3 nextBoundary = float3(
            stepDir.x > 0 ? cellMax.x : cellMin.x,
            stepDir.y > 0 ? cellMax.y : cellMin.y,
            stepDir.z > 0 ? cellMax.z : cellMin.z
        );

        float3 tMax = (nextBoundary - worldOrigin) / rayDir;

        float distanceTraveled = 0.0f;
        int maxSteps = 128;

        for (int step = 0; step < maxSteps; step++)
        {
            if (!IsInsideCascade(currentCell, HDDAGI.GridSize))
                break;

            int3 regionCoord = currentCell / HDDAGI_REGION_SIZE;

            // Region Occupancy check (Level 1 hierarchy)
            uint regionOccupancy = CombinedRegionBits[regionCoord];
            if (regionOccupancy == 0)
            {
                // Fast-skip empty 8^3 region
                int3 targetCell = currentCell + stepDir * 8;
                currentCell = clamp(targetCell, int3(0,0,0), HDDAGI.GridSize - 1);
                continue;
            }

            int3 blockCoord = currentCell / HDDAGI_BLOCK_SIZE;

            // Block Occupancy check (Level 2 hierarchy)
            uint2 blockBits = uint2(CombinedBlockBits[int3(blockCoord.x * 2 + 0, blockCoord.y, blockCoord.z)], CombinedBlockBits[int3(blockCoord.x * 2 + 1, blockCoord.y, blockCoord.z)]);
            if (blockBits.x == 0 && blockBits.y == 0)
            {
                // Fast-skip empty 4^3 block
                int3 targetCell = currentCell + stepDir * 4;
                currentCell = clamp(targetCell, int3(0,0,0), HDDAGI.GridSize - 1);
                continue;
            }

            // Voxel-level occupancy bit test
            int3 localVoxel = currentCell % HDDAGI_BLOCK_SIZE;
            uint bitIdx = localVoxel.z * 16 + localVoxel.y * 4 + localVoxel.x;
            bool isOccupied = bitIdx < 32 ? ((blockBits.x & (1u << bitIdx)) != 0) : ((blockBits.y & (1u << (bitIdx - 32))) != 0);

            if (isOccupied)
            {
                HDDAGIHit hit;
                hit.Hit = true;
                hit.Cascade = cascade;
                hit.Cell = currentCell;
                hit.Distance = distanceTraveled;
                hit.Region = regionCoord;
                hit.HitFace = -stepDir;
                hit.ExitAxis = (tMax.x < tMax.y) ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
                return hit;
            }

            // Advance DDA step
            if (tMax.x < tMax.y)
            {
                if (tMax.x < tMax.z)
                {
                    currentCell.x += stepDir.x;
                    distanceTraveled = tMax.x;
                    tMax.x += deltaT.x;
                }
                else
                {
                    currentCell.z += stepDir.z;
                    distanceTraveled = tMax.z;
                    tMax.z += deltaT.z;
                }
            }
            else
            {
                if (tMax.y < tMax.z)
                {
                    currentCell.y += stepDir.y;
                    distanceTraveled = tMax.y;
                    tMax.y += deltaT.y;
                }
                else
                {
                    currentCell.z += stepDir.z;
                    distanceTraveled = tMax.z;
                    tMax.z += deltaT.z;
                }
            }
        }
    }

    return HDDAGIMiss();
}

// 5x5 Octahedral Workgroup (25 threads per probe)
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(5, 5, 1)]
void CS_IntegrateProbes(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    uint cascadeIdx = CascadeIndex;
    int3 probeCoord = int3(groupId.xyz);
    uint probeIndex = probeCoord.x + probeCoord.y * HDDAGI.ProbeAxisSize.x + probeCoord.z * (HDDAGI.ProbeAxisSize.x * HDDAGI.ProbeAxisSize.y);
    uint probePriority = 1;

    if (ScheduledProbeCount > 0)
    {
        uint probeIdx = groupId.x;
        if (probeIdx >= ScheduledProbeCount)
            return;

        HDDAGIProbeScheduleEntry schedule = ProbeSchedule[probeIdx];
        cascadeIdx = schedule.Cascade;
        probeIndex = schedule.ProbeIndex;
        probePriority = schedule.Priority;

        probeCoord.x = schedule.ProbeIndex % HDDAGI.ProbeAxisSize.x;
        probeCoord.y = (schedule.ProbeIndex / HDDAGI.ProbeAxisSize.x) % HDDAGI.ProbeAxisSize.y;
        probeCoord.z = schedule.ProbeIndex / (HDDAGI.ProbeAxisSize.x * HDDAGI.ProbeAxisSize.y);
    }

    HDDAGICascadeData cascade = HDDAGI.Cascades[cascadeIdx];

    int2 binCoord = int2(groupThreadId.xy);
    uint binIdx = binCoord.y * 5 + binCoord.x;

    float3 probeWorldPos = CascadeCellToWorld(cascade, probeCoord * HDDAGI_REGION_SIZE);
    float3 rayDir = GetHDDAGIBinDirection(binCoord);

    // 2D texture coordinates
    int2 binTexCoord = GetHDDAGIProbeBinCoord(probeCoord, binCoord);

    float3 incomingRadiance = float3(0, 0, 0);

    // Full Fixed-Point HDDA Traversal
    HDDAGIHit hit = TraceHDDA(probeWorldPos, rayDir, cascadeIdx);

    if (hit.Hit)
    {
        incomingRadiance = VoxelRadiance[hit.Cell].rgb;
    }
    else
    {
        // Sample skybox fallback
        incomingRadiance = HDDAGI.FallbackIrradiance.rgb * HDDAGI.Energy;
    }

    s_BinSamples[binIdx] = incomingRadiance;
    GroupMemoryBarrierWithGroupSync();

    // Temporal Moving-Window History Ring Update
    uint historyFrames = max(HDDAGI.HistoryFrames, 1u);
    uint currentRingSlot = HDDAGI.GlobalFrame % historyFrames;
    int2 historyTexCoord = GetHDDAGIProbeHistoryCoord(probeCoord, currentRingSlot, binCoord);

    uint oldSamplePacked = ProbeHistory[historyTexCoord];
    float3 oldSample = UnpackRGBE(oldSamplePacked);

    ProbeHistory[historyTexCoord] = PackRGBE(incomingRadiance);

    float4 runningSum = ProbeRunningSum[binTexCoord];
    uint validCount = (uint)runningSum.a;

    if (validCount == 0)
    {
        // First sample initialization
        runningSum = float4(incomingRadiance * (float)historyFrames, 1.0f);
        validCount = 1;
    }
    else
    {
        // Moving window update: add new, subtract exiting sample
        runningSum.rgb += (incomingRadiance - oldSample);
        validCount = min(validCount + 1u, historyFrames);
        runningSum.a = (float)validCount;
    }

    ProbeRunningSum[binTexCoord] = runningSum;

    float3 temporallyAveragedRadiance = runningSum.rgb / (float)max(validCount, 1u);

    // Octahedral Diffuse Convolution across 25 directions
    float3 convolvedDiffuse = float3(0, 0, 0);
    float totalWeight = 0.0f;
    for (int i = 0; i < 25; i++)
    {
        int2 otherBinCoord = int2(i % 5, i / 5);
        float3 otherDir = GetHDDAGIBinDirection(otherBinCoord);
        float cosineWeight = max(0.0f, dot(rayDir, otherDir));
        convolvedDiffuse += s_BinSamples[i] * cosineWeight;
        totalWeight += cosineWeight;
    }
    s_ConvolvedDiffuse[binIdx] = convolvedDiffuse / max(totalWeight, 0.001f);
    GroupMemoryBarrierWithGroupSync();

    // Write 7x7 Octahedral Tile into Probe Atlases
    int2 tileCoord = GetHDDAGIProbeTileCoord(probeCoord, binCoord + 1); // 1-pixel border offset

    ProbeSpecularOut[tileCoord] = float4(temporallyAveragedRadiance, 1.0f);
    ProbeDiffuseOut[tileCoord] = float4(s_ConvolvedDiffuse[binIdx], 1.0f);

    // Border wrapping on edge threads
    if (binCoord.x == 0 || binCoord.x == 4 || binCoord.y == 0 || binCoord.y == 4)
    {
        int2 wrappedCoord = WrapHDDAGIOctBorder(binCoord + 1);
        int2 borderTileCoord = GetHDDAGIProbeTileCoord(probeCoord, wrappedCoord);
        ProbeSpecularOut[borderTileCoord] = float4(temporallyAveragedRadiance, 1.0f);
        ProbeDiffuseOut[borderTileCoord] = float4(s_ConvolvedDiffuse[binIdx], 1.0f);
    }
}
