// Copyright (c) Wojciech Figat. All rights reserved.

#include "GlobalDistanceFieldGI.h"
#include "GlobalSurfaceAtlasPass.h"
#include "../GlobalSignDistanceFieldPass.h"
#include "../RenderList.h"
#include "../GBufferPass.h"
#include "Engine/Content/Content.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Core/Config/GraphicsSettings.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUBuffer.h"
#include "Engine/Graphics/GPUPipelineState.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/PostProcessSettings.h"
#include "Engine/Level/Actors/Sky.h"

GPU_CB_STRUCT(Data0 {
    GlobalDistanceFieldGIPass::ConstantsData GDFGI;
    GlobalSignDistanceFieldPass::ConstantsData GlobalSDF;
    GlobalSurfaceAtlasPass::ConstantsData GlobalSurfaceAtlas;
    ShaderGBufferData GBuffer;
    float SkyboxIntensity;
    uint32 CascadeIndex;
    uint32 ProbesCount;
    float TemporalTime;
    Int4 ProbeScrollClears[4];
    Float3 ViewDir;
    uint32 DynamicInvalidation;
});

class GDFGICustomBuffer : public RenderBuffers::CustomBuffer
{
public:
    struct Cascade
    {
        Float3 ProbesOrigin = Float3::Zero;
        float ProbesSpacing = 0.0f;
        Int3 ProbeScrollOffsets = Int3::Zero;
        bool PendingUpdate = true;
        Int3 ProbeScrollClears = Int3::Zero;

        void Clear()
        {
            ProbesOrigin = Float3::Zero;
            ProbeScrollOffsets = Int3::Zero;
            ProbeScrollClears = Int3::Zero;
            PendingUpdate = true;
        }
    } Cascades[4];

    int32 CascadesCount = 4;
    int32 HistoryFrames = GDFGI_DEFAULT_HISTORY_FRAMES;
    uint32 HistoryFrameIndex = 0;
    uint32 DynamicInvalidation = 0;
    // Conservative MVP topology. The original 32x16x32 grid submitted too
    // much Global SDF work for a single D3D11 frame and could trip TDR.
    Int3 ProbeCounts = Int3(16, 8, 16);
    int32 ProbesCountTotal = 0;
    Vector3 ViewOrigin = Vector3::Zero;
    Array<GlobalGIDirtyRegion> DirtyRegions;

    GPUTexture* ProbesData = nullptr;
    GPUTexture* ProbeStates = nullptr;
    GPUTexture* ProbesDistance = nullptr;
    GPUTexture* DirectionalRadianceRaw = nullptr;
    GPUTexture* DirectionalRadianceAvg = nullptr;
    GPUTexture* DirectionalRadianceHistory[2] = {};
    GPUTexture* DirectionalDiffuse = nullptr;

    GlobalDistanceFieldGIPass::BindingData Result;

    FORCE_INLINE void Release()
    {
        RenderTargetPool::Release(ProbesData);
        RenderTargetPool::Release(ProbeStates);
        RenderTargetPool::Release(ProbesDistance);
        RenderTargetPool::Release(DirectionalRadianceRaw);
        RenderTargetPool::Release(DirectionalRadianceAvg);
        RenderTargetPool::Release(DirectionalRadianceHistory[0]);
        RenderTargetPool::Release(DirectionalRadianceHistory[1]);
        RenderTargetPool::Release(DirectionalDiffuse);
    }

    ~GDFGICustomBuffer()
    {
        Release();
    }

    void Rebase(const Vector3& origin)
    {
        if (ViewOrigin == origin)
            return;
        const Vector3 delta = origin - ViewOrigin;
        ViewOrigin = origin;
        for (auto& cascade : Cascades)
        {
            cascade.ProbesOrigin -= (Float3)delta;
            cascade.PendingUpdate = true;
        }
        DynamicInvalidation = 1;
    }

    void OnSceneRenderingDirtyRegion(const GlobalGIDirtyRegion& region)
    {
        DirtyRegions.Add(region);
    }
};

String GlobalDistanceFieldGIPass::ToString() const
{
    return TEXT("GlobalDistanceFieldGIPass");
}

bool GlobalDistanceFieldGIPass::Init()
{
    const auto device = GPUDevice::Instance;
    _supported = device->GetFeatureLevel() >= FeatureLevel::SM5 && device->Limits.HasCompute && device->Limits.HasTypedUAVLoad;
    return false;
}

void GlobalDistanceFieldGIPass::Dispose()
{
    _ownerTask = nullptr;
    RendererPass::Dispose();
    _shader = nullptr;
    SAFE_DELETE_GPU_RESOURCE(_cb0);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting[0]);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting[1]);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    SAFE_DELETE_GPU_RESOURCE(_completionBuffer);
    SAFE_DELETE_GPU_RESOURCE(_completionReadbackBuffer);
}

