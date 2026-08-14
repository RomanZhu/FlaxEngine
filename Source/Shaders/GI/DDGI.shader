// Copyright (c) Wojciech Figat. All rights reserved.

// Implementation based on:
// "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Probes", Journal of Computer Graphics Tools, April 2019
// Zander Majercik, Jean-Philippe Guertin, Derek Nowrouzezahrai, and Morgan McGuire
// https://morgan3d.github.io/articles/2019-04-01-ddgi/index.html and https://gdcvault.com/play/1026182/
//
// Additional references:
// "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production", https://jcgt.org/published/0010/02/01/
// "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields", https://jcgt.org/published/0008/02/01/

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/Noise.hlsl"
#include "./Flax/Quaternion.hlsl"
#include "./Flax/MonteCarlo.hlsl"
#include "./Flax/GlobalSignDistanceField.hlsl"
#include "./Flax/GI/GlobalSurfaceAtlas.hlsl"
#include "./Flax/GI/DDGI.hlsl"

// This must match C++
#define DDGI_TRACE_RAYS_PROBES_COUNT_LIMIT 4096 // Maximum amount of probes to update at once during rays tracing and blending
#define DDGI_TRACE_RAYS_LIMIT 256 // Limit of rays per-probe (runtime value can be smaller)
#define DDGI_TRACE_RAYS_MIN 64 // Minimum amount of rays to shoot for sleepy probes (includes the fixed classification prefix)
#define DDGI_FIXED_RAY_COUNT 32 // Stable prefix reserved for classification and excluded from convolution
#define DDGI_TRACE_NEGATIVE 1 // Rays that start inside geometry use negative distance to indicate a backface/inside hit
#define DDGI_FIXED_RAY_INSIDE_THRESHOLD 0.25f
#define DDGI_LIGHTING_RAY_BACKFACE_THRESHOLD 0.10f
#define DDGI_PROBE_UPDATE_BORDERS_GROUP_SIZE 8
#define DDGI_PROBE_CLASSIFY_GROUP_SIZE 32
#define DDGI_PROBE_RELOCATE_ITERATIVE 1 // If true, probes relocation algorithm tries to move them in additive way, otherwise all nearby locations are checked to find the best position
#define DDGI_PROBE_RELOCATE_FIND_BEST 1 // If true, probes relocation algorithm tries to move to the best matching location within nearby area
#define DDGI_DEBUG_STATS 0 // Enables additional GPU-driven stats for probe/rays count
#define DDGI_DEBUG_INSTABILITY 0 // Enables additional probe irradiance instability debugging

META_CB_BEGIN(0, Data0)
DDGIData DDGI;
GlobalSDFData GlobalSDF;
GlobalSurfaceAtlasData GlobalSurfaceAtlas;
GBufferData GBuffer;
float4 RaysRotation;
float SkyboxIntensity;
uint ProbesCount;
float ResetBlend;
float TemporalTime;
int4 ProbeScrollClears[4];
float3 ViewDir;
float Padding1;
float3 QuantizationError;
uint FrameIndexMod8;
META_CB_END

META_CB_BEGIN(1, Data1)
float Padding2;
int StepSize;
uint CascadeIndex;
uint ProbeIndexOffset;
uint ProbeUpdateBudget;
uint ProbeWindowStart;
META_CB_END

// Calculates the evenly distributed direction ray on a sphere (Spherical Fibonacci lattice)
float3 GetSphericalFibonacci(float sampleIndex, float samplesCount)
{
    float b = (sqrt(5.0f) * 0.5f + 0.5f) - 1.0f;
    float s = sampleIndex * b;
    float phi = (2.0f * PI) * (s - floor(s));
    float cosTheta = 1.0f - (2.0f * sampleIndex + 1.0f) * (1.0f / samplesCount);
    float sinTheta = sqrt(saturate(1.0f - (cosTheta * cosTheta)));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Calculates a random normalized ray direction (based on the ray index and the current probes rotation phrase)
float3 GetProbeRayDirection(DDGIData data, uint rayIndex, uint raysCount, uint probeIndex, uint3 probeCoords)
{
    // The fixed prefix is stable across frames and quality levels. It is used
    // for state classification and never enters the lighting convolution.
    if (rayIndex < DDGI_FIXED_RAY_COUNT)
        return GetSphericalFibonacci((float)rayIndex, (float)DDGI_FIXED_RAY_COUNT);

    float4 rotation = RaysRotation;

    // Randomize rotation per-probe (otherwise all probes are in sync)
    float3 probePos = (float3)probeCoords / (float3)data.ProbesCounts;
    float3 randomAxisValue = Mod289(probePos);
    float randomAxisLength = length(randomAxisValue);
    float3 randomAxis = randomAxisLength > 1e-4f ? randomAxisValue / randomAxisLength : float3(0, 1, 0);
    float randomAngle = (float)probeIndex / (float)ProbesCount * (2.0f * PI);
    rotation = QuaternionMultiply(rotation, QuaternionFromAxisAngle(randomAxis, randomAngle));

    // Random rotation per-ray - relative to the per-frame rays rotation
    uint lightingRayIndex = rayIndex - DDGI_FIXED_RAY_COUNT;
    uint lightingRaysCount = max(raysCount - DDGI_FIXED_RAY_COUNT, 1u);
    float3 direction = GetSphericalFibonacci((float)lightingRayIndex, (float)lightingRaysCount);
    return normalize(QuaternionRotate(rotation, direction));
}

// Calculates amount of rays to allocate for a probe
uint GetProbeRaysCount(DDGIData data, float probeAttention)
{
    //return data.RaysCount;
    probeAttention = saturate((probeAttention - DDGI_PROBE_ATTENTION_MIN) / (DDGI_PROBE_ATTENTION_MAX - DDGI_PROBE_ATTENTION_MIN));
    return max(DDGI_FIXED_RAY_COUNT + 1u, DDGI_TRACE_RAYS_MIN + (uint)max(probeAttention * (float)(data.RaysCount - DDGI_TRACE_RAYS_MIN), 0.0f));
}

#ifdef _CS_Classify

RWTexture2D<snorm float4> RWProbesData : register(u0);
RWTexture2D<uint> RWProbeStates : register(u1);
RWByteAddressBuffer RWActiveProbes : register(u2);

Texture3D<snorm float> GlobalSDFTex : register(t0);
Texture3D<snorm float> GlobalSDFMip : register(t1);

float3 Remap(float3 value, float3 fromMin, float3 fromMax, float3 toMin, float3 toMax)
{
    return (value - fromMin) / (fromMax - fromMin) * (toMax - toMin) + toMin;
}

bool IsProbeAtBorder(uint3 probeCoords)
{
    return min(probeCoords.x, min(probeCoords.y, probeCoords.z)) == 0 || probeCoords.x == DDGI.ProbesCounts.x - 1 || probeCoords.y == DDGI.ProbesCounts.y - 1 || probeCoords.z == DDGI.ProbesCounts.z - 1;
}

float3 ClampDDGIProbeOffset(float3 offset, float probesSpacing)
{
    // Relocation is an ellipsoid in normalized cell space. The current global
    // clipmap uses uniform spacing, but keeping the normalization explicit
    // makes the bound correct if anisotropic spacing is added later.
    float normalizedLength = length(offset / max(probesSpacing, 1e-6f));
    if (normalizedLength > 0.45f)
        offset *= 0.45f / normalizedLength;
    return offset;
}

bool HasNearbyFixedRayGeometry(float3 probePosition, float probesSpacing)
{
    const float maxDistance = probesSpacing * 1.75f;
    UNROLL
    for (uint fixedRayIndex = 0; fixedRayIndex < DDGI_FIXED_RAY_COUNT; fixedRayIndex++)
    {
        GlobalSDFTrace fixedTrace;
        fixedTrace.Init(probePosition, GetSphericalFibonacci((float)fixedRayIndex, (float)DDGI_FIXED_RAY_COUNT), 0.0f, min(DDGI.RayMaxDistance, maxDistance));
        GlobalSDFHit fixedHit = RayTraceGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, fixedTrace);
        if (fixedHit.IsHit() && fixedHit.HitTime <= maxDistance && fixedHit.HitSDF <= GlobalSDF.CascadeVoxelSize[CascadeIndex] * 1.5f)
            return true;
    }
    return false;
}

