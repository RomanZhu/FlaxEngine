// Copyright (c) Wojciech Figat. All rights reserved.

#include "ExponentialHeightFog.h"
#include "DirectionalLight.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Level/Scene/SceneRendering.h"

ExponentialHeightFog::ExponentialHeightFog(const SpawnParams& params)
    : Actor(params)
{
    _drawNoCulling = 1;
    _drawCategory = SceneRendering::PreRender;

    // Load shader
    _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Fog"));
    if (_shader == nullptr)
    {
        LOG(Fatal, "Cannot load fog shader.");
    }
#if COMPILE_WITH_DEV_ENV
    _shader.Get()->OnReloading.Bind<ExponentialHeightFog, &ExponentialHeightFog::OnShaderReloading>(this);
#endif
}

void ExponentialHeightFog::Draw(RenderContext& renderContext)
{
    // Render only when shader is valid and fog can be rendered
    // Do not render exponential fog in orthographic views
    if (EnumHasAnyFlags(renderContext.View.Flags, ViewFlags::Fog)
        && EnumHasAnyFlags(renderContext.View.Pass, DrawPass::GBuffer)
        && _shader
        && _shader->IsLoaded()
        && renderContext.View.IsPerspectiveProjection())
    {
        if (_psFog.States[0] == nullptr)
            _psFog.CreatePipelineStates();
        if (!_psFog.States[0]->IsValid())
        {
            GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
            psDesc.DepthWriteEnable = false;
            psDesc.BlendMode.BlendEnable = true;
            psDesc.BlendMode.SrcBlend = BlendingMode::Blend::One;
            psDesc.BlendMode.DestBlend = BlendingMode::Blend::SrcAlpha;
            psDesc.BlendMode.BlendOp = BlendingMode::Operation::Add;
            psDesc.BlendMode.SrcBlendAlpha = BlendingMode::Blend::One;
            psDesc.BlendMode.DestBlendAlpha = BlendingMode::Blend::Zero;
            psDesc.BlendMode.BlendOpAlpha = BlendingMode::Operation::Add;
            psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RGB;
            if (_psFog.Create(psDesc, _shader->GetShader(), "PS_Fog"))
            {
                LOG(Warning, "Cannot create graphics pipeline state object for '{0}'.", ToString());
                return;
            }
        }

        // Register for Fog Pass
        renderContext.List->Fog.Init(renderContext.View, this);
    }
}

