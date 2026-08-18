// Copyright (c) Wojciech Figat. All rights reserved.

#include "./Flax/Common.hlsl"
#include "./Flax/GI/HDDAGICommon.hlsl"

META_CB_BEGIN(0, Data0)
HDDAGIData HDDAGI;
float4x4 ProjectionMatrix;
int CascadeIndex;
int VoxelizeAxis; // 0 = X, 1 = Y, 2 = Z
float2 GridExtent;
META_CB_END

RWTexture3D<uint> VoxelNormalBitsScratch : register(u0);
RWTexture3D<uint> VoxelAlbedoScratch : register(u1);
RWTexture3D<uint> VoxelEmissionScratch : register(u2);

struct VS_Input
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VS_Output
{
    float4 Position : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD1;
};

META_VS(true, FEATURE_LEVEL_SM5)
VS_Output VS_VoxelizeAxis(VS_Input input)
{
    VS_Output output;
    output.WorldPosition = input.Position;
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    output.Position = mul(float4(input.Position, 1.0f), ProjectionMatrix);
    return output;
}

META_PS(true, FEATURE_LEVEL_SM5)
void PS_VoxelizeOpaque(VS_Output input)
{
    HDDAGICascadeData cascade = HDDAGI.Cascades[CascadeIndex];
    int3 cell = WorldToCascadeCell(cascade, input.WorldPosition);

    if (!IsInsideCascade(cell, HDDAGI.GridSize))
        return;

    // Normal direction encoding (bits 0..5: +/-X, +/-Y, +/-Z)
    float3 absNormal = abs(input.Normal);
    uint normalMask = 0;
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z)
        normalMask = input.Normal.x > 0 ? (1u << 0) : (1u << 1);
    else if (absNormal.y >= absNormal.x && absNormal.y >= absNormal.z)
        normalMask = input.Normal.y > 0 ? (1u << 2) : (1u << 3);
    else
        normalMask = input.Normal.z > 0 ? (1u << 4) : (1u << 5);

    // Atomically mark occupied normal bit
    InterlockedOr(VoxelNormalBitsScratch[cell], normalMask | 0x80000000u);
}
