// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "../RendererPass.h"
#include "HDDAGIResources.h"
#include "HDDAGIVoxelization.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/Shaders/GPUShader.h"

/// <summary>
/// Hierarchical DDA Global Illumination (HDDAGI) rendering pass.
/// Implements fixed-point HDDA scene traversal across hierarchical occupancy voxel clipmaps,
/// directional octahedral probes with temporal moving-window history, local cache invalidation,
/// and dynamic rigid geometry updates.
/// </summary>
class FLAXENGINE_API HDDAGIPass : public RendererPass<HDDAGIPass>
{
public:
    /// <summary>
    /// Binding data for shaders consuming HDDAGI diffuse and specular fields.
    /// </summary>
    struct BindingData
    {
        HDDAGIConstantsData Constants;
        GPUTextureView* ProbeDiffuse = nullptr;
        GPUTextureView* ProbeSpecular = nullptr;
        GPUTextureView* Occlusion0 = nullptr;
        GPUTextureView* Occlusion1 = nullptr;
    };

private:
    bool _supported = false;
    RenderTask* _ownerTask = nullptr;
    HDDAGIVoxelization _voxelization;

    AssetReference<Shader> _shaderPreprocess;
    AssetReference<Shader> _shaderIntegrate;
    AssetReference<Shader> _shaderDirectLight;
    AssetReference<Shader> _shaderDebug;

    GPUConstantBuffer* _cb0 = nullptr;
    GPUPipelineState* _psIndirectLighting = nullptr;
    GPUPipelineState* _psDebug = nullptr;

public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

    /// <summary>
    /// Gets the HDDAGI binding data for shaders.
    /// </summary>
    bool Get(const RenderBuffers* buffers, BindingData& result);

    /// <summary>
    /// Queues a dynamic geometry or lighting invalidation event.
    /// </summary>
    void QueueInvalidation(RenderBuffers* buffers, const GlobalGIInvalidation& invalidation);

    /// <summary>
    /// Renders the HDDAGI pipeline.
    /// </summary>
    bool Render(RenderContext& renderContext, GPUContext* context, GPUTextureView* lightBuffer);

    /// <summary>
    /// Renders debug visualization for HDDAGI.
    /// </summary>
    void RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output);

private:
    bool setupResources();
    bool UpdateCascades(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool BuildDirtyRegionList(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool VoxelizeDirtyRegions(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool BuildOccupancyHierarchy(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool ReconstructVoxelMaterials(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool UpdateVoxelLighting(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool UpdateProbeMetadata(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool BuildProbeSchedule(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool IntegrateScheduledProbes(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    bool FilterProbes(RenderContext& renderContext, GPUContext* context, class HDDAGICustomBuffer& data);
    void PublishBindings(RenderContext& renderContext, class HDDAGICustomBuffer& data);
};
