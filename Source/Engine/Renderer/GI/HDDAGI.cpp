// Copyright (c) Wojciech Figat. All rights reserved.

#include "HDDAGI.h"
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

GPU_CB_STRUCT(HDDAGIData0 {
    HDDAGIConstantsData HDDAGI;
    ShaderGBufferData GBuffer;
    uint32 DebugViewMode;
    uint32 CascadeIndex;
    uint32 ScheduledProbeCount;
    uint32 LightCount;
});

class HDDAGICustomBuffer : public RenderBuffers::CustomBuffer
{
public:
    struct Cascade
    {
        Float3 WorldOffset = Float3::Zero;
        float CellSize = 0.0f;

        Int3 CellPosition = Int3::Zero;
        Int3 RegionWorldOffset = Int3::Zero;
        Int3 DirtyScroll = Int3::Zero;

        uint32 MotionAxisMask = 0;
        uint32 NextRegionVersion = 1;
        bool FullDirty = true;
    } Cascades[HDDAGI_MAX_CASCADES];

    int32 CascadesCount = 4;
    int32 HistoryFrames = HDDAGI_DEFAULT_HISTORY_FRAMES;
    uint32 GlobalFrame = 0;
    Vector3 ViewOrigin = Vector3::Zero;
    Array<GlobalGIInvalidation> PendingInvalidations;

    // Occupancy textures
    GPUTexture* StaticBlockBits = nullptr;
    GPUTexture* DynamicBlockBits = nullptr;
    GPUTexture* CombinedBlockBits = nullptr;

    GPUTexture* StaticRegionBits = nullptr;
    GPUTexture* DynamicRegionBits = nullptr;
    GPUTexture* CombinedRegionBits = nullptr;
    GPUTexture* RegionVersions = nullptr;

    // Voxel material & radiance fields
    GPUTexture* VoxelMaterial = nullptr;
    GPUTexture* VoxelEmission = nullptr;
    GPUTexture* VoxelRadiance = nullptr;
    GPUTexture* VoxelRadiancePrev = nullptr;

    // Probe atlases and metadata per cascade
    GPUTexture* ProbeSpecular[HDDAGI_MAX_CASCADES] = {};
    GPUTexture* ProbeDiffuse[HDDAGI_MAX_CASCADES] = {};
    GPUTexture* ProbeDiffuseFiltered[HDDAGI_MAX_CASCADES] = {};

    GPUTexture* ProbeHistory[HDDAGI_MAX_CASCADES] = {};
    GPUTexture* ProbeRunningSum[HDDAGI_MAX_CASCADES] = {};
    GPUTexture* ProbeRayHitCache[HDDAGI_MAX_CASCADES] = {};

    GPUTexture* ProbeProcessFrame = nullptr;
    GPUTexture* ProbeGeometryProximity = nullptr;
    GPUTexture* ProbeCameraVisibility = nullptr;
    GPUTexture* ProbeNeighbourVisibility = nullptr;
    GPUTexture* Occlusion[2] = {};

    // GPU buffers
    GPUBuffer* ProbePriorityList = nullptr;
    GPUBuffer* LightsBuffer = nullptr;

    HDDAGIPass::BindingData Result;