void ExponentialHeightFog::Serialize(SerializeStream& stream, const void* otherObj)
{
    // Base
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(ExponentialHeightFog);

    SERIALIZE(FogDensity);
    SERIALIZE(FogHeightFalloff);
    SERIALIZE(FogInscatteringColor);
    SERIALIZE(FogMaxOpacity);
    SERIALIZE(StartDistance);
    SERIALIZE(FogCutoffDistance);

    SERIALIZE(DirectionalInscatteringLight);
    SERIALIZE(DirectionalInscatteringExponent);
    SERIALIZE(DirectionalInscatteringStartDistance);
    SERIALIZE(DirectionalInscatteringColor);

    SERIALIZE(VolumetricFogEnable);
    SERIALIZE(VolumetricFogScatteringDistribution);
    SERIALIZE(VolumetricFogIndependentScatteringEnable);
    SERIALIZE(VolumetricFogScatteringIntensity);
    SERIALIZE(VolumetricFogDualLobePhaseEnable);
    SERIALIZE(VolumetricFogForwardScatteringWeight);
    SERIALIZE(VolumetricFogBackwardScatteringDistribution);
    SERIALIZE(VolumetricFogBackwardScatteringWeight);
    SERIALIZE(VolumetricFogPhaseDirectionality);
    SERIALIZE(VolumetricFogShadowPresentationEnable);
    SERIALIZE(VolumetricFogShadowContrast);
    SERIALIZE(VolumetricFogShadowExtinctionMultiplier);
    SERIALIZE(VolumetricFogShadowScatteringMultiplier);
    SERIALIZE(VolumetricFogMinimumAmbientScattering);
    SERIALIZE(VolumetricFogDirectionalShadowStrength);
    SERIALIZE(VolumetricFogAlbedo);
    SERIALIZE(VolumetricFogEmissive);
    SERIALIZE(VolumetricFogExtinctionScale);
    SERIALIZE(VolumetricFogDistance);
    SERIALIZE(VolumetricFogDistanceFade);
    SERIALIZE(VolumetricFogTemporalReprojection);
    SERIALIZE(VolumetricFogHistoryWeight);
    SERIALIZE(VolumetricFogReactiveHistory);
    SERIALIZE(VolumetricFogReactiveHistoryVelocityScale);
    SERIALIZE(VolumetricFogLocalHistoryRejectionEnable);
    SERIALIZE(VolumetricFogHistoryExtinctionDifferenceThreshold);
    SERIALIZE(VolumetricFogHistoryNeighborhoodClampStrength);
    SERIALIZE(VolumetricFogHistoryCameraMotionResponse);
    SERIALIZE(VolumetricFogMinimumHistoryWeight);
    SERIALIZE(VolumetricFogSecondLayerEnable);
    SERIALIZE(VolumetricFogSecondLayerDensity);
    SERIALIZE(VolumetricFogSecondLayerHeightOffset);
    SERIALIZE(VolumetricFogSecondLayerHeightFalloff);
    SERIALIZE(VolumetricFogSecondLayerDensityNoiseInfluence);
    SERIALIZE(VolumetricFogDensityNoiseEnable);
    SERIALIZE(VolumetricFogDensityNoiseScale);
    SERIALIZE(VolumetricFogDensityNoiseOctaves);
    SERIALIZE(VolumetricFogDensityNoiseLacunarity);
    SERIALIZE(VolumetricFogDensityNoiseGain);
    SERIALIZE(VolumetricFogDensityNoiseMin);
    SERIALIZE(VolumetricFogDensityNoiseMax);
    SERIALIZE(VolumetricFogDensityNoiseInfluence);
    SERIALIZE(VolumetricFogDensityNoiseVelocity);
    SERIALIZE(VolumetricFogDensityNoiseSeed);
    SERIALIZE(VolumetricFogDensityNoiseDecorrelateOctaves);
    SERIALIZE(VolumetricFogDensityNoiseShapingEnable);
    SERIALIZE(VolumetricFogDensityNoiseInvert);
    SERIALIZE(VolumetricFogDensityNoiseContrast);
    SERIALIZE(VolumetricFogDensityNoiseHeightEnable);
    SERIALIZE(VolumetricFogDensityNoiseHeightFalloff);
    SERIALIZE(VolumetricFogDensityNoiseHeightMinimumInfluence);
    SERIALIZE(VolumetricFogNearClarityEnable);
    SERIALIZE(VolumetricFogNearClarityRadius);
    SERIALIZE(VolumetricFogNearClarityFadeDistance);
    SERIALIZE(VolumetricFogNearClarityMinimumDensity);
    SERIALIZE(VolumetricFogDebug);
}

