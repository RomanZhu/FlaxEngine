// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/Noise.hlsl"
#include "./Flax/MonteCarlo.hlsl"
#include "./Flax/GlobalSignDistanceField.hlsl"
#include "./Flax/GI/GlobalSurfaceAtlas.hlsl"
#include "./Flax/GI/GDFGI.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/Random.hlsl"
#include "./Flax/LightingCommon.hlsl"
#include "./Flax/BRDF.hlsl"

#define GDFGI_THREAD_GROUP_SIZE 8

META_CB_BEGIN(0, Data0)
GDFGIData GDFGI;
GlobalSDFData GlobalSDF;
GlobalSurfaceAtlasData GlobalSurfaceAtlas;
GBufferData GBuffer;
float SkyboxIntensity;
uint CascadeIndex;
uint ProbesCount;
float TemporalTime;
int4 ProbeScrollClears[4];
float3 ViewDir;
uint DynamicInvalidation;
META_CB_END

#if defined(_CS_ClassifyProbes)

Texture3D<snorm float> GlobalSDFTex : register(t0);
Texture3D<snorm float> GlobalSDFMip : register(t1);

RWTexture2D<snorm float4> ProbesDataOut : register(u0);
RWTexture2D<uint> ProbeStatesOut : register(u1);

// Probe Classification & Relocation
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(32, 1, 1)]
void CS_ClassifyProbes(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint probeGlobalIndex = DispatchThreadID.x;
    if (probeGlobalIndex >= ProbesCount)
        return;

    uint probesPerCascade = GDFGI.ProbesCounts.x * GDFGI.ProbesCounts.y * GDFGI.ProbesCounts.z;
    uint cascadeIndex = probeGlobalIndex / probesPerCascade;
    uint probeIndex = probeGlobalIndex % probesPerCascade;

    uint3 probeCoords = GetGDFGIProbeCoords(GDFGI, probeIndex);
    uint scrolledIndex = GetGDFGIScrollingProbeIndex(GDFGI, cascadeIndex, probeCoords);
    uint2 texCoord = GetGDFGIProbeTexelCoords(GDFGI, cascadeIndex, scrolledIndex);

    float probesSpacing = GDFGI.ProbesOriginAndSpacing[cascadeIndex].w;
    float3 probeBasePos = GetGDFGIProbeWorldPosition(GDFGI, cascadeIndex, probeCoords);

    // Check if probe was scrolled
    int3 probeScroll = GDFGI.ProbeScrollClears[cascadeIndex].xyz;
    bool wasScrolled = false;
    UNROLL
    for (uint axis = 0; axis < 3; axis++)
    {
        int count = (int)GDFGI.ProbesCounts[axis];
        int coord = (int)probeCoords[axis];
        int scroll = probeScroll[axis];
        int clearCount = min(abs(scroll), count);
        if (clearCount >= count ||
            (scroll > 0 && coord >= count - clearCount) ||
            (scroll < 0 && coord < clearCount))
            wasScrolled = true;
    }

    // Query Global SDF distance and gradient for relocation
    float sdf = 0.0f;
    float3 sdfGradient = SampleGlobalSDFGradient(GlobalSDF, GlobalSDFTex, GlobalSDFMip, probeBasePos, sdf);
    float gradLen = length(sdfGradient);
    float3 sdfNormal = gradLen > 1e-4f ? sdfGradient / gradLen : float3(0, 1, 0);

    float voxelLimit = GlobalSDF.CascadeVoxelSize[cascadeIndex] * 0.8f;
    float relocateLimit = probesSpacing * 0.45f;
    float3 probeOffset = float3(0, 0, 0);

    if (wasScrolled)
    {
        probeOffset = float3(0, 0, 0);
    }
    else if (sdf <= voxelLimit)
    {
        // Relocate probe away from surface along SDF normal gradient
        float3 offsetCandidate = sdfNormal * (voxelLimit - sdf + 0.1f * probesSpacing);
        if (length(offsetCandidate) <= relocateLimit)
            probeOffset = offsetCandidate;
    }

    float3 finalProbePos = probeBasePos + probeOffset;
    float finalSdf = SampleGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, finalProbePos);

    uint state = GDFGI_PROBE_STATE_ACTIVE;
    if (finalSdf < -0.1f * probesSpacing)
    {
        state = GDFGI_PROBE_STATE_INACTIVE_INSIDE;
        probeOffset = float3(0, 0, 0);
    }

    if (wasScrolled)
        state |= GDFGI_PROBE_STATE_HISTORY_INVALID;

    ProbeStatesOut[texCoord] = state;
    ProbesDataOut[texCoord] = float4(probeOffset / max(probesSpacing, 1e-4f), (float)(state & GDFGI_PROBE_STATE_MASK));
}

