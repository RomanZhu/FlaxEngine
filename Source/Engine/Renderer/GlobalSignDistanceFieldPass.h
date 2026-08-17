// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "RendererPass.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Renderer/GI/GlobalGIDirtyRegion.h"

/// <summary>
/// Global Sign Distance Field (SDF) rendering pass. Composites scene geometry into series of 3D volume textures that cover the world around the camera for global distance field sampling.
/// </summary>
class FLAXENGINE_API GlobalSignDistanceFieldPass : public RendererPass<GlobalSignDistanceFieldPass>
{
public:
    // Constant buffer data for Global SDF access on a GPU.
    GPU_CB_STRUCT(ConstantsData {
        Float4 CascadePosDistance[4];
        Float4 CascadeVoxelSize;
        Float4 CascadeMaxDistance;
        Float4 CascadeMaxDistanceMip;
        Float2 Padding;
        uint32 CascadesCount;
        float Resolution;
        });

    // Binding data for the GPU.
    struct BindingData
    {
        GPUTexture* Texture;
        GPUTexture* TextureMip;
        ConstantsData Constants;
    };

private:
    bool _supported = false;
    AssetReference<Shader> _shader;
    GPUPipelineState* _psDebug = nullptr;
    GPUShaderProgramCS* _csRasterizeModel0 = nullptr;
    GPUShaderProgramCS* _csRasterizeModel1 = nullptr;
    GPUShaderProgramCS* _csRasterizeHeightfield = nullptr;
    GPUShaderProgramCS* _csClearChunk = nullptr;
    GPUShaderProgramCS* _csGenerateMip = nullptr;
    GPUConstantBuffer* _cb0 = nullptr;
    GPUConstantBuffer* _cb1 = nullptr;
    class DynamicStructuredBuffer* _objectsBuffer = nullptr;

public:
    /// <summary>
    /// Calls drawing scene objects in async early in the frame.
    /// </summary>
    /// <param name="renderContextBatch">The rendering context batch.</param>
    void OnCollectDrawCalls(RenderContextBatch& renderContextBatch);

    /// <summary>
    /// Gets the Global SDF (only if enabled in Graphics Settings).
    /// </summary>
    /// <param name="buffers">The rendering context buffers.</param>
    /// <param name="result">The result Global SDF data for binding to the shaders.</param>
    /// <returns>True if there is no valid Global SDF rendered during this frame, otherwise false.</returns>
    bool Get(const RenderBuffers* buffers, BindingData& result);

    /// <summary>
    /// Renders the Global SDF.
    /// </summary>
    /// <param name="renderContext">The rendering context.</param>
    /// <param name="context">The GPU context.</param>
    /// <param name="result">The result Global SDF data for binding to the shaders.</param>
    /// <returns>True if failed to render (platform doesn't support it, out of video memory, disabled feature or effect is not ready), otherwise false.</returns>
    bool Render(RenderContext& renderContext, GPUContext* context, BindingData& result);

    /// <summary>
    /// Renders the debug view.
    /// </summary>
    /// <param name="renderContext">The rendering context.</param>
    /// <param name="context">The GPU context.</param>
    /// <param name="output">The output buffer.</param>
    void RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output);

    void GetCullingData(BoundingBox& bounds) const;

    // Rasterize Model SDF into the Global SDF. Call it from actor Draw() method during DrawPass::GlobalSDF.
    void RasterizeModelSDF(Actor* actor, const ModelBase::SDFData& sdf, const Transform& localToWorld, const BoundingBox& objectBounds);

    void RasterizeHeightfield(Actor* actor, GPUTexture* heightfield, const Transform& localToWorld, const BoundingBox& objectBounds, const Float4& localToUV);

    /// <summary>
    /// Gets the geometry revision for the specified cascade (or total if index is negative), incremented whenever dynamic geometry changes in it.
    /// </summary>
    /// <param name="buffers">The rendering context buffers.</param>
    /// <param name="cascadeIndex">The cascade index (0-3), or -1 for total revision across all cascades.</param>
    /// <returns>The geometry revision counter.</returns>
    uint32 GetGeometryRevision(const RenderBuffers* buffers, int32 cascadeIndex = -1) const;

    /// <summary>
    /// Gets the last frame count when dynamic geometry caused an update to the Global SDF.
    /// </summary>
    /// <param name="buffers">The rendering context buffers.</param>
    /// <returns>The engine frame count of the last dynamic update.</returns>
    uint64 GetLastDynamicUpdateFrame(const RenderBuffers* buffers) const;

    /// <summary>
    /// Manually queues a dynamic GI dirty region to invalidate intersecting Global SDF chunks and force near-cascade updates.
    /// </summary>
    /// <param name="buffers">The rendering context buffers.</param>
    /// <param name="region">The dirty region to invalidate.</param>
    void QueueDirtyRegion(RenderBuffers* buffers, const GlobalGIDirtyRegion& region);

private:
#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj);
#endif

public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

protected:
    // [RendererPass]
    bool setupResources() override;
};