    FORCE_INLINE void Release()
    {
        SAFE_DELETE_GPU_RESOURCE(StaticBlockBits);
        SAFE_DELETE_GPU_RESOURCE(DynamicBlockBits);
        SAFE_DELETE_GPU_RESOURCE(CombinedBlockBits);

        SAFE_DELETE_GPU_RESOURCE(StaticRegionBits);
        SAFE_DELETE_GPU_RESOURCE(DynamicRegionBits);
        SAFE_DELETE_GPU_RESOURCE(CombinedRegionBits);
        SAFE_DELETE_GPU_RESOURCE(RegionVersions);

        SAFE_DELETE_GPU_RESOURCE(VoxelMaterial);
        SAFE_DELETE_GPU_RESOURCE(VoxelEmission);
        SAFE_DELETE_GPU_RESOURCE(VoxelRadiance);
        SAFE_DELETE_GPU_RESOURCE(VoxelRadiancePrev);

        for (int32 c = 0; c < HDDAGI_MAX_CASCADES; c++)
        {
            SAFE_DELETE_GPU_RESOURCE(ProbeSpecular[c]);
            SAFE_DELETE_GPU_RESOURCE(ProbeDiffuse[c]);
            SAFE_DELETE_GPU_RESOURCE(ProbeDiffuseFiltered[c]);
            SAFE_DELETE_GPU_RESOURCE(ProbeHistory[c]);
            SAFE_DELETE_GPU_RESOURCE(ProbeRunningSum[c]);
            SAFE_DELETE_GPU_RESOURCE(ProbeRayHitCache[c]);
        }

        SAFE_DELETE_GPU_RESOURCE(ProbeProcessFrame);
        SAFE_DELETE_GPU_RESOURCE(ProbeGeometryProximity);
        SAFE_DELETE_GPU_RESOURCE(ProbeCameraVisibility);
        SAFE_DELETE_GPU_RESOURCE(ProbeNeighbourVisibility);
        SAFE_DELETE_GPU_RESOURCE(Occlusion[0]);
        SAFE_DELETE_GPU_RESOURCE(Occlusion[1]);

        SAFE_DELETE_GPU_RESOURCE(ProbePriorityList);
        SAFE_DELETE_GPU_RESOURCE(LightsBuffer);
    }

    ~HDDAGICustomBuffer()
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
            cascade.WorldOffset -= (Float3)delta;
            cascade.FullDirty = true;
        }
    }
};

String HDDAGIPass::ToString() const
{
    return TEXT("HDDAGIPass");
}

bool HDDAGIPass::Init()
{
    const auto device = GPUDevice::Instance;
    _supported = device->GetFeatureLevel() >= FeatureLevel::SM5 && device->Limits.HasCompute && device->Limits.HasTypedUAVLoad;
    if (_voxelization.Init())
        return true;
    return false;
}

void HDDAGIPass::Dispose()
{
    _ownerTask = nullptr;
    _voxelization.Dispose();
    RendererPass::Dispose();
    _shaderPreprocess = nullptr;
    _shaderIntegrate = nullptr;
    _shaderDirectLight = nullptr;
    _shaderDebug = nullptr;
    SAFE_DELETE_GPU_RESOURCE(_cb0);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
}

bool HDDAGIPass::setupResources()
{
    if (!_supported)
        return true;

    if (_voxelization.Init())
        return true;

    if (!_cb0)
    {
        _cb0 = GPUDevice::Instance->CreateConstantBuffer(sizeof(HDDAGIData0), TEXT("HDDAGI.CB0"));
        if (!_cb0)
            return true;
    }

    if (!_shaderPreprocess)
    {
        _shaderPreprocess = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/HDDAGIPreprocess"));
        if (!_shaderPreprocess)
            return true;
    }

    if (!_shaderIntegrate)
    {
        _shaderIntegrate = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/HDDAGIIntegrate"));
        if (!_shaderIntegrate)
            return true;
    }

    if (!_shaderDirectLight)
    {
        _shaderDirectLight = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/HDDAGIDirectLight"));
        if (!_shaderDirectLight)
            return true;
    }

    if (!_shaderDebug)
    {
        _shaderDebug = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/HDDAGIDebug"));
        if (!_shaderDebug)
            return true;
    }

    if (!_shaderDebug->IsLoaded() || !_shaderIntegrate->IsLoaded() || !_shaderDirectLight->IsLoaded() || !_shaderPreprocess->IsLoaded())
        return true;

    auto shaderDebug = _shaderDebug->GetShader();
    if (!_psIndirectLighting)
    {
        _psIndirectLighting = GPUDevice::Instance->CreatePipelineState();
        GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
        psDesc.PS = shaderDebug->GetPS("PS_IndirectLighting");
        psDesc.BlendMode = BlendingMode::Add;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RGB;
        if (_psIndirectLighting->Init(psDesc))
            return true;
    }

    return false;
}