#elif defined(_CS_TraceDirectionalRadiance)

Texture3D<snorm float> GlobalSDFTex : register(t0);
Texture3D<snorm float> GlobalSDFMip : register(t1);
ByteAddressBuffer GlobalSurfaceAtlasChunks : register(t2);
ByteAddressBuffer GlobalSurfaceAtlasCulledObjects : register(t3);
Buffer<float4> GlobalSurfaceAtlasObjects : register(t4);
Texture2D GlobalSurfaceAtlasDepth : register(t5);
Texture2D GlobalSurfaceAtlasTex : register(t6);
Texture2D<uint> ProbeStatesIn : register(t7);
Texture2D<snorm float4> ProbesDataIn : register(t8);
TextureCube Skybox : register(t9);

RWTexture2D<float4> DirectionalRadianceRawOut : register(u0);
RWTexture2D<float2> ProbesDistanceOut : register(u1);

// Single-bounce ray tracing across 5x5 octahedral directional bins
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(GDFGI_THREAD_GROUP_SIZE, GDFGI_THREAD_GROUP_SIZE, 1)]
void CS_TraceDirectionalRadiance(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint rawWidth, rawHeight;
    DirectionalRadianceRawOut.GetDimensions(rawWidth, rawHeight);
    if (DispatchThreadID.x >= rawWidth || DispatchThreadID.y >= GDFGI.UpdateRowCount * GDFGI_OCT_RESOLUTION || DispatchThreadID.y + GDFGI.UpdateRowOffset * GDFGI_OCT_RESOLUTION >= rawHeight)
        return;
    uint2 octBin = DispatchThreadID.xy % GDFGI_OCT_RESOLUTION;
    uint2 absoluteCoord = uint2(DispatchThreadID.x, DispatchThreadID.y + GDFGI.UpdateRowOffset * GDFGI_OCT_RESOLUTION);
    uint2 probeGrid = absoluteCoord / GDFGI_OCT_RESOLUTION;

    uint probesPerPlane = GDFGI.ProbesCounts.x * GDFGI.ProbesCounts.z;
    uint planeIndex = probeGrid.x / GDFGI.ProbesCounts.x;
    uint gridX = probeGrid.x % GDFGI.ProbesCounts.x;
    uint gridZ = probeGrid.y % GDFGI.ProbesCounts.z;
    uint cascadeIndex = probeGrid.y / GDFGI.ProbesCounts.z;
    uint probeIndex = planeIndex * probesPerPlane + gridX + (GDFGI.ProbesCounts.x * gridZ);

    if (cascadeIndex >= GDFGI.CascadesCount || planeIndex >= GDFGI.ProbesCounts.y || probeIndex >= ProbesCount)
        return;

    uint3 probeCoords = GetGDFGIProbeCoords(GDFGI, probeIndex);
    uint scrolledIndex = GetGDFGIScrollingProbeIndex(GDFGI, cascadeIndex, probeCoords);
    uint2 stateTexCoord = GetGDFGIProbeTexelCoords(GDFGI, cascadeIndex, scrolledIndex);

    uint2 distCoord = probeGrid * GDFGI_OCT_TILE_SIZE + (octBin + 1);

    uint probeState = ProbeStatesIn[stateTexCoord];
    if ((probeState & GDFGI_PROBE_STATE_MASK) == GDFGI_PROBE_STATE_INACTIVE_INSIDE)
    {
        DirectionalRadianceRawOut[absoluteCoord] = float4(0, 0, 0, 1);
        float2 inactiveDist = float2(GDFGI.RayMaxDistance, GDFGI.RayMaxDistance * GDFGI.RayMaxDistance);
        ProbesDistanceOut[distCoord] = inactiveDist;
        if (octBin.x == 0)
            ProbesDistanceOut[distCoord - uint2(1, 0)] = inactiveDist;
        if (octBin.x == GDFGI_OCT_RESOLUTION - 1)
            ProbesDistanceOut[distCoord + uint2(1, 0)] = inactiveDist;
        if (octBin.y == 0)
            ProbesDistanceOut[distCoord - uint2(0, 1)] = inactiveDist;
        if (octBin.y == GDFGI_OCT_RESOLUTION - 1)
            ProbesDistanceOut[distCoord + uint2(0, 1)] = inactiveDist;
        if (octBin.x == 0 && octBin.y == 0)
            ProbesDistanceOut[distCoord - uint2(1, 1)] = inactiveDist;
        if (octBin.x == GDFGI_OCT_RESOLUTION - 1 && octBin.y == 0)
            ProbesDistanceOut[distCoord + uint2(1, -1)] = inactiveDist;
        if (octBin.x == 0 && octBin.y == GDFGI_OCT_RESOLUTION - 1)
            ProbesDistanceOut[distCoord + uint2(-1, 1)] = inactiveDist;
        if (octBin.x == GDFGI_OCT_RESOLUTION - 1 && octBin.y == GDFGI_OCT_RESOLUTION - 1)
            ProbesDistanceOut[distCoord + uint2(1, 1)] = inactiveDist;
        return;
    }

    float3 probeWorldPos = GetGDFGIProbePositionRelocated(GDFGI, ProbesDataIn, cascadeIndex, probeCoords);

    // Use the bin center on the DX11-safe path. Temporal jitter requires a
    // fully synchronized per-probe history; row-banded updates otherwise
    // produce periodic changes when a row cycles back through the scheduler.
    float2 jitter = float2(0.5f, 0.5f);
    float3 rayDir = GetGDFGIRayDirection(octBin, jitter);

    // Ray march Global SDF
    GlobalSDFTrace trace;
    trace.Init(probeWorldPos, rayDir, 0.1f, GDFGI.RayMaxDistance);
    float3 radiance = float3(0, 0, 0);
    float hitDistance = GDFGI.RayMaxDistance;

    GlobalSDFHit hit = RayTraceGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, trace, 0.0f, GDFGI.ThinGeometryExpansion);
    if (hit.IsHit())
    {
        hitDistance = hit.HitTime;
        float3 hitPosition = hit.GetHitPosition(trace);

        // Hit surface: sample Global Surface Atlas
        float surfaceThreshold = GetGlobalSurfaceAtlasThreshold(GlobalSDF, hit);
        float4 surfaceColor = SampleGlobalSurfaceAtlas(GlobalSurfaceAtlas, GlobalSurfaceAtlasChunks, GlobalSurfaceAtlasCulledObjects, GlobalSurfaceAtlasObjects, GlobalSurfaceAtlasDepth, GlobalSurfaceAtlasTex, hitPosition, -rayDir, surfaceThreshold, GDFGI.CulledObjectsCapacity, 128u);
        if (surfaceColor.a > 0.01f)
        {
            radiance = surfaceColor.rgb;
        }
        else
        {
            radiance = float3(0, 0, 0);
        }
    }
    else
    {
        // Miss: sample skybox or fallback irradiance
        float3 sky = Skybox.SampleLevel(SamplerLinearClamp, rayDir, 0).rgb;
        radiance = (any(sky > 0.0f) ? sky : GDFGI.FallbackIrradiance.rgb) * SkyboxIntensity;
    }

    if (any(isnan(radiance)) || any(isinf(radiance)))
        radiance = float3(0, 0, 0);

    DirectionalRadianceRawOut[absoluteCoord] = float4(radiance, 1.0f);

    // Distance moments for Chebyshev occlusion (stored in 7x7 tile with 1-texel border)
    float dist = hitDistance;
    float2 distMoments = float2(dist, dist * dist);
    ProbesDistanceOut[distCoord] = distMoments;
    if (octBin.x == 0)
        ProbesDistanceOut[distCoord - uint2(1, 0)] = distMoments;
    if (octBin.x == GDFGI_OCT_RESOLUTION - 1)
        ProbesDistanceOut[distCoord + uint2(1, 0)] = distMoments;
    if (octBin.y == 0)
        ProbesDistanceOut[distCoord - uint2(0, 1)] = distMoments;
    if (octBin.y == GDFGI_OCT_RESOLUTION - 1)
        ProbesDistanceOut[distCoord + uint2(0, 1)] = distMoments;
    if (octBin.x == 0 && octBin.y == 0)
        ProbesDistanceOut[distCoord - uint2(1, 1)] = distMoments;
    if (octBin.x == GDFGI_OCT_RESOLUTION - 1 && octBin.y == 0)
        ProbesDistanceOut[distCoord + uint2(1, -1)] = distMoments;
    if (octBin.x == 0 && octBin.y == GDFGI_OCT_RESOLUTION - 1)
        ProbesDistanceOut[distCoord + uint2(-1, 1)] = distMoments;
    if (octBin.x == GDFGI_OCT_RESOLUTION - 1 && octBin.y == GDFGI_OCT_RESOLUTION - 1)
        ProbesDistanceOut[distCoord + uint2(1, 1)] = distMoments;
}