// Compute shader for updating probes state between active and inactive and performing probes relocation.
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(DDGI_PROBE_CLASSIFY_GROUP_SIZE, 1, 1)]
void CS_Classify(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint gridProbeIndex = DispatchThreadId.x;
    if (gridProbeIndex >= ProbesCount)
        return;
    uint3 probeCoords = GetDDGIProbeCoords(DDGI, gridProbeIndex);
    uint probeIndex = gridProbeIndex;
    probeIndex = GetDDGIScrollingProbeIndex(DDGI, CascadeIndex, probeCoords);
    int2 probeDataCoords = GetDDGIProbeTexelCoords(DDGI, CascadeIndex, probeIndex);
    float probesSpacing = DDGI.ProbesOriginAndSpacing[CascadeIndex].w;
    float3 probeBasePosition = GetDDGIProbeWorldPosition(DDGI, CascadeIndex, probeCoords);

#ifdef DDGI_DEBUG_CASCADE
    // Single cascade-only debugging
    if (CascadeIndex != DDGI_DEBUG_CASCADE)
    {
        RWProbesData[probeDataCoords] = EncodeDDGIProbeData(float3(0, 0, 0), 0.0f);
        RWProbeStates[probeDataCoords] = DDGI_PROBE_STATE_INACTIVE_OVERLAP;
        return;
    }
#else
    // Disable probes that are is in the range of higher-quality cascade
    if (CascadeIndex > 0)
    {
        uint prevCascade = CascadeIndex - 1;
        float prevProbesSpacing = DDGI.ProbesOriginAndSpacing[prevCascade].w;
        float3 prevProbesOrigin = DDGI.ProbesScrollOffsets[prevCascade].xyz * prevProbesSpacing + DDGI.ProbesOriginAndSpacing[prevCascade].xyz;
        float3 prevProbesExtent = (DDGI.ProbesCounts - 1) * (prevProbesSpacing * 0.5f);
        prevProbesExtent -= probesSpacing * ceil(DDGI_CASCADE_BLEND_SIZE) * 2; // Apply safe margin to allow probes on cascade edges
        float prevCascadeWeight = Min3(prevProbesExtent - abs(probeBasePosition - prevProbesOrigin));
        if (prevCascadeWeight > 0.1f)
        {
            RWProbesData[probeDataCoords] = EncodeDDGIProbeData(float3(0, 0, 0), 0.0f);
            RWProbeStates[probeDataCoords] = DDGI_PROBE_STATE_INACTIVE_OVERLAP;
            return;
        }
    }
#endif

    // Check if probe was scrolled
    int3 probeScrollClears = ProbeScrollClears[CascadeIndex].xyz;
    bool wasScrolled = false;
    UNROLL
    for (uint planeIndex = 0; planeIndex < 3; planeIndex++)
    {
        int probeCount = (int)DDGI.ProbesCounts[planeIndex];
        int probeCoord = (int)probeCoords[planeIndex];
        int probeScroll = probeScrollClears[planeIndex];
        int clearCount = min(abs(probeScroll), probeCount);

        // Scrolling remaps the logical probe coordinates onto a ring buffer. Only the
        // newly exposed edge contains stale history: the high edge when scrolling in
        // the positive direction and the low edge when scrolling in the negative one.
        // Clearing both edges causes valid probes to disappear whenever the camera
        // crosses a single probe-cell boundary.
        if (clearCount >= probeCount ||
            (probeScroll > 0 && probeCoord >= probeCount - clearCount) ||
            (probeScroll < 0 && probeCoord < clearCount))
            wasScrolled = true;
    }

    // Load probe state and position
    float4 probeData = RWProbesData[probeDataCoords];
    float probeAttention = DecodeDDGIProbeAttention(probeData);
    uint probeState = RWProbeStates[probeDataCoords];
    uint probeStateOld = probeState & DDGI_PROBE_STATE_MASK;
    bool wasHistoryInvalid = (probeState & DDGI_PROBE_STATE_HISTORY_INVALID) != 0;
    uint probeConfirmation = (probeState & DDGI_PROBE_STATE_CONFIRMATION_MASK) >> DDGI_PROBE_STATE_CONFIRMATION_SHIFT;
    float3 probeOffset = probeData.xyz * probesSpacing; // Probe offset is [-1;1] within probes spacing
    if (wasScrolled || IsDDGIProbeInactive(probeStateOld))
    {
        probeOffset = float3(0, 0, 0); // Clear offset for a new probe
        probeAttention = 1.0f; // Wake-up
        wasHistoryInvalid = true;
        if (wasScrolled)
            probeConfirmation = 0;
    }
    float3 probeOffsetOld = probeOffset;
    float3 probePosition = probeBasePosition + probeOffset;

    // Use Global SDF to quickly get distance and direction to the scene geometry
#if DDGI_PROBE_RELOCATE_ITERATIVE
    float sdf;
    float3 sdfGradient = SampleGlobalSDFGradient(GlobalSDF, GlobalSDFTex, GlobalSDFMip, probePosition, sdf);
    float sdfGradientLength = length(sdfGradient);
    float3 sdfNormal = sdfGradientLength > 1e-4f ? sdfGradient / sdfGradientLength : float3(0, 1, 0);
#else
    float sdf = SampleGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, probePosition);
