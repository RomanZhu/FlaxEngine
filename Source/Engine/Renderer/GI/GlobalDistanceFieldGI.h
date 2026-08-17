// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "../RendererPass.h"
#include "Engine/Core/Math/Int4.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Graphics/Textures/GPUTexture.h"
#include "GlobalGIDirtyRegion.h"

#define GDFGI_OCT_RESOLUTION 5
#define GDFGI_OCT_TILE_SIZE 7 // 5 + 2 border texels
#define GDFGI_RAYS_COUNT 25
#define GDFGI_DEFAULT_HISTORY_FRAMES 8

class SceneRenderTask;

/// <summary>
/// Global Distance Field Global Illumination (GDFGI) rendering pass.
/// Implements cascaded camera-relative probe grid clipmaps, octahedral directional radiance, temporal history rings, and dynamic leak prevention.
/// </summary>
class FLAXENGINE_API GlobalDistanceFieldGIPass : public RendererPass<GlobalDistanceFieldGIPass>
{
public:
    // Constant buffer data for GDFGI access on a GPU.
    GPU_CB_STRUCT(ConstantsData {
        Float4 ProbesOriginAndSpacing[4];
        Float4 BlendOrigin[4]; // w is unused
        Int4 ProbesScrollOffsets[4]; // w is unused
        uint32 ProbesCounts[3];
        uint32 CascadesCount;
        Float3 ViewPos;
        float IndirectLightingIntensity;
        Float4 FallbackIrradiance;
        float RayMaxDistance;
        float NormalBias;
        float ViewBias;
        float ThinGeometryExpansion;
        uint32 RaysCount;
        uint32 HistoryFrames;
        uint32 HistoryFrameIndex;
        uint32 DynamicInvalidation;
        uint32 EnableDirectionalSpecular;
        uint32 Algorithm; // 2 for GDFGI
        uint32 UpdateRowOffset;
        uint32 UpdateRowCount;
        Float4 CascadeDirtyBoundsMin[4]; // xyz: min, w: 1.0 if active
        Float4 CascadeDirtyBoundsMax[4]; // xyz: max, w: unused
        Int4 ProbeScrollClears[4];
        uint32 DebugExecutionStage;
        uint32 CulledObjectsCapacity;
        uint32 ObjectsCount;
        float Padding;
    });

    // Binding data for the GPU.
    struct BindingData
    {
        ConstantsData Constants;
        GPUTextureView* ProbesData;
        GPUTextureView* ProbeStates;
        GPUTextureView* ProbesDistance;
        GPUTextureView* DirectionalRadiance;
        GPUTextureView* DirectionalDiffuse;
    };

private:
    bool _supported = false;
    AssetReference<Shader> _shader;
    GPUConstantBuffer* _cb0 = nullptr;
    GPUShaderProgramCS* _csClassify = nullptr;
    GPUShaderProgramCS* _csTraceDirectionalRadiance = nullptr;
    GPUShaderProgramCS* _csUpdateTemporalRing = nullptr;
    GPUShaderProgramCS* _csConvolveDiffuse = nullptr;
    GPUPipelineState* _psIndirectLighting[2] = {};
    GPUPipelineState* _psDebug = nullptr;
    GPUBuffer* _completionBuffer = nullptr;
    GPUBuffer* _completionReadbackBuffer = nullptr;
    uint64 _completionFrames[8] = {};
    int32 _completionCounterIndex = -1;
    uint32 _updateRowOffset = 0;
    SceneRenderTask* _ownerTask = nullptr;
#if USE_EDITOR
    AssetReference<Model> _debugModel;
    AssetReference<MaterialBase> _debugMaterial;
#endif

public:
    /// <summary>
    /// Gets the GDFGI binding data (only if enabled).
    /// </summary>
    /// <param name="buffers">The rendering context buffers.</param>
    /// <param name="result">The result GDFGI data for binding to the shaders.</param>
    /// <returns>True if failed to render (platform doesn't support it, out of video memory, disabled feature or effect is not ready), otherwise false.</returns>
    bool Get(const RenderBuffers* buffers, BindingData& result);

    /// <summary>
    /// Renders the GDFGI.
    /// </summary>
    /// <param name="renderContext">The rendering context.</param>
    /// <param name="context">The GPU context.</param>
    /// <param name="lightBuffer">The light accumulation buffer (input and output).</param>
    /// <returns>True if failed to render (platform doesn't support it, out of video memory, disabled feature or effect is not ready), otherwise false.</returns>
    bool Render(RenderContext& renderContext, GPUContext* context, GPUTextureView* lightBuffer);

    /// <summary>
    /// Renders debug probes in the viewport.
    /// </summary>
    void RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output);

    /// <summary>
    /// Manually queues a dynamic GI dirty region to invalidate intersecting probe history and force prompt radiance reconvergence.
    /// </summary>
    void QueueDirtyRegion(RenderBuffers* buffers, const GlobalGIDirtyRegion& region);

private:
    CriticalSection _locker;
    Array<GlobalGIDirtyRegion> _pendingDirtyRegions;
#if COMPILE_WITH_DEV_ENV
    uint64 LastFrameShaderReload = 0;
    void OnShaderReloading(Asset* obj);
#endif
    bool RenderInner(RenderContext& renderContext, GPUContext* context, class GDFGICustomBuffer& gdfgiData);

public:
    // [RendererPass]
    String ToString() const override;
    bool Init() override;
    void Dispose() override;

protected:
    // [RendererPass]
    bool setupResources() override;
};
