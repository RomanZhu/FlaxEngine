// Copyright (c) Wojciech Figat. All rights reserved.

// Implementation based on:
// "Volumetric fog: Unified, compute shader based solution to atmospheric scattering" - Bart Wronski at Siggraph 2014
// and
// "Physically Based and Unified Volumetric Rendering in Frostbite" - Sebastien Hillaire at Siggraph 2015

#define NO_GBUFFER_SAMPLING
#define LIGHTING_NO_DIRECTIONAL 1
#define LIGHTING_NO_SPECULAR 0
#define SHADOWS_QUALITY 1

// Debug voxels world space positions
#define DEBUG_VOXEL_WS_POS 0

// Debug voxels so CS_FinalIntegration will just copy data without modifications
#define DEBUG_VOXELS 0
#define VOLUMETRIC_FOG_DEBUG_NONE 0
#define VOLUMETRIC_FOG_DEBUG_DENSITY 1
#define VOLUMETRIC_FOG_DEBUG_EXTINCTION 2
#define VOLUMETRIC_FOG_DEBUG_SCATTERING 3
#define VOLUMETRIC_FOG_DEBUG_HISTORY_WEIGHT 4
#define VOLUMETRIC_FOG_DEBUG_FROXEL_GRID 5

#include "./Flax/Common.hlsl"
#include "./Flax/Math.hlsl"
#include "./Flax/LightingCommon.hlsl"
#include "./Flax/ShadowsSampling.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/VolumetricFog.hlsl"
#include "./Flax/GI/DDGI.hlsl"

struct SkyLightData
{
	float3 MultiplyColor;
	float VolumetricScatteringIntensity;
	float3 AdditiveColor;
	float Dummy0;	
};

META_CB_BEGIN(0, Data)
GBufferData GBuffer;

float3 GlobalAlbedo;
float GlobalExtinctionScale;

float3 GlobalEmissive;
float HistoryWeight;

float3 GridSize;
uint MissedHistorySamplesCount;

uint3 GridSizeInt;
float PhaseG;

float2 VolumetricFogRange;
float VolumetricFogDistanceFade;
float InverseSquaredLightDistanceBiasScale;

float4 FogParameters;
float4 FogLayer2Parameters;
float4 GridSliceParameters;

float4 DensityNoiseParameters0;
float4 DensityNoiseParameters1;
float4 DensityNoiseOffsets[4];
float4 DensityNoiseParameters2;
float4 ScatteringParameters;
float PhaseDirectionality;
float3 PhasePadding;
float4 NearClarityParameters;
float4 ShadowParameters0;
float4 ShadowParameters1;
float4 TemporalParameters0;
float4 TemporalParameters1;
float4 DebugParameters;
float4 PreviousOriginDelta;

float4x4 PrevWorldToClip;

float4 FrameJitterOffsets[8];

LightData DirectionalLight;
SkyLightData SkyLight;
DDGIData DDGI;
META_CB_END

META_CB_BEGIN(2, PerLight)
float2 SliceToDepth;
int MinZ;
float LocalLightScatteringIntensity;

float4 ViewSpaceBoundingSphere;
float4x4 ViewToVolumeClip;

LightData LocalLight;
META_CB_END

// The Henyey-Greenstein phase function
// [Henyey and Greenstein 1941, https://www.astro.umd.edu/~jph/HG_note.pdf]
float HenyeyGreensteinPhase(float g, float cosTheta)
{
	return (1 - g * g) / (4 * PI * pow(1 + g * g + 2 * g * cosTheta, 1.5f));
}

float GetPhase(float g, float cosTheta)
{
	float forwardWeight = ScatteringParameters.y;
	float backwardWeight = ScatteringParameters.w;
	float weightSum = forwardWeight + backwardWeight;
	if (weightSum < 0.0001f)
		return HenyeyGreensteinPhase(0.0f, cosTheta);
	float phase = HenyeyGreensteinPhase(g, cosTheta) * forwardWeight;
	BRANCH
	if (backwardWeight > 0.0001f)
		phase += HenyeyGreensteinPhase(ScatteringParameters.z, cosTheta) * backwardWeight;
	return phase / weightSum;
}