bool HDDAGIPass::Get(const RenderBuffers* buffers, BindingData& result)
{
    if (!buffers)
        return true;
    const auto customBuffer = buffers->FindCustomBuffer<HDDAGICustomBuffer>(TEXT("HDDAGI"));
    if (!customBuffer || !customBuffer->ProbeDiffuse[0])
        return true;
    result = customBuffer->Result;
    return false;
}

void HDDAGIPass::QueueInvalidation(RenderBuffers* buffers, const GlobalGIInvalidation& invalidation)
{
    if (!buffers)
        return;
    auto customBuffer = buffers->GetCustomBuffer<HDDAGICustomBuffer>(TEXT("HDDAGI"));
    if (customBuffer)
    {
        customBuffer->PendingInvalidations.Add(invalidation);
    }
}

bool HDDAGIPass::UpdateCascades(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    const auto graphicsSettings = GraphicsSettings::Get();
    data.CascadesCount = Math::Clamp((int32)graphicsSettings->HDDAGICascadeCount, 1, HDDAGI_MAX_CASCADES);
    data.HistoryFrames = Math::Clamp((int32)graphicsSettings->HDDAGIHistoryFrames, 1, 32);

    float baseCellSize = graphicsSettings->HDDAGIMinCellSize > 0.1f ? graphicsSettings->HDDAGIMinCellSize : 10.0f;
    Vector3 cameraPos = renderContext.View.Position;

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        auto& cascade = data.Cascades[c];
        cascade.CellSize = baseCellSize * (float)(1 << c);
        float cascadeExtent = cascade.CellSize * (float)HDDAGI_CASCADE_SIZE_X;

        // Snap cascade center to 8-voxel region boundaries
        float regionStep = cascade.CellSize * (float)HDDAGI_REGION_SIZE;
        Int3 targetRegionCoord = Int3(
            Math::FloorToInt((float)cameraPos.X / regionStep),
            Math::FloorToInt((float)cameraPos.Y / regionStep),
            Math::FloorToInt((float)cameraPos.Z / regionStep)
        );

        if (cascade.FullDirty || cascade.RegionWorldOffset != targetRegionCoord)
        {
            cascade.RegionWorldOffset = targetRegionCoord;
            cascade.WorldOffset = Float3(
                (float)targetRegionCoord.X * regionStep - cascadeExtent * 0.5f,
                (float)targetRegionCoord.Y * regionStep - (cascade.CellSize * (float)HDDAGI_CASCADE_SIZE_Y) * 0.5f,
                (float)targetRegionCoord.Z * regionStep - cascadeExtent * 0.5f
            );
            cascade.NextRegionVersion++;
            cascade.FullDirty = false;
        }
    }

    return false;
}

bool HDDAGIPass::BuildDirtyRegionList(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    data.PendingInvalidations.Clear();
    return false;
}

bool HDDAGIPass::VoxelizeDirtyRegions(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    return false;
}

bool HDDAGIPass::BuildOccupancyHierarchy(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    if (!_shaderPreprocess || !_shaderPreprocess->IsLoaded())
        return true;
    auto shader = _shaderPreprocess->GetShader();
    auto cs = shader->GetCS("CS_BuildHierarchy");
    if (!cs)
        return true;

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        cbData.HDDAGI = data.Result.Constants;
        cbData.CascadeIndex = c;
        cbData.DebugViewMode = 2; // Combined
        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindUA(0, data.CombinedBlockBits->ViewVolume());
        context->BindUA(1, data.CombinedRegionBits->ViewVolume());
        context->BindUA(2, data.RegionVersions->ViewVolume());

        context->BindSR(0, _voxelization.VoxelNormalBitsScratch ? _voxelization.VoxelNormalBitsScratch->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(1, (GPUResourceView*)nullptr);

        context->Dispatch(cs, HDDAGI_CASCADE_SIZE_X / 8, HDDAGI_CASCADE_SIZE_Y / 8, HDDAGI_CASCADE_SIZE_Z / 8);
    }

    context->ResetUA();
    context->ResetSR();
    return false;
}

bool HDDAGIPass::ReconstructVoxelMaterials(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    return false;
}