#elif defined(_CS_UpdateTemporalRing)

Texture2D<float4> DirectionalRadianceRawIn : register(t0);
Texture2D<float4> DirectionalRadianceHistoryIn : register(t1);

RWTexture2D<float4> DirectionalRadianceAvgOut : register(u0);
RWTexture2D<float4> DirectionalRadianceHistoryOut : register(u1);

// Temporal update with localized dynamic invalidation and exponential moving average
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(GDFGI_THREAD_GROUP_SIZE, GDFGI_THREAD_GROUP_SIZE, 1)]
void CS_UpdateTemporalRing(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint2 texCoord = uint2(DispatchThreadID.x, DispatchThreadID.y + GDFGI.UpdateRowOffset * GDFGI_OCT_RESOLUTION);
    uint historyWidth, historyHeight;
    DirectionalRadianceHistoryOut.GetDimensions(historyWidth, historyHeight);
    if (texCoord.x >= historyWidth || texCoord.y >= historyHeight || DispatchThreadID.y >= GDFGI.UpdateRowCount * GDFGI_OCT_RESOLUTION)
        return;
    float3 newSample = DirectionalRadianceRawIn[texCoord].rgb;
    if (any(isnan(newSample)) || any(isinf(newSample)))
        newSample = float3(0, 0, 0);

    uint2 probeGrid = texCoord / GDFGI_OCT_RESOLUTION;
    uint probesPerPlane = GDFGI.ProbesCounts.x * GDFGI.ProbesCounts.z;
    uint planeIndex = probeGrid.x / GDFGI.ProbesCounts.x;
    uint gridX = probeGrid.x % GDFGI.ProbesCounts.x;
    uint gridZ = probeGrid.y % GDFGI.ProbesCounts.z;
    uint cascadeIndex = probeGrid.y / GDFGI.ProbesCounts.z;
    uint probeIndex = planeIndex * probesPerPlane + gridX + (GDFGI.ProbesCounts.x * gridZ);

    if (cascadeIndex >= GDFGI.CascadesCount || planeIndex >= GDFGI.ProbesCounts.y || probeIndex >= ProbesCount)
        return;

    // Row-banded probes can go many frames between updates. Publishing the
    // deterministic current sample avoids alternating between stale ping-pong
    // histories and removes the periodic history-length flicker.
    float3 blended = newSample;
    if (any(isnan(blended)) || any(isinf(blended)))
        blended = float3(0, 0, 0);

    DirectionalRadianceHistoryOut[texCoord] = float4(blended, 1.0f);
    DirectionalRadianceAvgOut[texCoord] = float4(max(blended, 0.0f), 1.0f);
}

