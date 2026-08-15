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
#include "./Flax/Octahedral.hlsl"

#define DDGI_PROBE_STATE_INACTIVE 0
#define DDGI_PROBE_STATE_ACTIVE 1
#define DDGI_PROBE_STATE_ACTIVATED 2
#define DDGI_PROBE_STATE_INACTIVE_INSIDE 3
#define DDGI_PROBE_STATE_INACTIVE_EMPTY 4
#define DDGI_PROBE_STATE_INACTIVE_OVERLAP 5
#define DDGI_PROBE_STATE_INACTIVE_INVALID 6
#define DDGI_PROBE_STATE_INACTIVE_OUTSIDE 7
#define DDGI_PROBE_STATE_MASK 0x0Fu
#define DDGI_PROBE_STATE_HISTORY_INVALID 0x80u
#define DDGI_PROBE_STATE_CONFIRMATION_MASK 0x70u
#define DDGI_PROBE_STATE_CONFIRMATION_SHIFT 4u
#define DDGI_PROBE_REACTIVATION_CONFIRM_FRAMES 2u
#define DDGI_FALLBACK_COORDS_ENCODE(coord) ((float3)(coord + 1) / 128.0f)
#define DDGI_FALLBACK_COORDS_DECODE(data) (uint3)(data.xyz * 128.0f - 1)
#define DDGI_FALLBACK_COORDS_VALID(data) (length(data.xyz) > 0)
#define DDGI_PROBE_ATTENTION_MIN 0.02f // Minimum probe attention value that still makes it active.
#define DDGI_PROBE_ATTENTION_MAX 0.98f // Maximum probe attention value that still makes it active (but not activated which is 1.0f).
#define DDGI_PROBE_RESOLUTION_IRRADIANCE 6 // Resolution (in texels) for probe irradiance data (excluding 1px padding on each side)
#define DDGI_PROBE_RESOLUTION_DISTANCE 14 // Resolution (in texels) for probe distance data (excluding 1px padding on each side)
#define DDGI_CASCADE_BLEND_SIZE 2.0f // Distance in probes over which cascades blending happens
#ifndef DDGI_CASCADE_BLEND_SMOOTH
#define DDGI_CASCADE_BLEND_SMOOTH 0 // Enables smooth cascade blending, otherwise dithering will be used
#endif
#define DDGI_SRGB_BLENDING 1 // Enables blending in sRGB color space, otherwise irradiance blending is done in linear space
#define DDGI_DEFAULT_BIAS 0.2f // Default value for DDGI sampling bias
//#define DDGI_DEBUG_CASCADE 0 // Forces a specific cascade to be only in use (for debugging)

// DDGI data for a constant buffer
struct DDGIData
{
    float4 ProbesOriginAndSpacing[4];
    float4 BlendOrigin[4]; // w is unused
    int4 ProbesScrollOffsets[4]; // w is unused
    uint3 ProbesCounts;
    uint CascadesCount;
    float IrradianceGamma;
    float ProbeHistoryWeight;
    float RayMaxDistance;
    float IndirectLightingIntensity;
    float3 ViewPos;
    uint RaysCount;
    float4 FallbackIrradiance;
    float NormalBias;
    float ViewBias;
    float DistanceExponent;
    float VisibilityFloor;
    float ThinGeometryExpansion;
    uint FixedRayCount;
    uint ProbeRayBudget;
    uint ActiveTraceBackend;
    uint Algorithm; // 0: legacy DDGI, 1: DDGI+
    uint3 Padding;
};

uint GetDDGIProbeIndex(DDGIData data, uint3 probeCoords)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = probeCoords.y;
    uint probeIndexInPlane = probeCoords.x + (data.ProbesCounts.x * probeCoords.z);
    return planeIndex * probesPerPlane + probeIndexInPlane;
}

uint GetDDGIProbeIndex(DDGIData data, uint2 texCoords, uint texResolution)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = texCoords.x / (data.ProbesCounts.x * texResolution);
    uint probeIndexInPlane = (texCoords.x / texResolution) - (planeIndex * data.ProbesCounts.x) + (data.ProbesCounts.x * (texCoords.y / texResolution));
    return planeIndex * probesPerPlane + probeIndexInPlane;
}

