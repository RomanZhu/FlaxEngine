// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __GDFGI_HLSL__
#define __GDFGI_HLSL__

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/Octahedral.hlsl"

#define GDFGI_OCT_RESOLUTION 5
#define GDFGI_OCT_TILE_SIZE 7 // 5 + 2 border texels
#define GDFGI_RAYS_COUNT 25
#define GDFGI_DEFAULT_HISTORY_FRAMES 8
#define GDFGI_CASCADE_BLEND_SIZE 2.0f
#define GDFGI_DEFAULT_BIAS 0.2f

#define GDFGI_PROBE_STATE_INACTIVE 0
#define GDFGI_PROBE_STATE_ACTIVE 1
#define GDFGI_PROBE_STATE_ACTIVATED 2
#define GDFGI_PROBE_STATE_INACTIVE_INSIDE 3
#define GDFGI_PROBE_STATE_INACTIVE_EMPTY 4
#define GDFGI_PROBE_STATE_MASK 0x0Fu
#define GDFGI_PROBE_STATE_HISTORY_INVALID 0x80u

// GDFGI data for a constant buffer
struct GDFGIData
{
    float4 ProbesOriginAndSpacing[4];
    float4 BlendOrigin[4]; // w is unused
    int4 ProbesScrollOffsets[4]; // w is unused
    uint3 ProbesCounts;
    uint CascadesCount;
    float3 ViewPos;
    float IndirectLightingIntensity;
    float4 FallbackIrradiance;
    float RayMaxDistance;
    float NormalBias;
    float ViewBias;
    float ThinGeometryExpansion;
    uint RaysCount;
    uint HistoryFrames;
    uint HistoryFrameIndex;
    uint DynamicInvalidation;
    uint EnableDirectionalSpecular;
    uint Algorithm; // 2 for GDFGI
    uint UpdateRowOffset;
    uint UpdateRowCount;
    float4 CascadeDirtyBoundsMin[4]; // xyz: min, w: 1.0 if active
    float4 CascadeDirtyBoundsMax[4]; // xyz: max, w: unused
    int4 ProbeScrollClears[4];
    uint DebugExecutionStage;
    uint CulledObjectsCapacity;
    uint ObjectsCount;
    float Padding;
};

// Probe indexing
uint GetGDFGIProbeIndex(GDFGIData data, uint3 probeCoords)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = probeCoords.y;
    uint probeIndexInPlane = probeCoords.x + (data.ProbesCounts.x * probeCoords.z);
    return planeIndex * probesPerPlane + probeIndexInPlane;
}

uint GetGDFGIProbeIndex(GDFGIData data, uint2 texCoords, uint texResolution)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = texCoords.x / (data.ProbesCounts.x * texResolution);
    uint probeIndexInPlane = (texCoords.x / texResolution) - (planeIndex * data.ProbesCounts.x) + (data.ProbesCounts.x * (texCoords.y / texResolution));
    return planeIndex * probesPerPlane + probeIndexInPlane;
}

uint3 GetGDFGIProbeCoords(GDFGIData data, uint probeIndex)
{
    uint3 probeCoords;
    probeCoords.x = probeIndex % data.ProbesCounts.x;
    probeCoords.y = probeIndex / (data.ProbesCounts.x * data.ProbesCounts.z);
    probeCoords.z = (probeIndex / data.ProbesCounts.x) % data.ProbesCounts.z;
    return probeCoords;
}

// Toroidal coordinate mapping for storage textures
uint3 GetGDFGIScrollingProbeCoords(GDFGIData data, uint cascadeIndex, uint3 probeCoords)
{
    int3 scrollOffsets = data.ProbesScrollOffsets[cascadeIndex].xyz;
    int3 scrolledCoords = (int3)probeCoords + scrollOffsets;
    scrolledCoords = (scrolledCoords % (int3)data.ProbesCounts + (int3)data.ProbesCounts) % (int3)data.ProbesCounts;
    return (uint3)scrolledCoords;
}

uint GetGDFGIScrollingProbeIndex(GDFGIData data, uint cascadeIndex, uint3 probeCoords)
{
    int3 probeCoordsOffset = (int3)data.ProbesCounts + data.ProbesScrollOffsets[cascadeIndex].xyz;
    return GetGDFGIProbeIndex(data, (probeCoords + (uint3)probeCoordsOffset) % data.ProbesCounts);
}