float3 GetCellPositionWS(uint3 gridCoordinate, float3 cellOffset, out float sceneDepth)
{
	float2 volumeUV = (gridCoordinate.xy + cellOffset.xy) / GridSize.xy;
	sceneDepth = GetDepthFromSlice(GridSliceParameters, gridCoordinate.z + cellOffset.z) / GBuffer.ViewFar;
	float deviceDepth = LinearZ2DeviceDepth(GBuffer, sceneDepth);
	return GetWorldPos(GBuffer, volumeUV, deviceDepth);
}

float3 GetCellPositionWS(uint3 gridCoordinate, float3 cellOffset)
{
	float sceneDepth;
	return GetCellPositionWS(gridCoordinate, cellOffset, sceneDepth);
}

float3 GetVolumeUV(float3 worldPosition, float4x4 worldToClip)
{
	float4 ndcPosition = mul(float4(worldPosition, 1), worldToClip);
	ndcPosition.xy /= ndcPosition.w;
    ndcPosition.w = (ndcPosition.w - VolumetricFogRange.x) / VolumetricFogRange.y; // TODO: convert into MAD
	return float3(ndcPosition.xy * float2(0.5f, -0.5f) + 0.5f, ndcPosition.w);
}

// Vertex shader that writes to a range of slices of a volume texture
META_VS(true, FEATURE_LEVEL_SM5)
META_FLAG(VertexToGeometryShader)
Quad_VS2GS VS_WriteToSlice(float2 TexCoord : TEXCOORD0, uint LayerIndex : SV_InstanceID)
{
	Quad_VS2GS output;

	uint slice = LayerIndex + MinZ;
	float depth = (slice / SliceToDepth.x) * SliceToDepth.y;
	float depthOffset = abs(depth - ViewSpaceBoundingSphere.z);

	float radius = sqrt(ViewSpaceBoundingSphere.w * ViewSpaceBoundingSphere.w - depthOffset * depthOffset);
	float3 positionVS = float3(ViewSpaceBoundingSphere.xy + (TexCoord * 2 - 1) * radius, depth);
	output.Vertex.Position = mul(float4(positionVS, 1), ViewToVolumeClip);
#if VULKAN
    output.Vertex.Position.y *= -1;
#endif

	output.Vertex.TexCoord = TexCoord;
	output.LayerIndex = slice;

	return output;
}

// Geometry shader that writes to a range of slices of a volume texture
META_GS(true, FEATURE_LEVEL_SM5)
[maxvertexcount(3)]
void GS_WriteToSlice(triangle Quad_VS2GS input[3], inout TriangleStream<Quad_GS2PS> stream)
{
	Quad_GS2PS vertex;

	vertex.Vertex = input[0].Vertex;
	vertex.LayerIndex = input[0].LayerIndex;
	stream.Append(vertex);

	vertex.Vertex = input[1].Vertex;
	vertex.LayerIndex = input[1].LayerIndex;
	stream.Append(vertex);

	vertex.Vertex = input[2].Vertex;
	vertex.LayerIndex = input[2].LayerIndex;
	stream.Append(vertex);
}

#if USE_SHADOW
Texture2D<float> ShadowMap : register(t0);
Buffer<float4> ShadowsBuffer : register(t1);
#endif