uint3 GetDDGIProbeCoords(DDGIData data, uint probeIndex)
{
    uint3 probeCoords;
    probeCoords.x = probeIndex % data.ProbesCounts.x;
    probeCoords.y = probeIndex / (data.ProbesCounts.x * data.ProbesCounts.z);
    probeCoords.z = (probeIndex / data.ProbesCounts.x) % data.ProbesCounts.z;
    return probeCoords;
}

uint2 GetDDGIProbeTexelCoords(DDGIData data, uint cascadeIndex, uint probeIndex)
{
    uint probesPerPlane = data.ProbesCounts.x * data.ProbesCounts.z;
    uint planeIndex = probeIndex / probesPerPlane;
    uint gridSpaceX = probeIndex % data.ProbesCounts.x;
    uint gridSpaceY = probeIndex / data.ProbesCounts.x;
    uint x = gridSpaceX + (planeIndex * data.ProbesCounts.x);
    uint y = gridSpaceY % data.ProbesCounts.z + cascadeIndex * data.ProbesCounts.z;
    return uint2(x, y);
}

uint GetDDGIScrollingProbeIndex(DDGIData data, uint cascadeIndex, uint3 probeCoords)
{
    // Probes are scrolled on edges to stabilize GI when camera moves
    int3 probeCoordsOffset = (int3)data.ProbesCounts + data.ProbesScrollOffsets[cascadeIndex].xyz;
    return GetDDGIProbeIndex(data, (probeCoords + (uint3)probeCoordsOffset) % data.ProbesCounts);
}

float3 GetDDGIProbeWorldPosition(DDGIData data, uint cascadeIndex, uint3 probeCoords)
{
    float3 probesOrigin = data.ProbesOriginAndSpacing[cascadeIndex].xyz;
    float probesSpacing = data.ProbesOriginAndSpacing[cascadeIndex].w;
    float3 probePosition = probeCoords * probesSpacing;
    float3 probeGridOffset = (probesSpacing * (data.ProbesCounts - 1)) * 0.5f;
    float3 probeScrollOffset = data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing;
    return probesOrigin + probePosition - probeGridOffset + probeScrollOffset;
}

// Loads probe probe data (encoded)
float4 LoadDDGIProbeData(DDGIData data, Texture2D<snorm float4> probesData, uint cascadeIndex, uint probeIndex)
{
    int2 probeDataCoords = GetDDGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    return probesData.Load(int3(probeDataCoords, 0));
}

// Encodes probe position and temporal attention. Probe lifecycle state is stored separately.
float4 EncodeDDGIProbeData(float3 offset, float attention)
{
    // [0;1] -> [-1;1]
    attention = saturate(attention) * 2.0f - 1.0f;
    return float4(offset, attention);
}

// Legacy encoding overload used by the original DDGI fallback propagation pass.
float4 EncodeDDGIProbeData(float3 offset, uint state, float attention)
{
    if (state == DDGI_PROBE_STATE_INACTIVE)
        attention = 0.0f;
    else if (state == DDGI_PROBE_STATE_ACTIVATED)
        attention = 1.0f;
    return EncodeDDGIProbeData(offset, attention);
}

// Decodes probe attention value from the encoded state
float DecodeDDGIProbeAttention(float4 probeData)
{
    // [-1;1] -> [0;1]
    if (probeData.w <= -1.0f)
        return 0.0f;
    if (probeData.w >= 1.0f)
        return 1.0f;
    return probeData.w * 0.5f + 0.5f;
}

// Compatibility helper for materials that only bind ProbesData (including the
// editor probe visualization material). The authoritative lifecycle reason is
// stored in ProbeStates, but inactive and newly activated probes still encode
// attention as the legacy endpoint values so the lightweight visualization can
// distinguish their broad state without another texture binding.
uint DecodeDDGIProbeState(float4 probeData)
{
    if (probeData.w <= -1.0f)
        return DDGI_PROBE_STATE_INACTIVE;
    if (probeData.w >= 1.0f)
        return DDGI_PROBE_STATE_ACTIVATED;
    return DDGI_PROBE_STATE_ACTIVE;
}