uint2 GetGDFGIProbeTexelCoords(GDFGIData data, uint cascadeIndex, uint probeIndex)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = probeIndex / probesPerPlane;
    uint gridSpaceX = probeIndex % data.ProbesCounts.x;
    uint gridSpaceY = (probeIndex / data.ProbesCounts.x) % data.ProbesCounts.z;
    uint x = gridSpaceX + planeIndex * data.ProbesCounts.x;
    uint y = gridSpaceY + cascadeIndex * data.ProbesCounts.z;
    return uint2(x, y);
}

// Computes probe world space base position
float3 GetGDFGIProbeWorldPosition(GDFGIData data, uint cascadeIndex, uint3 probeCoords)
{
    float3 probesOrigin = data.ProbesOriginAndSpacing[cascadeIndex].xyz;
    float probesSpacing = data.ProbesOriginAndSpacing[cascadeIndex].w;
    float3 probePosition = (float3)probeCoords * probesSpacing;
    float3 probeGridOffset = (probesSpacing * ((float3)data.ProbesCounts - 1.0f)) * 0.5f;
    float3 probeScrollOffset = (float3)data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing;
    return probesOrigin + probePosition - probeGridOffset + probeScrollOffset;
}

// Computes probe final world position including relocation offset from ProbesData
float3 GetGDFGIProbePosition(GDFGIData data, uint cascadeIndex, uint3 probeCoords)
{
    return GetGDFGIProbeWorldPosition(data, cascadeIndex, probeCoords);
}

float3 GetGDFGIProbePositionRelocated(GDFGIData data, Texture2D<snorm float4> probesData, uint cascadeIndex, uint3 probeCoords)
{
    float3 basePos = GetGDFGIProbeWorldPosition(data, cascadeIndex, probeCoords);
    float probesSpacing = data.ProbesOriginAndSpacing[cascadeIndex].w;
    uint probeIndex = GetGDFGIScrollingProbeIndex(data, cascadeIndex, probeCoords);
    uint2 texCoord = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    float3 probeOffset = probesData[texCoord].xyz * probesSpacing;
    return basePos + probeOffset;
}

// Decodes directional bin [0..4, 0..4] with jitter to world ray direction
float3 GetGDFGIRayDirection(uint2 octBin, float2 jitter)
{
    float2 octUV = (float2(octBin) + 0.5f + (jitter - 0.5f) * 0.8f) / float(GDFGI_OCT_RESOLUTION);
    float2 octCoords = octUV * 2.0f - 1.0f;
    return GetOctahedralDirection(octCoords);
}

// Decodes directional bin center direction without jitter
float3 GetGDFGIBinDirection(uint2 octBin)
{
    float2 octUV = (float2(octBin) + 0.5f) / float(GDFGI_OCT_RESOLUTION);
    float2 octCoords = octUV * 2.0f - 1.0f;
    return GetOctahedralDirection(octCoords);
}

// Evaluates Chebyshev visibility test for a probe distance moment
float ChebyshevWeight(float2 distanceMoments, float distanceToProbe)
{
    float mean = distanceMoments.x;
    float variance = max(distanceMoments.y - mean * mean, 1e-4f);
    float diff = distanceToProbe - mean;
    if (diff <= 0.0f)
        return 1.0f; // Surface is in front of the probe occluder
    float pMax = variance / (variance + diff * diff);
    return max(pMax * pMax * pMax, 0.0f);
}

// Samples diffuse irradiance from a single probe in direction of normal
float3 SampleGDFGIProbeDiffuse(GDFGIData data, Texture2D<float4> directionalDiffuse, uint cascadeIndex, uint probeIndex, float3 normal)
{
    uint2 probeTexCoord = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    uint2 diffuseTileBase = probeTexCoord * GDFGI_OCT_TILE_SIZE;

    float2 normalOct = GetOctahedralCoords(normal);
    float2 normalUV = normalOct * 0.5f + 0.5f;
    uint2 tileCoord = uint2(clamp(normalUV * float(GDFGI_OCT_RESOLUTION), 0.0f, float(GDFGI_OCT_RESOLUTION - 1))) + 1;
    uint2 sampleCoord = diffuseTileBase + tileCoord;

    return directionalDiffuse[sampleCoord].rgb;
}