bool HDDAGIPass::UpdateVoxelLighting(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    if (!_shaderDirectLight || !_shaderDirectLight->IsLoaded())
        return true;

    auto shader = _shaderDirectLight->GetShader();
    auto cs = shader->GetCS("CS_InjectLights");
    if (!cs)
        return true;

    // Collect active scene lights
    HDDAGILightData lights[32];
    uint32 lightCount = 0;

    for (int32 i = 0; i < renderContext.List->DirectionalLights.Count() && lightCount < 32; i++)
    {
        const auto& dirLight = renderContext.List->DirectionalLights[i];
        lights[lightCount].Color = dirLight.Color;
        lights[lightCount].Energy = dirLight.IndirectLightingIntensity;
        lights[lightCount].Direction = dirLight.Direction;
        lights[lightCount].HasShadow = dirLight.CastVolumetricShadow ? 1 : 0;
        lights[lightCount].Position = Float3::Zero;
        lights[lightCount].Radius = 1e6f;
        lights[lightCount].Type = 0;
        lights[lightCount].SpotCos = 0.0f;
        lights[lightCount].SpotInvAttenuation = 0.0f;
        lights[lightCount].Padding = 0.0f;
        lightCount++;
    }

    for (int32 i = 0; i < renderContext.List->PointLights.Count() && lightCount < 32; i++)
    {
        const auto& ptLight = renderContext.List->PointLights[i];
        lights[lightCount].Color = ptLight.Color;
        lights[lightCount].Energy = ptLight.IndirectLightingIntensity;
        lights[lightCount].Direction = Float3::UnitY;
        lights[lightCount].HasShadow = 0;
        lights[lightCount].Position = ptLight.Position;
        lights[lightCount].Radius = ptLight.Radius;
        lights[lightCount].Type = 1;
        lights[lightCount].SpotCos = 0.0f;
        lights[lightCount].SpotInvAttenuation = 0.0f;
        lights[lightCount].Padding = 0.0f;
        lightCount++;
    }

    for (int32 i = 0; i < renderContext.List->SpotLights.Count() && lightCount < 32; i++)
    {
        const auto& spLight = renderContext.List->SpotLights[i];
        lights[lightCount].Color = spLight.Color;
        lights[lightCount].Energy = spLight.IndirectLightingIntensity;
        lights[lightCount].Direction = spLight.Direction;
        lights[lightCount].HasShadow = 0;
        lights[lightCount].Position = spLight.Position;
        lights[lightCount].Radius = spLight.Radius;
        lights[lightCount].Type = 2;
        lights[lightCount].SpotCos = spLight.CosOuterCone;
        lights[lightCount].SpotInvAttenuation = spLight.InvCosConeDifference;
        lights[lightCount].Padding = 0.0f;
        lightCount++;
    }

    if (!data.LightsBuffer)
    {
        GPUBufferDescription desc = GPUBufferDescription::Structured(32, (int32)sizeof(HDDAGILightData));
        data.LightsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("HDDAGI.LightsBuffer"));
        data.LightsBuffer->Init(desc);
    }
    if (lightCount > 0)
    {
        context->UpdateBuffer(data.LightsBuffer, lights, sizeof(HDDAGILightData) * lightCount);
    }

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        cbData.HDDAGI = data.Result.Constants;
        cbData.CascadeIndex = c;
        cbData.LightCount = lightCount;
        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindUA(0, data.VoxelRadiance->ViewVolume());
        context->BindSR(0, data.LightsBuffer->View());
        context->BindSR(1, data.VoxelMaterial ? data.VoxelMaterial->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(2, data.VoxelEmission ? data.VoxelEmission->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(3, data.VoxelRadiancePrev ? data.VoxelRadiancePrev->ViewVolume() : (GPUResourceView*)nullptr);

        context->Dispatch(cs, HDDAGI_CASCADE_SIZE_X / 8, HDDAGI_CASCADE_SIZE_Y / 8, HDDAGI_CASCADE_SIZE_Z / 8);
    }

    context->ResetUA();
    context->ResetSR();
    return false;
}