void ExponentialHeightFog::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // Base
    Actor::Deserialize(stream, modifier);

    DESERIALIZE(FogDensity);
    DESERIALIZE(FogHeightFalloff);
    DESERIALIZE(FogInscatteringColor);
    DESERIALIZE(FogMaxOpacity);
    DESERIALIZE(StartDistance);
    DESERIALIZE(FogCutoffDistance);

    DESERIALIZE(DirectionalInscatteringLight);
    DESERIALIZE(DirectionalInscatteringExponent);
    DESERIALIZE(DirectionalInscatteringStartDistance);
    DESERIALIZE(DirectionalInscatteringColor);

    DESERIALIZE(VolumetricFogEnable);
    DESERIALIZE(VolumetricFogScatteringDistribution);
    DESERIALIZE(VolumetricFogIndependentScatteringEnable);
    DESERIALIZE(VolumetricFogScatteringIntensity);
    DESERIALIZE(VolumetricFogDualLobePhaseEnable);
    DESERIALIZE(VolumetricFogForwardScatteringWeight);
    DESERIALIZE(VolumetricFogBackwardScatteringDistribution);
    DESERIALIZE(VolumetricFogBackwardScatteringWeight);
    DESERIALIZE(VolumetricFogPhaseDirectionality);
    DESERIALIZE(VolumetricFogShadowPresentationEnable);
    DESERIALIZE(VolumetricFogShadowContrast);
    DESERIALIZE(VolumetricFogShadowExtinctionMultiplier);
    DESERIALIZE(VolumetricFogShadowScatteringMultiplier);
    DESERIALIZE(VolumetricFogMinimumAmbientScattering);
    DESERIALIZE(VolumetricFogDirectionalShadowStrength);
    DESERIALIZE(VolumetricFogAlbedo);
    DESERIALIZE(VolumetricFogEmissive);
    DESERIALIZE(VolumetricFogExtinctionScale);
    DESERIALIZE(VolumetricFogDistance);
    DESERIALIZE(VolumetricFogDistanceFade);
    DESERIALIZE(VolumetricFogTemporalReprojection);
    DESERIALIZE(VolumetricFogHistoryWeight);
    DESERIALIZE(VolumetricFogReactiveHistory);
    DESERIALIZE(VolumetricFogReactiveHistoryVelocityScale);
    DESERIALIZE(VolumetricFogLocalHistoryRejectionEnable);
    DESERIALIZE(VolumetricFogHistoryExtinctionDifferenceThreshold);
    DESERIALIZE(VolumetricFogHistoryNeighborhoodClampStrength);
    DESERIALIZE(VolumetricFogHistoryCameraMotionResponse);
    DESERIALIZE(VolumetricFogMinimumHistoryWeight);
    DESERIALIZE(VolumetricFogSecondLayerEnable);
    DESERIALIZE(VolumetricFogSecondLayerDensity);
    DESERIALIZE(VolumetricFogSecondLayerHeightOffset);
    DESERIALIZE(VolumetricFogSecondLayerHeightFalloff);
    DESERIALIZE(VolumetricFogSecondLayerDensityNoiseInfluence);
    DESERIALIZE(VolumetricFogDensityNoiseEnable);
    DESERIALIZE(VolumetricFogDensityNoiseScale);
    DESERIALIZE(VolumetricFogDensityNoiseOctaves);
    DESERIALIZE(VolumetricFogDensityNoiseLacunarity);
    DESERIALIZE(VolumetricFogDensityNoiseGain);
    DESERIALIZE(VolumetricFogDensityNoiseMin);
    DESERIALIZE(VolumetricFogDensityNoiseMax);
    DESERIALIZE(VolumetricFogDensityNoiseInfluence);
    DESERIALIZE(VolumetricFogDensityNoiseVelocity);
    DESERIALIZE(VolumetricFogDensityNoiseSeed);
    DESERIALIZE(VolumetricFogDensityNoiseDecorrelateOctaves);
    DESERIALIZE(VolumetricFogDensityNoiseShapingEnable);
    DESERIALIZE(VolumetricFogDensityNoiseInvert);
    DESERIALIZE(VolumetricFogDensityNoiseContrast);
    DESERIALIZE(VolumetricFogDensityNoiseHeightEnable);
    DESERIALIZE(VolumetricFogDensityNoiseHeightFalloff);
    DESERIALIZE(VolumetricFogDensityNoiseHeightMinimumInfluence);
    DESERIALIZE(VolumetricFogNearClarityEnable);
    DESERIALIZE(VolumetricFogNearClarityRadius);
    DESERIALIZE(VolumetricFogNearClarityFadeDistance);
    DESERIALIZE(VolumetricFogNearClarityMinimumDensity);
    DESERIALIZE(VolumetricFogDebug);
}

