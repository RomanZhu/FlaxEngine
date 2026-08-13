// Copyright (c) Wojciech Figat. All rights reserved.

#include "DirectionalLight.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Renderer/RenderList.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Level/Scene/SceneRendering.h"

DirectionalLight::DirectionalLight(const SpawnParams& params)
    : LightWithShadow(params)
{
    _drawNoCulling = 1;
    Brightness = 8.0f;
}

void DirectionalLight::Draw(RenderContext& renderContext)
{
    float brightness = Brightness;
    AdjustBrightness(renderContext.View, brightness);
    Float3 position;
    if (Brightness > ZeroTolerance
        && EnumHasAnyFlags(renderContext.View.Flags, ViewFlags::DirectionalLights)
        && EnumHasAnyFlags(renderContext.View.Pass, DrawPass::GBuffer)
        && CheckViewDistance(renderContext.View.Position, renderContext.View.Origin, position, brightness))
    {
        RenderDirectionalLightData data;
        data.Position = position;
        data.MinRoughness = MinRoughness;
        data.ShadowsDistance = ShadowsDistance;
        data.Color = Color.ToFloat3() * (Color.A * brightness);
        data.ShadowsStrength = ShadowsStrength;
        data.Direction = GetForward();
        data.ShadowsFadeDistance = ShadowsFadeDistance;
        data.ShadowsNormalOffsetScale = ShadowsNormalOffsetScale;
        data.ShadowsDepthBias = ShadowsDepthBias;
        data.ShadowsSharpness = ShadowsSharpness;
        data.VolumetricScatteringIntensity = VolumetricScatteringIntensity;
        data.IndirectLightingIntensity = IndirectLightingIntensity;
        data.CastVolumetricShadow = CastVolumetricShadow;
        data.ShadowsUpdateRate = ShadowsUpdateRate;
        data.ShadowFrame = _invalidateShadowFrame;
        data.ShadowsResolution = (int32)ShadowsResolution;
        data.ShadowsUpdateRateAtDistance = ShadowsUpdateRateAtDistance;
        data.ShadowsMode = ShadowsMode;
        data.CascadeCount = CascadeCount;
        data.Cascade1Spacing = Cascade1Spacing;
        data.Cascade2Spacing = Cascade2Spacing;
        data.Cascade3Spacing = Cascade3Spacing;
        data.Cascade4Spacing = Cascade4Spacing;
        data.FarShadowsDistance = FarShadowsDistance;
        data.FarShadowsTransitionMode = FarShadowsTransitionMode;
        data.FarShadowsTransitionDistance = FarShadowsTransitionDistance;
        data.ShadowsPenumbraColor = ShadowsPenumbraColor.ToFloat3();
        data.ShadowsPenumbraColorStrength = ShadowsPenumbraColorStrength * ShadowsPenumbraColor.A;
        data.ShadowsPenumbraColorOffset = ShadowsPenumbraColorOffset;
        data.ShadowsPenumbraColorInsideShadow = ShadowsPenumbraColorInsideShadow;
        data.ShadowsEdgeAAStrength = ShadowsEdgeAAStrength;
        data.ShadowsEdgeAASampleRadius = ShadowsEdgeAASampleRadius;
        data.PartitionMode = PartitionMode;
        data.ContactShadowsLength = ContactShadowsLength;
        data.StaticFlags = GetStaticFlags();
        data.ID = GetID();
        data.ScreenSize = 1.0f;
        renderContext.List->DirectionalLights.Add(data);
    }
}

void DirectionalLight::Serialize(SerializeStream& stream, const void* otherObj)
{
    // Base
    LightWithShadow::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(DirectionalLight);

    SERIALIZE(CascadeCount);
    SERIALIZE(Cascade1Spacing);
    SERIALIZE(Cascade2Spacing);
    SERIALIZE(Cascade3Spacing);
    SERIALIZE(Cascade4Spacing);
    SERIALIZE(FarShadowsDistance);
    SERIALIZE(FarShadowsTransitionMode);
    SERIALIZE(FarShadowsTransitionDistance);
    SERIALIZE(ShadowsPenumbraColor);
    SERIALIZE(ShadowsPenumbraColorStrength);
    SERIALIZE(ShadowsPenumbraColorOffset);
    SERIALIZE(ShadowsPenumbraColorInsideShadow);
    SERIALIZE(ShadowsEdgeAAStrength);
    SERIALIZE(ShadowsEdgeAASampleRadius);
    SERIALIZE(PartitionMode);
}

void DirectionalLight::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // Base
    LightWithShadow::Deserialize(stream, modifier);

    DESERIALIZE(CascadeCount);
    DESERIALIZE(Cascade1Spacing);
    DESERIALIZE(Cascade2Spacing);
    DESERIALIZE(Cascade3Spacing);
    DESERIALIZE(Cascade4Spacing);
    DESERIALIZE(FarShadowsDistance);
    DESERIALIZE(FarShadowsTransitionMode);
    DESERIALIZE(FarShadowsTransitionDistance);
    DESERIALIZE(ShadowsPenumbraColor);
    DESERIALIZE(ShadowsPenumbraColorStrength);
    DESERIALIZE(ShadowsPenumbraColorOffset);
    DESERIALIZE(ShadowsPenumbraColorInsideShadow);
    DESERIALIZE(ShadowsEdgeAAStrength);
    DESERIALIZE(ShadowsEdgeAASampleRadius);
    DESERIALIZE(PartitionMode);
}

bool DirectionalLight::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void DirectionalLight::OnTransformChanged()
{
    // Base
    LightWithShadow::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);
}