bool GlobalDistanceFieldGIPass::setupResources()
{
    if (!_supported)
        return true;

    if (!_shader)
    {
        _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/GDFGI"));
        if (_shader == nullptr)
            return true;
#if COMPILE_WITH_DEV_ENV
        _shader.Get()->OnReloading.Bind<GlobalDistanceFieldGIPass, &GlobalDistanceFieldGIPass::OnShaderReloading>(this);
#endif
    }
    if (!_shader->IsLoaded())
        return true;

    const auto shader = _shader->GetShader();
    if (!shader)
        return true;

    _cb0 = shader->GetCB(0);
    if (!_cb0)
        return true;

    _csClassify = shader->GetCS("CS_ClassifyProbes");
    _csTraceDirectionalRadiance = shader->GetCS("CS_TraceDirectionalRadiance");
    _csUpdateTemporalRing = shader->GetCS("CS_UpdateTemporalRing");
    _csConvolveDiffuse = shader->GetCS("CS_ConvolveDiffuse");

    if (!_csClassify || !_csTraceDirectionalRadiance || !_csUpdateTemporalRing || !_csConvolveDiffuse)
        return true;

    if (!_psIndirectLighting[0])
    {
        auto psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.BlendMode = BlendingMode::Add;
        psDesc.PS = shader->GetPS("PS_IndirectLighting");
        if (!psDesc.PS)
            return true;
        _psIndirectLighting[0] = GPUDevice::Instance->CreatePipelineState();
        if (_psIndirectLighting[0]->Init(psDesc))
            return true;
    }

    if (!_psDebug)
    {
        auto psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = shader->GetPS("PS_Debug");
        if (psDesc.PS)
        {
            _psDebug = GPUDevice::Instance->CreatePipelineState();
            _psDebug->Init(psDesc);
        }
    }

    return false;
}

#if COMPILE_WITH_DEV_ENV
void GlobalDistanceFieldGIPass::OnShaderReloading(Asset* obj)
{
    _csClassify = nullptr;
    _csTraceDirectionalRadiance = nullptr;
    _csUpdateTemporalRing = nullptr;
    _csConvolveDiffuse = nullptr;
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting[0]);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting[1]);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    _cb0 = nullptr;
    LastFrameShaderReload = Engine::FrameCount;
}
#endif

bool GlobalDistanceFieldGIPass::Get(const RenderBuffers* buffers, BindingData& result)
{
    auto* customBuffer = buffers ? buffers->FindCustomBuffer<GDFGICustomBuffer>(TEXT("GDFGI")) : nullptr;
    // Allocation happens before Surface Atlas rendering, but Result is only
    // published after the first complete GDFGI update. Do not expose partially
    // initialized bindings to the atlas multibounce pass during that cycle.
    if (customBuffer &&
        customBuffer->Result.ProbesData &&
        customBuffer->Result.ProbeStates &&
        customBuffer->Result.ProbesDistance &&
        customBuffer->Result.DirectionalRadiance &&
        customBuffer->Result.DirectionalDiffuse)
    {
        result = customBuffer->Result;
        return false;
    }
    return true;
}

void GlobalDistanceFieldGIPass::QueueDirtyRegion(RenderBuffers* buffers, const GlobalGIDirtyRegion& region)
{
    if (buffers)
    {
        auto* customBuffer = buffers->FindCustomBuffer<GDFGICustomBuffer>(TEXT("GDFGI"));
        if (customBuffer)
            const_cast<GDFGICustomBuffer*>(customBuffer)->OnSceneRenderingDirtyRegion(region);
    }
    else
    {
        ScopeLock lock(_locker);
        _pendingDirtyRegions.Add(region);
    }
}