bool ExponentialHeightFog::HasContentLoaded() const
{
    return _shader && _shader->IsLoaded();
}

bool ExponentialHeightFog::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void ExponentialHeightFog::GetVolumetricFogOptions(VolumetricFogOptions& result) const
{
    const float height = (float)GetPosition().Y;
    const float density = FogDensity / 1000.0f;
    const float heightFalloff = FogHeightFalloff / 1000.0f;

    result.Enable = VolumetricFogEnable;
    result.TemporalReprojection = VolumetricFogTemporalReprojection;
    result.ScatteringDistribution = VolumetricFogScatteringDistribution;
    result.ScatteringIntensity = VolumetricFogIndependentScatteringEnable ? Math::Max(VolumetricFogScatteringIntensity, 0.0f) : 1.0f;
    result.ForwardScatteringWeight = VolumetricFogDualLobePhaseEnable ? Math::Saturate(VolumetricFogForwardScatteringWeight) : 1.0f;
    result.BackwardScatteringDistribution = Math::Clamp(VolumetricFogBackwardScatteringDistribution, -0.9f, 0.9f);
    result.BackwardScatteringWeight = VolumetricFogDualLobePhaseEnable ? Math::Saturate(VolumetricFogBackwardScatteringWeight) : 0.0f;
    result.PhaseDirectionality = Math::Saturate(VolumetricFogPhaseDirectionality);
    result.ShadowPresentationEnable = VolumetricFogShadowPresentationEnable;
    result.ShadowContrast = VolumetricFogShadowPresentationEnable ? Math::Clamp(VolumetricFogShadowContrast, 0.1f, 8.0f) : 1.0f;
    result.ShadowExtinctionMultiplier = VolumetricFogShadowPresentationEnable ? Math::Clamp(VolumetricFogShadowExtinctionMultiplier, 0.0f, 8.0f) : 1.0f;
    result.ShadowScatteringMultiplier = VolumetricFogShadowPresentationEnable ? Math::Clamp(VolumetricFogShadowScatteringMultiplier, 0.0f, 4.0f) : 1.0f;
    result.MinimumAmbientScattering = VolumetricFogShadowPresentationEnable ? Math::Saturate(VolumetricFogMinimumAmbientScattering) : 0.0f;
    result.DirectionalShadowStrength = VolumetricFogShadowPresentationEnable ? Math::Saturate(VolumetricFogDirectionalShadowStrength) : 1.0f;
    result.HistoryWeight = Math::Saturate(VolumetricFogHistoryWeight);
    result.ReactiveHistory = VolumetricFogReactiveHistory;
    result.ReactiveHistoryVelocityScale = Math::Max(VolumetricFogReactiveHistoryVelocityScale, 1.0f);
    result.LocalHistoryRejectionEnable = VolumetricFogLocalHistoryRejectionEnable;
    result.HistoryExtinctionDifferenceThreshold = VolumetricFogLocalHistoryRejectionEnable ? Math::Saturate(VolumetricFogHistoryExtinctionDifferenceThreshold) : 1.0f;
    result.HistoryNeighborhoodClampStrength = VolumetricFogLocalHistoryRejectionEnable ? Math::Saturate(VolumetricFogHistoryNeighborhoodClampStrength) : 0.0f;
    result.HistoryCameraMotionResponse = VolumetricFogLocalHistoryRejectionEnable ? Math::Clamp(VolumetricFogHistoryCameraMotionResponse, 0.0f, 4.0f) : 0.0f;
    result.MinimumHistoryWeight = VolumetricFogLocalHistoryRejectionEnable ? Math::Clamp(VolumetricFogMinimumHistoryWeight, 0.0f, 0.99f) : 0.0f;
    result.Albedo = VolumetricFogAlbedo * FogInscatteringColor;
    result.Emissive = VolumetricFogEmissive * (1.0f / 100.0f);
    result.ExtinctionScale = VolumetricFogExtinctionScale;
    result.Distance = VolumetricFogDistance;
    result.DistanceFade = Math::Saturate(VolumetricFogDistanceFade);
    result.FogParameters = Float4(density, height, heightFalloff, 0.0f);
    result.FogLayer2Parameters = VolumetricFogSecondLayerEnable
        ? Float4(
            Math::Max(VolumetricFogSecondLayerDensity, 0.0f) / 1000.0f,
            height + VolumetricFogSecondLayerHeightOffset,
            Math::Max(VolumetricFogSecondLayerHeightFalloff, 0.0001f) / 1000.0f,
            Math::Saturate(VolumetricFogSecondLayerDensityNoiseInfluence))
        : Float4::Zero;
    result.DensityNoiseEnable = VolumetricFogDensityNoiseEnable;
    result.DensityNoiseDecorrelateOctaves = VolumetricFogDensityNoiseDecorrelateOctaves;
    result.DensityNoiseInvert = VolumetricFogDensityNoiseShapingEnable && VolumetricFogDensityNoiseInvert;
    result.DensityNoiseOctaves = Math::Clamp(VolumetricFogDensityNoiseOctaves, 1, 4);
    result.DensityNoiseSeed = VolumetricFogDensityNoiseSeed;
    result.DensityNoiseScale = Math::Max(VolumetricFogDensityNoiseScale, 1.0f);
    result.DensityNoiseLacunarity = Math::Clamp(VolumetricFogDensityNoiseLacunarity, 1.0f, 4.0f);
    result.DensityNoiseGain = Math::Saturate(VolumetricFogDensityNoiseGain);
    result.DensityNoiseMin = Math::Saturate(VolumetricFogDensityNoiseMin);
    result.DensityNoiseMax = Math::Max(Math::Saturate(VolumetricFogDensityNoiseMax), result.DensityNoiseMin + 0.0001f);
    result.DensityNoiseInfluence = Math::Saturate(VolumetricFogDensityNoiseInfluence);
    result.DensityNoiseContrast = VolumetricFogDensityNoiseShapingEnable ? Math::Clamp(VolumetricFogDensityNoiseContrast, 0.01f, 8.0f) : 1.0f;
    result.DensityNoiseHeightFalloff = VolumetricFogDensityNoiseHeightEnable ? Math::Max(VolumetricFogDensityNoiseHeightFalloff, 0.0f) / 1000.0f : 0.0f;
    result.DensityNoiseHeightMinimumInfluence = VolumetricFogDensityNoiseHeightEnable ? Math::Saturate(VolumetricFogDensityNoiseHeightMinimumInfluence) : 0.0f;
    result.DensityNoiseVelocity = VolumetricFogDensityNoiseVelocity;
    result.NearClarityEnable = VolumetricFogNearClarityEnable;
    result.NearClarityRadius = Math::Max(VolumetricFogNearClarityRadius, 0.0f);
    result.NearClarityFadeDistance = Math::Max(VolumetricFogNearClarityFadeDistance, 0.001f);
    result.NearClarityMinimumDensity = Math::Saturate(VolumetricFogNearClarityMinimumDensity);
    result.DebugMode = VolumetricFogDebug;
}

