// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "../Actor.h"
#include "Engine/Content/AssetReference.h"
#include "Engine/Content/Assets/Shader.h"
#include "Engine/Content/Assets/Texture.h"
#include "Engine/Content/Assets/CubeTexture.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Renderer/Config.h"
#include "Engine/Renderer/DrawCall.h"

class GPUPipelineState;

/// <summary>
/// Sky actor renders atmosphere around the scene with fog and sky.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Visuals/Sky/Sky\"), ActorToolbox(\"Visuals\")")
class FLAXENGINE_API Sky : public Actor, public IAtmosphericFogRenderer, public ISkyRenderer
{
    DECLARE_SCENE_OBJECT(Sky);
private:
    AssetReference<Shader> _shader;
    GPUPipelineState* _psSky = nullptr;
    int32 _sceneRenderingKey = -1;

public:
    ~Sky();

public:
    /// <summary>
    /// Directional light that is used to simulate the sun.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Sky\")")
    ScriptingObjectReference<DirectionalLight> SunLight;

    /// <summary>
    /// The sun disc scale.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Sky\"), Limit(0, 100, 0.01f)")
    float SunDiscScale = 2.0f;

    /// <summary>
    /// The sun power.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Sky\"), Limit(0, 1000, 0.01f)")
    float SunPower = 8.0f;

    /// <summary>
    /// Controls how much sky will contribute indirect lighting. When set to 0, there is no GI from the sky. The default value is 1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), Limit(0, 100, 0.1f), EditorDisplay(\"Sky\")")
    float IndirectLightingIntensity = 1.0f;

    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Sky\"), Limit(0, 4, 0.01f)")
    float AtmosphereSunIntensity = 1.0f;
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Clouds\")") AssetReference<Texture> CloudTexture;
    API_FIELD(Attributes="EditorOrder(61), EditorDisplay(\"Clouds\"), Limit(0, 1, 0.01f)") float CloudCoverage = 0.0f;
    API_FIELD(Attributes="EditorOrder(62), EditorDisplay(\"Clouds\"), Limit(0, 1, 0.01f)") float CloudDensity = 0.0f;
    API_FIELD(Attributes="EditorOrder(63), EditorDisplay(\"Clouds\"), Limit(0.01f, 0.5f, 0.01f)") float CloudSoftness = 0.12f;
    API_FIELD(Attributes="EditorOrder(64), EditorDisplay(\"Clouds\")") Float2 CloudScale = Float2(0.012f);
    API_FIELD(Attributes="EditorOrder(65), EditorDisplay(\"Clouds\"), Limit(0, 8, 0.01f)") float CloudDetailScale = 2.7f;
    API_FIELD(Attributes="EditorOrder(66), EditorDisplay(\"Clouds\")") Float2 CloudWind = Float2::Zero;
    API_FIELD(Attributes="EditorOrder(67), EditorDisplay(\"Clouds\")") Float2 CloudOffset = Float2::Zero;
    API_FIELD(Attributes="EditorOrder(68), EditorDisplay(\"Clouds\")") Color CloudDayColor = Color(0.8f, 0.82f, 0.85f);
    API_FIELD(Attributes="EditorOrder(69), EditorDisplay(\"Clouds\")") Color CloudNightColor = Color(0.04f, 0.05f, 0.08f);
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"Clouds\")") Color CloudStormColor = Color(0.12f, 0.14f, 0.18f);
    API_FIELD(Attributes="EditorOrder(71), EditorDisplay(\"Clouds\"), Limit(0, 4, 0.01f)") float CloudSunRimIntensity = 1.0f;
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"Night\")") AssetReference<CubeTexture> StarsTexture;
    API_FIELD(Attributes="EditorOrder(81), EditorDisplay(\"Night\"), Limit(0, 4, 0.01f)") float StarsIntensity = 1.0f;
    API_FIELD(Attributes="EditorOrder(82), EditorDisplay(\"Night\")") Float3 MoonDirection = Float3(0, 1, 0);
    API_FIELD(Attributes="EditorOrder(83), EditorDisplay(\"Night\"), Limit(0.01f, 20, 0.01f)") float MoonAngularRadius = 0.27f;
    API_FIELD(Attributes="EditorOrder(84), EditorDisplay(\"Night\"), Limit(0, 8, 0.01f)") float MoonIntensity = 1.0f;
    API_FIELD(Attributes="EditorOrder(85), EditorDisplay(\"Night\")") Color MoonColor = Color::White;

private:
#if COMPILE_WITH_DEV_ENV
    void OnShaderReloading(Asset* obj)
    {
        _psSky = nullptr;
    }
#endif
    void InitConfig(ShaderAtmosphericFogData& config) const;

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(50);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
#endif
    void Draw(RenderContext& renderContext) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    bool HasContentLoaded() const override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;

    // [IAtmosphericFogRenderer]
    void DrawFog(GPUContext* context, RenderContext& renderContext, GPUTextureView* output) override;

    // [ISkyRenderer]
    bool IsDynamicSky() const override;
    float GetIndirectLightingIntensity() const override;
    void ApplySky(GPUContext* context, RenderContext& renderContext, const Matrix& world) override;

protected:
    // [Actor]
    void EndPlay() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
};