bool GlobalDistanceFieldGIPass::Render(RenderContext& renderContext, GPUContext* context, GPUTextureView* lightBuffer)
{
    if (checkIfSkipPass())
        return true;

    auto* graphicsSettings = GraphicsSettings::Get();
    const auto debugStage = graphicsSettings->GDFGIDebugStage;

    const auto giMode = renderContext.List->Settings.GlobalIllumination.Mode;
    if (giMode != GlobalIlluminationMode::GDFGI)
        return true;
    if (renderContext.List->Scenes.Count() == 0)
        return true;

    if (setupResources())
        return true;

    RenderBuffers* renderBuffers = renderContext.Buffers;
    bool render = true;
    if (_ownerTask && !RenderTask::Tasks.Contains(_ownerTask))
        _ownerTask = nullptr;
    if (!_ownerTask)
    {
        // Do not let an asset thumbnail or probe capture become the persistent
        // camera-relative clipmap owner before the real scene viewport renders.
        const float viewArea = renderContext.View.ScreenSize.X * renderContext.View.ScreenSize.Y;
        if (!renderContext.Task || renderContext.View.IsOfflinePass || viewArea < 512.0f * 512.0f)
            return true;
        _ownerTask = renderContext.Task;
    }
    if (renderContext.Task != _ownerTask)
    {
        // Preview, reflection, and other secondary views must reuse the
        // largest active scene view's clipmap. Allocating a complete GDFGI set
        // for every auxiliary RenderBuffers instance causes extreme VRAM
        // churn and can trip the DX11 watchdog during composition.
        auto* primaryGDFGI = _ownerTask->Buffers ? _ownerTask->Buffers->FindCustomBuffer<GDFGICustomBuffer>(TEXT("GDFGI")) : nullptr;
        if (primaryGDFGI && primaryGDFGI->LastFrameUsed + 1 >= Engine::FrameCount)
        {
            renderBuffers = _ownerTask->Buffers;
            render = false;
        }
        else
            return true;
    }
    auto& gdfgiData = *renderBuffers->GetCustomBuffer<GDFGICustomBuffer>(TEXT("GDFGI"));
    if (gdfgiData.LastFrameUsed == Engine::FrameCount)
        render = false;

    if (render)
    {
        if (RenderInner(renderContext, context, gdfgiData))
        {
            context->ResetRenderTarget();
            context->ResetSR();
            context->ResetUA();
            context->SetViewportAndScissors(renderContext.View.ScreenSize.X, renderContext.View.ScreenSize.Y);
            return true;
        }
    }

    if (renderContext.View.Mode == ViewMode::GlobalIllumination && lightBuffer)
    {
        RenderDebug(renderContext, context, (GPUTexture*)lightBuffer->GetParent());
        return false;
    }

    // Fullscreen deferred indirect lighting pass
    const bool allowComposite = (debugStage == GDFGIDebugExecutionStage::Disabled || debugStage >= GDFGIDebugExecutionStage::Composite);
    if (allowComposite && lightBuffer && _psIndirectLighting[0] && gdfgiData.Result.ProbesData && gdfgiData.Result.DirectionalDiffuse)
    {
        PROFILE_GPU_CPU_NAMED("GDFGI Indirect Lighting");
        context->BindSR(0, renderContext.Buffers->GBuffer0->View());
        context->BindSR(1, renderContext.Buffers->GBuffer1->View());
        context->BindSR(2, renderContext.Buffers->GBuffer2->View());
        context->BindSR(3, renderContext.Buffers->DepthBuffer->View());
        context->BindSR(4, gdfgiData.Result.ProbesData);
        context->BindSR(5, gdfgiData.Result.ProbeStates);
        context->BindSR(6, gdfgiData.Result.DirectionalDiffuse);
        context->BindSR(7, gdfgiData.Result.ProbesDistance);
        context->BindSR(8, gdfgiData.Result.DirectionalRadiance);
        context->SetViewportAndScissors(renderContext.View.ScreenSize.X, renderContext.View.ScreenSize.Y);
        context->SetRenderTarget(lightBuffer);
        context->SetState(_psIndirectLighting[0]);
        context->DrawFullscreenTriangle();
        context->ResetSR();
        context->ResetRenderTarget();
    }

    return false;
}

