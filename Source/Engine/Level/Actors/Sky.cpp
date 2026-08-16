// Copyright (c) Wojciech Figat. All rights reserved.

#include "Sky.h"
#include "DirectionalLight.h"
#include "Engine/Core/Math/Color.h"
#include "Engine/Content/Content.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Renderer/AtmospherePreCompute.h"
#include "Engine/Renderer/GBufferPass.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/GPUContext.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/Shaders/GPUConstantBuffer.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Engine/Time.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Level/Scene/SceneRendering.h"
#if USE_EDITOR
#include "Engine/Renderer/Lightmaps.h"
#endif

GPU_CB_STRUCT(Data {
    Matrix WorldViewProjection;
    Matrix InvViewProjection;
    Float3 ViewOffset;
    float NoiseScale;
    ShaderGBufferData GBuffer;
    ShaderAtmosphericFogData Fog;
    Float4 CloudParams0;
    Float4 CloudParams1;
    Float4 CloudDayColor;
    Float4 CloudNightColor;
    Float4 CloudStormColor;
    Float4 NightParams;
    Float4 MoonDirection;
    Float4 MoonColor;
    Float4 SkyParams;
    });

Sky::Sky(const SpawnParams& params)
    : Actor(params)
{
    _drawNoCulling = 1;
    _drawCategory = SceneRendering::PreRender;

    // Load shader
    _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/Sky"));
    if (_shader == nullptr)
    {
        LOG(Fatal, "Cannot load sky shader.");
        return;
    }
#if COMPILE_WITH_DEV_ENV
    _shader.Get()->OnReloading.Bind<Sky, &Sky::OnShaderReloading>(this);
#endif
}

Sky::~Sky()
{
    SAFE_DELETE_GPU_RESOURCE(_psSky);
}

void Sky::InitConfig(ShaderAtmosphericFogData& config) const
{
    config.AtmosphericFogDensityScale = 1.0f;
    config.AtmosphericFogSunDiscScale = SunDiscScale;
    config.AtmosphericFogDistanceScale = 1;
    config.AtmosphericFogGroundOffset = 0;

    config.AtmosphericFogAltitudeScale = 1;
    config.AtmosphericFogStartDistance = 0;
    config.AtmosphericFogPower = 1;
    config.AtmosphericFogDistanceOffset = 0;

    config.AtmosphericFogSunPower = SunPower;
    config.AtmosphericFogDensityOffset = 0.0f;
#if USE_EDITOR
    if (IsRunningRadiancePass)
    config.AtmosphericFogSunPower *= IndirectLightingIntensity;
#endif

    if (SunLight)
    {
        config.AtmosphericFogSunDirection = -SunLight->GetForward();
        config.AtmosphericFogSunColor = SunLight->Color.ToFloat3() * SunLight->Color.A * AtmosphereSunIntensity;
    }
    else
    {
        config.AtmosphericFogSunDirection = Float3::UnitY;
        config.AtmosphericFogSunColor = Float3::One;
    }
}

void Sky::Draw(RenderContext& renderContext)
{
    if (HasContentLoaded() && EnumHasAnyFlags(renderContext.View.Flags, ViewFlags::Sky))
    {
        // Ensure to have pipeline state cache created
        if (_psSky == nullptr)
        {
            const auto shader = _shader->GetShader();

            // Create pipeline states
            if (_psSky == nullptr)
            {
                _psSky = GPUDevice::Instance->CreatePipelineState();

                GPUPipelineState::Description psDesc = GPUPipelineState::Description::Default;
                psDesc.VS = shader->GetVS("VS");
                psDesc.PS = shader->GetPS("PS_Sky");
                psDesc.CullMode = CullMode::Inverted;
                psDesc.DepthWriteEnable = false;
                psDesc.DepthClipEnable = false;
                psDesc.DepthFunc = ComparisonFunc::LessEqual;

                if (_psSky->Init(psDesc))
                {
                    LOG(Warning, "Cannot create graphics pipeline state object for '{0}'.", ToString());
                }
            }
        }

        // Register for the sky and fog pass
        renderContext.List->Sky = this;
        //if(renderContext.View.Flags & ViewFlags::Fog) != 0)
        //renderContext.List->AtmosphericFog = this; // TODO: finish atmosphere fog
    }
}

void Sky::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(Sky);

    SERIALIZE_MEMBER(Sun, SunLight);
    SERIALIZE(SunDiscScale);
    SERIALIZE(SunPower);
    SERIALIZE(IndirectLightingIntensity);
    SERIALIZE(AtmosphereSunIntensity);
    SERIALIZE(CloudTexture);
    SERIALIZE(CloudCoverage);
    SERIALIZE(CloudDensity);
    SERIALIZE(CloudSoftness);
    SERIALIZE(CloudScale);
    SERIALIZE(CloudDetailScale);
    SERIALIZE(CloudWind);
    SERIALIZE(CloudOffset);
    SERIALIZE(CloudDayColor);
    SERIALIZE(CloudNightColor);
    SERIALIZE(CloudStormColor);
    SERIALIZE(CloudSunRimIntensity);
    SERIALIZE(StarsTexture);
    SERIALIZE(StarsIntensity);
    SERIALIZE(MoonDirection);
    SERIALIZE(MoonAngularRadius);
    SERIALIZE(MoonIntensity);
    SERIALIZE(MoonColor);
}