// Loads the explicit probe lifecycle state.
uint LoadDDGIProbeState(DDGIData data, Texture2D<uint> probeStates, uint cascadeIndex, uint probeIndex)
{
    int2 probeDataCoords = GetDDGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    return probeStates.Load(int3(probeDataCoords, 0));
}

bool IsDDGIProbeActive(uint state)
{
    state &= DDGI_PROBE_STATE_MASK;
    return state == DDGI_PROBE_STATE_ACTIVE || state == DDGI_PROBE_STATE_ACTIVATED;
}

// Newly activated/scrolled probes become ready after their first complete
// irradiance+distance update clears HISTORY_INVALID. ACTIVATED remains a
// lifecycle hint until the next classification and is otherwise sampleable.
bool IsDDGIProbeReady(uint state)
{
    state &= DDGI_PROBE_STATE_MASK;
    return state == DDGI_PROBE_STATE_ACTIVE || state == DDGI_PROBE_STATE_ACTIVATED;
}

bool IsDDGIProbeInactive(uint state)
{
    return !IsDDGIProbeActive(state);
}

// Decodes probe world-space position (XYZ) from the encoded state
float3 DecodeDDGIProbePosition(DDGIData data, float4 probeData, uint cascadeIndex, uint probeIndex, uint3 probeCoords)
{
    float3 probePosition = probeData.xyz;
    probePosition *= data.ProbesOriginAndSpacing[cascadeIndex].w; // Probe offset is [-1;1] within probes spacing
    probePosition += GetDDGIProbeWorldPosition(data, cascadeIndex, probeCoords); // Place probe on a grid
    return probePosition;
}

// Calculates texture UVs for sampling probes atlas texture (irradiance or distance)
float2 GetDDGIProbeUV(DDGIData data, uint cascadeIndex, uint probeIndex, float2 octahedralCoords, uint resolution)
{
    uint2 coords = GetDDGIProbeTexelCoords(data, cascadeIndex, probeIndex);
    float probeTexelSize = resolution + 2.0f;
    float2 textureSize = float2(data.ProbesCounts.x * data.ProbesCounts.y, data.ProbesCounts.z * data.CascadesCount) * probeTexelSize;
    float2 uv = float2(coords.x * probeTexelSize, coords.y * probeTexelSize) + (probeTexelSize * 0.5f);
    uv += octahedralCoords.xy * (resolution * 0.5f);
    uv /= textureSize;
    return uv;
}

