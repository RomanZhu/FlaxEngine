// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/MaterialCommon.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/Common.hlsl"
#include "./Flax/Noise.hlsl"
#include "./Flax/AtmosphereFog.hlsl"

META_CB_BEGIN(0, Data)
float4x4 WorldViewProjection;
float4x4 InvViewProjection;
float3 ViewOffset;
float NoiseScale;
GBufferData GBuffer;
AtmosphericFogData AtmosphericFog;
float4 CloudParams0;
float4 CloudParams1;
float4 CloudDayColor;
float4 CloudNightColor;
float4 CloudStormColor;
float4 NightParams;
float4 MoonDirection;
float4 MoonColor;
float4 SkyParams;
META_CB_END

Texture2D CloudTexture : register(t7);
TextureCube StarsTexture : register(t8);

DECLARE_GBUFFERDATA_ACCESS(GBuffer)

struct MaterialInput
{
	float4 Position : SV_Position;
	float4 ScreenPos : TEXCOORD0;
};

// Vertex Shader function for GBuffer Pass
META_VS(true, FEATURE_LEVEL_ES2)
META_VS_IN_ELEMENT(POSITION, 0, R32G32B32_FLOAT, 0, 0, PER_VERTEX, 0, true)
MaterialInput VS(ModelInput_PosOnly input)
{
	MaterialInput output;

	// Compute vertex position
	output.Position = PROJECT_POINT(float4(input.Position.xyz, 1), WorldViewProjection);
	output.ScreenPos = output.Position;

	return output;
}

// Pixel Shader function for GBuffer Pass
META_PS(true, FEATURE_LEVEL_ES2)
GBufferOutput PS_Sky(MaterialInput input)
{
	GBufferOutput output;

    // Calculate view vector (unproject at the far plane)
	GBufferData gBufferData = GetGBufferData();
	float4 clipPos = float4(input.ScreenPos.xy / input.ScreenPos.w, 1.0, 1.0);
	clipPos = PROJECT_POINT(clipPos, InvViewProjection);
	float3 worldPos = clipPos.xyz / clipPos.w;
    float3 viewVector = normalize(worldPos - gBufferData.ViewPos);

	// Sample atmosphere color
    float4 color = GetAtmosphericFog(AtmosphericFog, gBufferData.ViewFar, gBufferData.ViewPos + ViewOffset, viewVector, gBufferData.ViewFar, float3(0, 0, 0));

    float daylight = smoothstep(-0.12, 0.10, AtmosphericFog.AtmosphericFogSunDirection.y);
    float night = 1.0 - smoothstep(-0.10, 0.04, AtmosphericFog.AtmosphericFogSunDirection.y);
    color.rgb += StarsTexture.Sample(SamplerLinearClamp, viewVector).rgb * NightParams.x * night;
    float moonDot = dot(viewVector, normalize(MoonDirection.xyz));
    color.rgb += MoonColor.rgb * NightParams.w * smoothstep(NightParams.y - NightParams.z, NightParams.y, moonDot) * night;

    float hy = max(viewVector.y, 0.08);
    float2 cuv = (viewVector.xz / hy) * CloudParams1.xy + CloudParams1.zw;
    float n0 = CloudTexture.Sample(SamplerLinearWrap, cuv).r;
    float n1 = CloudTexture.Sample(SamplerLinearWrap, cuv * CloudParams0.w + CloudParams1.zw * 1.37).r;
    float shape = saturate(n0 * 0.72 + n1 * 0.28);
    float threshold = lerp(0.95, 0.18, CloudParams0.x);
    float broken = smoothstep(threshold, threshold + CloudParams0.z, shape);
    float overcast = smoothstep(0.78, 0.98, CloudParams0.x);
    float coveragePresent = smoothstep(0.001, 0.01, CloudParams0.x);
    float mask = lerp(broken, 1.0, overcast) * coveragePresent;
    float3 cloud = lerp(CloudNightColor.rgb, CloudDayColor.rgb, daylight);
    cloud *= lerp(1.0, 0.55, saturate(CloudParams0.y));
    cloud = lerp(cloud, CloudStormColor.rgb, saturate(CloudParams0.y * overcast));
    cloud *= lerp(1.0, lerp(0.68, 1.05, shape), overcast);
    cloud += pow(saturate(dot(viewVector, AtmosphericFog.AtmosphericFogSunDirection)), 24.0) * SkyParams.y * daylight * (1.0 - overcast);
    float horizonFade = smoothstep(0.02, 0.16, viewVector.y);
    color.rgb = lerp(color.rgb, cloud, mask * horizonFade);

    // Apply dithering to hide banding artifacts
    float2 uv = (input.ScreenPos.xy / input.ScreenPos.w) * float2(0.5, -0.5) + float2(0.5, 0.5);
    float luminance = Luminance(saturate(color.rgb));
    color.rgb += rand2dTo1d(uv) * luminance * NoiseScale;

	// Pack GBuffer
	output.Light = color;
	output.RT0 = float4(0, 0, 0, 0);
	output.RT1 = float4(1, 0, 0, SHADING_MODEL_UNLIT);
	output.RT2 = float4(0, 0, 0, 0);
	output.RT3 = float4(0, 0, 0, 0);

	return output;
}