#elif defined(_CS_ConvolveDiffuse)

Texture2D<float4> DirectionalRadianceAvgIn : register(t0);

RWTexture2D<float4> DirectionalDiffuseOut : register(u0);

// Diffuse cosine convolution from 25 raw directional bins into octahedral diffuse irradiance
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(GDFGI_THREAD_GROUP_SIZE, GDFGI_THREAD_GROUP_SIZE, 1)]
void CS_ConvolveDiffuse(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint diffuseWidth, diffuseHeight;
    DirectionalDiffuseOut.GetDimensions(diffuseWidth, diffuseHeight);
    uint2 absoluteCoord = uint2(DispatchThreadID.x, DispatchThreadID.y + GDFGI.UpdateRowOffset * GDFGI_OCT_TILE_SIZE);
    if (DispatchThreadID.x >= diffuseWidth || DispatchThreadID.y >= GDFGI.UpdateRowCount * GDFGI_OCT_TILE_SIZE || absoluteCoord.y >= diffuseHeight)
        return;
    uint2 tileCoord = absoluteCoord % GDFGI_OCT_TILE_SIZE;
    uint2 probeGrid = absoluteCoord / GDFGI_OCT_TILE_SIZE;

    // Decode target normal for this texel in the 7x7 tile (mapping 1..5 inner texels to octahedral coordinates)
    float2 innerUV = (float2(tileCoord) - 0.5f) / float(GDFGI_OCT_RESOLUTION);
    float2 octCoords = clamp(innerUV * 2.0f - 1.0f, -1.0f, 1.0f);
    float3 normal = GetOctahedralDirection(octCoords);

    uint2 rawBaseCoord = probeGrid * GDFGI_OCT_RESOLUTION;
    uint rawWidth, rawHeight;
    DirectionalRadianceAvgIn.GetDimensions(rawWidth, rawHeight);

    float3 diffuseRadiance = float3(0, 0, 0);
    float weightSum = 0.0f;

    // Convolve over 5x5 directional bins
    for (uint dy = 0; dy < GDFGI_OCT_RESOLUTION; dy++)
    {
        for (uint dx = 0; dx < GDFGI_OCT_RESOLUTION; dx++)
        {
            uint2 binCoord = uint2(dx, dy);
            float3 binDir = GetGDFGIBinDirection(binCoord);
            float weight = max(dot(normal, binDir), 0.0f);

            uint2 sampleCoord = rawBaseCoord + binCoord;
            float3 sampleVal = float3(0, 0, 0);
            if (sampleCoord.x < rawWidth && sampleCoord.y < rawHeight)
                sampleVal = DirectionalRadianceAvgIn[sampleCoord].rgb;
            if (any(isnan(sampleVal)) || any(isinf(sampleVal)))
                sampleVal = float3(0, 0, 0);
            diffuseRadiance += sampleVal * weight;
            weightSum += weight;
        }
    }

    if (weightSum > 1e-4f)
        diffuseRadiance /= weightSum;

    DirectionalDiffuseOut[absoluteCoord] = float4(diffuseRadiance, 1.0f);
}