#endif
    float sdfDst = abs(sdf);
    const float ProbesDistanceLimits[4] = { 1.1f, 2.3f, 2.5f, 2.5f };
    float voxelLimit = GlobalSDF.CascadeVoxelSize[CascadeIndex] * 0.8f;
    float distanceLimit = probesSpacing * ProbesDistanceLimits[CascadeIndex];
    const float relocateLimit = probesSpacing * 0.45f;
    const float insideLimit = GlobalSDF.CascadeVoxelSize[CascadeIndex] * DDGI_FIXED_RAY_INSIDE_THRESHOLD;
    const bool missingSDF = abs(sdf) >= GLOBAL_SDF_WORLD_SIZE * 0.5f;
    const bool insideGeometry = sdf < -insideLimit;
    bool hasFixedBlocker = false;
    if (!missingSDF && !insideGeometry && sdfDst > voxelLimit)
        hasFixedBlocker = HasNearbyFixedRayGeometry(probePosition, probesSpacing);

    if (missingSDF)
    {
        // Do not allow stale irradiance to survive a missing/invalid SDF tile.
        probeOffset = float3(0, 0, 0);
        probeState = DDGI_PROBE_STATE_INACTIVE_INVALID;
        probeAttention = 0.0f;
        probeConfirmation = 0;
    }
    else if (insideGeometry)
    {
        probeOffset = float3(0, 0, 0);
        probeState = DDGI_PROBE_STATE_INACTIVE_INSIDE;
        probeAttention = 0.0f;
        probeConfirmation = 0;
    }
    else if (sdfDst > distanceLimit + length(probeOffset) && !hasFixedBlocker)
    {
        // A positive, distant SDF sample is known empty space. It is kept as
        // an explicit reason so it can be reactivated when fixed rays see a
        // blocker after the camera or geometry moves.
        probeOffset = float3(0, 0, 0);
        probeState = DDGI_PROBE_STATE_INACTIVE_EMPTY;
        probeAttention = 0.0f;
        probeConfirmation = 0;
    }
    else
    {
        bool pendingActivation = false;
        if (IsDDGIProbeInactive(probeStateOld))
        {
            probeConfirmation = min(probeConfirmation + 1u, DDGI_PROBE_REACTIVATION_CONFIRM_FRAMES);
            pendingActivation = probeConfirmation < DDGI_PROBE_REACTIVATION_CONFIRM_FRAMES;
            if (pendingActivation)
            {
                // Keep the probe inactive while the fixed-ray observation is
                // confirmed on the following classification pass.
                probeOffset = float3(0, 0, 0);
                probeState = DDGI_PROBE_STATE_INACTIVE_EMPTY;
                probeAttention = 0.0f;
                wasHistoryInvalid = true;
            }
        }
        if (pendingActivation)
        {
            probeConfirmation = min(probeConfirmation, DDGI_PROBE_REACTIVATION_CONFIRM_FRAMES - 1u);
        }
        else
        {
        probeConfirmation = 0;
        // Apply distance/view heuristics to probe attention
        probeState = DDGI_PROBE_STATE_ACTIVE;
        float3 viewToProbe = probePosition - GBuffer.ViewPos;
        float distanceToProbe = length(viewToProbe);
        viewToProbe = distanceToProbe > 1e-4f ? viewToProbe / distanceToProbe : ViewDir;
        float probeViewDot = dot(viewToProbe, ViewDir);
        probeAttention *= lerp(0.1f, 1.0f, saturate(probeViewDot)); // Reduce quality for probes behind the camera (or away from view dir)
        probeAttention *= lerp(1.0f, 0.5f, saturate(sdfDst / voxelLimit)); // Reduce quality for probes far away from geometry
        probeAttention += (1.0f - saturate(distanceToProbe / 1000.0f)) * 1.2f; // Boost quality for probes nearby view
        //probeAttention = 0.0f; // Debug test lowest ray count
        //probeAttention = 1.0f; // Debug test highest ray count
        probeAttention = clamp(probeAttention, DDGI_PROBE_ATTENTION_MIN, DDGI_PROBE_ATTENTION_MAX);

        // Relocate only if probe location is not good enough
        BRANCH
        if (sdf <= voxelLimit)
        {
#if DDGI_PROBE_RELOCATE_ITERATIVE
            {
                // Use SDF gradient to relocate probe away the surface
                float iterativeRelocateSpeed = probeStateOld != DDGI_PROBE_STATE_ACTIVE ? 1.0f : 0.3f;
                float3 offsetToSet = ClampDDGIProbeOffset(probeOffset + sdfNormal * ((sdf + voxelLimit) * iterativeRelocateSpeed), probesSpacing);
                if (length(offsetToSet) <= relocateLimit)
                {
                    // Relocate it
                    probeOffset = offsetToSet;
                }
                else
                {
                    // Reset offset
                    probeOffset = float3(0, 0, 0);
                }

                // Read SDF at the new position for additional check
                probePosition = probeBasePosition + probeOffset;
                sdf = SampleGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, probePosition);
                sdfDst = abs(sdf);
            }
            if (sdf <= voxelLimit * 1.1f) // Add some safe-bias to reduce artifacts
#endif
            {
#if DDGI_PROBE_RELOCATE_FIND_BEST
                // Sample Global SDF around the probe base location
                uint sdfCascade = GetGlobalSDFCascade(GlobalSDF, probeBasePosition);
                float4 CachedProbeOffsets[64];
                for (uint x = 0; x < 4; x++)
                for (uint y = 0; y < 4; y++)
                for (uint z = 0; z < 4; z++)
                {
                    float3 offset = ClampDDGIProbeOffset(Remap(float3(x, y, z), 0, 3, -0.707f, 0.707f) * relocateLimit, probesSpacing);
                    float offsetSdf = SampleGlobalSDFCascade(GlobalSDF, GlobalSDFTex, probeBasePosition + offset, sdfCascade);
                    CachedProbeOffsets[x * 16 + y * 4 + z] = float4(offset, offsetSdf);
                }

                // Select the best probe location around the base position
                float4 bestOffset = CachedProbeOffsets[0];
                for (uint i = 1; i < 64; i++)
                {
                    if (CachedProbeOffsets[i].w > bestOffset.w)
                        bestOffset = CachedProbeOffsets[i];
                }
                if (bestOffset.w <= voxelLimit || bestOffset.w <= 0.0f)
                {
                    // No valid free-space location exists within the strict
                    // relocation ellipsoid.
                    probeOffset = float3(0, 0, 0);
                    probeState = DDGI_PROBE_STATE_INACTIVE_INSIDE;
                    probeAttention = 0.0f;
                }
                else
                {
                    // Relocate the probe to the best found location
                    probeOffset = ClampDDGIProbeOffset(bestOffset.xyz, probesSpacing);
                }
#elif DDGI_PROBE_RELOCATE_ITERATIVE
                // Disable probe
                probeOffset = float3(0, 0, 0);
                probeState = DDGI_PROBE_STATE_INACTIVE_INSIDE;
                probeAttention = 0.0f;
#endif
            }
        }

        // If probe was in a different location or was activated now then mark it as activated
        bool wasActivated = IsDDGIProbeInactive(probeStateOld) || wasScrolled || wasHistoryInvalid;
        bool wasRelocated = distance(probeOffset, probeOffsetOld) > probesSpacing * 0.05f;
#if DDGI_PROBE_RELOCATE_FIND_BEST || DDGI_PROBE_RELOCATE_ITERATIVE
        BRANCH
        if (wasRelocated && !wasActivated)
        {
            // If probe was relocated but the previous location is visible from the new one, then don't re-activate it for smoother blend
            float3 diff = probeOffsetOld - probeOffset;
            float diffLen = length(diff);
            if (diffLen > 1e-4f)
            {
                float3 diffDir = diff / diffLen;
                GlobalSDFTrace trace;
                trace.Init(probeBasePosition + probeOffset, diffDir, 0.0f, diffLen);
                GlobalSDFHit hit = RayTraceGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, trace);
                if (!hit.IsHit())
                    wasRelocated = false;
            }
        }