float3 SampleDDGIIrradianceCascadeLegacy(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, uint cascadeIndex, float3 probesOrigin, float3 probesExtent, float probesSpacing, float3 biasedWorldPosition)
{
    bool invalidCascade = cascadeIndex >= data.CascadesCount;
    cascadeIndex = min(cascadeIndex, data.CascadesCount - 1);
    uint3 probeCoordsEnd = data.ProbesCounts - uint3(1, 1, 1);
    uint3 baseProbeCoords = clamp(uint3((worldPosition - probesOrigin + probesExtent) / probesSpacing), uint3(0, 0, 0), probeCoordsEnd);
    float3 baseProbeWorldPosition = GetDDGIProbeWorldPosition(data, cascadeIndex, baseProbeCoords);
    float3 biasAlpha = saturate((biasedWorldPosition - baseProbeWorldPosition) / probesSpacing);
    float4 irradiance = float4(0, 0, 0, 0);
    for (uint i = 0; i < 8; i++)
    {
        uint3 probeCoordsOffset = uint3(i, i >> 1, i >> 2) & 1;
        uint3 probeCoords = clamp(baseProbeCoords + probeCoordsOffset, uint3(0, 0, 0), probeCoordsEnd);
        uint probeIndex = GetDDGIScrollingProbeIndex(data, cascadeIndex, probeCoords);
        float4 probeData = LoadDDGIProbeData(data, probesData, cascadeIndex, probeIndex);
        uint probeState = DecodeDDGIProbeState(probeData);
        bool useVisibility = true;
        float minimumWeight = 0.000001f;
        if (probeState == DDGI_PROBE_STATE_INACTIVE && DDGI_FALLBACK_COORDS_VALID(probeData))
        {
            uint3 fallbackCoords = clamp(DDGI_FALLBACK_COORDS_DECODE(probeData), uint3(0, 0, 0), probeCoordsEnd);
            float fallbackToProbeDist = length((float3)probeCoords - (float3)fallbackCoords);
            useVisibility = fallbackToProbeDist <= 1.0f;
            if (fallbackToProbeDist > 2.0f)
                minimumWeight = 1.0f;
            probeCoords = fallbackCoords;
            probeIndex = GetDDGIScrollingProbeIndex(data, cascadeIndex, fallbackCoords);
            probeData = LoadDDGIProbeData(data, probesData, cascadeIndex, probeIndex);
        }

        float3 probePosition = baseProbeWorldPosition + (((float3)probeCoords - (float3)baseProbeCoords) * probesSpacing) + probeData.xyz * probesSpacing;
        float3 worldPosToProbe = normalize(probePosition - worldPosition);
        float3 biasedPosToProbe = normalize(probePosition - biasedWorldPosition);
        float biasedPosToProbeDist = length(probePosition - biasedWorldPosition) * 0.95f;
        float weight = max(Square(dot(worldPosToProbe, worldNormal) * 0.5f + 0.5f), 0.1f);
        float2 octahedralCoords = GetOctahedralCoords(-biasedPosToProbe);
        float2 uv = GetDDGIProbeUV(data, cascadeIndex, probeIndex, octahedralCoords, DDGI_PROBE_RESOLUTION_DISTANCE);
        float2 probeDistance = probesDistance.SampleLevel(SamplerLinearClamp, uv, 0).rg * 2.0f;
        if (biasedPosToProbeDist > probeDistance.x && useVisibility)
        {
            float variance = abs(Square(probeDistance.x) - probeDistance.y);
            float visibilityWeight = variance / (variance + Square(biasedPosToProbeDist - probeDistance.x));
            weight *= visibilityWeight * visibilityWeight * visibilityWeight;
        }
        weight = max(weight, minimumWeight);
        const float minWeightThreshold = 0.2f;
        if (weight < minWeightThreshold)
            weight *= (weight * weight) / (minWeightThreshold * minWeightThreshold);
        float3 trilinear = lerp(1.0f - biasAlpha, biasAlpha, (float3)probeCoordsOffset);
        weight *= saturate(trilinear.x * trilinear.y * trilinear.z * 2.0f);
        octahedralCoords = GetOctahedralCoords(worldNormal);
        uv = GetDDGIProbeUV(data, cascadeIndex, probeIndex, octahedralCoords, DDGI_PROBE_RESOLUTION_IRRADIANCE);
        float3 probeIrradiance = probesIrradiance.SampleLevel(SamplerLinearClamp, uv, 0).rgb;
#if DDGI_SRGB_BLENDING
        probeIrradiance = pow(probeIrradiance, data.IrradianceGamma * 0.5f);
#endif
        irradiance += float4(probeIrradiance * weight, weight);
    }
    if (irradiance.a > 0.0f)
    {
        irradiance.rgb /= invalidCascade ? irradiance.a : lerp(1, irradiance.a, saturate(irradiance.a * irradiance.a + 0.9f));
#if DDGI_SRGB_BLENDING
        irradiance.rgb *= irradiance.rgb;
#endif
        irradiance.rgb *= 2.0f * PI;
    }
    return irradiance.rgb;
}