void ExponentialHeightFog::GetExponentialHeightFogData(const RenderView& view, ShaderExponentialHeightFogData& result) const
{
    // Shader world positions are relative to the current large-world render origin.
    const float height = (float)(GetPosition().Y - view.Origin.Y);
    const float density = FogDensity / 1000.0f;
    const float heightFalloff = FogHeightFalloff / 1000.0f;
    const float viewHeight = view.Position.Y;
    const bool useDirectionalLightInscattering = DirectionalInscatteringLight != nullptr;

    result.FogInscatteringColor = FogInscatteringColor.ToFloat3();
    result.FogMinOpacity = 1.0f - FogMaxOpacity;
    result.FogDensity = density;
    result.FogHeight = height;
    result.FogHeightFalloff = heightFalloff;
    result.FogAtViewPosition = density * Math::Pow(2.0f, Math::Clamp(-heightFalloff * (viewHeight - height), -125.f, 126.f));
    result.StartDistance = StartDistance;
    result.FogMinOpacity = 1.0f - FogMaxOpacity;
    result.FogCutoffDistance = FogCutoffDistance >= 0 ? FogCutoffDistance : view.Far + FogCutoffDistance;
    if (useDirectionalLightInscattering)
    {
        result.InscatteringLightDirection = -DirectionalInscatteringLight->GetForward();
        result.DirectionalInscatteringColor = DirectionalInscatteringColor.ToFloat3();
        result.DirectionalInscatteringExponent = Math::Clamp(DirectionalInscatteringExponent, 0.000001f, 1000.0f);
        result.DirectionalInscatteringStartDistance = Math::Min(DirectionalInscatteringStartDistance, view.Far - 1.0f);
    }
    else
    {
        result.InscatteringLightDirection = Float3::Zero;
        result.DirectionalInscatteringColor = Float3::Zero;
        result.DirectionalInscatteringExponent = 4.0f;
        result.DirectionalInscatteringStartDistance = 0.0f;
    }
    result.ApplyDirectionalInscattering = useDirectionalLightInscattering ? 1.0f : 0.0f;
    result.VolumetricFogMaxDistance = VolumetricFogDistance;
}

