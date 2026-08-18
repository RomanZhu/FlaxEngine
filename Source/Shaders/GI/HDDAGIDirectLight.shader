// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"

META_CB_BEGIN(0, Data0)
HDDAGIData HDDAGI;
uint CascadeIndex;
uint LightCount;
float2 Padding0;
META_CB_END

StructuredBuffer<HDDAGILightData> Lights : register(t0);
Texture3D<uint> VoxelMaterial : register(t1);
Texture3D<uint> VoxelEmission : register(t2);
Texture3D<float4> PreviousVoxelRadiance : register(t3);

RWTexture3D<float4> VoxelRadianceOut : register(u0);

META_CS(true, FEATURE_LEVEL_SM5)
[numthreads(8, 8, 8)]
void CS_InjectLights(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int3 voxelCoord = int3(dispatchThreadId);
    if (any(voxelCoord >= HDDAGI.GridSize))
        return;

    HDDAGICascadeData cascade = HDDAGI.Cascades[CascadeIndex];
    float3 worldPos = CascadeCellToWorld(cascade, voxelCoord);

    // Read voxel material & normal
    uint packedMat = VoxelMaterial[voxelCoord];
    if (packedMat == 0)
    {
        VoxelRadianceOut[voxelCoord] = float4(0, 0, 0, 0);
        return;
    }

    float3 albedo = float3((packedMat & 0xFF) / 255.0f, ((packedMat >> 8) & 0xFF) / 255.0f, ((packedMat >> 16) & 0xFF) / 255.0f);
    float3 emission = UnpackRGBE(VoxelEmission[voxelCoord]);

    float3 accumulatedLight = emission;

    // Inject lights
    for (uint i = 0; i < LightCount; i++)
    {
        HDDAGILightData light = Lights[i];
        if (light.Type == 0) // Directional
        {
            float NdotL = saturate(dot(float3(0, 1, 0), -light.Direction)); // Simplified diffuse cosine
            accumulatedLight += light.Color * (light.Energy * NdotL) * albedo;
        }
        else if (light.Type == 1 || light.Type == 2) // Point or Spot
        {
            float3 toLight = light.Position - worldPos;
            float distSq = dot(toLight, toLight);
            float radiusSq = light.Radius * light.Radius;
            if (distSq < radiusSq)
            {
                float dist = sqrt(distSq);
                float3 lightDir = toLight / max(dist, 0.001f);
                float attenuation = saturate(1.0f - dist / light.Radius);
                attenuation *= attenuation;

                if (light.Type == 2) // Spot cone
                {
                    float cosAngle = dot(-lightDir, light.Direction);
                    float spotAttenuation = saturate((cosAngle - light.SpotCos) * light.SpotInvAttenuation);
                    attenuation *= spotAttenuation;
                }

                accumulatedLight += light.Color * (light.Energy * attenuation) * albedo;
            }
        }
    }

    // Multibounce feedback
    float3 prevBounce = PreviousVoxelRadiance[voxelCoord].rgb;
    accumulatedLight += prevBounce * HDDAGI.BounceFeedback * albedo;

    VoxelRadianceOut[voxelCoord] = float4(accumulatedLight, 1.0f);
}