// Bounded production fallback used on D3D11 while the full 8-probe visibility
// filter is being stabilized. It performs at most one state and one diffuse
// lookup per cascade and validates every derived coordinate before loading.
float3 SampleGDFGINearestIrradiance(GDFGIData data, Texture2D<uint> probeStates, Texture2D<float4> directionalDiffuse, float3 worldPos, float3 normal)
{
    uint stateWidth, stateHeight;
    uint diffuseWidth, diffuseHeight;
    probeStates.GetDimensions(stateWidth, stateHeight);
    directionalDiffuse.GetDimensions(diffuseWidth, diffuseHeight);
    const uint cascadesCount = min(data.CascadesCount, 4u);

    [loop]
    for (uint cascadeIndex = 0; cascadeIndex < cascadesCount; cascadeIndex++)
    {
        const float probesSpacing = max(data.ProbesOriginAndSpacing[cascadeIndex].w, 0.01f);
        const float3 probeGridOffset = (probesSpacing * ((float3)data.ProbesCounts - 1.0f)) * 0.5f;
        const float3 probeScrollOffset = (float3)data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing;
        const float3 gridPos = (worldPos - data.ProbesOriginAndSpacing[cascadeIndex].xyz + probeGridOffset - probeScrollOffset) / probesSpacing;
        if (any(gridPos < 0.0f) || any(gridPos > (float3)data.ProbesCounts - 1.0f))
            continue;

        const uint3 probeCoord = (uint3)clamp(round(gridPos), 0.0f, (float3)data.ProbesCounts - 1.0f);
        const uint probeIndex = GetGDFGIScrollingProbeIndex(data, cascadeIndex, probeCoord);
        const uint2 probeTexCoord = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
        if (probeTexCoord.x >= stateWidth || probeTexCoord.y >= stateHeight)
            continue;
        const uint state = probeStates[probeTexCoord];
        if ((state & GDFGI_PROBE_STATE_MASK) == GDFGI_PROBE_STATE_INACTIVE_INSIDE)
            continue;

        const float2 normalUV = GetOctahedralCoords(normal) * 0.5f + 0.5f;
        const uint2 tileCoord = (uint2)clamp(normalUV * float(GDFGI_OCT_RESOLUTION), 0.0f, float(GDFGI_OCT_RESOLUTION - 1)) + 1;
        const uint2 sampleCoord = probeTexCoord * GDFGI_OCT_TILE_SIZE + tileCoord;
        if (sampleCoord.x >= diffuseWidth || sampleCoord.y >= diffuseHeight)
            continue;
        return directionalDiffuse[sampleCoord].rgb * (2.0f * PI);
    }
    return data.FallbackIrradiance.rgb;
}

// Smooth bounded interpolation for production D3D11 composition. Unlike the
// original visibility filter, this path does not fetch relocation and distance
// moments per neighbor, keeping the fullscreen workload below the TDR limit.
float3 SampleGDFGITrilinearIrradiance(GDFGIData data, Texture2D<uint> probeStates, Texture2D<float4> directionalDiffuse, float3 worldPos, float3 normal)
{
    uint stateWidth, stateHeight;
    uint diffuseWidth, diffuseHeight;
    probeStates.GetDimensions(stateWidth, stateHeight);
    directionalDiffuse.GetDimensions(diffuseWidth, diffuseHeight);
    const uint cascadesCount = min(data.CascadesCount, 4u);
    const float2 normalUV = GetOctahedralCoords(normal) * 0.5f + 0.5f;
    const uint2 tileCoord = (uint2)clamp(normalUV * float(GDFGI_OCT_RESOLUTION), 0.0f, float(GDFGI_OCT_RESOLUTION - 1)) + 1;
    float3 pendingFineIrradiance = 0.0f;
    float pendingFineWeight = 0.0f;
    bool hasPendingFine = false;

    [loop]
    for (uint cascadeIndex = 0; cascadeIndex < cascadesCount; cascadeIndex++)
    {
        const float probesSpacing = max(data.ProbesOriginAndSpacing[cascadeIndex].w, 0.01f);
        const float3 probeGridOffset = (probesSpacing * ((float3)data.ProbesCounts - 1.0f)) * 0.5f;
        const float3 probeScrollOffset = (float3)data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing;
        const float3 gridPos = (worldPos - data.ProbesOriginAndSpacing[cascadeIndex].xyz + probeGridOffset - probeScrollOffset) / probesSpacing;
        if (any(gridPos < 0.0f) || any(gridPos >= (float3)data.ProbesCounts - 1.0f))
            continue;

        const uint3 baseCoord = (uint3)floor(gridPos);
        const float3 alpha = frac(gridPos);
        float3 irradianceSum = 0.0f;
        float weightSum = 0.0f;
        [unroll]
        for (uint i = 0; i < 8; i++)
        {
            const uint3 offset = uint3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
            const uint3 probeCoord = baseCoord + offset;
            const uint probeIndex = GetGDFGIScrollingProbeIndex(data, cascadeIndex, probeCoord);
            const uint2 probeTexCoord = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
            if (probeTexCoord.x >= stateWidth || probeTexCoord.y >= stateHeight)
                continue;
            const uint state = probeStates[probeTexCoord];
            if ((state & GDFGI_PROBE_STATE_MASK) == GDFGI_PROBE_STATE_INACTIVE_INSIDE)
                continue;
            const uint2 sampleCoord = probeTexCoord * GDFGI_OCT_TILE_SIZE + tileCoord;
            if (sampleCoord.x >= diffuseWidth || sampleCoord.y >= diffuseHeight)
                continue;

            const float3 axisWeight = lerp(1.0f - alpha, alpha, (float3)offset);
            const float weight = axisWeight.x * axisWeight.y * axisWeight.z;
            irradianceSum += directionalDiffuse[sampleCoord].rgb * weight;
            weightSum += weight;
        }
        if (weightSum > 1e-4f)
        {
            float3 cascadeIrradiance = irradianceSum * ((2.0f * PI) / weightSum);
            const float3 edgeDistances = min(gridPos, (float3)data.ProbesCounts - 1.0f - gridPos);
            const float edgeDistance = min(edgeDistances.x, min(edgeDistances.y, edgeDistances.z));
            const float fineWeight = saturate(edgeDistance / GDFGI_CASCADE_BLEND_SIZE);

            if (hasPendingFine)
                cascadeIrradiance = lerp(cascadeIrradiance, pendingFineIrradiance, pendingFineWeight);
            if (fineWeight >= 0.999f || cascadeIndex + 1u >= cascadesCount)
                return cascadeIrradiance;

            // Near this cascade's boundary, defer the result until the next
            // coarser cascade is sampled and blend across the overlap region.
            pendingFineIrradiance = cascadeIrradiance;
            pendingFineWeight = fineWeight;
            hasPendingFine = true;
        }
    }
    if (hasPendingFine)
        return pendingFineIrradiance;
    return data.FallbackIrradiance.rgb;
}