#endif
        if ((wasActivated || wasScrolled || wasRelocated) && probeState == DDGI_PROBE_STATE_ACTIVE)
        {
            probeState = DDGI_PROBE_STATE_ACTIVATED;
            probeAttention = 1.0f;
            wasHistoryInvalid = true;
        }
        }
    }

    // Save probe state
    probeOffset /= probesSpacing; // Move offset back to [-1;1] space
    RWProbesData[probeDataCoords] = EncodeDDGIProbeData(probeOffset, IsDDGIProbeActive(probeState) ? probeAttention : 0.0f);
    uint storedProbeState = probeState & DDGI_PROBE_STATE_MASK;
    storedProbeState |= (probeConfirmation << DDGI_PROBE_STATE_CONFIRMATION_SHIFT) & DDGI_PROBE_STATE_CONFIRMATION_MASK;
    if (IsDDGIProbeActive(probeState) && wasHistoryInvalid)
        storedProbeState |= DDGI_PROBE_STATE_HISTORY_INVALID;
    RWProbeStates[probeDataCoords] = storedProbeState;

    // Collect only the rotating window allowed by the global ray budget. All
    // probes are classified every update, so the window cannot strand a state
    // transition indefinitely.
    uint maxProbes = ProbeUpdateBudget == 0 ? ProbesCount : max(1u, ProbeUpdateBudget / max(DDGI.RaysCount, 1u));
    maxProbes = min(maxProbes, ProbesCount);
    uint windowStart = ProbeWindowStart % max(ProbesCount, 1u);
    uint windowOffset = (gridProbeIndex + ProbesCount - windowStart) % max(ProbesCount, 1u);
    if (IsDDGIProbeActive(probeState) && (ProbeUpdateBudget == 0 || windowOffset < maxProbes))
    {
        uint activeProbeIndex;
        RWActiveProbes.InterlockedAdd(0, 1, activeProbeIndex); // Counter at 0
        if (activeProbeIndex < ProbesCount)
            RWActiveProbes.Store(activeProbeIndex * 4 + 4, gridProbeIndex);
    }
}

#endif

#ifdef _CS_UpdateProbesInitArgs

RWBuffer<uint> UpdateProbesInitArgs : register(u0);
ByteAddressBuffer ActiveProbes : register(t0);

// Compute shader for building indirect dispatch arguments for CS_TraceRays and CS_UpdateProbes.
META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(1, 1, 1)]
void CS_UpdateProbesInitArgs()
{
    uint activeProbesCount = ActiveProbes.Load(0); // Counter at 0
    activeProbesCount = min(activeProbesCount, ProbesCount);
    // The CPU submits every allocated batch, so always overwrite all arguments to avoid
    // dispatching stale work left by a previous cascade or frame.
    uint arg = 0;
    for (uint probesOffset = 0; probesOffset < ProbesCount; probesOffset += DDGI_TRACE_RAYS_PROBES_COUNT_LIMIT)
    {
        uint probesBatchSize = probesOffset < activeProbesCount ? min(activeProbesCount - probesOffset, DDGI_TRACE_RAYS_PROBES_COUNT_LIMIT) : 0;
        UpdateProbesInitArgs[arg++] = probesBatchSize;
        UpdateProbesInitArgs[arg++] = 1;
        UpdateProbesInitArgs[arg++] = 1;
    }
}

#endif

#ifdef _CS_TraceRays

RWTexture2D<float4> RWProbesTrace : register(u0);
#if DDGI_DEBUG_STATS
RWByteAddressBuffer RWStats : register(u1);
#endif

Texture3D<snorm float> GlobalSDFTex : register(t0);
Texture3D<snorm float> GlobalSDFMip : register(t1);
ByteAddressBuffer GlobalSurfaceAtlasChunks : register(t2);
ByteAddressBuffer RWGlobalSurfaceAtlasCulledObjects : register(t3);
Buffer<float4> GlobalSurfaceAtlasObjects : register(t4);
Texture2D GlobalSurfaceAtlasDepth : register(t5);
Texture2D GlobalSurfaceAtlasTex : register(t6);
Texture2D<snorm float4> ProbesData : register(t7);
TextureCube Skybox : register(t8);
ByteAddressBuffer ActiveProbes : register(t9);
Texture2D<uint> ProbeStates : register(t10);

