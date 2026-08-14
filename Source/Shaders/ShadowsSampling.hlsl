// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __SHADOWS_SAMPLING__
#define __SHADOWS_SAMPLING__

#ifndef SHADOWS_CSM_BLENDING
#define SHADOWS_CSM_BLENDING 0
#endif
#ifndef SHADOWS_CSM_DITHERING
#define SHADOWS_CSM_DITHERING 0
#endif
#ifndef SHADOWS_EDGE_AA
#define SHADOWS_EDGE_AA 0
#endif

#include "./Flax/ShadowsCommon.hlsl"
#include "./Flax/GBufferCommon.hlsl"
#include "./Flax/LightingCommon.hlsl"
#include "./Flax/Random.hlsl"

#if FEATURE_LEVEL >= FEATURE_LEVEL_SM5 || defined(WGSL)
#define SAMPLE_SHADOW_MAP(shadowMap, shadowUV, sceneDepth) shadowMap.SampleCmpLevelZero(ShadowSamplerLinear, shadowUV, sceneDepth)
#define SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowUV, texelOffset, sceneDepth) shadowMap.SampleCmpLevelZero(ShadowSamplerLinear, shadowUV, sceneDepth, texelOffset)
#else
#define SAMPLE_SHADOW_MAP(shadowMap, shadowUV, sceneDepth) (sceneDepth < shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0).r)
#define SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowUV, texelOffset, sceneDepth) (sceneDepth < shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0, texelOffset).r)
#endif
#if defined(WGSL)
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) SAMPLE_RT_DEPTH(shadowMap, shadowUV)
#elif VULKAN || FEATURE_LEVEL < FEATURE_LEVEL_SM5
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) shadowMap.SampleLevel(SamplerPointClamp, shadowUV, 0).r
#else
#define LOAD_SHADOW_MAP(shadowMap, shadowUV) shadowMap.SampleLevel(SamplerLinearClamp, shadowUV, 0).r
#endif

float4 GetShadowMask(ShadowSample shadow)
{
    return float4(shadow.SurfaceShadow, shadow.TransmissionShadow, 1, 1);
}

// Gets the cube texture face index to use for shadow map sampling for the given view-to-light direction vector
// Where: direction = normalize(worldPosition - lightPosition)
uint GetCubeFaceIndex(float3 direction)
{
    uint cubeFaceIndex;
    float3 absDirection = abs(direction);
    float maxDirection = max(absDirection.x, max(absDirection.y, absDirection.z));
    if (maxDirection == absDirection.x)
        cubeFaceIndex = absDirection.x == direction.x ? 0 : 1;
    else if (maxDirection == absDirection.y)
        cubeFaceIndex = absDirection.y == direction.y ? 2 : 3;
    else
        cubeFaceIndex = absDirection.z == direction.z ? 4 : 5;
    return cubeFaceIndex;
}

float2 GetLightShadowAtlasUV(ShadowData shadow, ShadowTileData shadowTile, float3 samplePosition, out float4 shadowPosition)
{
    // Project into shadow space (WorldToShadow is pre-multiplied to convert Clip Space to UV Space)
    shadowPosition = mul(float4(samplePosition, 1.0f), shadowTile.WorldToShadow);
    shadowPosition.z -= shadow.Bias;
    shadowPosition.xyz /= shadowPosition.w;

    // UV Space -> Atlas Tile UV Space
    float2 shadowMapUV = saturate(shadowPosition.xy);
    shadowMapUV = shadowMapUV * shadowTile.ShadowToAtlas.xy + shadowTile.ShadowToAtlas.zw;
    return shadowMapUV;
}

float SampleShadowMap(Texture2D<float> shadowMap, float2 shadowMapUV, float sceneDepth)
{
    float result = SAMPLE_SHADOW_MAP(shadowMap, shadowMapUV, sceneDepth);
#if SHADOWS_QUALITY == 1
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 0), sceneDepth);
	result = result * (1.0f / 4.0);
#elif SHADOWS_QUALITY == 2 || SHADOWS_QUALITY == 3
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(-1, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(0, 1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, -1), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 0), sceneDepth);
	result += SAMPLE_SHADOW_MAP_OFFSET(shadowMap, shadowMapUV, int2(1, 1), sceneDepth);
	result = result * (1.0f / 9.0);
#endif
    return result;
}