void Sky::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE_MEMBER(Sun, SunLight);
    DESERIALIZE(SunDiscScale);
    DESERIALIZE(SunPower);
    DESERIALIZE(IndirectLightingIntensity);
    DESERIALIZE(AtmosphereSunIntensity);
    DESERIALIZE(CloudTexture);
    DESERIALIZE(CloudCoverage);
    DESERIALIZE(CloudDensity);
    DESERIALIZE(CloudSoftness);
    DESERIALIZE(CloudScale);
    DESERIALIZE(CloudDetailScale);
    DESERIALIZE(CloudWind);
    DESERIALIZE(CloudOffset);
    DESERIALIZE(CloudDayColor);
    DESERIALIZE(CloudNightColor);
    DESERIALIZE(CloudStormColor);
    DESERIALIZE(CloudSunRimIntensity);
    DESERIALIZE(StarsTexture);
    DESERIALIZE(StarsIntensity);
    DESERIALIZE(MoonDirection);
    DESERIALIZE(MoonAngularRadius);
    DESERIALIZE(MoonIntensity);
    DESERIALIZE(MoonColor);
}

bool Sky::HasContentLoaded() const
{
    return _shader && _shader->IsLoaded() && AtmospherePreCompute::GetCache(nullptr);
}

bool Sky::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void Sky::DrawFog(GPUContext* context, RenderContext& renderContext, GPUTextureView* output)
{
    MISSING_CODE("sky fog");
}

bool Sky::IsDynamicSky() const
{
    return !IsStatic() || (SunLight && !SunLight->IsStatic());
}

float Sky::GetIndirectLightingIntensity() const
{
    return IndirectLightingIntensity;
}

void Sky::ApplySky(GPUContext* context, RenderContext& renderContext, const Matrix& world)
{
    // Get precomputed cache and bind it to the pipeline
    AtmosphereCache cache;
    if (!AtmospherePreCompute::GetCache(&cache))
        return;
    context->BindSR(4, cache.Transmittance);
    context->BindSR(5, cache.Irradiance);
    context->BindSR(6, cache.Inscatter->ViewVolume());
    const bool hasCloudTexture = CloudTexture && CloudTexture->GetTexture();
    const bool hasStarsTexture = StarsTexture && StarsTexture->GetTexture();
    context->BindSR(7, hasCloudTexture ? CloudTexture->GetTexture() : GPUDevice::Instance->GetDefaultWhiteTexture());
    context->BindSR(8, hasStarsTexture ? StarsTexture->GetTexture() : nullptr);

    // Setup constants data
    Matrix m;
    Data data;
    Matrix::Multiply(world, renderContext.View.ViewProjection(), m);
    Matrix::Transpose(m, data.WorldViewProjection);
    Matrix::Transpose(renderContext.View.IVP, data.InvViewProjection);
    GBufferPass::SetInputs(renderContext.View, data.GBuffer);
    data.ViewOffset = renderContext.View.Origin + GetPosition();
    data.NoiseScale = renderContext.View.IsSingleFrame ? 0.01f : 0.03f;
    InitConfig(data.Fog);
    data.CloudParams0 = Float4(hasCloudTexture ? CloudCoverage : 0.0f, CloudDensity, CloudSoftness, CloudDetailScale);
    const float cloudTime = Time::GetGameTime();
    data.CloudParams1 = Float4(CloudScale.X, CloudScale.Y, CloudOffset.X + CloudWind.X * cloudTime, CloudOffset.Y + CloudWind.Y * cloudTime);
    data.CloudDayColor = Float4(CloudDayColor.ToFloat3(), CloudDayColor.A);
    data.CloudNightColor = Float4(CloudNightColor.ToFloat3(), CloudNightColor.A);
    data.CloudStormColor = Float4(CloudStormColor.ToFloat3(), CloudStormColor.A);
    const float moonCos = Math::Cos(MoonAngularRadius * 0.01745329252f);
    data.NightParams = Float4(hasStarsTexture ? StarsIntensity : 0.0f, moonCos, 0.01f, MoonIntensity);
    data.MoonDirection = Float4(MoonDirection, 0);
    data.MoonColor = Float4(MoonColor.ToFloat3(), MoonColor.A);
    data.SkyParams = Float4(AtmosphereSunIntensity, CloudSunRimIntensity, 0, 0);
    //data.Fog.AtmosphericFogSunPower *= SunLight ? SunLight->Brightness : 1.0f;
    if (EnumHasNoneFlags(renderContext.View.Flags, ViewFlags::SpecularLight))
    {
        // Hide sun disc if specular light is disabled
        data.Fog.AtmosphericFogSunDiscScale = 0;
    }

    // Bind pipeline
    auto cb = _shader->GetShader()->GetCB(0);
    context->UpdateCB(cb, &data);
    context->BindCB(0, cb);
    context->SetState(_psSky);
}

void Sky::EndPlay()
{
    SAFE_DELETE_GPU_RESOURCE(_psSky);

    Actor::EndPlay();
}

void Sky::OnEnable()
{
    GetSceneRendering()->AddActor(this, _sceneRenderingKey);
#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    Actor::OnEnable();
}

void Sky::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif
    GetSceneRendering()->RemoveActor(this, _sceneRenderingKey);

    Actor::OnDisable();
}

void Sky::OnTransformChanged()
{
    Actor::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);
}