// Compute shader for tracing rays for probes using Global SDF and Global Surface Atlas (1 ray per-thread).
META_CS(true, FEATURE_LEVEL_SM5)
META_PERMUTATION_1(DDGI_TRACE_RAYS_COUNT=96)
META_PERMUTATION_1(DDGI_TRACE_RAYS_COUNT=128)
META_PERMUTATION_1(DDGI_TRACE_RAYS_COUNT=192)
META_PERMUTATION_1(DDGI_TRACE_RAYS_COUNT=256)
[numthreads(1, DDGI_TRACE_RAYS_COUNT, 1)]
void CS_TraceRays(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint rayIndex = DispatchThreadId.y;
    uint probeIndex = ActiveProbes.Load((DispatchThreadId.x + ProbeIndexOffset + 1) * 4);
    uint3 probeCoords = GetDDGIProbeCoords(DDGI, probeIndex);
    probeIndex = GetDDGIScrollingProbeIndex(DDGI, CascadeIndex, probeCoords);

    // Load current probe state and position
    float4 probeData = LoadDDGIProbeData(DDGI, ProbesData, CascadeIndex, probeIndex);
    float probeAttention = DecodeDDGIProbeAttention(probeData);
    uint probeState = LoadDDGIProbeState(DDGI, ProbeStates, CascadeIndex, probeIndex);
    uint probeRaysCount = GetProbeRaysCount(DDGI, probeAttention);
    if (!IsDDGIProbeActive(probeState) || rayIndex >= probeRaysCount)
        return; // Skip disabled probes or if current thread's ray is unused
    float3 probePosition = DecodeDDGIProbePosition(DDGI, probeData, CascadeIndex, probeIndex, probeCoords);
    float3 probeRayDirection = GetProbeRayDirection(DDGI, rayIndex, probeRaysCount, probeIndex, probeCoords);
    // TODO: implement ray-guiding based on the probe irradiance (prioritize directions with high luminance)

    // Trace ray with Global SDF
    GlobalSDFTrace trace;
    trace.Init(probePosition, probeRayDirection, 0.0f, DDGI.RayMaxDistance);
    GlobalSDFHit hit = RayTraceGlobalSDF(GlobalSDF, GlobalSDFTex, GlobalSDFMip, trace);

    // Calculate radiance and distance
    float4 radiance;
    if (hit.IsHit())
    {
#if DDGI_TRACE_NEGATIVE
        if (hit.HitSDF <= 0.0f && hit.HitTime <= GlobalSDF.CascadeVoxelSize[hit.HitCascade] * DDGI_LIGHTING_RAY_BACKFACE_THRESHOLD)
        {
            // Ray starts inside geometry (mark as negative distance and reduce it's influence during irradiance blending)
            radiance = float4(0, 0, 0, -max(hit.HitTime, GlobalSDF.CascadeVoxelSize[hit.HitCascade] * DDGI_LIGHTING_RAY_BACKFACE_THRESHOLD));
        }
        else
#endif
        {
            // Sample Global Surface Atlas to get the lighting at the hit location
            float3 hitPosition = hit.GetHitPosition(trace);
            float surfaceThreshold = GetGlobalSurfaceAtlasThreshold(GlobalSDF, hit);
            float4 surfaceColor = SampleGlobalSurfaceAtlas(GlobalSurfaceAtlas, GlobalSurfaceAtlasChunks, RWGlobalSurfaceAtlasCulledObjects, GlobalSurfaceAtlasObjects, GlobalSurfaceAtlasDepth, GlobalSurfaceAtlasTex, hitPosition, -probeRayDirection, surfaceThreshold);
            // Missing atlas coverage is low-confidence data. Do not let it
            // reinforce either irradiance or distance history.
            if (surfaceColor.a <= 0.01f)
            {
                radiance = float4(0, 0, 0, -max(hit.HitTime, GlobalSDF.CascadeVoxelSize[hit.HitCascade] * DDGI_LIGHTING_RAY_BACKFACE_THRESHOLD));
            }
            else
            {
                // SampleGlobalSurfaceAtlas already returns normalized RGB.
                // Dividing by coverage again amplifies low-confidence samples
                // and can blow the editor view out to white.
                radiance = float4(surfaceColor.rgb, hit.HitTime);
                // Optional, bounded software tolerance for thin geometry.
                radiance.w = max(radiance.w + min(DDGI.ThinGeometryExpansion, GlobalSDF.CascadeVoxelSize[hit.HitCascade]), 0);
            }
        }
    }
    else
    {
        // Ray hits sky
        radiance.rgb = Skybox.SampleLevel(SamplerLinearClamp, probeRayDirection, 0).rgb * SkyboxIntensity;
        radiance.a = 1e27f; // Sky is the limit
    }

    // Write into probes trace results
    RWProbesTrace[uint2(rayIndex, DispatchThreadId.x)] = radiance;

#if DDGI_DEBUG_STATS
    // Update stats
    uint tmp;
    RWStats.InterlockedAdd(0, 1, tmp);
    if (rayIndex == 0)
        RWStats.InterlockedAdd(4, 1, tmp);
#endif
}

#endif

#if defined(_CS_UpdateProbes)

#if DDGI_PROBE_UPDATE_MODE == 0
// Update irradiance
#define DDGI_PROBE_RESOLUTION DDGI_PROBE_RESOLUTION_IRRADIANCE
groupshared float4 CachedProbesTraceRadiance[DDGI_TRACE_RAYS_LIMIT];
groupshared float OutputInstability[DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION];
#else
// Update distance
#define DDGI_PROBE_RESOLUTION DDGI_PROBE_RESOLUTION_DISTANCE
groupshared float CachedProbesTraceDistance[DDGI_TRACE_RAYS_LIMIT];
#endif