META_PS(true, FEATURE_LEVEL_SM5)
META_PERMUTATION_1(USE_SHADOW=0)
META_PERMUTATION_1(USE_SHADOW=1)
float4 PS_InjectLight(Quad_GS2PS input) : SV_Target0
{
	uint3 gridCoordinate = uint3(input.Vertex.Position.xy, input.LayerIndex);
	if (any(gridCoordinate >= GridSizeInt))
		return 0;
        
    // Supersample if history buffer is outside the view
	float3 historyUV = GetVolumeUV(GetCellPositionWS(gridCoordinate, 0.5f) + PreviousOriginDelta.xyz, PrevWorldToClip);
	float historyAlpha = HistoryWeight;
	FLATTEN
	if (any(historyUV < 0) || any(historyUV > 1))
		historyAlpha = 0;
	uint samplesCount = historyAlpha < 0.01f ? MissedHistorySamplesCount : 1;

	float NoL = 0;
	bool isSpotLight = LocalLight.SpotAngles.x > -2.0f;
	float4 scattering = 0;
	for (uint sampleIndex = 0; sampleIndex < samplesCount; sampleIndex++)
	{
		float3 cellOffset = FrameJitterOffsets[sampleIndex].xyz;
		float3 positionWS = GetCellPositionWS(gridCoordinate, cellOffset);
		float3 cameraVector = normalize(positionWS - GBuffer.ViewPos);
		float cellRadius = length(positionWS - GetCellPositionWS(gridCoordinate + uint3(1, 1, 1), cellOffset));
		float distanceBias = max(cellRadius * InverseSquaredLightDistanceBiasScale, 1);
		float3 toLight = LocalLight.Position - positionWS;
		float distanceSqr = dot(toLight, toLight);
		float3 L = toLight * rsqrt(distanceSqr);

		// Calculate the light attenuation
		float attenuation = 1;
		GetRadialLightAttenuation(LocalLight, isSpotLight, float3(0, 0, 1), distanceSqr, distanceBias * distanceBias, toLight, L, NoL, attenuation);

		// Peek the shadow
		float shadow = 1.0f;
#if USE_SHADOW
		if (attenuation > 0)
		{
            if (isSpotLight)
                shadow = SampleSpotLightShadow(LocalLight, ShadowsBuffer, ShadowMap, positionWS).SurfaceShadow;
	        else
                shadow = SamplePointLightShadow(LocalLight, ShadowsBuffer, ShadowMap, positionWS).SurfaceShadow;
		}
#endif

		scattering.rgb += LocalLight.Color * (GetPhase(PhaseG, dot(L, -cameraVector)) * attenuation * shadow * LocalLightScatteringIntensity);
	}

	scattering.rgb /= (float)samplesCount;
	return scattering;
}

#define DENSITY_NOISE_TEXTURE_SIZE 64
#define DENSITY_NOISE_LATTICE_SIZE 8

#if defined(_CS_GenerateDensityNoise)

RWTexture3D<float> RWDensityNoiseTexture : register(u0);

float HashDensityNoise(uint3 cell)
{
	uint hash = cell.x * 374761393u + cell.y * 668265263u + cell.z * 2246822519u;
	hash = (hash ^ (hash >> 13)) * 1274126177u;
	hash ^= hash >> 16;
	return (hash & 0x00ffffffu) * (1.0f / 16777215.0f);
}

float3 GetDensityNoiseGradient(uint3 cell)
{
	float3 gradient = float3(
		HashDensityNoise(cell),
		HashDensityNoise(cell + uint3(19u, 47u, 101u)),
		HashDensityNoise(cell + uint3(73u, 29u, 53u))) * 2.0f - 1.0f;
	return normalize(gradient + 0.0001f);
}

float PeriodicDensityNoise(float3 position)
{
	uint3 cell0 = (uint3)floor(position) % DENSITY_NOISE_LATTICE_SIZE;
	uint3 cell1 = (cell0 + 1u) % DENSITY_NOISE_LATTICE_SIZE;
	float3 localPosition = frac(position);
	float3 weight = localPosition;
	weight = weight * weight * (3.0f - 2.0f * weight);

	float n000 = dot(GetDensityNoiseGradient(uint3(cell0.x, cell0.y, cell0.z)), localPosition - float3(0, 0, 0));
	float n100 = dot(GetDensityNoiseGradient(uint3(cell1.x, cell0.y, cell0.z)), localPosition - float3(1, 0, 0));
	float n010 = dot(GetDensityNoiseGradient(uint3(cell0.x, cell1.y, cell0.z)), localPosition - float3(0, 1, 0));
	float n110 = dot(GetDensityNoiseGradient(uint3(cell1.x, cell1.y, cell0.z)), localPosition - float3(1, 1, 0));
	float n001 = dot(GetDensityNoiseGradient(uint3(cell0.x, cell0.y, cell1.z)), localPosition - float3(0, 0, 1));
	float n101 = dot(GetDensityNoiseGradient(uint3(cell1.x, cell0.y, cell1.z)), localPosition - float3(1, 0, 1));
	float n011 = dot(GetDensityNoiseGradient(uint3(cell0.x, cell1.y, cell1.z)), localPosition - float3(0, 1, 1));
	float n111 = dot(GetDensityNoiseGradient(uint3(cell1.x, cell1.y, cell1.z)), localPosition - float3(1, 1, 1));

	float n00 = lerp(n000, n100, weight.x);
	float n10 = lerp(n010, n110, weight.x);
	float n01 = lerp(n001, n101, weight.x);
	float n11 = lerp(n011, n111, weight.x);
	return saturate(lerp(lerp(n00, n10, weight.y), lerp(n01, n11, weight.y), weight.z) * 0.85f + 0.5f);
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(4, 4, 4)]
void CS_GenerateDensityNoise(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	if (any(DispatchThreadId >= DENSITY_NOISE_TEXTURE_SIZE))
		return;
	float3 position = (DispatchThreadId + 0.5f) * ((float)DENSITY_NOISE_LATTICE_SIZE / (float)DENSITY_NOISE_TEXTURE_SIZE);
	RWDensityNoiseTexture[DispatchThreadId] = PeriodicDensityNoise(position);
}