float SampleShadowMapOptimizedPCFHelper(Texture2D<float> shadowMap, float2 baseUV, float u, float v, float2 shadowMapSizeInv, float sceneDepth)
{
    float2 uv = baseUV + float2(u, v) * shadowMapSizeInv;
    return SAMPLE_SHADOW_MAP(shadowMap, uv, sceneDepth);
}

// [Shadow map sampling method used in The Witness, https://github.com/TheRealMJP/Shadows]
float SampleShadowMapOptimizedPCF(Texture2D<float> shadowMap, float2 shadowMapUV, float sceneDepth)
{
#if SHADOWS_QUALITY != 0
    float2 shadowMapSize;
    shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);

    float2 uv = shadowMapUV.xy * shadowMapSize; // 1 unit - 1 texel
    float2 shadowMapSizeInv = 1.0f / shadowMapSize;

    float2 baseUV;
    baseUV.x = floor(uv.x + 0.5);
    baseUV.y = floor(uv.y + 0.5);
    float s = (uv.x + 0.5 - baseUV.x);
    float t = (uv.y + 0.5 - baseUV.y);
    baseUV -= float2(0.5, 0.5);
    baseUV *= shadowMapSizeInv;

    float sum = 0;
#endif
#if SHADOWS_QUALITY == 0
    return SAMPLE_SHADOW_MAP(shadowMap, shadowMapUV, sceneDepth);
#elif SHADOWS_QUALITY == 1
	float uw0 = (3 - 2 * s);
	float uw1 = (1 + 2 * s);

	float u0 = (2 - s) / uw0 - 1;
	float u1 = s / uw1 + 1;

	float vw0 = (3 - 2 * t);
	float vw1 = (1 + 2 * t);

	float v0 = (2 - t) / vw0 - 1;
	float v1 = t / vw1 + 1;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth);
	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth);

	return sum * 1.0f / 16;
#elif SHADOWS_QUALITY == 2
	float uw0 = (4 - 3 * s);
	float uw1 = 7;
	float uw2 = (1 + 3 * s);

	float u0 = (3 - 2 * s) / uw0 - 2;
	float u1 = (3 + s) / uw1;
	float u2 = s / uw2 + 2;

	float vw0 = (4 - 3 * t);
	float vw1 = 7;
	float vw2 = (1 + 3 * t);

	float v0 = (3 - 2 * t) / vw0 - 2;
	float v1 = (3 + t) / vw1;
	float v2 = t / vw2 + 2;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v0, shadowMapSizeInv, sceneDepth);

	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v1, shadowMapSizeInv, sceneDepth);

	sum += uw0 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v2, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v2, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v2, shadowMapSizeInv, sceneDepth);

	return sum * 1.0f / 144;
#elif SHADOWS_QUALITY == 3
	float uw0 = (5 * s - 6);
	float uw1 = (11 * s - 28);
	float uw2 = -(11 * s + 17);
	float uw3 = -(5 * s + 1);

	float u0 = (4 * s - 5) / uw0 - 3;
	float u1 = (4 * s - 16) / uw1 - 1;
	float u2 = -(7 * s + 5) / uw2 + 1;
	float u3 = -s / uw3 + 3;

	float vw0 = (5 * t - 6);
	float vw1 = (11 * t - 28);
	float vw2 = -(11 * t + 17);
	float vw3 = -(5 * t + 1);

	float v0 = (4 * t - 5) / vw0 - 3;
	float v1 = (4 * t - 16) / vw1 - 1;
	float v2 = -(7 * t + 5) / vw2 + 1;
	float v3 = -t / vw3 + 3;

	sum += uw0 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v0, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v0, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v0, shadowMapSizeInv, sceneDepth);
	sum += uw3 * vw0 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v0, shadowMapSizeInv, sceneDepth);

	sum += uw0 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v1, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v1, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v1, shadowMapSizeInv, sceneDepth);
	sum += uw3 * vw1 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v1, shadowMapSizeInv, sceneDepth);

	sum += uw0 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v2, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v2, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v2, shadowMapSizeInv, sceneDepth);
	sum += uw3 * vw2 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v2, shadowMapSizeInv, sceneDepth);

	sum += uw0 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u0, v3, shadowMapSizeInv, sceneDepth);
	sum += uw1 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u1, v3, shadowMapSizeInv, sceneDepth);
	sum += uw2 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u2, v3, shadowMapSizeInv, sceneDepth);
	sum += uw3 * vw3 * SampleShadowMapOptimizedPCFHelper(shadowMap, baseUV, u3, v3, shadowMapSizeInv, sceneDepth);

	return sum * (1.0f / 2704);
