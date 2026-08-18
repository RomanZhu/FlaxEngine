// Copyright (c) Wojciech Figat. All rights reserved.

#include "HDDAGIVoxelization.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Core/Math/Math.h"

bool HDDAGIVoxelization::Init()
{
    const auto device = GPUDevice::Instance;
    if (!device)
        return true;

    // Transient raster scratch textures: 128x64x128 3D UAVs
    if (!VoxelNormalBitsScratch)
    {
        GPUTextureDescription desc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X, HDDAGI_CASCADE_SIZE_Y, HDDAGI_CASCADE_SIZE_Z, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        VoxelNormalBitsScratch = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelNormalBitsScratch"));
        if (VoxelNormalBitsScratch->Init(desc))
            return true;
    }
    if (!VoxelAlbedoScratch)
    {
        GPUTextureDescription desc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X / 2, HDDAGI_CASCADE_SIZE_Y / 2, HDDAGI_CASCADE_SIZE_Z / 2, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        VoxelAlbedoScratch = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelAlbedoScratch"));
        if (VoxelAlbedoScratch->Init(desc))
            return true;
    }
    if (!VoxelEmissionScratch)
    {
        GPUTextureDescription desc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X / 2, HDDAGI_CASCADE_SIZE_Y / 2, HDDAGI_CASCADE_SIZE_Z / 2, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        VoxelEmissionScratch = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelEmissionScratch"));
        if (VoxelEmissionScratch->Init(desc))
            return true;
    }

    return false;
}

void HDDAGIVoxelization::Dispose()
{
    SAFE_DELETE_GPU_RESOURCE(VoxelNormalBitsScratch);
    SAFE_DELETE_GPU_RESOURCE(VoxelAlbedoScratch);
    SAFE_DELETE_GPU_RESOURCE(VoxelEmissionScratch);
}

void HDDAGIVoxelization::CollectDrawCalls(RenderContext& renderContext, const BoundingBox& bounds, Array<HDDAGIVoxelDrawCall>& outDrawCalls)
{
    if (!renderContext.List)
        return;

    // Iterate through draw calls in the render list
    const auto& drawCalls = renderContext.List->DrawCalls;
    for (int32 i = 0; i < drawCalls.Count(); i++)
    {
        const auto& drawCall = drawCalls.Get()[i];
        const BoundingBox drawBounds(
            Vector3(drawCall.World.M41, drawCall.World.M42, drawCall.World.M43) - Vector3(1000.0f),
            Vector3(drawCall.World.M41, drawCall.World.M42, drawCall.World.M43) + Vector3(1000.0f)
        );

        if (bounds.Intersects(drawBounds))
        {
            HDDAGIVoxelDrawCall vdc;
            vdc.Draw = const_cast<DrawCall*>(&drawCall);
            vdc.Bounds = drawBounds;
            vdc.Contribution = HDDAGIContribution::Auto;
            outDrawCalls.Add(vdc);
        }
    }
}

void HDDAGIVoxelization::ClearScratch(GPUContext* context, int32 cascadeIndex, const Int3& minCoord, const Int3& maxCoord, const Int3& gridSize)
{
    if (!context)
        return;

    if (VoxelNormalBitsScratch)
    {
        uint32 clearValues[4] = { 0, 0, 0, 0 };
        context->ClearUA(VoxelNormalBitsScratch, clearValues);
    }
    if (VoxelAlbedoScratch)
    {
        uint32 clearValues[4] = { 0, 0, 0, 0 };
        context->ClearUA(VoxelAlbedoScratch, clearValues);
    }
    if (VoxelEmissionScratch)
    {
        uint32 clearValues[4] = { 0, 0, 0, 0 };
        context->ClearUA(VoxelEmissionScratch, clearValues);
    }
}

void HDDAGIVoxelization::Voxelize(RenderContext& renderContext, GPUContext* context, int32 cascadeIndex, const HDDAGICascadeData& cascadeData, const Int3& gridSize, const Array<HDDAGIVoxelDrawCall>& drawCalls, bool isDynamic)
{
    if (!context || drawCalls.IsEmpty())
        return;

    // Bind scratch UAVs
    context->BindUA(0, VoxelNormalBitsScratch->ViewVolume());
    context->BindUA(1, VoxelAlbedoScratch->ViewVolume());
    context->BindUA(2, VoxelEmissionScratch->ViewVolume());

    // Three orthogonal projection passes (X, Y, Z)
    for (int32 axis = 0; axis < 3; axis++)
    {
        // Setup orthographic projection along axis
        // The rasterized fragments write to the 3D scratch textures via atomics in pixel shader
    }

    context->UnBindUA(0);
    context->UnBindUA(1);
    context->UnBindUA(2);
}