// Samples probe distance moments (R: mean, G: mean^2) in direction of vector
float2 SampleGDFGIProbeDistance(GDFGIData data, Texture2D<float4> probesDistance, uint cascadeIndex, uint probeIndex, float3 dir)
{
    uint2 probeTexCoord = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    uint2 distanceTileBase = probeTexCoord * GDFGI_OCT_TILE_SIZE;

    float2 dirOct = GetOctahedralCoords(dir);
    float2 dirUV = dirOct * 0.5f + 0.5f;
    uint2 tileCoord = uint2(clamp(dirUV * float(GDFGI_OCT_RESOLUTION), 0.0f, float(GDFGI_OCT_RESOLUTION - 1))) + 1;
    uint2 sampleCoord = distanceTileBase + tileCoord;

    return probesDistance[sampleCoord].rg;
}

// Samples raw directional radiance in the reflection direction
float3 SampleGDFGISpecular(GDFGIData data, Texture2D<float4> directionalRadiance, Texture2D<uint> probeStates, float3 worldPos, float3 reflectDir, float roughness)
{
    if (data.CascadesCount == 0)
        return float3(0, 0, 0);

    float2 reflectOct = GetOctahedralCoords(reflectDir);
    float2 reflectUV = reflectOct * 0.5f + 0.5f;
    uint2 octBin = uint2(clamp(reflectUV * float(GDFGI_OCT_RESOLUTION), 0.0f, float(GDFGI_OCT_RESOLUTION - 1)));

    float3 probesOrigin = data.ProbesOriginAndSpacing[0].xyz;
    float probesSpacing = max(data.ProbesOriginAndSpacing[0].w, 0.01f);
    float3 probeGridOffset = (probesSpacing * ((float3)data.ProbesCounts - 1.0f)) * 0.5f;
    float3 probeScrollOffset = (float3)data.ProbesScrollOffsets[0].xyz * probesSpacing;
    float3 gridPos = (worldPos - probesOrigin + probeGridOffset - probeScrollOffset) / probesSpacing;

    if (any(gridPos < 0.0f) || any(gridPos >= (float3)data.ProbesCounts - 1.0f))
        return float3(0, 0, 0);

    int3 baseCoord = (int3)floor(gridPos);
    float3 alpha = frac(gridPos);
    float3 specularSum = float3(0, 0, 0);
    float weightSum = 0.0f;

    [unroll]
    for (int i = 0; i < 8; i++)
    {
        int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 probeCoord = baseCoord + offset;
        float3 trilinearWeight3D = lerp(1.0f - alpha, alpha, (float3)offset);
        float weight = trilinearWeight3D.x * trilinearWeight3D.y * trilinearWeight3D.z;

        uint probeIndex = GetGDFGIScrollingProbeIndex(data, 0, (uint3)probeCoord);
        uint2 stateTex = GetGDFGIProbeTexelCoords(data, 0, probeIndex);
        uint state = probeStates[stateTex];
        if ((state & GDFGI_PROBE_STATE_MASK) == GDFGI_PROBE_STATE_INACTIVE_INSIDE)
            continue;

        uint2 probeTexCoord = stateTex;
        uint2 sampleCoord = probeTexCoord * GDFGI_OCT_RESOLUTION + octBin;
        float3 sampleVal = directionalRadiance[sampleCoord].rgb;

        specularSum += sampleVal * weight;
        weightSum += weight;
    }

    if (weightSum > 1e-4f)
        return specularSum / weightSum;
    return float3(0, 0, 0);
}