// Source: https://github.com/turanszkij/WickedEngine
#define BorderOffsetsSize (4 * DDGI_PROBE_RESOLUTION + 4)
#if DDGI_PROBE_RESOLUTION == 6
static const uint4 BorderOffsets[BorderOffsetsSize] = {
    uint4(6, 1, 1, 0),
    uint4(5, 1, 2, 0),
    uint4(4, 1, 3, 0),
    uint4(3, 1, 4, 0),
    uint4(2, 1, 5, 0),
    uint4(1, 1, 6, 0),

    uint4(6, 6, 1, 7),
    uint4(5, 6, 2, 7),
    uint4(4, 6, 3, 7),
    uint4(3, 6, 4, 7),
    uint4(2, 6, 5, 7),
    uint4(1, 6, 6, 7),

    uint4(1, 1, 0, 6),
    uint4(1, 2, 0, 5),
    uint4(1, 3, 0, 4),
    uint4(1, 4, 0, 3),
    uint4(1, 5, 0, 2),
    uint4(1, 6, 0, 1),

    uint4(6, 1, 7, 6),
    uint4(6, 2, 7, 5),
    uint4(6, 3, 7, 4),
    uint4(6, 4, 7, 3),
    uint4(6, 5, 7, 2),
    uint4(6, 6, 7, 1),

    uint4(1, 1, 7, 7),
    uint4(6, 1, 0, 7),
    uint4(1, 6, 7, 0),
    uint4(6, 6, 0, 0)
};
#elif DDGI_PROBE_RESOLUTION == 14
static const uint4 BorderOffsets[BorderOffsetsSize] = {
    uint4(14, 1, 1, 0),
    uint4(13, 1, 2, 0),
    uint4(12, 1, 3, 0),
    uint4(11, 1, 4, 0),
    uint4(10, 1, 5, 0),
    uint4(9, 1, 6, 0),
    uint4(8, 1, 7, 0),
    uint4(7, 1, 8, 0),
    uint4(6, 1, 9, 0),
    uint4(5, 1, 10, 0),
    uint4(4, 1, 11, 0),
    uint4(3, 1, 12, 0),
    uint4(2, 1, 13, 0),
    uint4(1, 1, 14, 0),

    uint4(14, 14, 1, 15),
    uint4(13, 14, 2, 15),
    uint4(12, 14, 3, 15),
    uint4(11, 14, 4, 15),
    uint4(10, 14, 5, 15),
    uint4(9, 14, 6, 15),
    uint4(8, 14, 7, 15),
    uint4(7, 14, 8, 15),
    uint4(6, 14, 9, 15),
    uint4(5, 14, 10, 15),
    uint4(4, 14, 11, 15),
    uint4(3, 14, 12, 15),
    uint4(2, 14, 13, 15),
    uint4(1, 14, 14, 15),

    uint4(1, 14, 0, 1),
    uint4(1, 13, 0, 2),
    uint4(1, 12, 0, 3),
    uint4(1, 11, 0, 4),
    uint4(1, 10, 0, 5),
    uint4(1, 9, 0, 6),
    uint4(1, 8, 0, 7),
    uint4(1, 7, 0, 8),
    uint4(1, 6, 0, 9),
    uint4(1, 5, 0, 10),
    uint4(1, 4, 0, 11),
    uint4(1, 3, 0, 12),
    uint4(1, 2, 0, 13),
    uint4(1, 1, 0, 14),

    uint4(14, 14, 15, 1),
    uint4(14, 13, 15, 2),
    uint4(14, 12, 15, 3),
    uint4(14, 11, 15, 4),
    uint4(14, 10, 15, 5),
    uint4(14, 9, 15, 6),
    uint4(14, 8, 15, 7),
    uint4(14, 7, 15, 8),
    uint4(14, 6, 15, 9),
    uint4(14, 5, 15, 10),
    uint4(14, 4, 15, 11),
    uint4(14, 3, 15, 12),
    uint4(14, 2, 15, 13),
    uint4(14, 1, 15, 14),

    uint4(14, 14, 0, 0),
    uint4(1, 14, 15, 0),
    uint4(14, 1, 0, 15),
    uint4(1, 1, 15, 15)
};
#else
#error "Unsupported probe size for border values copy."
#endif

groupshared float3 CachedProbesTraceDirection[DDGI_TRACE_RAYS_LIMIT];

RWTexture2D<float4> RWOutput : register(u0);
#if DDGI_PROBE_UPDATE_MODE == 0
RWTexture2D<snorm float4> RWProbesData : register(u1);
#if DDGI_DEBUG_INSTABILITY
RWTexture2D<float> RWOutputInstability : register(u3);
#endif
RWTexture2D<uint> RWProbeStates : register(u2);
#else
Texture2D<snorm float4> ProbesData : register(t0);
Texture2D<uint> ProbeStates : register(t3);
#endif
Texture2D<float4> ProbesTrace : register(t1);
ByteAddressBuffer ActiveProbes : register(t2);