#else
    return 0.0f;
#endif
}

// Samples the shadow cascade for the given directional light on the material surface (supports subsurface shadowing)
ShadowSample SampleDirectionalLightShadowCascade(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, ShadowData shadow, float3 samplePosition, uint cascadeIndex)
{
    ShadowSample result;
    ShadowTileData shadowTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, cascadeIndex);

    // Project position into shadow atlas UV
    float4 shadowPosition;
    float2 shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, samplePosition, shadowPosition);

    // Sample the existing optimized PCF kernel.
    result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z);

    // Add an explicit filter to the selected detailed cascades. This block is enabled
    // only by the camera shadow-mask pixel shader (not volumetric or compute users).
#if SHADOWS_EDGE_AA
    float edgeAAStrength = saturate(GetDirectionalLightShadowEdgeAAStrength(light));
    float sampleRadius = max(GetDirectionalLightShadowEdgeAASampleRadius(light, cascadeIndex), 0.0f);
    uint edgeAACascadeCount = GetDirectionalLightShadowEdgeAACascadeCount(light);
    float2 receiverFootprintX = ddx(shadowMapUV);
    float2 receiverFootprintY = ddy(shadowMapUV);
    BRANCH
    if (cascadeIndex < edgeAACascadeCount && edgeAAStrength > 0.0f && sampleRadius > 0.0f)
    {
        float2 shadowMapSize;
        shadowMap.GetDimensions(shadowMapSize.x, shadowMapSize.y);
        float2 shadowTexelSize = 1.0f / shadowMapSize;
        float2 tileUVMin = shadowTile.ShadowToAtlas.zw + shadowTexelSize * 0.5f;
        float2 tileUVMax = shadowTile.ShadowToAtlas.zw + shadowTile.ShadowToAtlas.xy - shadowTexelSize * 0.5f;
        uint filterMode = GetDirectionalLightShadowEdgeAAFilterMode(light);
        float2 receiverBasisX = receiverFootprintX * shadowMapSize;
        float2 receiverBasisY = receiverFootprintY * shadowMapSize;
        float receiverBasisLengthX = length(receiverBasisX);
        float receiverBasisLengthY = length(receiverBasisY);
        receiverBasisX = receiverBasisLengthX > 1e-4f
            ? receiverBasisX * (clamp(receiverBasisLengthX, 1.0f, 4.0f) / receiverBasisLengthX)
            : float2(1.0f, 0.0f);
        receiverBasisY = receiverBasisLengthY > 1e-4f
            ? receiverBasisY * (clamp(receiverBasisLengthY, 1.0f, 4.0f) / receiverBasisLengthY)
            : float2(0.0f, 1.0f);

        // Fill the complete disk instead of sampling only its four extremes. A sparse
        // cross produces several visibly displaced copies of the shadow silhouette.
        const float2 EdgeAADisk[12] =
        {
            float2(0.204124f, 0.000000f),
            float2(-0.260699f, 0.238822f),
            float2(0.039904f, -0.454688f),
            float2(0.328595f, 0.428593f),
            float2(-0.603011f, -0.106664f),
            float2(0.571225f, -0.363367f),
            float2(-0.191064f, 0.710747f),
            float2(-0.364379f, -0.701590f),
            float2(0.790557f, 0.288710f),
            float2(-0.822442f, 0.339492f),
            float2(0.396472f, -0.847237f),
            float2(0.292982f, 0.934074f)
        };
        // Probe four well-separated outer-disk points first. Fully lit and fully
        // shadowed pixels keep the original result and skip the eight interior taps.
        float filteredShadow = result.SurfaceShadow;
        float probeMin = result.SurfaceShadow;
        float probeMax = result.SurfaceShadow;
        UNROLL
        for (uint sampleIndex = 8; sampleIndex < 12; sampleIndex++)
        {
            float2 diskSample = EdgeAADisk[sampleIndex] * sampleRadius;
            float2 fixedDiskOffset = diskSample * shadowTexelSize;
            float2 receiverFootprintOffset = (diskSample.x * receiverBasisX + diskSample.y * receiverBasisY) * shadowTexelSize;
            float2 sampleUV = shadowMapUV + (filterMode == 0U ? fixedDiskOffset : receiverFootprintOffset);
            float probeShadow = SAMPLE_SHADOW_MAP(shadowMap, clamp(sampleUV, tileUVMin, tileUVMax), shadowPosition.z);
            filteredShadow += probeShadow;
            probeMin = min(probeMin, probeShadow);
            probeMax = max(probeMax, probeShadow);
        }

        bool centerIsFilteredEdge = result.SurfaceShadow > (1.0f / 255.0f) && result.SurfaceShadow < (254.0f / 255.0f);
        bool diskCrossesEdge = probeMax - probeMin > (1.0f / 255.0f);
        BRANCH
        if (centerIsFilteredEdge || diskCrossesEdge)
        {
            UNROLL
            for (uint sampleIndex = 0; sampleIndex < 8; sampleIndex++)
            {
                float2 diskSample = EdgeAADisk[sampleIndex] * sampleRadius;
                float2 fixedDiskOffset = diskSample * shadowTexelSize;
                float2 receiverFootprintOffset = (diskSample.x * receiverBasisX + diskSample.y * receiverBasisY) * shadowTexelSize;
                float2 sampleUV = shadowMapUV + (filterMode == 0U ? fixedDiskOffset : receiverFootprintOffset);
                filteredShadow += SAMPLE_SHADOW_MAP(shadowMap, clamp(sampleUV, tileUVMin, tileUVMax), shadowPosition.z);
            }
            result.SurfaceShadow = lerp(result.SurfaceShadow, filteredShadow * (1.0f / 13.0f), edgeAAStrength);
        }
    }