GPU_CB_STRUCT(Data {
    ShaderGBufferData GBuffer;
    ShaderExponentialHeightFogData ExponentialHeightFog;
    ShaderVolumetricFogData VolumetricFog;
    Float4 TemporalAAJitter;
    });

void ExponentialHeightFog::DrawFog(GPUContext* context, RenderContext& renderContext, GPUTextureView* output)
{
    PROFILE_GPU_CPU("Exponential Height Fog");
    auto volumetricFogTexture = renderContext.List->Fog.VolumetricFogTexture;
    bool useVolumetricFog = volumetricFogTexture != nullptr;

    // Setup shader inputs
    Data data;
    GBufferPass::SetInputs(renderContext.View, data.GBuffer);
    data.ExponentialHeightFog = renderContext.List->Fog.ExponentialHeightFogData;
    data.VolumetricFog = renderContext.List->Fog.VolumetricFogData;
    data.TemporalAAJitter = renderContext.View.TemporalAAJitter;
    auto cb = _shader->GetShader()->GetCB(0);
    ASSERT_LOW_LAYER(cb->GetSize() == sizeof(Data));
    context->UpdateCB(cb, &data);
    context->BindCB(0, cb);
    context->BindSR(0, renderContext.Buffers->DepthBuffer);
    context->BindSR(1, volumetricFogTexture);

    // TODO: instead of rendering fullscreen triangle, draw quad transformed at the fog start distance (also it could use early depth discard)
    // TODO: or use DepthBounds to limit the fog rendering to the distance range

    // Draw fog
    const int32 psIndex = (useVolumetricFog ? 1 : 0);
    context->SetState(_psFog.Get(psIndex));
    context->SetRenderTarget(output);
    context->DrawFullscreenTriangle();
}

void ExponentialHeightFog::OnEnable()
{
    GetSceneRendering()->AddActor(this, _sceneRenderingKey);
#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    // Base
    Actor::OnEnable();
}

void ExponentialHeightFog::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif
    GetSceneRendering()->RemoveActor(this, _sceneRenderingKey);

    // Base
    Actor::OnDisable();
}

void ExponentialHeightFog::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);
}