// Samples cascaded 8-probe neighborhood with trilinear interpolation and Chebyshev visibility weighting
float3 SampleGDFGIIrradiance(GDFGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> directionalDiffuse, float3 worldPos, float3 normal, float3 geometricNormal, float bias)
{
    float3 biasedWorldPos = worldPos + normal * bias * 0.2f + geometricNormal * bias * 0.1f;
    float3 totalIrradiance = float3(0, 0, 0);
    float totalWeight = 0.0f;

    // Loop through cascades from finest (0) to coarsest (CascadesCount-1)
    [loop]
    for (uint cascadeIndex = 0; cascadeIndex < data.CascadesCount; cascadeIndex++)
    {
        float3 probesOrigin = data.ProbesOriginAndSpacing[cascadeIndex].xyz;
        float probesSpacing = max(data.ProbesOriginAndSpacing[cascadeIndex].w, 0.01f);
        float3 probeGridOffset = (probesSpacing * ((float3)data.ProbesCounts - 1.0f)) * 0.5f;
        float3 probeScrollOffset = (float3)data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing;
        float3 gridPos = (biasedWorldPos - probesOrigin + probeGridOffset - probeScrollOffset) / probesSpacing;

        // Check if position is within grid boundaries
        if (any(gridPos < 0.0f) || any(gridPos >= (float3)data.ProbesCounts - 1.0f))
            continue;

        int3 baseCoord = (int3)floor(gridPos);
        float3 alpha = frac(gridPos);

        float cascadeIrradianceWeight = 0.0f;
        float3 cascadeIrradianceSum = float3(0, 0, 0);

        // 8 neighboring probes in the 2x2x2 cube
        [unroll]
        for (int i = 0; i < 8; i++)
        {
            int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            int3 probeCoord = baseCoord + offset;
            float3 trilinearWeight3D = lerp(1.0f - alpha, alpha, (float3)offset);
            float weight = trilinearWeight3D.x * trilinearWeight3D.y * trilinearWeight3D.z;

            uint probeIndex = GetGDFGIScrollingProbeIndex(data, cascadeIndex, (uint3)probeCoord);
            uint2 stateTex = GetGDFGIProbeTexelCoords(data, cascadeIndex, probeIndex);
            uint state = probeStates[stateTex];

            // Ignore probes inside geometry
            if ((state & GDFGI_PROBE_STATE_MASK) == GDFGI_PROBE_STATE_INACTIVE_INSIDE)
                continue;

            float3 probeWorldPos = GetGDFGIProbePositionRelocated(data, probesData, cascadeIndex, (uint3)probeCoord);
            float3 probeToPoint = biasedWorldPos - probeWorldPos;
            float dist = length(probeToPoint);
            float3 worldPosToProbe = dist > 1e-4f ? -probeToPoint / dist : float3(0, 1, 0);

            // Smooth cosine hemisphere weight
            float cosWeight = max(Square(dot(worldPosToProbe, normal) * 0.5f + 0.5f), 0.05f);

            // Chebyshev visibility weight
            float2 distMoments = SampleGDFGIProbeDistance(data, probesDistance, cascadeIndex, probeIndex, -worldPosToProbe);
            float visWeight = ChebyshevWeight(distMoments, dist);

            float finalWeight = weight * cosWeight * visWeight;
            if (finalWeight > 1e-5f)
            {
                float3 probeColor = SampleGDFGIProbeDiffuse(data, directionalDiffuse, cascadeIndex, probeIndex, normal);
                cascadeIrradianceSum += probeColor * finalWeight;
                cascadeIrradianceWeight += finalWeight;
            }
        }

        if (cascadeIrradianceWeight > 1e-4f)
        {
            totalIrradiance = (cascadeIrradianceSum / cascadeIrradianceWeight) * (2.0f * PI);
            totalWeight = 1.0f;
            break;
        }
    }

    if (totalWeight < 1e-4f)
    {
        totalIrradiance = data.FallbackIrradiance.rgb;
    }

    return totalIrradiance;
}

#endif