#endif

    // Increase the sharpness for higher cascades to match the filter radius
    // Keep the far cascade soft; its job is to preserve broad silhouettes, not fine detail.
    const float SharpnessScale[MaxNumCascades] = { 1.0f, 1.5f, 3.0f, 3.5f, 1.0f };
    if (shadow.Sharpness >= 1.0f)
        shadow.Sharpness *= SharpnessScale[cascadeIndex];

    result.TransmissionShadow = 1;
#if defined(USE_GBUFFER_CUSTOM_DATA)
	// Subsurface shadowing
	BRANCH
	if (IsSubsurfaceMode(gBuffer.ShadingModel))
	{
		float opacity = gBuffer.CustomData.a;
        shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, gBuffer.WorldPos, shadowPosition);
		float shadowMapDepth = LOAD_SHADOW_MAP(shadowMap, shadowMapUV);
		result.TransmissionShadow = CalculateSubsurfaceOcclusion(opacity, shadowPosition.z, shadowMapDepth);
        result.TransmissionShadow = PostProcessShadow(shadow, result.TransmissionShadow);
	}
#endif

    result.SurfaceShadow = PostProcessShadow(shadow, result.SurfaceShadow);

    return result;
}

// Samples the shadow for the given directional light on the material surface (supports subsurface shadowing)
ShadowSample SampleDirectionalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, float dither = 0.0f)
{
#if !LIGHTING_NO_DIRECTIONAL
    // Skip if surface is in a full shadow
    float NoL = dot(gBuffer.Normal, light.Direction);
    BRANCH
    if (NoL <= 0
#if defined(USE_GBUFFER_CUSTOM_DATA)
        && !IsSubsurfaceMode(gBuffer.ShadingModel)
#endif
        )
        return (ShadowSample)0;
#endif

    ShadowSample result;
    result.SurfaceShadow = 1;
    result.TransmissionShadow = 1;
    
    // Load shadow data
    if (light.ShadowsBufferAddress == 0)
        return result; // No shadow assigned
    ShadowData shadow = LoadShadowsBuffer(shadowsBuffer, light.ShadowsBufferAddress);

    // Create an eased blend factor from fully shadowed to fully lit across the fade range.
    float viewDepth = gBuffer.ViewPos.z;
    float lastCascadeSplit = GetShadowCascadeSplit(shadow, shadow.TilesCount - 1);
    float fade = smoothstep(0.0f, 1.0f, saturate((viewDepth - lastCascadeSplit + shadow.FadeDistance) / shadow.FadeDistance));
    BRANCH
    if (fade >= 1.0)
        return result;

    // Figure out which cascade to sample from
    uint cascadeIndex = 0;
    for (uint i = 0; i < shadow.TilesCount - 1; i++)
    {
        if (viewDepth > GetShadowCascadeSplit(shadow, i))
            cascadeIndex = i + 1;
    }

    // Transition the detailed fourth cascade into the coarse far cascade. The far cascade projection
    // is expanded by the same distance on the CPU so either sampling mode is valid throughout this band.
    bool transitionFarCascade = false;
    bool fadeFarCascade = false;
    bool ditherFarCascade = false;
    float farCascadeBlend = 0.0f;
    if (shadow.TilesCount == MaxNumCascades && cascadeIndex == MaxNumCascades - 2)
    {
        float distanceToFarCascade = GetShadowCascadeSplit(shadow, cascadeIndex) - viewDepth;
        transitionFarCascade = distanceToFarCascade <= shadow.FarCascadeTransitionDistance;
        fadeFarCascade = transitionFarCascade && shadow.FarCascadeTransitionMode == FAR_SHADOW_TRANSITION_FADE;
        ditherFarCascade = transitionFarCascade && shadow.FarCascadeTransitionMode == FAR_SHADOW_TRANSITION_DITHER;
        farCascadeBlend = saturate(1.0f - distanceToFarCascade / max(shadow.FarCascadeTransitionDistance, 0.0001f));
    }
#if SHADOWS_CSM_DITHERING || SHADOWS_CSM_BLENDING
	float nextSplit = GetShadowCascadeSplit(shadow, cascadeIndex);
	float splitSize = cascadeIndex == 0 ? nextSplit : max(nextSplit - GetShadowCascadeSplit(shadow, cascadeIndex - 1), 0.0001f);
	float splitDist = (nextSplit - viewDepth) / splitSize;
#endif
#if SHADOWS_CSM_DITHERING && !SHADOWS_CSM_BLENDING
	const float BlendThreshold = 0.05f;
    if (!transitionFarCascade && splitDist <= BlendThreshold && cascadeIndex != shadow.TilesCount - 1)
    {
        // Dither with the next cascade but with screen-space dithering (gets cleaned out by TAA)
        float lerpAmount = 1 - splitDist / BlendThreshold;
        if (step(RandN2(gBuffer.ViewPos.xy + dither).x, lerpAmount))
            cascadeIndex++;
    }
#endif

    if (ditherFarCascade && step(RandN2(gBuffer.ViewPos.xy + dither).x, farCascadeBlend))
        cascadeIndex++;

    // Sample cascade
    float3 samplePosition = gBuffer.WorldPos;
#if !LIGHTING_NO_DIRECTIONAL
    // Apply normal offset bias
    samplePosition += GetShadowPositionOffset(shadow.NormalOffsetScale, NoL, gBuffer.Normal);
#endif
    result = SampleDirectionalLightShadowCascade(light, shadowsBuffer, shadowMap, gBuffer, shadow, samplePosition, cascadeIndex);

    BRANCH
    if (fadeFarCascade)
    {
        ShadowSample farResult = SampleDirectionalLightShadowCascade(light, shadowsBuffer, shadowMap, gBuffer, shadow, samplePosition, cascadeIndex + 1);
        result.SurfaceShadow = lerp(result.SurfaceShadow, farResult.SurfaceShadow, farCascadeBlend);
        result.TransmissionShadow = lerp(result.TransmissionShadow, farResult.TransmissionShadow, farCascadeBlend);
    }

#if SHADOWS_CSM_BLENDING
	const float BlendThreshold = 0.1f;
    if (!transitionFarCascade && splitDist <= BlendThreshold && cascadeIndex != shadow.TilesCount - 1)
    {
	    // Sample the next cascade, and blend between the two results to smooth the transition
        ShadowSample nextResult = SampleDirectionalLightShadowCascade(light, shadowsBuffer, shadowMap, gBuffer, shadow, samplePosition, cascadeIndex + 1);
		float blendAmount = splitDist / BlendThreshold;
		result.SurfaceShadow = lerp(nextResult.SurfaceShadow, result.SurfaceShadow, blendAmount);
		result.TransmissionShadow = lerp(nextResult.TransmissionShadow, result.TransmissionShadow, blendAmount);
    }
#endif

    // Fade the last cascade into unshadowed lighting to hide the end of the shadow range.
    result.SurfaceShadow = lerp(result.SurfaceShadow, 1.0f, fade);
    result.TransmissionShadow = lerp(result.TransmissionShadow, 1.0f, fade);

    return result;
}