#elif defined(_PS_IndirectLighting)

Texture2D<snorm float4> ProbesDataIn : register(t4);
Texture2D<uint> ProbeStatesIn : register(t5);
Texture2D<float4> DirectionalDiffuseIn : register(t6);
Texture2D<float4> ProbesDistanceIn : register(t7);
Texture2D<float4> DirectionalRadianceIn : register(t8);

// Screen-space deferred indirect lighting composite pass
META_PS(true, FEATURE_LEVEL_SM5)
void PS_IndirectLighting(Quad_VS2PS input, out float4 output : SV_Target0)
{
    output = 0;
    GBufferSample gBuffer = SampleGBuffer(GBuffer, input.TexCoord);

    if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
    {
        discard;
        return;
    }

    float3 geometricNormal = normalize(cross(ddx(gBuffer.WorldPos), ddy(gBuffer.WorldPos)));
    if (dot(geometricNormal, gBuffer.Normal) < 0.0f)
        geometricNormal = -geometricNormal;
    if (any(isnan(geometricNormal)) || length(geometricNormal) < 0.5f)
        geometricNormal = gBuffer.Normal;

    // Composite is also the shader-level isolation gate. It proves the
    // fullscreen draw, GBuffer reads, render-target state, and blending without
    // executing probe texture addressing; Full/production enables sampling.
    float3 irradiance = GDFGI.DebugExecutionStage == 6
        ? GDFGI.FallbackIrradiance.rgb
        : SampleGDFGITrilinearIrradiance(GDFGI, ProbeStatesIn, DirectionalDiffuseIn, gBuffer.WorldPos, gBuffer.Normal);

    float3 diffuseColor = GetDiffuseColor(gBuffer);
    float3 diffuse = Diffuse_Lambert(diffuseColor) * irradiance;

    // Optional Directional Specular
    float3 specular = float3(0, 0, 0);
    if (GDFGI.EnableDirectionalSpecular != 0)
    {
        float3 V = normalize(GBuffer.ViewPos - gBuffer.WorldPos);
        float3 R = reflect(-V, gBuffer.Normal);
        float3 rawSpecular = SampleGDFGISpecular(GDFGI, DirectionalRadianceIn, ProbeStatesIn, gBuffer.WorldPos, R, gBuffer.Roughness);
        float3 specularColor = GetSpecularColor(gBuffer);
        float NoV = saturate(dot(gBuffer.Normal, V));
        float3 F = EnvBRDFApprox(specularColor, gBuffer.Roughness, NoV);
        specular = rawSpecular * F * (1.0f - gBuffer.Roughness);
    }

    output.rgb = (diffuse + specular) * GDFGI.IndirectLightingIntensity * gBuffer.AO;
    output.a = 1.0f;
}

#elif defined(_PS_Debug)

Texture2D<snorm float4> ProbesDataIn : register(t4);
Texture2D<uint> ProbeStatesIn : register(t5);
Texture2D<float4> DirectionalDiffuseIn : register(t6);
Texture2D<float4> ProbesDistanceIn : register(t7);

// Debug view shader for GDFGI visualization
META_PS(true, FEATURE_LEVEL_SM5)
void PS_Debug(Quad_VS2PS input, out float4 output : SV_Target0)
{
    GBufferSample gBuffer = SampleGBuffer(GBuffer, input.TexCoord);
    if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
    {
        output = float4(0, 0, 0, 1);
        return;
    }

    float3 geometricNormal = gBuffer.Normal;
    float3 irradiance = SampleGDFGIIrradiance(GDFGI, ProbesDataIn, ProbeStatesIn, ProbesDistanceIn, DirectionalDiffuseIn, gBuffer.WorldPos, gBuffer.Normal, geometricNormal, GDFGI_DEFAULT_BIAS);

    output = float4(irradiance * GDFGI.IndirectLightingIntensity, 1.0f);
}

#endif
