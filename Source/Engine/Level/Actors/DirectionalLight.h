// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Light.h"

/// <summary>
/// Directional light emits light from direction in space.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Lights/Directional Light\"), ActorToolbox(\"Lights\")")
class FLAXENGINE_API DirectionalLight : public LightWithShadow
{
    DECLARE_SCENE_OBJECT(DirectionalLight);
public:
    /// <summary>
    /// The partitioning mode for the shadow cascades.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(64), DefaultValue(PartitionMode.Manual), EditorDisplay(\"Shadow\")")
    PartitionMode PartitionMode = PartitionMode::Manual;

    /// <summary>
    /// The number of cascades used for slicing the range of depth covered by the light during shadow rendering. A typical scene uses 4 cascades. The fifth cascade covers the range from Shadow Distance to Far Shadow Distance.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(65), DefaultValue(4), Limit(1, 5), EditorDisplay(\"Shadow\")")
    int32 CascadeCount = 4;

    /// <summary>
    /// Percentage of the shadow distance used by the first cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(66), DefaultValue(0.05f), VisibleIf(nameof(ShowCascade1)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade1Spacing = 0.05f;

    /// <summary>
    /// Percentage of the shadow distance used by the second cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(67), DefaultValue(0.15f), VisibleIf(nameof(ShowCascade2)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade2Spacing = 0.15f;

    /// <summary>
    /// Percentage of the shadow distance used by the third cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(68), DefaultValue(0.50f), VisibleIf(nameof(ShowCascade3)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade3Spacing = 0.50f;

    /// <summary>
    /// Percentage of the shadow distance used by the fourth cascade.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(69), DefaultValue(1.0f), VisibleIf(nameof(ShowCascade4)), Limit(0, 1, 0.001f), EditorDisplay(\"Shadow\")")
    float Cascade4Spacing = 1.0f;

    /// <summary>
    /// Maximum distance from the view covered by the fifth, low-detail shadow cascade. Used only when Cascade Count is 5 and clamped to be no less than Shadow Distance.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(70), DefaultValue(20000.0f), VisibleIf(nameof(ShowFarShadowsDistance)), Limit(0, 1000000), EditorDisplay(\"Shadow\", \"Far Shadow Distance\")")
    float FarShadowsDistance = 20000.0f;

    /// <summary>
    /// Controls how the fourth shadow cascade transitions into the fifth, low-detail cascade. Fade is smooth without TAA but samples both cascades. Dither samples one cascade and is intended for use with TAA.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(71), DefaultValue(FarShadowTransitionMode.Fade), VisibleIf(nameof(ShowFarShadowsDistance)), EditorDisplay(\"Shadow\", \"Far Shadow Transition Mode\")")
    FarShadowTransitionMode FarShadowsTransitionMode = FarShadowTransitionMode::Fade;

    /// <summary>
    /// Distance over which the fourth shadow cascade transitions into the fifth, low-detail cascade. Larger dither transitions make the pattern more visible when TAA is disabled.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(72), DefaultValue(2000.0f), VisibleIf(nameof(ShowFarShadowsDistance)), Limit(0, 1000000), EditorDisplay(\"Shadow\", \"Far Shadow Transition Distance\")")
    float FarShadowsTransitionDistance = 2000.0f;

    /// <summary>
    /// The color tint applied to the filtered transition between fully lit and fully shadowed pixels. White preserves the original light color. This affects deferred shadow-map lighting only.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(110), DefaultValue(typeof(Color), \"0.42,0.36,1,1\"), EditorDisplay(\"Shadow\", \"Penumbra Color\")")
    ::Color ShadowsPenumbraColor = ::Color(0.42f, 0.36f, 1.0f, 1.0f);

    /// <summary>
    /// The strength of the shadow penumbra color tint. Set to 0 to disable the effect. This colors the existing filtered shadow transition and does not change its width.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(111), DefaultValue(0.0f), Limit(0.0f, 1.0f, 0.01f), EditorDisplay(\"Shadow\", \"Penumbra Color Strength\")")
    float ShadowsPenumbraColorStrength = 0.0f;

    /// <summary>
    /// Screen-space offset of the colored penumbra mask in pixels. Positive X moves it right and positive Y moves it down. This does not move or soften the actual shadow.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(112), Limit(-8.0f, 8.0f, 0.1f), EditorDisplay(\"Shadow\", \"Penumbra Color Offset (Pixels)\")")
    Float2 ShadowsPenumbraColorOffset = Float2::Zero;

    /// <summary>
    /// Places the colored fringe inside the shadow rather than on the illuminated side of the shadow edge.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(113), DefaultValue(true), EditorDisplay(\"Shadow\", \"Penumbra Color Inside Shadow\")")
    bool ShadowsPenumbraColorInsideShadow = true;

public:
    // [LightWithShadow]
    void Draw(RenderContext& renderContext) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;

protected:
    // [LightWithShadow]
    void OnTransformChanged() override;
};