bool GlobalDistanceFieldGIPass::RenderInner(RenderContext& renderContext, GPUContext* context, GDFGICustomBuffer& gdfgiData)
{
    PROFILE_GPU_CPU_NAMED("GDFGI");

    auto* graphicsSettings = GraphicsSettings::Get();
    const auto debugStage = graphicsSettings->GDFGIDebugStage;
    gdfgiData.HistoryFrames = Math::Clamp((int32)graphicsSettings->GDFGIHistoryFrames, 1, 32);

    {
        ScopeLock lock(_locker);
        gdfgiData.DirtyRegions.Add(_pendingDirtyRegions);
        _pendingDirtyRegions.Clear();
    }

    gdfgiData.Rebase(renderContext.View.Origin);
    gdfgiData.ProbesCountTotal = gdfgiData.ProbeCounts.X * gdfgiData.ProbeCounts.Y * gdfgiData.ProbeCounts.Z * gdfgiData.CascadesCount;
    gdfgiData.LastFrameUsed = Engine::FrameCount;

    const int32 gridProbesX = gdfgiData.ProbeCounts.X * gdfgiData.ProbeCounts.Y;
    const int32 gridProbesZ = gdfgiData.ProbeCounts.Z * gdfgiData.CascadesCount;
    const uint32 raysPerRow = Math::Max(gridProbesX * GDFGI_RAYS_COUNT, 1);
    const uint32 safeRayBudget = Math::Min(
            graphicsSettings->GDFGIProbeRayBudget == 0 ? 3200u : graphicsSettings->GDFGIProbeRayBudget,
            3200u);
    const uint32 scheduledRows = safeRayBudget == 0
            ? (uint32)gridProbesZ
            : Math::Clamp(safeRayBudget / raysPerRow, 1u, (uint32)gridProbesZ);
    _updateRowOffset %= (uint32)gridProbesZ;
    uint32 rowCount = Math::Min(scheduledRows, (uint32)gridProbesZ - _updateRowOffset);

    // Allocate / resize GPU textures if needed
    if (!gdfgiData.ProbesData)
    {
        GPUTextureDescription desc = GPUTextureDescription::New2D(gridProbesX, gridProbesZ, PixelFormat::R8G8B8A8_SNorm, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        gdfgiData.ProbesData = RenderTargetPool::Get(desc);
        desc = GPUTextureDescription::New2D(gridProbesX, gridProbesZ, PixelFormat::R8_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        gdfgiData.ProbeStates = RenderTargetPool::Get(desc);

        // Directional radiance textures (5x5 bins per probe)
        desc = GPUTextureDescription::New2D(gridProbesX * GDFGI_OCT_RESOLUTION, gridProbesZ * GDFGI_OCT_RESOLUTION, PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        gdfgiData.DirectionalRadianceRaw = RenderTargetPool::Get(desc);
        gdfgiData.DirectionalRadianceAvg = RenderTargetPool::Get(desc);
        gdfgiData.DirectionalRadianceHistory[0] = RenderTargetPool::Get(desc);
        gdfgiData.DirectionalRadianceHistory[1] = RenderTargetPool::Get(desc);

        // Distance moments texture (7x7 per probe with 1-texel border)
        desc = GPUTextureDescription::New2D(gridProbesX * GDFGI_OCT_TILE_SIZE, gridProbesZ * GDFGI_OCT_TILE_SIZE, PixelFormat::R16G16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        gdfgiData.ProbesDistance = RenderTargetPool::Get(desc);

        // Diffuse convolved texture (7x7 per probe with 1-texel border)
        desc = GPUTextureDescription::New2D(gridProbesX * GDFGI_OCT_TILE_SIZE, gridProbesZ * GDFGI_OCT_TILE_SIZE, PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        gdfgiData.DirectionalDiffuse = RenderTargetPool::Get(desc);

        // Clear all textures upon initialization
        context->ClearUA(gdfgiData.ProbesData, Float4::Zero);
        uint32 zeroUint[4] = {};
        context->ClearUA(gdfgiData.ProbeStates, zeroUint);
        context->ClearUA(gdfgiData.DirectionalRadianceRaw, Float4::Zero);
        context->ClearUA(gdfgiData.DirectionalRadianceAvg, Float4::Zero);
        context->ClearUA(gdfgiData.DirectionalRadianceHistory[0], Float4::Zero);
        context->ClearUA(gdfgiData.DirectionalRadianceHistory[1], Float4::Zero);
        context->ClearUA(gdfgiData.ProbesDistance, Float4::Zero);
        context->ClearUA(gdfgiData.DirectionalDiffuse, Float4::Zero);

        if (debugStage != GDFGIDebugExecutionStage::Disabled)
        {
            LOG(Info, "GDFGI allocated textures: ProbesData={}x{} ProbeStates={}x{} Radiance={}x{} Distance={}x{} Diffuse={}x{}",
                gridProbesX, gridProbesZ, gridProbesX, gridProbesZ,
                gridProbesX * GDFGI_OCT_RESOLUTION, gridProbesZ * GDFGI_OCT_RESOLUTION,
                gridProbesX * GDFGI_OCT_TILE_SIZE, gridProbesZ * GDFGI_OCT_TILE_SIZE,
                gridProbesX * GDFGI_OCT_TILE_SIZE, gridProbesZ * GDFGI_OCT_TILE_SIZE);
        }
    }

    // Staging completion buffer setup
    if (!_completionBuffer)
    {
        _completionBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GDFGI.CompletionBuffer"));
        _completionBuffer->Init(GPUBufferDescription::Typed(1, PixelFormat::R32G32B32A32_UInt, true));
    }
    if (!_completionReadbackBuffer)
    {
        Platform::MemoryClear(_completionFrames, sizeof(_completionFrames));
        _completionReadbackBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GDFGI.CompletionReadbackBuffer"));
        const GPUBufferDescription desc = GPUBufferDescription::Buffer(ARRAY_COUNT(_completionFrames) * sizeof(uint32) * 4, GPUBufferFlags::None, PixelFormat::R32_UInt, nullptr, sizeof(uint32), GPUResourceUsage::StagingReadback);
        _completionReadbackBuffer->Init(desc);
    }

    // Read the newest available ordered GPU marker without stalling. Each
    // stage copies over the same frame slot, so the value identifies the last
    // dispatch that actually completed before a later timeout/device removal.
    if (_completionCounterIndex != -1)
    {
        if (const auto markers = (const uint32*)_completionReadbackBuffer->Map(GPUResourceMapMode::Read | GPUResourceMapMode::NoWait))
        {
            const uint32* marker = markers + _completionCounterIndex * 4;
            if (debugStage != GDFGIDebugExecutionStage::Disabled)
                LOG(Info, "GDFGI GPU completion marker: frame={} stage={}", marker[0], marker[1]);
            _completionReadbackBuffer->Unmap();
        }
    }
    _completionCounterIndex = (int32)(Engine::FrameCount % ARRAY_COUNT(_completionFrames));
    _completionFrames[_completionCounterIndex] = Engine::FrameCount;
    const auto writeCompletionMarker = [&](uint32 stage)
    {
        const uint32 marker[4] = { (uint32)Engine::FrameCount, stage, 0, 0 };
        context->ClearUA(_completionBuffer, marker);
        context->CopyBuffer(_completionReadbackBuffer, _completionBuffer, sizeof(marker), _completionCounterIndex * sizeof(marker), 0);
    };
    writeCompletionMarker(0);

    // Phase 3: AllocateOnly stage exits early without executing any compute dispatch
    if (debugStage == GDFGIDebugExecutionStage::AllocateOnly)
    {
        gdfgiData.Result.ProbesData = gdfgiData.ProbesData->View();
        gdfgiData.Result.ProbeStates = gdfgiData.ProbeStates->View();
        gdfgiData.Result.ProbesDistance = gdfgiData.ProbesDistance->View();
        gdfgiData.Result.DirectionalRadiance = gdfgiData.DirectionalRadianceAvg->View();
        gdfgiData.Result.DirectionalDiffuse = gdfgiData.DirectionalDiffuse->View();
        return false;
    }

    GlobalSignDistanceFieldPass::BindingData sdfBinding;
    if (GlobalSignDistanceFieldPass::Instance()->Render(renderContext, context, sdfBinding))
        return true;

    GlobalSurfaceAtlasPass::BindingData surfaceAtlasBinding;
    if (GlobalSurfaceAtlasPass::Instance()->Render(renderContext, context, surfaceAtlasBinding))
        return true;

    const bool hasSurfaceAtlas = surfaceAtlasBinding.Chunks && surfaceAtlasBinding.CulledObjects && surfaceAtlasBinding.Objects && surfaceAtlasBinding.AtlasDepth && surfaceAtlasBinding.AtlasLighting;

    const Float3 viewPos = (Float3)renderContext.View.Position;
    float baseSpacing = graphicsSettings->GIProbesSpacing > 0 ? (float)graphicsSettings->GIProbesSpacing : 50.0f;

    // Toroidal clipmap scrolling computation
    bool cascadeSkipUpdate[4] = {};
    bool cascadeForceUpdate[4] = {};
    bool hasForcedUpdate = false;
    for (int32 c = 0; c < gdfgiData.CascadesCount; c++)
    {
        auto& cascade = gdfgiData.Cascades[c];
        float spacing = baseSpacing * Math::Pow(2.0f, (float)c);
        cascade.ProbesSpacing = spacing;
        if (cascade.ProbesOrigin == Float3::Zero && cascade.ProbeScrollOffsets == Int3::Zero)
        {
            Float3 extent = Float3(gdfgiData.ProbeCounts - 1) * spacing;
            Float3 origin = viewPos - extent * 0.5f;
            cascade.ProbesOrigin = Float3::Floor(origin / spacing) * spacing;
        }

        const Float3 volumeOrigin = cascade.ProbesOrigin + Float3(cascade.ProbeScrollOffsets) * cascade.ProbesSpacing;
        const Float3 translation = viewPos - volumeOrigin;
        cascadeForceUpdate[c] = Math::Abs(translation.X) >= cascade.ProbesSpacing ||
                                Math::Abs(translation.Y) >= cascade.ProbesSpacing ||
                                Math::Abs(translation.Z) >= cascade.ProbesSpacing;
        cascade.PendingUpdate |= cascadeForceUpdate[c];
        hasForcedUpdate |= cascadeForceUpdate[c];
    }
    if (hasForcedUpdate)
        rowCount = Math::Min(Math::Max(rowCount, 4u), (uint32)gridProbesZ - _updateRowOffset);

    const uint64 cascadeFrequencies[] = { 1, 2, 3, 4 };
    for (int32 c = 0; c < gdfgiData.CascadesCount; c++)
    {
        auto& cascade = gdfgiData.Cascades[c];
        cascade.PendingUpdate |= (gdfgiData.LastFrameUsed % cascadeFrequencies[c]) == 0;
        cascade.PendingUpdate |= hasForcedUpdate;
        cascadeSkipUpdate[c] = !cascade.PendingUpdate;
    }

    for (int32 c = 0; c < gdfgiData.CascadesCount; c++)
    {
        if (cascadeSkipUpdate[c])
            continue;
        auto& cascade = gdfgiData.Cascades[c];
        cascade.PendingUpdate = false;

        const Float3 volumeOrigin = cascade.ProbesOrigin + Float3(cascade.ProbeScrollOffsets) * cascade.ProbesSpacing;
        const Float3 translation = viewPos - volumeOrigin;
        for (int32 axis = 0; axis < 3; axis++)
        {
            const float value = translation.Raw[axis] / cascade.ProbesSpacing;
            const int32 scroll = value >= 0.0f ? (int32)Math::Floor(value) : (int32)Math::Ceil(value);
            cascade.ProbeScrollOffsets.Raw[axis] += scroll;
            cascade.ProbeScrollClears.Raw[axis] = scroll;
        }

        for (int32 axis = 0; axis < 3; axis++)
        {
            const int32 probeCount = gdfgiData.ProbeCounts.Raw[axis];
            int32& scrollOffset = cascade.ProbeScrollOffsets.Raw[axis];
            while (scrollOffset >= probeCount)
            {
                cascade.ProbesOrigin.Raw[axis] += cascade.ProbesSpacing * probeCount;
                scrollOffset -= probeCount;
            }
            while (scrollOffset <= -probeCount)
            {
                cascade.ProbesOrigin.Raw[axis] -= cascade.ProbesSpacing * probeCount;
                scrollOffset += probeCount;
            }
        }
    }

    Data0 data;
    Platform::MemoryClear(&data, sizeof(data));
    data.GDFGI.CascadesCount = gdfgiData.CascadesCount;
    data.GDFGI.ProbesCounts[0] = gdfgiData.ProbeCounts.X;
    data.GDFGI.ProbesCounts[1] = gdfgiData.ProbeCounts.Y;
    data.GDFGI.ProbesCounts[2] = gdfgiData.ProbeCounts.Z;
    data.GDFGI.HistoryFrames = gdfgiData.HistoryFrames;
    data.GDFGI.HistoryFrameIndex = gdfgiData.HistoryFrameIndex;
    data.GDFGI.DynamicInvalidation = gdfgiData.DynamicInvalidation;
    data.GDFGI.IndirectLightingIntensity = renderContext.List->Settings.GlobalIllumination.Intensity;
    data.GDFGI.RayMaxDistance = Math::Clamp(renderContext.List->Settings.GlobalIllumination.Distance, 500.0f, 100000.0f);
    data.GDFGI.ViewPos = (Float3)renderContext.View.Position;
    const Color fallback = renderContext.List->Settings.GlobalIllumination.FallbackIrradiance;
    data.GDFGI.FallbackIrradiance = Float4(fallback.R, fallback.G, fallback.B, 1.0f);
    data.GDFGI.NormalBias = renderContext.List->Settings.GlobalIllumination.NormalBias;
    data.GDFGI.ViewBias = renderContext.List->Settings.GlobalIllumination.ViewBias;
    data.GDFGI.ThinGeometryExpansion = graphicsSettings->GDFGIThinGeometryExpansion;
    data.GDFGI.EnableDirectionalSpecular = graphicsSettings->GDFGIEnableDirectionalSpecular ? 1 : 0;
    data.GDFGI.Algorithm = 2;
    data.GDFGI.UpdateRowOffset = _updateRowOffset;
    data.GDFGI.UpdateRowCount = rowCount;
    data.GDFGI.DebugExecutionStage = (uint32)debugStage;
    data.GDFGI.CulledObjectsCapacity = hasSurfaceAtlas ? surfaceAtlasBinding.CulledObjects->GetSize() / sizeof(uint32) : 0;
    data.GDFGI.ObjectsCount = hasSurfaceAtlas ? surfaceAtlasBinding.Constants.ObjectsCount : 0;
    data.GlobalSDF = sdfBinding.Constants;
    data.GlobalSurfaceAtlas = surfaceAtlasBinding.Constants;
    if (!hasSurfaceAtlas)
        data.GlobalSurfaceAtlas.ObjectsCount = 0;
    data.DynamicInvalidation = gdfgiData.DynamicInvalidation;
    data.ProbesCount = gdfgiData.ProbesCountTotal;
    data.SkyboxIntensity = renderContext.List->Sky ? renderContext.List->Sky->GetIndirectLightingIntensity() : 1.0f;
    GBufferPass::SetInputs(renderContext.View, data.GBuffer);

    // Compute localized dirty bounding boxes per cascade
    for (int32 c = 0; c < gdfgiData.CascadesCount; c++)
    {
        auto& cascade = gdfgiData.Cascades[c];
        data.GDFGI.ProbesOriginAndSpacing[c] = Float4(cascade.ProbesOrigin, cascade.ProbesSpacing);
        data.GDFGI.ProbesScrollOffsets[c] = Int4(cascade.ProbeScrollOffsets, 0);
        data.GDFGI.ProbeScrollClears[c] = Int4(cascade.ProbeScrollClears, 0);
        data.ProbeScrollClears[c] = Int4(cascade.ProbeScrollClears, 0);

        float margin = graphicsSettings->GDFGIDynamicDirtyRadiusInProbeSpacings * cascade.ProbesSpacing;
        BoundingBox cascadeDirtyBox = BoundingBox::Empty;
        for (const auto& region : gdfgiData.DirtyRegions)
        {
            BoundingBox combined = region.GetCombinedBounds();
            if (combined.Minimum.X <= combined.Maximum.X)
            {
                combined.Minimum -= Vector3(margin);
                combined.Maximum += Vector3(margin);
                BoundingBox::Merge(cascadeDirtyBox, combined, cascadeDirtyBox);
            }
        }
        if (cascadeDirtyBox.Minimum.X <= cascadeDirtyBox.Maximum.X)
        {
            data.GDFGI.CascadeDirtyBoundsMin[c] = Float4((Float3)cascadeDirtyBox.Minimum, 1.0f);
            data.GDFGI.CascadeDirtyBoundsMax[c] = Float4((Float3)cascadeDirtyBox.Maximum, 0.0f);
        }
        else
        {
            data.GDFGI.CascadeDirtyBoundsMin[c] = Float4::Zero;
            data.GDFGI.CascadeDirtyBoundsMax[c] = Float4::Zero;
        }
    }
    gdfgiData.DirtyRegions.Clear();

    context->UpdateCB(_cb0, &data);
    context->BindCB(0, _cb0);

    // 1. Classify Probes & Relocation (Phase 4)
    {
        PROFILE_GPU_CPU_NAMED("GDFGI Classify Probes");
        context->BindSR(0, sdfBinding.Texture ? sdfBinding.Texture->ViewVolume() : nullptr);
        context->BindSR(1, sdfBinding.TextureMip ? sdfBinding.TextureMip->ViewVolume() : nullptr);
        context->BindUA(0, gdfgiData.ProbesData->View());
        context->BindUA(1, gdfgiData.ProbeStates->View());
        context->Dispatch(_csClassify, Math::CeilToInt((float)gdfgiData.ProbesCountTotal / 32.0f), 1, 1);
        context->ResetUA();
        context->ResetSR();
    }
    writeCompletionMarker(1);
    for (int32 c = 0; c < gdfgiData.CascadesCount; c++)
        gdfgiData.Cascades[c].ProbeScrollClears = Int3::Zero;

    if (debugStage == GDFGIDebugExecutionStage::Classify)
    {
        gdfgiData.Result.Constants = data.GDFGI;
        gdfgiData.Result.ProbesData = gdfgiData.ProbesData->View();
        gdfgiData.Result.ProbeStates = gdfgiData.ProbeStates->View();
        gdfgiData.Result.ProbesDistance = gdfgiData.ProbesDistance->View();
        gdfgiData.Result.DirectionalRadiance = gdfgiData.DirectionalRadianceAvg->View();
        gdfgiData.Result.DirectionalDiffuse = gdfgiData.DirectionalDiffuse->View();
        return false;
    }

    // 2. Trace Directional Radiance & Distance Moments (Phase 5)
    {
        PROFILE_GPU_CPU_NAMED("GDFGI Trace Radiance");
        GPUTextureView* skybox = GBufferPass::Instance()->RenderSkybox(renderContext, context);
        context->BindSR(0, sdfBinding.Texture ? sdfBinding.Texture->ViewVolume() : nullptr);
        context->BindSR(1, sdfBinding.TextureMip ? sdfBinding.TextureMip->ViewVolume() : nullptr);
        if (hasSurfaceAtlas)
        {
            context->BindSR(2, surfaceAtlasBinding.Chunks ? surfaceAtlasBinding.Chunks->View() : nullptr);
            context->BindSR(3, surfaceAtlasBinding.CulledObjects ? surfaceAtlasBinding.CulledObjects->View() : nullptr);
            context->BindSR(4, surfaceAtlasBinding.Objects ? surfaceAtlasBinding.Objects->View() : nullptr);
            context->BindSR(5, surfaceAtlasBinding.AtlasDepth ? surfaceAtlasBinding.AtlasDepth->View() : nullptr);
            context->BindSR(6, surfaceAtlasBinding.AtlasLighting ? surfaceAtlasBinding.AtlasLighting->View() : nullptr);
        }
        context->BindSR(7, gdfgiData.ProbeStates->View());
        context->BindSR(8, gdfgiData.ProbesData->View());
        context->BindSR(9, skybox);
        context->BindUA(0, gdfgiData.DirectionalRadianceRaw->View());
        context->BindUA(1, gdfgiData.ProbesDistance->View());
        context->Dispatch(_csTraceDirectionalRadiance, Math::CeilToInt((float)(gridProbesX * GDFGI_OCT_RESOLUTION) / 8.0f), Math::CeilToInt((float)(rowCount * GDFGI_OCT_RESOLUTION) / 8.0f), 1);
        context->ResetUA();
        context->ResetSR();
    }
    writeCompletionMarker(2);

    if (debugStage == GDFGIDebugExecutionStage::Trace)
    {
        gdfgiData.Result.Constants = data.GDFGI;
        gdfgiData.Result.ProbesData = gdfgiData.ProbesData->View();
        gdfgiData.Result.ProbeStates = gdfgiData.ProbeStates->View();
        gdfgiData.Result.ProbesDistance = gdfgiData.ProbesDistance->View();
        gdfgiData.Result.DirectionalRadiance = gdfgiData.DirectionalRadianceRaw->View();
        gdfgiData.Result.DirectionalDiffuse = gdfgiData.DirectionalDiffuse->View();
        return false;
    }

    // 3. Update Temporal History Buffer (Ping-Pong) (Phase 7)
    {
        PROFILE_GPU_CPU_NAMED("GDFGI Temporal Update");
        const int32 historyNextIndex = (int32)(gdfgiData.HistoryFrameIndex % 2);
        const int32 historyPrevIndex = 1 - historyNextIndex;
        context->BindSR(0, gdfgiData.DirectionalRadianceRaw->View());
        context->BindSR(1, gdfgiData.DirectionalRadianceHistory[historyPrevIndex]->View());
        context->BindUA(0, gdfgiData.DirectionalRadianceAvg->View());
        context->BindUA(1, gdfgiData.DirectionalRadianceHistory[historyNextIndex]->View());
        context->Dispatch(_csUpdateTemporalRing, Math::CeilToInt((float)(gridProbesX * GDFGI_OCT_RESOLUTION) / 8.0f), Math::CeilToInt((float)(rowCount * GDFGI_OCT_RESOLUTION) / 8.0f), 1);
        context->ResetUA();
        context->ResetSR();
    }
    writeCompletionMarker(3);

    if (debugStage == GDFGIDebugExecutionStage::Temporal)
    {
        gdfgiData.Result.Constants = data.GDFGI;
        gdfgiData.Result.ProbesData = gdfgiData.ProbesData->View();
        gdfgiData.Result.ProbeStates = gdfgiData.ProbeStates->View();
        gdfgiData.Result.ProbesDistance = gdfgiData.ProbesDistance->View();
        gdfgiData.Result.DirectionalRadiance = gdfgiData.DirectionalRadianceAvg->View();
        gdfgiData.Result.DirectionalDiffuse = gdfgiData.DirectionalDiffuse->View();
        return false;
    }

    // 4. Convolve Diffuse (Phase 8)
    {
        PROFILE_GPU_CPU_NAMED("GDFGI Diffuse Convolution");
        context->BindSR(0, gdfgiData.DirectionalRadianceAvg->View());
        context->BindUA(0, gdfgiData.DirectionalDiffuse->View());
        context->Dispatch(_csConvolveDiffuse, Math::CeilToInt((float)(gridProbesX * GDFGI_OCT_TILE_SIZE) / 8.0f), Math::CeilToInt((float)(rowCount * GDFGI_OCT_TILE_SIZE) / 8.0f), 1);
        context->ResetUA();
        context->ResetSR();
    }
    writeCompletionMarker(4);

    // Setup result binding data
    gdfgiData.Result.Constants = data.GDFGI;
    gdfgiData.Result.ProbesData = gdfgiData.ProbesData->View();
    gdfgiData.Result.ProbeStates = gdfgiData.ProbeStates->View();
    gdfgiData.Result.ProbesDistance = gdfgiData.ProbesDistance->View();
    gdfgiData.Result.DirectionalRadiance = gdfgiData.DirectionalRadianceAvg->View();
    gdfgiData.Result.DirectionalDiffuse = gdfgiData.DirectionalDiffuse->View();

    gdfgiData.HistoryFrameIndex++;
    _updateRowOffset = (_updateRowOffset + rowCount) % (uint32)gridProbesZ;
    gdfgiData.DynamicInvalidation = 0;

    return false;
}

void GlobalDistanceFieldGIPass::RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output)
{
    auto* customBuffer = renderContext.Buffers ? renderContext.Buffers->FindCustomBuffer<GDFGICustomBuffer>(TEXT("GDFGI")) : nullptr;
    if (!customBuffer || !customBuffer->DirectionalDiffuse || !_psDebug)
        return;

    PROFILE_GPU_CPU_NAMED("GDFGI Debug");
    context->BindSR(0, renderContext.Buffers->GBuffer0->View());
    context->BindSR(1, renderContext.Buffers->GBuffer1->View());
    context->BindSR(2, renderContext.Buffers->GBuffer2->View());
    context->BindSR(3, renderContext.Buffers->DepthBuffer->View());
    context->BindSR(4, customBuffer->Result.ProbesData);
    context->BindSR(5, customBuffer->Result.ProbeStates);
    context->BindSR(6, customBuffer->Result.DirectionalDiffuse);
    context->BindSR(7, customBuffer->Result.ProbesDistance);
    context->SetViewportAndScissors(renderContext.View.ScreenSize.X, renderContext.View.ScreenSize.Y);
    context->SetRenderTarget(output ? output->View() : nullptr);
    context->SetState(_psDebug);
    context->DrawFullscreenTriangle();
}
