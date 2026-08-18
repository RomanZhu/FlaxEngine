// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/Matrix.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/RenderTask.h"
#include "HDDAGIResources.h"

struct RenderContext;
class GPUContext;
struct DrawCall;

/// <summary>
/// Contributor draw call for HDDAGI voxelization.
/// </summary>
struct HDDAGIVoxelDrawCall
{
    DrawCall* Draw = nullptr;
    BoundingBox Bounds = BoundingBox::Empty;
    HDDAGIContribution Contribution = HDDAGIContribution::Auto;
    uint32 MaterialFlags = 0;
};

/// <summary>
/// Handles 3-axis scene geometry voxelization and scratch resource management for HDDAGI.
/// </summary>
class HDDAGIVoxelization
{
public:
    // Transient raster scratch textures reused across cascades
    GPUTexture* VoxelNormalBitsScratch = nullptr;
    GPUTexture* VoxelAlbedoScratch = nullptr;
    GPUTexture* VoxelEmissionScratch = nullptr;

    bool Init();
    void Dispose();

    /// <summary>
    /// Collects voxelizable draw calls for the given scene bounds and contribution mode.
    /// </summary>
    void CollectDrawCalls(RenderContext& renderContext, const BoundingBox& bounds, Array<HDDAGIVoxelDrawCall>& outDrawCalls);

    /// <summary>
    /// Clears voxel scratch resources for the specified dirty region in the cascade.
    /// </summary>
    void ClearScratch(GPUContext* context, int32 cascadeIndex, const Int3& minCoord, const Int3& maxCoord, const Int3& gridSize);

    /// <summary>
    /// Voxelizes draw calls across three orthogonal axes (X, Y, Z) into scratch UAVs.
    /// </summary>
    void Voxelize(RenderContext& renderContext, GPUContext* context, int32 cascadeIndex, const HDDAGICascadeData& cascadeData, const Int3& gridSize, const Array<HDDAGIVoxelDrawCall>& drawCalls, bool isDynamic);
};
