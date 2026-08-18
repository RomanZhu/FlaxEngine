// Copyright (c) Wojciech Figat. All rights reserved.

#ifndef __HDDAGI_SAMPLING__
#define __HDDAGI_SAMPLING__

#include "./Flax/Common.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"

struct HDDAGISampleResult
{
    float3 Diffuse;
    float3 Specular;
    float CascadeBlend;
};

// Samples cascaded HDDAGI diffuse and specular radiance for a surface point
HDDAGISampleResult SampleHDDAGI(
    HDDAGIData data,
    Texture2D probeDiffuse,
    Texture2D probeSpecular,
    Texture3D occlusion0,
    Texture3D occlusion1,
    float3 worldPosition,
    float3 normal,
    float3 reflectionDir,
    float roughness)
{
    HDDAGISampleResult result;
    result.Diffuse = float3(0, 0, 0);
    result.Specular = float3(0, 0, 0);
    result.CascadeBlend = 0.0f;

    if (data.CascadesCount == 0)
        return result;

    float3 biasedWorldPos = worldPosition + normal * (data.NormalBias * data.Cascades[0].CellSize);

    // Cascade 0
    HDDAGICascadeData cascade = data.Cascades[0];
    float3 localPos = (biasedWorldPos - cascade.WorldOffset) * cascade.ToCell;
    float3 probeGridPos = localPos / (float)HDDAGI_REGION_SIZE;
    int3 baseProbe = clamp((int3)floor(probeGridPos), int3(0, 0, 0), data.ProbeAxisSize - int3(2, 2, 2));
    float3 fracCoord = saturate(probeGridPos - (float3)baseProbe);

    float2 normalOctUV = GetOctahedralCoords(normal) * 0.5f + 0.5f;
    float2 reflectOctUV = GetOctahedralCoords(reflectionDir) * 0.5f + 0.5f;

    float3 totalDiffuse = float3(0, 0, 0);
    float3 totalSpecular = float3(0, 0, 0);
    float totalWeight = 0.0f;

    // Trilinear interpolation across 8 probe corners
    for (int dz = 0; dz <= 1; dz++)
    {
        for (int dy = 0; dy <= 1; dy++)
        {
            for (int dx = 0; dx <= 1; dx++)
            {
                int3 pCoord = baseProbe + int3(dx, dy, dz);
                float weight = (dx ? fracCoord.x : (1.0f - fracCoord.x)) *
                               (dy ? fracCoord.y : (1.0f - fracCoord.y)) *
                               (dz ? fracCoord.z : (1.0f - fracCoord.z));

                int2 probeTileOrigin = int2(pCoord.x + pCoord.z * data.ProbeAxisSize.x, pCoord.y) * HDDAGI_OCT_TILE_SIZE;
                float2 sampleDiffuseUV = (float2(probeTileOrigin + 1) + normalOctUV * (float)HDDAGI_OCT_SIZE) / float2(data.ProbeAxisSize.x * data.ProbeAxisSize.z * HDDAGI_OCT_TILE_SIZE, data.ProbeAxisSize.y * HDDAGI_OCT_TILE_SIZE);
                float2 sampleSpecularUV = (float2(probeTileOrigin + 1) + reflectOctUV * (float)HDDAGI_OCT_SIZE) / float2(data.ProbeAxisSize.x * data.ProbeAxisSize.z * HDDAGI_OCT_TILE_SIZE, data.ProbeAxisSize.y * HDDAGI_OCT_TILE_SIZE);

                float3 pDiffuse = probeDiffuse.SampleLevel(SamplerLinearClamp, sampleDiffuseUV, 0).rgb;
                float3 pSpecular = probeSpecular.SampleLevel(SamplerLinearClamp, sampleSpecularUV, 0).rgb;

                totalDiffuse += pDiffuse * weight;
                totalSpecular += pSpecular * weight;
                totalWeight += weight;
            }
        }
    }

    if (totalWeight > 0.001f)
    {
        result.Diffuse = totalDiffuse / totalWeight;
        result.Specular = lerp(totalSpecular / totalWeight, result.Diffuse, roughness);
    }

    return result;
}

#endif