// Compute shader for updating probes irradiance or distance texture.
META_CS(true, FEATURE_LEVEL_SM5)
META_PERMUTATION_1(DDGI_PROBE_UPDATE_MODE=0)
META_PERMUTATION_1(DDGI_PROBE_UPDATE_MODE=1)
[numthreads(DDGI_PROBE_RESOLUTION, DDGI_PROBE_RESOLUTION, 1)]
void CS_UpdateProbes(uint3 GroupThreadId : SV_GroupThreadID, uint3 GroupId : SV_GroupID, uint GroupIndex : SV_GroupIndex)
{
    // GroupThreadId.xy - coordinates of the probe texel: [0; DDGI_PROBE_RESOLUTION)
    // GroupId.x - index of the thread group which is probe index within a batch: [0; batchSize)
    // GroupIndex.x - index of the thread within a thread group: [0; DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION)
    uint probeIndex = ActiveProbes.Load((GroupId.x + ProbeIndexOffset + 1) * 4);
    uint3 probeCoords = GetDDGIProbeCoords(DDGI, probeIndex);
    probeIndex = GetDDGIScrollingProbeIndex(DDGI, CascadeIndex, probeCoords);

    // Load probe data
#if DDGI_PROBE_UPDATE_MODE == 0
    int2 probeDataCoords = GetDDGIProbeTexelCoords(DDGI, CascadeIndex, probeIndex);
    float4 probeData = RWProbesData[probeDataCoords];
    uint probeState = RWProbeStates[probeDataCoords];
#else
    float4 probeData = LoadDDGIProbeData(DDGI, ProbesData, CascadeIndex, probeIndex);
    uint probeState = LoadDDGIProbeState(DDGI, ProbeStates, CascadeIndex, probeIndex);
#endif
    float probeAttention = DecodeDDGIProbeAttention(probeData);
    uint probeRaysCount = GetProbeRaysCount(DDGI, probeAttention);

#if DDGI_PROBE_UPDATE_MODE == 0
#else
    float probesSpacing = DDGI.ProbesOriginAndSpacing[CascadeIndex].w;
    float distanceLimit = probesSpacing * 1.5f;
#endif

    // Load trace rays results into shared memory to reuse across whole thread group (raysCount per thread)
    uint raysCount = (uint)(ceil((float)probeRaysCount / (float)(DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION)));
    uint raysStart = GroupIndex * raysCount;
    raysCount = max(min(raysStart + raysCount, probeRaysCount), raysStart) - raysStart;
    for (uint i = 0; i < raysCount; i++)
    {
        uint rayIndex = raysStart + i;
#if DDGI_PROBE_UPDATE_MODE == 0
        CachedProbesTraceRadiance[rayIndex] = ProbesTrace[uint2(rayIndex, GroupId.x)];
#else
        float rayDistance = ProbesTrace[uint2(rayIndex, GroupId.x)].w;
        CachedProbesTraceDistance[rayIndex] = min(abs(rayDistance), distanceLimit);
#endif
        CachedProbesTraceDirection[rayIndex] = GetProbeRayDirection(DDGI, rayIndex, probeRaysCount, probeIndex, probeCoords);
    }
    GroupMemoryBarrierWithGroupSync();
    probeCoords = GetDDGIProbeCoords(DDGI, probeIndex);

    // Calculate octahedral projection for probe (unwraps spherical projection into a square)
    float2 octahedralCoords = GetOctahedralCoords(GroupThreadId.xy, DDGI_PROBE_RESOLUTION);
    float3 octahedralDirection = GetOctahedralDirection(octahedralCoords);

    // Loop over rays
    float4 result = float4(0, 0, 0, 0);
    LOOP
    // Fixed classification rays are deliberately excluded from both moment
    // convolutions. They are stable state evidence, not lighting samples.
    for (uint rayIndex = DDGI_FIXED_RAY_COUNT; rayIndex < probeRaysCount; rayIndex++)
    {
        float3 rayDirection = CachedProbesTraceDirection[rayIndex];
        float rayWeight = max(dot(octahedralDirection, rayDirection), 0.0f);

#if DDGI_PROBE_UPDATE_MODE == 0
        float4 rayRadiance = CachedProbesTraceRadiance[rayIndex];
        if (rayRadiance.w < 0.0f)
            continue;

        // Add radiance (RGB) and weight (A)
        result += float4(rayRadiance.rgb * rayWeight, rayWeight);
#else
        // Add distance (R), distance^2 (G) and weight (A)
        float rayDistance = CachedProbesTraceDistance[rayIndex];
        if (ProbesTrace[uint2(rayIndex, GroupId.x)].w < 0.0f)
            continue;
        // Increase reaction speed for depth discontinuities while keeping the
        // exponent runtime-tunable for regression captures.
        rayWeight = pow(rayWeight, max(1.0f, DDGI.DistanceExponent));
        result += float4(rayDistance, rayDistance * rayDistance, 0.0f, 1.0f) * rayWeight;
#endif
    }

    // Normalize results
    float epsilon = (float)max(probeRaysCount - DDGI_FIXED_RAY_COUNT, 1u) * 1e-9f;
    result.rgb *= 1.0f / (2.0f * max(result.a, epsilon));

    // Load current probe value
    uint2 outputCoords = GetDDGIProbeTexelCoords(DDGI, CascadeIndex, probeIndex) * (DDGI_PROBE_RESOLUTION + 2) + 1 + GroupThreadId.xy;
    float3 previous = RWOutput[outputCoords].rgb;
    bool wasActivated = (probeState & DDGI_PROBE_STATE_MASK) == DDGI_PROBE_STATE_ACTIVATED || (probeState & DDGI_PROBE_STATE_HISTORY_INVALID) != 0 || ResetBlend;

#if DDGI_PROBE_UPDATE_MODE == 0
    // Probe history is stored with the irradiance encoding gamma applied. Put
    // the new sample into that same domain before measuring changes or blending
    // it with history. Comparing linear samples with encoded history made every
    // update look unstable and continuously drove probes onto the fast path.
    result *= DDGI.IndirectLightingIntensity;
#if DDGI_SRGB_BLENDING
    result.rgb = pow(max(result.rgb, 0), 1.0f / DDGI.IrradianceGamma);
#endif
#endif

    if (wasActivated)
        previous = result.rgb;

#if DDGI_PROBE_UPDATE_MODE == 0
    // Calculate instability of the irradiance
    float previousLuma = Luminance(previous.rgb);
    float resultLuma = Luminance(result.rgb);
    float instability = abs(previousLuma - resultLuma) / max(previousLuma, 1e-4f); // Percentage change in luminance of irradiance
    instability = max(instability, Max3(abs(result.rgb - previous) / max(abs(previous), 1e-4f))); // Percentage of color delta change of irradiance
    //instability *= saturate(result.a); // Reduce instability in areas with a small ray-coverage
    //instability = pow(instability, 1.2f); // Increase contrast
    instability *= 2.0f; // Make it stronger on scene changes
    //instability = saturate(instability);
    OutputInstability[GroupIndex] = instability;
#if DDGI_DEBUG_INSTABILITY
    RWOutputInstability[outputCoords] = instability;
    //RWOutputInstability[outputCoords] = probeAttention; // Debug test probe attention visualization
#endif
#endif

    // Blend current value with the previous probe data
    float historyWeightFast = DDGI.ProbeHistoryWeight;
    float historyWeightSlow = 0.97f;
#if DDGI_PROBE_UPDATE_MODE == 0
    float3 irradianceDelta = result.rgb - previous;
    float irradianceDeltaLen = length(irradianceDelta);
#endif
    float historyWeight = lerp(historyWeightSlow, historyWeightFast, probeAttention * probeAttention * probeAttention);
    //historyWeight = 1.0f; // Debug full-blend
    //historyWeight = 0.0f; // Debug no-blend
    if (wasActivated)
        historyWeight = 0.0f;
#if DDGI_PROBE_UPDATE_MODE == 0
    // Match RTXGI's temporal safeguards. A large reduction in lighting should
    // clear promptly, while a single bright stochastic sample must not replace
    // a probe texel in one update.
    if (Max3(previous - result.rgb) > 0.2f)
        historyWeight = max(0.0f, historyWeight - 0.75f);
    if (irradianceDeltaLen > 2.0f)
        irradianceDelta *= 0.25f;

    float3 lerpDelta = (1.0f - historyWeight) * irradianceDelta;
    if (Max3(result.rgb) < Max3(previous.rgb))
    {
        // R11G11B10 can otherwise quantize a small darkening update back to the
        // previous value forever. Ensure history always converges downward.
        const float minDarkeningStep = 1.0f / 1024.0f;
        lerpDelta = min(max(minDarkeningStep, abs(lerpDelta)), abs(irradianceDelta)) * sign(lerpDelta);
    }
    result = float4(previous.rgb + lerpDelta, 1.0f);

    // Do not inject positive temporal quantization noise into persistent probe
    // history. Repeated updates turned that one-sided error into energy drift
    // and visible shimmer. Native render-target quantization is sufficient.
#else
    result = float4(lerp(result.rg, previous.rg, historyWeight), 0.0f, 1.0f);
#endif

    RWOutput[outputCoords] = result;
    GroupMemoryBarrierWithGroupSync();

    uint2 baseCoords = GetDDGIProbeTexelCoords(DDGI, CascadeIndex, probeIndex) * (DDGI_PROBE_RESOLUTION + 2);

#if DDGI_PROBE_UPDATE_MODE == 0
    // The first thread updates the probe attention based on the instability of all texels
    BRANCH
    if (GroupIndex == 0 && IsDDGIProbeActive(probeState))
    {
        // Calculate instability statistics for a whole probe
        float instabilityAvg = 0;
        for (uint i = 0; i < DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION; i++)
            instabilityAvg += OutputInstability[i];
        instabilityAvg *= 1.0f / float(DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION);
        instabilityAvg = saturate(instabilityAvg);
        instability = instabilityAvg;

        // Calculate probe attention
        float taregAttention = lerp(0.5f, DDGI_PROBE_ATTENTION_MAX, instability); // Use some base level
        if (taregAttention >= probeAttention)
            probeAttention = taregAttention; // Quick jump up
        else
            probeAttention = lerp(probeAttention, taregAttention, 0.2f); // Slow blend down
        if ((probeState & DDGI_PROBE_STATE_MASK) == DDGI_PROBE_STATE_ACTIVATED)
            probeAttention = DDGI_PROBE_ATTENTION_MAX;

        // Keep a newly activated/scrolled probe in warm-up for one complete
        // update. Sampling ignores ACTIVATED probes, preventing a single bright
        // ray batch from appearing as a distant flash. The next classification
        // promotes it to ACTIVE and the second update confirms its history.
        probeState = (probeState & DDGI_PROBE_STATE_MASK) == DDGI_PROBE_STATE_ACTIVATED
            ? DDGI_PROBE_STATE_ACTIVATED
            : DDGI_PROBE_STATE_ACTIVE;
        RWProbeStates[probeDataCoords] = probeState;
        RWProbesData[probeDataCoords] = EncodeDDGIProbeData(probeData.xyz, probeAttention);
    }

#if DDGI_DEBUG_INSTABILITY
	// Copy border pixels
	for (uint borderIndex = GroupIndex; borderIndex < BorderOffsetsSize; borderIndex += DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION)
	{
        uint4 borderOffsets = BorderOffsets[borderIndex];
		RWOutputInstability[baseCoords + borderOffsets.zw] = RWOutputInstability[baseCoords + borderOffsets.xy];
	}
#endif
#endif

    // Copy border pixels
	for (uint borderIndex = GroupIndex; borderIndex < BorderOffsetsSize; borderIndex += DDGI_PROBE_RESOLUTION * DDGI_PROBE_RESOLUTION)
	{
        uint4 borderOffsets = BorderOffsets[borderIndex];
		RWOutput[baseCoords + borderOffsets.zw] = RWOutput[baseCoords + borderOffsets.xy];
	}
}