float3 SampleDDGIIrradianceCascade(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, float3 visibilityNormal, uint cascadeIndex, float3 probesOrigin, float3 probesExtent, float probesSpacing, float3 biasedWorldPosition, out float volumeVisibility, out float probeCoverage)
{
    bool invalidCascade = cascadeIndex >= data.CascadesCount;
    cascadeIndex = min(cascadeIndex, data.CascadesCount - 1);
    float visibilityNormalLength = length(visibilityNormal);
    visibilityNormal = visibilityNormalLength > 1e-4f ? visibilityNormal / visibilityNormalLength : float3(0, 1, 0);
    uint3 probeCoordsEnd = data.ProbesCounts - uint3(1, 1, 1);
    // The biased point determines the base cell. This keeps the eight-corner
    // interpolation on the same side of a surface as the visibility query.
    int3 baseProbeCoordsSigned = (int3)floor((biasedWorldPosition - probesOrigin + probesExtent) / probesSpacing);
    baseProbeCoordsSigned = clamp(baseProbeCoordsSigned, int3(0, 0, 0), (int3)probeCoordsEnd);
    uint3 baseProbeCoords = (uint3)baseProbeCoordsSigned;

    // Get the grid coordinates of the probe nearest the biased world position
    float3 baseProbeWorldPosition = GetDDGIProbeWorldPosition(data, cascadeIndex, baseProbeCoords);
    float3 biasAlpha = saturate((biasedWorldPosition - baseProbeWorldPosition) / probesSpacing);

    // Loop over the closest probes to accumulate their contributions
    float4 irradiance = float4(0, 0, 0, 0);
    float visibilitySum = 0.0f;
    float visibilityWeightSum = 0.0f;
    for (uint i = 0; i < 8; i++)
    {
        uint3 probeCoordsOffset = uint3(i, i >> 1, i >> 2) & 1;
        uint3 probeCoords = clamp(baseProbeCoords + probeCoordsOffset, uint3(0, 0, 0), probeCoordsEnd);
        uint probeIndex = GetDDGIScrollingProbeIndex(data, cascadeIndex, probeCoords);

        // Load probe position and state
        float4 probeData = LoadDDGIProbeData(data, probesData, cascadeIndex, probeIndex);
        uint probeState = LoadDDGIProbeState(data, probeStates, cascadeIndex, probeIndex);
        if (!IsDDGIProbeReady(probeState) || (probeState & DDGI_PROBE_STATE_HISTORY_INVALID) != 0)
            continue;

        // Calculate probe position
        float3 probePosition = baseProbeWorldPosition + (((float3)probeCoords - (float3)baseProbeCoords) * probesSpacing) + probeData.xyz * probesSpacing;

        // Calculate the distance and direction from the (biased and non-biased) shading point and the probe
        float3 worldPosToProbeVector = probePosition - worldPosition;
        float worldPosToProbeDistance = length(worldPosToProbeVector);
        float3 worldPosToProbe = worldPosToProbeDistance > 1e-4f ? worldPosToProbeVector / worldPosToProbeDistance : visibilityNormal;
        float3 biasedPosToProbeVector = probePosition - biasedWorldPosition;
        float biasedPosToProbeVectorLength = length(biasedPosToProbeVector);
        float3 biasedPosToProbe = biasedPosToProbeVectorLength > 1e-4f ? biasedPosToProbeVector / biasedPosToProbeVectorLength : visibilityNormal;
        float biasedPosToProbeDist = length(probePosition - biasedWorldPosition);

        // Smooth backface test
        float weight = Square(dot(worldPosToProbe, visibilityNormal) * 0.5f + 0.5f);
        weight = max(weight, data.VisibilityFloor);

        // Sample distance texture
        float2 octahedralCoords = GetOctahedralCoords(-biasedPosToProbe);
        float2 uv = GetDDGIProbeUV(data, cascadeIndex, probeIndex, octahedralCoords, DDGI_PROBE_RESOLUTION_DISTANCE);
        float2 probeDistance = probesDistance.SampleLevel(SamplerLinearClamp, uv, 0).rg * 2.0f;

        // Visibility weight (Chebyshev)
        float visibilityWeight = 1.0f;
        if (biasedPosToProbeDist > probeDistance.x)
        {
            float variance = abs(Square(probeDistance.x) - probeDistance.y);
            visibilityWeight = variance / max(variance + Square(biasedPosToProbeDist - probeDistance.x), 1e-6f);
            weight *= max(visibilityWeight, data.VisibilityFloor);
        }

        // Keep a small floor only for valid active probes. Inactive probes do
        // not participate in the normalization or provide fallback lighting.
        weight = max(weight, data.VisibilityFloor);

        // Adjust weight curve to inject a small portion of light
        const float minWeightThreshold = 0.2f;
        if (weight < minWeightThreshold) weight *= (weight * weight) * (1.0f / (minWeightThreshold * minWeightThreshold));

        // Calculate trilinear weights based on the distance to each probe to smoothly transition between grid of 8 probes
        float3 trilinear = lerp(1.0f - biasAlpha, biasAlpha, (float3)probeCoordsOffset);
        float trilinearWeight = trilinear.x * trilinear.y * trilinear.z;
        weight *= trilinearWeight;

        // Preserve the unnormalized distance-moment visibility for participating
        // media. Surface irradiance normalizes probe weights by design, but that
        // normalization otherwise turns a uniformly occluded fog cell back into
        // fully lit scattering.
        visibilitySum += saturate(visibilityWeight) * trilinearWeight;
        visibilityWeightSum += trilinearWeight;

        // Sample irradiance texture
        octahedralCoords = GetOctahedralCoords(worldNormal);
        uv = GetDDGIProbeUV(data, cascadeIndex, probeIndex, octahedralCoords, DDGI_PROBE_RESOLUTION_IRRADIANCE);
        float3 probeIrradiance = probesIrradiance.SampleLevel(SamplerLinearClamp, uv, 0).rgb;
#if DDGI_SRGB_BLENDING
        probeIrradiance = pow(probeIrradiance, data.IrradianceGamma * 0.5f);
#endif

        // Debug probe offset visualization
        //probeIrradiance = float3(max(frac(probeData.xyz) * 2, 0.1f));

        // Accumulate weighted irradiance
        if (weight > 0.0f)
            irradiance += float4(probeIrradiance * weight, weight);
    }

#if 0
    // Debug DDGI cascades with colors
    if (cascadeIndex == 0)
        irradiance = float4(1, 0, 0, 1);
    else if (cascadeIndex == 1)
        irradiance = float4(0, 1, 0, 1);
    else if (cascadeIndex == 2)
        irradiance = float4(0, 0, 1, 1);
    else
        irradiance = float4(1, 0, 1, 1);
#endif

    // Track the actual trilinear coverage of ready probes separately from
    // distance-moment visibility. Renormalizing a partially valid cell to full
    // strength makes probe activation changes appear as large lighting blocks.
    probeCoverage = saturate(visibilityWeightSum);

    // A negative value is an internal sentinel meaning that this cascade has no
    // ready probes at the sample location. The caller uses it to fall through to
    // a coarser cascade instead of presenting a transient zero-GI hole.
    volumeVisibility = probeCoverage > 0.0f ? saturate(visibilitySum / visibilityWeightSum) : -1.0f;
    if (irradiance.a > 0.0f)
    {
        // Normalize only by probes that actually contributed.
        irradiance.rgb /= max(irradiance.a, 1e-6f);
#if DDGI_SRGB_BLENDING
        irradiance.rgb *= irradiance.rgb;
#endif
        irradiance.rgb *= 2.0f * PI;
    }
    return irradiance.rgb;
}