#elif defined(_CS_Initialize)

RWTexture3D<float4> RWVBufferA : register(u0);
RWTexture3D<float4> RWVBufferB : register(u1);
Texture3D<float> DensityNoiseTexture : register(t0);

float GetDensityNoise(float3 positionWS, float froxelSize)
{
	float3 uvw = positionWS * DensityNoiseParameters0.x;
	float frequency = 1.0f;
	float amplitude = 1.0f;
	float value = 0.0f;
	float weight = 0.0f;
	int octaveCount = (int)DensityNoiseParameters1.z;
	[loop]
	for (int octave = 0; octave < 4; octave++)
	{
		if (octave >= octaveCount)
			break;
		// Fade detail before it becomes smaller than a froxel. This keeps distant fog stable without relying on temporal AA to hide aliasing.
		float featureSize = rcp(DensityNoiseParameters0.x * frequency * DENSITY_NOISE_LATTICE_SIZE);
		float octaveWeight = amplitude * saturate(featureSize / max(froxelSize * 2.0f, 0.0001f));
		value += DensityNoiseTexture.SampleLevel(SamplerLinearWrap, uvw + DensityNoiseOffsets[octave].xyz, 0) * octaveWeight;
		weight += octaveWeight;
		if (octave + 1 < octaveCount)
		{
			frequency *= DensityNoiseParameters0.y;
			amplitude *= DensityNoiseParameters0.z;
			if (DensityNoiseOffsets[0].w > 0.5f)
			{
				// Orthonormal rotation plus an irrational-looking offset decorrelates the existing periodic field between octaves.
				const float3x3 octaveRotation = float3x3(
					0.00f, 0.80f, 0.60f,
					-0.80f, 0.36f, -0.48f,
					-0.60f, -0.48f, 0.64f);
				uvw = mul(octaveRotation, uvw) * DensityNoiseParameters0.y;
			}
			else
			{
				uvw *= DensityNoiseParameters0.y;
			}
		}
	}
	if (weight < 0.0001f)
		return 1.0f;
	value /= weight;
	value = saturate((value - DensityNoiseParameters1.x) / max(DensityNoiseParameters1.y - DensityNoiseParameters1.x, 0.0001f));
	if (DensityNoiseParameters2.y > 0.5f)
		value = 1.0f - value;
	return pow(value, DensityNoiseParameters2.x);
}

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(4, 4, 4)]
void CS_Initialize(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	uint3 gridCoordinate = DispatchThreadId;
	if (any(gridCoordinate >= GridSizeInt))
		return;
	float3 positionWS = GetCellPositionWS(gridCoordinate, 0.5f);

	// Unpack the fog parameters (packing done in C++ ExponentialHeightFog::GetVolumetricFogOptions)
	float fogDensity = FogParameters.x;
	float fogHeight = FogParameters.y;
	float fogHeightFalloff = FogParameters.z;

	// Calculate the global fog density that matches the exponential height fog density.
	float layer1Density = fogDensity * exp2(clamp(-fogHeightFalloff * (positionWS.y - fogHeight), -125.0f, 126.0f));
	float layer2Density = 0.0f;
	BRANCH
	if (FogLayer2Parameters.x > 0.0f)
		layer2Density = FogLayer2Parameters.x * exp2(clamp(-FogLayer2Parameters.z * (positionWS.y - FogLayer2Parameters.y), -125.0f, 126.0f));
	if (DensityNoiseParameters1.w > 0.5f)
	{
		uint3 neighborCoordinate = min(gridCoordinate + 1u, GridSizeInt - 1u);
		if (all(neighborCoordinate == gridCoordinate))
			neighborCoordinate = gridCoordinate - 1u;
		float froxelSize = length(GetCellPositionWS(neighborCoordinate, 0.5f) - positionWS);
		float densityNoise = GetDensityNoise(positionWS, froxelSize);
		float heightInfluence = lerp(
			DensityNoiseParameters2.w,
			1.0f,
			exp2(-DensityNoiseParameters2.z * max(positionWS.y - fogHeight, 0.0f)));
		layer1Density *= lerp(1.0f, densityNoise, DensityNoiseParameters0.w * heightInfluence);
		layer2Density *= lerp(1.0f, densityNoise, FogLayer2Parameters.w);
	}
	float globalDensity = layer1Density + layer2Density;
	if (NearClarityParameters.x > 0.5f)
	{
		float distanceToCamera = distance(positionWS, GBuffer.ViewPos);
		float clarity = smoothstep(NearClarityParameters.y, NearClarityParameters.y + NearClarityParameters.z, distanceToCamera);
		globalDensity *= lerp(NearClarityParameters.w, 1.0f, clarity);
	}
	float extinction = max(0.0f, globalDensity * GlobalExtinctionScale * 0.24f);

	float3 scattering = GlobalAlbedo * extinction;
	float absorption = max(0.0f, extinction - Luminance(scattering));

	RWVBufferA[gridCoordinate] = float4(scattering, absorption);
	RWVBufferB[gridCoordinate] = float4(GlobalEmissive, 0);
}

