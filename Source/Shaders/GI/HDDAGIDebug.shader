// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GBuffer.hlsl"
#include "./Flax/LightingCommon.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"
#include "./Flax/GI/HDDAGISampling.hlsl"

META_CB_BEGIN(0, Data0)
HDDAGIData HDDAGI;
GBufferData GBuffer;
uint DebugViewMode; // 0 = Diffuse, 1 = Specular, 2 = Occupancy, 3 = RegionVersions, 4 = Proximity
uint CascadeIndex;
float2 Padding0;
META_CB_END

Texture2D ProbeDiffuse : register(t4);
Texture2D ProbeSpecular : register(t5);
Texture3D<uint> CombinedRegionBits : register(t6);
Texture3D<uint> RegionVersions : register(t7);
Texture3D<float4> VoxelRadiance : register(t8);
Texture3D Occlusion0 : register(t9);
Texture3D Occlusion1 : register(t10);

struct VS_Output
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

META_VS(true, FEATURE_LEVEL_SM5)
VS_Output VS_Debug(uint vertexId : SV_VertexID)
{
    VS_Output output;
    output.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
    output.TexCoord.y = 1.0f - output.TexCoord.y;
    return output;
}

META_PS(true, FEATURE_LEVEL_SM5)
float4 PS_Debug(VS_Output input) : SV_Target
{
    if (DebugViewMode == 0) // Probe Diffuse Atlas
    {
        return ProbeDiffuse.SampleLevel(SamplerLinearClamp, input.TexCoord, 0);
    }
    else if (DebugViewMode == 1) // Probe Specular Atlas
    {
        return ProbeSpecular.SampleLevel(SamplerLinearClamp, input.TexCoord, 0);
    }
    else if (DebugViewMode == 2) // Occupancy
    {
        int3 regionCoord = int3(input.TexCoord.x * 16.0f, input.TexCoord.y * 8.0f, 0);
        uint occ = CombinedRegionBits[regionCoord];
        return occ > 0 ? float4(1, 1, 1, 1) : float4(0.1, 0.1, 0.1, 1);
    }
    else if (DebugViewMode == 3) // Region Versions
    {
        int3 regionCoord = int3(input.TexCoord.x * 16.0f, input.TexCoord.y * 8.0f, 0);
        uint v = RegionVersions[regionCoord];
        return float4(frac(v * 0.1f), frac(v * 0.23f), frac(v * 0.47f), 1.0f);
    }
    return float4(0, 0, 0, 1);
}

META_PS(true, FEATURE_LEVEL_SM5)
void PS_IndirectLighting(Quad_VS2PS input, out float4 output : SV_Target0)
{
    output = 0;

    // Sample GBuffer
    GBufferSample gBuffer = SampleGBuffer(GBuffer, input.TexCoord);

    BRANCH
    if (gBuffer.ShadingModel == SHADING_MODEL_UNLIT)
    {
        discard;
        return;
    }

    float3 viewVector = normalize(gBuffer.WorldPos - HDDAGI.ViewPosition);
    float3 reflectVector = reflect(viewVector, gBuffer.Normal);

    HDDAGISampleResult gi = SampleHDDAGI(
        HDDAGI,
        ProbeDiffuse,
        ProbeSpecular,
        Occlusion0,
        Occlusion1,
        gBuffer.WorldPos,
        gBuffer.Normal,
        reflectVector,
        gBuffer.Roughness
    );

    float3 diffuseColor = GetDiffuseColor(gBuffer);
    float3 diffuse = Diffuse_Lambert(diffuseColor);
    float3 specularColor = GetSpecularColor(gBuffer);

    output.rgb = (diffuse * gi.Diffuse + gi.Specular * specularColor) * gBuffer.AO * HDDAGI.IndirectLightingIntensity;
    output.a = 1.0f;
}