bool HDDAGIPass::UpdateProbeMetadata(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    if (!_shaderPreprocess || !_shaderPreprocess->IsLoaded())
        return true;
    auto shader = _shaderPreprocess->GetShader();
    auto cs = shader->GetCS("CS_UpdateProbeMetadata");
    if (!cs)
        return true;

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        cbData.HDDAGI = data.Result.Constants;
        cbData.CascadeIndex = c;
        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindUA(0, data.ProbeGeometryProximity->ViewVolume());
        context->BindUA(1, data.ProbeCameraVisibility->ViewVolume());
        context->BindUA(2, data.ProbeNeighbourVisibility->ViewVolume());

        context->BindSR(2, data.CombinedRegionBits->ViewVolume());

        context->Dispatch(cs, Math::DivideAndRoundUp(HDDAGI_PROBES_X, 8), Math::DivideAndRoundUp(HDDAGI_PROBES_Y, 8), 1);
    }

    context->ResetUA();
    context->ResetSR();
    return false;
}

bool HDDAGIPass::BuildProbeSchedule(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    if (!_shaderPreprocess || !_shaderPreprocess->IsLoaded())
        return true;
    auto shader = _shaderPreprocess->GetShader();
    auto cs = shader->GetCS("CS_BuildProbePriorityList");
    if (!cs)
        return true;

    if (!data.ProbePriorityList)
    {
        GPUBufferDescription desc = GPUBufferDescription::Structured(HDDAGI_PROBES_X * HDDAGI_PROBES_Y * HDDAGI_PROBES_Z, (int32)sizeof(HDDAGIProbeScheduleEntry), true);
        data.ProbePriorityList = GPUDevice::Instance->CreateBuffer(TEXT("HDDAGI.ProbePriorityList"));
        data.ProbePriorityList->Init(desc);
    }

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        cbData.HDDAGI = data.Result.Constants;
        cbData.CascadeIndex = c;
        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindUA(0, data.ProbePriorityList->View());

        context->BindSR(3, data.ProbeProcessFrame->ViewVolume());
        context->BindSR(4, data.ProbeGeometryProximity->ViewVolume());
        context->BindSR(5, data.ProbeCameraVisibility->ViewVolume());

        uint32 totalProbes = HDDAGI_PROBES_X * HDDAGI_PROBES_Y * HDDAGI_PROBES_Z;
        context->Dispatch(cs, Math::DivideAndRoundUp(totalProbes, 64u), 1, 1);
    }

    context->ResetUA();
    context->ResetSR();
    return false;
}