float3 SampleDDGIIrradianceCascadeSelected(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, float3 visibilityNormal, uint cascadeIndex, float3 probesOrigin, float3 probesExtent, float probesSpacing, float3 biasedWorldPosition, out float volumeVisibility, out float probeCoverage)
{
    if (data.Algorithm == 0)
    {
        volumeVisibility = 1.0f;
        probeCoverage = 1.0f;
        return SampleDDGIIrradianceCascadeLegacy(data, probesData, probesDistance, probesIrradiance, worldPosition, worldNormal, cascadeIndex, probesOrigin, probesExtent, probesSpacing, biasedWorldPosition);
    }
    return SampleDDGIIrradianceCascade(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, visibilityNormal, cascadeIndex, probesOrigin, probesExtent, probesSpacing, biasedWorldPosition, volumeVisibility, probeCoverage);
}

float3 GetDDGISurfaceBias(DDGIData data, float3 viewDir, float probesSpacing, float3 geometricNormal, float bias)
{
    if (data.Algorithm == 0)
        return (geometricNormal * 0.2f + viewDir * 0.8f) * (0.6f * probesSpacing * bias);

    // Bias the world-space position to reduce artifacts. The actual values
    // are world-space settings; bias is kept as a compatibility scale for
    // callers that intentionally request no bias (for example fog).
    float biasScale = bias / max(DDGI_DEFAULT_BIAS, 1e-6f);
    return (geometricNormal * data.NormalBias + viewDir * data.ViewBias) * biasScale;
}