#endif

#ifdef _PS_IndirectLighting

#include "./Flax/GBuffer.hlsl"
#include "./Flax/Random.hlsl"
#include "./Flax/LightingCommon.hlsl"

Texture2D<snorm float4> ProbesData : register(t4);
Texture2D<uint> ProbeStates : register(t5);
Texture2D<float4> ProbesDistance : register(t6);
Texture2D<float4> ProbesIrradiance : register(t7);

// Pixel shader for drawing indirect lighting in fullscreen
META_PS(true, FEATURE_LEVEL_SM5)
META_PERMUTATION_1(DDGI_CASCADE_BLEND_SMOOTH=0)
META_PERMUTATION_1(DDGI_CASCADE_BLEND_SMOOTH=1)
void PS_IndirectLighting(Quad_VS2PS input, out float4 output : SV_Target0)
{
    output = 0;

    // Sample GBuffer
    GBufferSample gBuffer = SampleGBuffer(GBuffer, input.TexCoord);

    // Check if cannot shadow pixel
    BRANCH
    if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
    {
        discard;
        return;
    }

    // Sample irradiance
    float dither = RandN2(input.TexCoord + TemporalTime).x;
    // Reconstruct a conservative geometric normal from the depth-derived
    // world position derivatives. Keep the shading normal for irradiance
    // direction lookup, but use the geometric normal for visibility bias.
    float3 geometricNormal = normalize(cross(ddx(gBuffer.WorldPos), ddy(gBuffer.WorldPos)));
    if (dot(geometricNormal, gBuffer.Normal) < 0.0f)
        geometricNormal = -geometricNormal;
    if (any(isnan(geometricNormal)) || length(geometricNormal) < 0.5f)
        geometricNormal = gBuffer.Normal;
    // Keep the sample offset deterministic. Using the temporal cascade dither
    // as a position offset made cell selection shimmer in editor views.
    float3 samplePos = gBuffer.WorldPos + geometricNormal * 0.1f;
    float3 irradiance = SampleDDGIIrradianceWithVisibilityNormal(DDGI, ProbesData, ProbeStates, ProbesDistance, ProbesIrradiance, samplePos, gBuffer.Normal, geometricNormal, DDGI_DEFAULT_BIAS, dither);

    // Calculate lighting
    float3 diffuseColor = GetDiffuseColor(gBuffer);
    float3 diffuse = Diffuse_Lambert(diffuseColor);
    output.rgb = diffuse * irradiance * gBuffer.AO;
}

#endif