// Samples the shadow for the given local light on the material surface (supports subsurface shadowing)
ShadowSample SampleLocalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer, float3 L, float toLightLength, uint tileIndex)
{
#if !LIGHTING_NO_DIRECTIONAL
    // Skip if surface is in a full shadow
    float NoL = dot(gBuffer.Normal, L);
    BRANCH
    if (NoL <= 0
#if defined(USE_GBUFFER_CUSTOM_DATA)
        && !IsSubsurfaceMode(gBuffer.ShadingModel)
#endif
        )
        return (ShadowSample)0;
#endif

    ShadowSample result;
    result.SurfaceShadow = 1;
    result.TransmissionShadow = 1;

    // Skip pixels outside of the light influence
    BRANCH
    if (toLightLength > light.Radius)
        return result;

    // Load shadow data
    if (light.ShadowsBufferAddress == 0)
        return result; // No shadow assigned
    ShadowData shadow = LoadShadowsBuffer(shadowsBuffer, light.ShadowsBufferAddress);
    ShadowTileData shadowTile = LoadShadowsBufferTile(shadowsBuffer, light.ShadowsBufferAddress, tileIndex);

    float3 samplePosition = gBuffer.WorldPos;
#if !LIGHTING_NO_DIRECTIONAL
    // Apply normal offset bias
    samplePosition += GetShadowPositionOffset(shadow.NormalOffsetScale, NoL, gBuffer.Normal);
#endif

    // Project position into shadow atlas UV
    float4 shadowPosition;
    float2 shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, samplePosition, shadowPosition);

    // Sample shadow map
    result.SurfaceShadow = SampleShadowMapOptimizedPCF(shadowMap, shadowMapUV, shadowPosition.z);