#elif defined(_CS_LightScattering)

RWTexture3D<float4> RWLightScattering : register(u0);

Texture3D<float4> VBufferA : register(t0);
Texture3D<float4> VBufferB : register(t1);
Texture3D<float4> LightScatteringHistory : register(t2);
Texture3D<float4> LocalShadowedLightScattering : register(t3);
Texture2D<float> ShadowMap : register(t4);
Buffer<float4> ShadowsBuffer : register(t5);
#if USE_DDGI
Texture2D<snorm float4> ProbesData : register(t6);
Texture2D<uint> ProbeStates : register(t7);
Texture2D<float4> ProbesDistance : register(t8);
Texture2D<float4> ProbesIrradiance : register(t9);
#else
TextureCube SkyLightImage : register(t6);
#endif

float GetMaterialExtinction(int3 coordinate)
{
	int3 maxCoordinate = int3(GridSizeInt) - 1;
	uint3 clampedCoordinate = (uint3)clamp(coordinate, int3(0, 0, 0), maxCoordinate);
	float4 material = VBufferA[clampedCoordinate];
	return material.w + Luminance(material.xyz);
}

META_CS(true, FEATURE_LEVEL_SM5)
META_PERMUTATION_1(USE_DDGI=0)
META_PERMUTATION_1(USE_DDGI=1)
[numthreads(4, 4, 4)]
void CS_LightScattering(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	uint3 gridCoordinate = DispatchThreadId;
	if (any(gridCoordinate >= GridSizeInt))
		return;
        
    // Supersample if history buffer is outside the view
	float3 historyUV = GetVolumeUV(GetCellPositionWS(gridCoordinate, 0.5f) + PreviousOriginDelta.xyz, PrevWorldToClip);
	float historyAlpha = HistoryWeight;
	FLATTEN
	if (any(historyUV < 0) || any(historyUV > 1))
		historyAlpha = 0;
	// Most debug modes display current froxel data. History Weight executes the temporal
	// rejection logic and stores the resulting local weight for final visualization.
	uint debugMode = (uint)DebugParameters.x;
	if (debugMode != VOLUMETRIC_FOG_DEBUG_NONE && debugMode != VOLUMETRIC_FOG_DEBUG_HISTORY_WEIGHT)
		historyAlpha = 0;
	uint samplesCount = historyAlpha < 0.01f ? MissedHistorySamplesCount : 1;
    
	float3 lightScattering = 0;
#if USE_DDGI
	float3 ddgiScattering = 0;
#endif
	float directionalShadowVisibility = 0.0f;
	for (uint sampleIndex = 0; sampleIndex < samplesCount; sampleIndex++)
	{
		float3 cellOffset = FrameJitterOffsets[sampleIndex].xyz;
		float3 positionWS = GetCellPositionWS(gridCoordinate, cellOffset);
		float3 cameraVector = positionWS - GBuffer.ViewPos;
		float cameraVectorLength = length(cameraVector);
		float3 cameraVectorNormalized = cameraVector / cameraVectorLength;

		// Directional light
        {
            float shadow = SampleDirectionalLightShadow(DirectionalLight, ShadowsBuffer, ShadowMap, positionWS, cameraVectorLength).SurfaceShadow;
			shadow = pow(saturate(shadow), ShadowParameters0.x);
			shadow = lerp(1.0f, shadow, ShadowParameters1.x);
			directionalShadowVisibility += shadow;
			float directionalVisibility = max(shadow, ShadowParameters0.w);
			float cosTheta = dot(DirectionalLight.Direction, -cameraVectorNormalized);
			float directionalPhase = GetPhase(PhaseG, cosTheta);
			float isotropicPhase = 1.0f / (4.0f * PI);
			float phase = lerp(isotropicPhase, directionalPhase, PhaseDirectionality);
			lightScattering += DirectionalLight.Color * (8 * directionalVisibility * phase);
		}

#if USE_DDGI
        // Dynamic Diffuse Global Illumination. Keep this term separate from
        // direct and local lighting so volumetric shadows can suppress probe
        // fill in enclosed regions without incorrectly shadowing local lights.
        ddgiScattering += SampleDDGIIrradiance(DDGI, ProbesData, ProbeStates, ProbesDistance, ProbesIrradiance, positionWS, cameraVectorNormalized, 0.0f, cellOffset.x);
#else
		// Sky light
		if (SkyLight.VolumetricScatteringIntensity > 0)
		{
			float3 skyLighting = SkyLightImage.SampleLevel(SamplerLinearClamp, float3(0, 0, 0), 10000).rgb;
			skyLighting = skyLighting * SkyLight.MultiplyColor + SkyLight.AdditiveColor;
			lightScattering += skyLighting * SkyLight.VolumetricScatteringIntensity;
		}
#endif
	}
	lightScattering /= (float)samplesCount;
	directionalShadowVisibility = saturate(directionalShadowVisibility / (float)samplesCount);

#if USE_DDGI
	ddgiScattering /= (float)samplesCount;
	// Shadow presentation used to be a no-op at its default scattering
	// multiplier of one. Couple DDGI fog to the sampled directional visibility
	// when the feature is enabled, while preserving the user-selected ambient
	// floor for genuinely indirect illumination in shadow.
	float ddgiShadowVisibility = max(directionalShadowVisibility, ShadowParameters0.w);
	ddgiShadowVisibility = lerp(1.0f, ddgiShadowVisibility, ShadowParameters1.y);
	lightScattering += ddgiScattering * (DDGI.Algorithm != 0 ? ddgiShadowVisibility : 1.0f);
#endif

	// Apply scattering from the point and spot lights
	lightScattering += LocalShadowedLightScattering[gridCoordinate].rgb;
	lightScattering *= lerp(ShadowParameters0.z, 1.0f, directionalShadowVisibility);

	float4 materialScatteringAndAbsorption = VBufferA[gridCoordinate];
	float extinction = materialScatteringAndAbsorption.w + Luminance(materialScatteringAndAbsorption.xyz);
	extinction *= lerp(ShadowParameters0.y, 1.0f, directionalShadowVisibility);
	float3 materialEmissive = VBufferB[gridCoordinate].xyz;
	float4 scatteringAndExtinction = float4(lightScattering * materialScatteringAndAbsorption.xyz * ScatteringParameters.x + materialEmissive, extinction);

	BRANCH
	if (historyAlpha > 0)
	{
		float4 historyScatteringAndExtinction = LightScatteringHistory.SampleLevel(SamplerLinearClamp, historyUV, 0);
		BRANCH
		if (TemporalParameters1.x > 0.5f)
		{
			float stability = 1.0f;

			// Reject history when current and reprojected medium extinction disagree. The
			// relative comparison behaves consistently across thin and dense fog presets.
			float extinctionDifference = abs(historyScatteringAndExtinction.w - scatteringAndExtinction.w);
			float extinctionReference = max(max(historyScatteringAndExtinction.w, scatteringAndExtinction.w), 0.000001f);
			float relativeExtinctionDifference = extinctionDifference / extinctionReference;
			float extinctionStability = 1.0f - saturate(
				(relativeExtinctionDifference - TemporalParameters0.x) /
				max(1.0f - TemporalParameters0.x, 0.0001f));
			stability = min(stability, extinctionStability);

			// Reprojection displacement captures camera rotation, translation, and close-range
			// disocclusion without relying on scene TAA motion vectors.
			float2 currentVolumeUV = (gridCoordinate.xy + 0.5f) / GridSize.xy;
			float reprojectionMotion = length(historyUV.xy - currentVolumeUV);
			float cameraMotionStability = exp2(-reprojectionMotion * TemporalParameters0.z * 16.0f);
			stability = min(stability, cameraMotionStability);

			BRANCH
			if (TemporalParameters0.y > 0.0001f)
			{
				// Clamp history to the current six-neighbor extinction envelope. The envelope
				// includes the possible artistic shadow-extinction range without resampling shadows.
				int3 coordinate = int3(gridCoordinate);
				float neighborhoodMin = scatteringAndExtinction.w;
				float neighborhoodMax = scatteringAndExtinction.w;
				float neighborExtinction = GetMaterialExtinction(coordinate + int3(-1, 0, 0));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);
				neighborExtinction = GetMaterialExtinction(coordinate + int3(1, 0, 0));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);
				neighborExtinction = GetMaterialExtinction(coordinate + int3(0, -1, 0));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);
				neighborExtinction = GetMaterialExtinction(coordinate + int3(0, 1, 0));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);
				neighborExtinction = GetMaterialExtinction(coordinate + int3(0, 0, -1));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);
				neighborExtinction = GetMaterialExtinction(coordinate + int3(0, 0, 1));
				neighborhoodMin = min(neighborhoodMin, neighborExtinction);
				neighborhoodMax = max(neighborhoodMax, neighborExtinction);

				float shadowExtinctionMin = min(ShadowParameters0.y, 1.0f);
				float shadowExtinctionMax = max(ShadowParameters0.y, 1.0f);
				neighborhoodMin *= shadowExtinctionMin;
				neighborhoodMax *= shadowExtinctionMax;
				float clampedHistoryExtinction = clamp(historyScatteringAndExtinction.w, neighborhoodMin, neighborhoodMax);
				float clampDifference = abs(historyScatteringAndExtinction.w - clampedHistoryExtinction) / extinctionReference;
				stability = min(stability, 1.0f - saturate(clampDifference * TemporalParameters0.y));

				float historyExtinctionScale = clampedHistoryExtinction / max(historyScatteringAndExtinction.w, 0.000001f);
				float4 clampedHistory = float4(historyScatteringAndExtinction.rgb * historyExtinctionScale, clampedHistoryExtinction);
				historyScatteringAndExtinction = lerp(historyScatteringAndExtinction, clampedHistory, TemporalParameters0.y);
			}

			float minimumHistoryWeight = min(TemporalParameters0.w, historyAlpha);
			historyAlpha = lerp(minimumHistoryWeight, historyAlpha, saturate(stability));
		}
		scatteringAndExtinction = lerp(scatteringAndExtinction, historyScatteringAndExtinction, historyAlpha);
	}
	if (debugMode == VOLUMETRIC_FOG_DEBUG_HISTORY_WEIGHT)
		scatteringAndExtinction.rgb = historyAlpha;

	scatteringAndExtinction = select(or(isnan(scatteringAndExtinction), isinf(scatteringAndExtinction)), 0, scatteringAndExtinction);
	RWLightScattering[gridCoordinate] = max(scatteringAndExtinction, 0);
}