bool HDDAGIPass::IntegrateScheduledProbes(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    if (!_shaderIntegrate || !_shaderIntegrate->IsLoaded())
        return true;

    auto shader = _shaderIntegrate->GetShader();
    auto cs = shader->GetCS("CS_IntegrateProbes");
    if (!cs)
        return true;

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        if (!data.ProbeDiffuse[c])
            continue;

        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        cbData.HDDAGI = data.Result.Constants;
        cbData.CascadeIndex = c;
        cbData.ScheduledProbeCount = 0; // Grid mode
        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindUA(0, data.ProbeSpecular[c]->View());
        context->BindUA(1, data.ProbeDiffuse[c]->View());
        context->BindUA(2, data.ProbeHistory[c]->View());
        context->BindUA(3, data.ProbeRunningSum[c]->View());

        context->BindSR(0, (GPUResourceView*)nullptr);
        context->BindSR(1, data.CombinedBlockBits ? data.CombinedBlockBits->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(2, data.CombinedRegionBits ? data.CombinedRegionBits->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(3, data.RegionVersions ? data.RegionVersions->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(4, data.VoxelRadiance ? data.VoxelRadiance->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(5, (GPUResourceView*)nullptr);

        context->Dispatch(cs, HDDAGI_PROBES_X, HDDAGI_PROBES_Y, HDDAGI_PROBES_Z);
    }

    context->ResetUA();
    context->ResetSR();
    return false;
}

bool HDDAGIPass::FilterProbes(RenderContext& renderContext, GPUContext* context, HDDAGICustomBuffer& data)
{
    return false;
}

void HDDAGIPass::PublishBindings(RenderContext& renderContext, HDDAGICustomBuffer& data)
{
    auto& res = data.Result;
    Platform::MemoryClear(&res.Constants, sizeof(res.Constants));

    res.Constants.CascadesCount = data.CascadesCount;
    res.Constants.HistoryFrames = data.HistoryFrames;
    res.Constants.GridSize = Int3(HDDAGI_CASCADE_SIZE_X, HDDAGI_CASCADE_SIZE_Y, HDDAGI_CASCADE_SIZE_Z);
    res.Constants.ProbeAxisSize = Int3(HDDAGI_PROBES_X, HDDAGI_PROBES_Y, HDDAGI_PROBES_Z);
    res.Constants.ViewPosition = (Float3)renderContext.View.Position;
    res.Constants.IndirectLightingIntensity = renderContext.List->Settings.GlobalIllumination.Intensity;
    res.Constants.BounceFeedback = GraphicsSettings::Get()->HDDAGIBounceFeedback;
    res.Constants.NormalBias = renderContext.List->Settings.GlobalIllumination.NormalBias;
    res.Constants.FallbackIrradiance = (Float4)renderContext.List->Settings.GlobalIllumination.FallbackIrradiance;
    res.Constants.Energy = 1.0f;
    res.Constants.GlobalFrame = data.GlobalFrame;

    for (int32 c = 0; c < data.CascadesCount; c++)
    {
        auto& src = data.Cascades[c];
        auto& dst = res.Constants.Cascades[c];
        dst.WorldOffset = src.WorldOffset;
        dst.CellSize = src.CellSize;
        dst.RegionWorldOffset = src.RegionWorldOffset;
        dst.WorldExtent = Float3(src.CellSize * (float)HDDAGI_CASCADE_SIZE_X, src.CellSize * (float)HDDAGI_CASCADE_SIZE_Y, src.CellSize * (float)HDDAGI_CASCADE_SIZE_Z);
        dst.ToCell = 1.0f / Math::Max(src.CellSize, 1e-4f);
    }

    if (data.ProbeDiffuse[0])
        res.ProbeDiffuse = data.ProbeDiffuse[0]->View();
    if (data.ProbeSpecular[0])
        res.ProbeSpecular = data.ProbeSpecular[0]->View();
    if (data.Occlusion[0])
        res.Occlusion0 = data.Occlusion[0]->ViewVolume();
    if (data.Occlusion[1])
        res.Occlusion1 = data.Occlusion[1]->ViewVolume();
}

bool HDDAGIPass::Render(RenderContext& renderContext, GPUContext* context, GPUTextureView* lightBuffer)
{
    if (setupResources())
        return true;

    auto& data = *renderContext.Buffers->GetCustomBuffer<HDDAGICustomBuffer>(TEXT("HDDAGI"));
    data.Rebase(renderContext.View.Origin);

    PROFILE_GPU_CPU_NAMED("HDDAGI");

    // 1. Allocate / verify persistent textures
    const int32 cascades = Math::Clamp((int32)GraphicsSettings::Get()->HDDAGICascadeCount, 1, HDDAGI_MAX_CASCADES);
    if (!data.ProbeDiffuse[0] || data.CascadesCount != cascades)
    {
        data.CascadesCount = cascades;
        int32 probeAtlasWidth = HDDAGI_PROBES_X * HDDAGI_PROBES_Z * HDDAGI_OCT_TILE_SIZE; // 17*17*7 = 2023
        int32 probeAtlasHeight = HDDAGI_PROBES_Y * HDDAGI_OCT_TILE_SIZE; // 9*7 = 63

        int32 binAtlasWidth = HDDAGI_PROBES_X * HDDAGI_PROBES_Z * HDDAGI_OCT_SIZE; // 17*17*5 = 1445
        int32 binAtlasHeight = HDDAGI_PROBES_Y * HDDAGI_OCT_SIZE; // 9*5 = 45

        for (int32 c = 0; c < cascades; c++)
        {
            GPUTextureDescription diffuseDesc = GPUTextureDescription::New2D(probeAtlasWidth, probeAtlasHeight, PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess | GPUTextureFlags::RenderTarget);
            data.ProbeDiffuse[c] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeDiffuse"));
            data.ProbeDiffuse[c]->Init(diffuseDesc);

            data.ProbeSpecular[c] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeSpecular"));
            data.ProbeSpecular[c]->Init(diffuseDesc);

            GPUTextureDescription histDesc = GPUTextureDescription::New2D(binAtlasWidth, binAtlasHeight * HDDAGI_MAX_HISTORY_FRAMES, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
            data.ProbeHistory[c] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeHistory"));
            data.ProbeHistory[c]->Init(histDesc);

            GPUTextureDescription sumDesc = GPUTextureDescription::New2D(binAtlasWidth, binAtlasHeight, PixelFormat::R32G32B32A32_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
            data.ProbeRunningSum[c] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeRunningSum"));
            data.ProbeRunningSum[c]->Init(sumDesc);

            GPUTextureDescription rayCacheDesc = GPUTextureDescription::New2D(binAtlasWidth, binAtlasHeight, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
            data.ProbeRayHitCache[c] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeRayHitCache"));
            data.ProbeRayHitCache[c]->Init(rayCacheDesc);
        }

        GPUTextureDescription procFrameDesc = GPUTextureDescription::New3D(HDDAGI_PROBES_X, HDDAGI_PROBES_Y, HDDAGI_PROBES_Z, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.ProbeProcessFrame = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeProcessFrame"));
        data.ProbeProcessFrame->Init(procFrameDesc);

        data.ProbeGeometryProximity = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeGeometryProximity"));
        data.ProbeGeometryProximity->Init(procFrameDesc);

        data.ProbeCameraVisibility = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeCameraVisibility"));
        data.ProbeCameraVisibility->Init(procFrameDesc);

        data.ProbeNeighbourVisibility = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.ProbeNeighbourVisibility"));
        data.ProbeNeighbourVisibility->Init(procFrameDesc);

        GPUTextureDescription occDesc = GPUTextureDescription::New3D(HDDAGI_PROBES_X, HDDAGI_PROBES_Y, HDDAGI_PROBES_Z, PixelFormat::R8G8B8A8_UNorm, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.Occlusion[0] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.Occlusion0"));
        data.Occlusion[0]->Init(occDesc);
        data.Occlusion[1] = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.Occlusion1"));
        data.Occlusion[1]->Init(occDesc);

        GPUTextureDescription regionDesc = GPUTextureDescription::New3D(16, 8, 16, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.CombinedRegionBits = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.CombinedRegionBits"));
        data.CombinedRegionBits->Init(regionDesc);

        GPUTextureDescription blockDesc = GPUTextureDescription::New3D(32 * 2, 16, 32, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.CombinedBlockBits = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.CombinedBlockBits"));
        data.CombinedBlockBits->Init(blockDesc);

        GPUTextureDescription versionDesc = GPUTextureDescription::New3D(16, 8, 16, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.RegionVersions = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.RegionVersions"));
        data.RegionVersions->Init(versionDesc);

        GPUTextureDescription radianceDesc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X, HDDAGI_CASCADE_SIZE_Y, HDDAGI_CASCADE_SIZE_Z, PixelFormat::R16G16B16A16_Float, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.VoxelRadiance = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelRadiance"));
        data.VoxelRadiance->Init(radianceDesc);
        data.VoxelRadiancePrev = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelRadiancePrev"));
        data.VoxelRadiancePrev->Init(radianceDesc);

        GPUTextureDescription matDesc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X, HDDAGI_CASCADE_SIZE_Y, HDDAGI_CASCADE_SIZE_Z, PixelFormat::R8G8B8A8_UNorm, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.VoxelMaterial = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelMaterial"));
        data.VoxelMaterial->Init(matDesc);

        GPUTextureDescription emissDesc = GPUTextureDescription::New3D(HDDAGI_CASCADE_SIZE_X, HDDAGI_CASCADE_SIZE_Y, HDDAGI_CASCADE_SIZE_Z, PixelFormat::R32_UInt, GPUTextureFlags::ShaderResource | GPUTextureFlags::UnorderedAccess);
        data.VoxelEmission = GPUDevice::Instance->CreateTexture(TEXT("HDDAGI.VoxelEmission"));
        data.VoxelEmission->Init(emissDesc);

        // Clear newly created textures to fallback irradiance
        Color fallbackColor = renderContext.List->Settings.GlobalIllumination.FallbackIrradiance;
        for (int32 c = 0; c < cascades; c++)
        {
            context->Clear(data.ProbeDiffuse[c]->View(), fallbackColor);
            context->Clear(data.ProbeSpecular[c]->View(), fallbackColor);
        }
        context->Clear(data.VoxelRadiance->ViewVolume(), fallbackColor);
    }

    // 2. Update cascade transforms and region snapping
    UpdateCascades(renderContext, context, data);

    // 3. Process dirty invalidations
    BuildDirtyRegionList(renderContext, context, data);

    // 4. Voxelize scene geometry in dirty regions
    VoxelizeDirtyRegions(renderContext, context, data);

    // 5. Build occupancy hierarchy
    BuildOccupancyHierarchy(renderContext, context, data);

    // 6. Reconstruct voxel material properties
    ReconstructVoxelMaterials(renderContext, context, data);

    // 7. Inject direct lighting and multibounce
    UpdateVoxelLighting(renderContext, context, data);

    // 8. Update probe metadata & priority schedule
    UpdateProbeMetadata(renderContext, context, data);
    BuildProbeSchedule(renderContext, context, data);

    // 9. Integrate probes with HDDA ray tracing & temporal ring
    IntegrateScheduledProbes(renderContext, context, data);

    // 10. Optional probe spatial filtering
    if (GraphicsSettings::Get()->HDDAGIEnableProbeFilter)
    {
        FilterProbes(renderContext, context, data);
    }

    // 11. Publish binding data for materials & composite
    PublishBindings(renderContext, data);

    // 12. Render indirect lighting into lightBuffer if provided
    if (lightBuffer && data.ProbeDiffuse[0])
    {
        HDDAGIData0 cbData;
        Platform::MemoryClear(&cbData, sizeof(cbData));
        GBufferPass::SetInputs(renderContext.View, cbData.GBuffer);
        cbData.HDDAGI = data.Result.Constants;
        cbData.DebugViewMode = 0;
        cbData.CascadeIndex = 0;

        context->UpdateCB(_cb0, &cbData);
        context->BindCB(0, _cb0);

        context->BindSR(0, renderContext.Buffers->GBuffer0->View());
        context->BindSR(1, renderContext.Buffers->GBuffer1->View());
        context->BindSR(2, renderContext.Buffers->GBuffer2->View());
        context->BindSR(3, renderContext.Buffers->DepthBuffer->View());
        context->BindSR(4, data.ProbeDiffuse[0]->View());
        context->BindSR(5, data.ProbeSpecular[0]->View());
        context->BindSR(6, data.CombinedRegionBits->ViewVolume());
        context->BindSR(7, data.RegionVersions->ViewVolume());
        context->BindSR(8, data.VoxelRadiance ? data.VoxelRadiance->ViewVolume() : (GPUResourceView*)nullptr);
        context->BindSR(9, data.Occlusion[0]->ViewVolume());
        context->BindSR(10, data.Occlusion[1]->ViewVolume());

        context->SetViewportAndScissors(renderContext.View.ScreenSize.X, renderContext.View.ScreenSize.Y);
        context->SetRenderTarget(lightBuffer);
        context->SetState(_psIndirectLighting);
        context->DrawFullscreenTriangle();

        context->ResetRenderTarget();
        context->ResetSR();
        context->ResetUA();
        context->SetViewportAndScissors(renderContext.View.ScreenSize.X, renderContext.View.ScreenSize.Y);
    }

    data.GlobalFrame++;
    return false;
}

void HDDAGIPass::RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output)
{
}