#if defined(USE_GBUFFER_CUSTOM_DATA)
	// Subsurface shadowing
	BRANCH
	if (IsSubsurfaceMode(gBuffer.ShadingModel))
	{
		float opacity = gBuffer.CustomData.a;
        shadowMapUV = GetLightShadowAtlasUV(shadow, shadowTile, gBuffer.WorldPos, shadowPosition);
		float shadowMapDepth = LOAD_SHADOW_MAP(shadowMap, shadowMapUV);
		result.TransmissionShadow = CalculateSubsurfaceOcclusion(opacity, shadowPosition.z, shadowMapDepth);
        result.TransmissionShadow = PostProcessShadow(shadow, result.TransmissionShadow);
	}
#endif

    result.SurfaceShadow = PostProcessShadow(shadow, result.SurfaceShadow);
    return result;
}

// Samples the shadow for the given spot light on the material surface (supports subsurface shadowing)
ShadowSample SampleSpotLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer)
{
    float3 toLight = light.Position - gBuffer.WorldPos;
    float toLightLength = length(toLight);
    float3 L = toLight / toLightLength;
    uint tileIndex = 0;
    if (light.ShadowsBufferAddress != 0)
    {
        // Very wide spotlights use the numerically stable cube-face projection
        // path. The spotlight attenuation still clips lighting to its cone.
        ShadowData shadow = LoadShadowsBuffer(shadowsBuffer, light.ShadowsBufferAddress);
        if (shadow.TilesCount == 6)
            tileIndex = GetCubeFaceIndex(-L);
    }
    return SampleLocalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, L, toLightLength, tileIndex);
}

// Samples the shadow for the given point light on the material surface (supports subsurface shadowing)
ShadowSample SamplePointLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, GBufferSample gBuffer)
{
    float3 toLight = light.Position - gBuffer.WorldPos;
    float toLightLength = length(toLight);
    float3 L = toLight / toLightLength;

    // Figure out which cube face we're sampling from
    uint cubeFaceIndex = GetCubeFaceIndex(-L);

    return SampleLocalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, L, toLightLength, cubeFaceIndex);
}

GBufferSample GetDummyGBufferSample(float3 worldPosition)
{
    GBufferSample gBuffer = (GBufferSample)0;
    gBuffer.ShadingModel = SHADING_MODEL_LIT;
    gBuffer.WorldPos = worldPosition;
    return gBuffer;
}

// Samples the shadow for the given directional light at custom location
ShadowSample SampleDirectionalLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition, float viewDepth, float dither = 0.0f)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    gBuffer.ViewPos.z = viewDepth;
    return SampleDirectionalLightShadow(light, shadowsBuffer, shadowMap, gBuffer, dither);
}

// Samples the shadow for the given spot light at custom location
ShadowSample SampleSpotLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    return SampleSpotLightShadow(light, shadowsBuffer, shadowMap, gBuffer);
}

// Samples the shadow for the given point light at custom location
ShadowSample SamplePointLightShadow(LightData light, Buffer<float4> shadowsBuffer, Texture2D<float> shadowMap, float3 worldPosition)
{
    GBufferSample gBuffer = GetDummyGBufferSample(worldPosition);
    return SamplePointLightShadow(light, shadowsBuffer, shadowMap, gBuffer);
}

#endif