// [Inigo Quilez, https://iquilezles.org/articles/distfunctions/]
float sdRoundBox(float3 p, float3 b, float r)
{
    float3 q = abs(p) - b + r;
    return length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f) - r;
}

// Samples DDGI probes volume at the given world-space position and returns the irradiance.
// bias - scales the world-space bias settings (zero disables the bias).
// dither - randomized per-pixel value in range 0-1, used to smooth dithering for cascades blending.
float3 SampleDDGIIrradianceInternal(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, float3 visibilityNormal, float bias, float dither, out float volumeVisibility)
{
    volumeVisibility = 0.0f;
    // Select the highest cascade that contains the sample location
    float probesSpacing = 0, cascadeWeight = 0;
    float3 probesOrigin = (float3)0, probesExtent = (float3)0, biasedWorldPosition = (float3)0;
    float3 viewVector = data.ViewPos - worldPosition;
    float viewDistance = length(viewVector);
    float3 viewDir = viewDistance > 1e-4f ? viewVector / viewDistance : worldNormal;
#if DDGI_CASCADE_BLEND_SMOOTH
    dither = 0.0f;
#endif
#ifdef DDGI_DEBUG_CASCADE
    uint cascadeIndex = DDGI_DEBUG_CASCADE;
#else
    uint cascadeIndex = 0;
    if (data.CascadesCount == 0)
        return float3(0, 0, 0);
    for (; cascadeIndex < data.CascadesCount; cascadeIndex++)
    {
        // Get cascade data
        probesSpacing = data.ProbesOriginAndSpacing[cascadeIndex].w;
        probesOrigin = data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing + data.ProbesOriginAndSpacing[cascadeIndex].xyz;
        probesExtent = (data.ProbesCounts - 1) * (probesSpacing * 0.5f);
        biasedWorldPosition = worldPosition + GetDDGISurfaceBias(data, viewDir, probesSpacing, visibilityNormal, bias);

        // Calculate cascade blending weight (use input bias to smooth transition)
        float fadeDistance = probesSpacing * DDGI_CASCADE_BLEND_SIZE;
        float3 blendPos = worldPosition - data.BlendOrigin[cascadeIndex].xyz;
        cascadeWeight = sdRoundBox(blendPos, probesExtent - probesSpacing, probesSpacing * 2) + fadeDistance;
        cascadeWeight = 1 - saturate(cascadeWeight / fadeDistance);
        if (cascadeWeight > dither)
            break;
    }
#endif

    // Sample cascade
    float probeCoverage;
    float3 result = SampleDDGIIrradianceCascadeSelected(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, visibilityNormal, cascadeIndex, probesOrigin, probesExtent, probesSpacing, biasedWorldPosition, volumeVisibility, probeCoverage);

    // Probe classification and scrolling can leave a cell with only part of
    // its eight-corner interpolation ready. Blend by actual trilinear coverage
    // toward the best ready outer cascade instead of renormalizing the remaining
    // probes to full strength and switching an entire cell at once.
    bool usedProbeFallback = false;
    if (data.Algorithm != 0 && probeCoverage < 0.999f)
    {
        float3 fallbackResult = float3(0, 0, 0);
        float fallbackVisibility = 1.0f;
        float fallbackCoverageBest = 0.0f;
        for (uint fallbackCascade = cascadeIndex + 1; fallbackCascade < data.CascadesCount; fallbackCascade++)
        {
            probesSpacing = data.ProbesOriginAndSpacing[fallbackCascade].w;
            probesOrigin = data.ProbesScrollOffsets[fallbackCascade].xyz * probesSpacing + data.ProbesOriginAndSpacing[fallbackCascade].xyz;
            probesExtent = (data.ProbesCounts - 1) * (probesSpacing * 0.5f);
            biasedWorldPosition = worldPosition + GetDDGISurfaceBias(data, viewDir, probesSpacing, visibilityNormal, bias);
            float fallbackVisibilityCandidate;
            float fallbackCoverage;
            float3 fallbackCandidate = SampleDDGIIrradianceCascadeSelected(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, visibilityNormal, fallbackCascade, probesOrigin, probesExtent, probesSpacing, biasedWorldPosition, fallbackVisibilityCandidate, fallbackCoverage);
            if (fallbackCoverage > fallbackCoverageBest)
            {
                fallbackResult = fallbackCandidate;
                fallbackVisibility = fallbackVisibilityCandidate;
                fallbackCoverageBest = fallbackCoverage;
            }
            if (fallbackCoverage >= 0.999f)
                break;
        }
        if (fallbackCoverageBest > 0.0f)
        {
            float readyWeight = smoothstep(0.15f, 0.9f, probeCoverage);
            result = lerp(fallbackResult, result, readyWeight);
            volumeVisibility = lerp(max(fallbackVisibility, 0.0f), max(volumeVisibility, 0.0f), readyWeight);
            usedProbeFallback = readyWeight < 0.999f;
        }
    }

    // Blend with the next cascade (or fallback irradiance outside the volume)
#if DDGI_CASCADE_BLEND_SMOOTH && !defined(DDGI_DEBUG_CASCADE)
    cascadeIndex++;
    if (!usedProbeFallback && cascadeIndex < data.CascadesCount && cascadeWeight < 0.99f)
    {
        probesSpacing = data.ProbesOriginAndSpacing[cascadeIndex].w;
        probesOrigin = data.ProbesScrollOffsets[cascadeIndex].xyz * probesSpacing + data.ProbesOriginAndSpacing[cascadeIndex].xyz;
        probesExtent = (data.ProbesCounts - 1) * (probesSpacing * 0.5f);
        biasedWorldPosition = worldPosition + GetDDGISurfaceBias(data, viewDir, probesSpacing, visibilityNormal, bias);
        float volumeVisibilityNext;
        float probeCoverageNext;
        float3 resultNext = SampleDDGIIrradianceCascadeSelected(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, visibilityNormal, cascadeIndex, probesOrigin, probesExtent, probesSpacing, biasedWorldPosition, volumeVisibilityNext, probeCoverageNext);
        result *= cascadeWeight;
        result += resultNext * (1 - cascadeWeight);
        volumeVisibility = lerp(volumeVisibilityNext, volumeVisibility, cascadeWeight);
    }
#endif
    if (cascadeIndex >= data.CascadesCount)
    {
        // Blend between the last cascade and the fallback irradiance
        float fallbackWeight = (1 - cascadeWeight) * data.FallbackIrradiance.a;
        result = lerp(result, data.FallbackIrradiance.rgb, fallbackWeight);
        volumeVisibility = lerp(volumeVisibility, 1.0f, fallbackWeight);
    }

    volumeVisibility = max(volumeVisibility, 0.0f);
    return result;
}