#elif defined(_CS_FinalIntegration)

RWTexture3D<float4> RWIntegratedLightScattering : register(u0);

Texture3D<float4> LightScattering : register(t0);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 1)]
void CS_FinalIntegration(uint3 DispatchThreadId : SV_DispatchThreadID)
{
	uint3 gridCoordinate = DispatchThreadId;
	if (any(gridCoordinate.xy >= GridSizeInt.xy))
		return;
	float4 acc = float4(0, 0, 0, 1);
	float3 prevPositionWS = GetCellPositionWS(uint3(gridCoordinate.xy, 0), 0.5f);

	for (uint layerIndex = 0; layerIndex < GridSizeInt.z; layerIndex++)
	{
		uint3 coords = uint3(gridCoordinate.xy, layerIndex);
		float4 scatteringExtinction = LightScattering[coords];
		float3 positionWS = GetCellPositionWS(coords, 0.5f);

		uint debugMode = (uint)DebugParameters.x;
		BRANCH
		if (debugMode != VOLUMETRIC_FOG_DEBUG_NONE)
		{
			float3 debugColor = 0.0f;
			if (debugMode == VOLUMETRIC_FOG_DEBUG_DENSITY)
			{
				float density = scatteringExtinction.w / max(GlobalExtinctionScale * 0.24f, 0.000001f);
				float referenceDensity = max(FogParameters.x + FogLayer2Parameters.x, 0.000001f);
				debugColor = saturate(density / referenceDensity);
			}
			else if (debugMode == VOLUMETRIC_FOG_DEBUG_EXTINCTION)
			{
				debugColor = 1.0f - exp(-scatteringExtinction.w * VolumetricFogRange.y);
			}
			else if (debugMode == VOLUMETRIC_FOG_DEBUG_SCATTERING)
			{
				// Scattering is stored as a small per-world-unit coefficient. Preview its accumulated
				// contribution over the fog range so useful values do not appear nearly black.
				debugColor = 1.0f - exp(-scatteringExtinction.rgb * max(VolumetricFogRange.y, 1.0f));
			}
			else if (debugMode == VOLUMETRIC_FOG_DEBUG_HISTORY_WEIGHT)
			{
				debugColor = scatteringExtinction.rgb;
			}
			else if (debugMode == VOLUMETRIC_FOG_DEBUG_FROXEL_GRID)
			{
				uint checker = (coords.x ^ coords.y ^ coords.z) & 1u;
				float depth = (coords.z + 0.5f) / GridSize.z;
				debugColor = checker != 0u ? float3(0.1f, 0.8f, depth) : float3(0.02f, 0.08f, depth * 0.5f);
			}
			RWIntegratedLightScattering[coords] = float4(debugColor, 0.0f);
			prevPositionWS = positionWS;
			continue;
		}

		// Fade both scattering and extinction so the volumetric medium transitions cleanly into distance fog.
		if (VolumetricFogDistanceFade > 0.0001f)
		{
			float lastLayer = GridSizeInt.z - 1.0f;
			float fadeStart = lastLayer * (1.0f - VolumetricFogDistanceFade);
			float distanceFade = 1.0f - saturate((layerIndex - fadeStart) / max(lastLayer - fadeStart, 0.0001f));
			scatteringExtinction *= distanceFade;
		}
		
		// Ref: "Physically Based and Unified Volumetric Rendering in Frostbite"
		float stepDistance = length(positionWS - prevPositionWS);
		float transmittance = exp(-scatteringExtinction.w * stepDistance);
		float3 scatteringIntegratedOverSlice = (scatteringExtinction.rgb - scatteringExtinction.rgb * transmittance) / max(scatteringExtinction.w, 0.00001f);

		// Accumulate
		acc.rgb += scatteringIntegratedOverSlice * acc.a;
		acc.a *= transmittance;
#if DEBUG_VOXELS
		RWIntegratedLightScattering[coords] = float4(scatteringExtinction.rgb, 1.0f);
#elif DEBUG_VOXEL_WS_POS
		RWIntegratedLightScattering[coords] = float4(positionWS.rgb, 1.0f);
#else
		RWIntegratedLightScattering[coords] = acc;
#endif

		prevPositionWS = positionWS;
	}
}

#endif