// Default material/fog path. It preserves the public helper signature while
// using the shading normal as the conservative visibility normal when no
// screen-space geometric normal is available.
float3 SampleDDGIIrradiance(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, float bias = DDGI_DEFAULT_BIAS, float dither = 0.0f)
{
    float volumeVisibility;
    return SampleDDGIIrradianceInternal(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, worldNormal, bias, dither, volumeVisibility);
}

// Pixel materials can provide a reconstructed geometric normal for visibility
// while retaining the shading normal for irradiance direction lookup.
float3 SampleDDGIIrradianceWithVisibilityNormal(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 worldNormal, float3 visibilityNormal, float bias = DDGI_DEFAULT_BIAS, float dither = 0.0f)
{
    float volumeVisibility;
    return SampleDDGIIrradianceInternal(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, worldNormal, visibilityNormal, bias, dither, volumeVisibility);
}

// Participating media needs the probe visibility that surface interpolation
// intentionally normalizes away. The alpha component is the aggregate
// distance-moment visibility and can be combined with direct-light shadows.
float4 SampleDDGIVolumetricIrradiance(DDGIData data, Texture2D<snorm float4> probesData, Texture2D<uint> probeStates, Texture2D<float4> probesDistance, Texture2D<float4> probesIrradiance, float3 worldPosition, float3 scatteringDirection, float dither = 0.0f)
{
    float volumeVisibility;
    float3 irradiance = SampleDDGIIrradianceInternal(data, probesData, probeStates, probesDistance, probesIrradiance, worldPosition, scatteringDirection, scatteringDirection, 0.0f, dither, volumeVisibility);
    return float4(irradiance, volumeVisibility);
}
