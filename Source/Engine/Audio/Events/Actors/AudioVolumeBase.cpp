// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioVolumeBase.h"
#include "Engine/Audio/Events/AudioWorld.h"
#include "Engine/Debug/DebugDraw.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Level/Scene/SceneRendering.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Serialization/Serialization.h"

AudioVolumeBase::AudioVolumeBase(const SpawnParams& params)
    : Actor(params)
{
}

void AudioVolumeBase::SetShape(AudioVolumeShape value)
{
    if (_shape != value)
    {
        _shape = value;
        UpdateBounds();
    }
}

void AudioVolumeBase::SetBoxSize(const Vector3& value)
{
    _boxSize = Vector3::Max(value, Vector3::One);
    UpdateBounds();
}

void AudioVolumeBase::SetSphereRadius(float value)
{
    _sphereRadius = Math::Max(value, 1.0f);
    UpdateBounds();
}

void AudioVolumeBase::SetCapsuleRadius(float value)
{
    _capsuleRadius = Math::Max(value, 1.0f);
    UpdateBounds();
}

void AudioVolumeBase::SetCapsuleHeight(float value)
{
    _capsuleHeight = Math::Max(value, 1.0f);
    UpdateBounds();
}

void AudioVolumeBase::SetBlendDistanceOutside(float value)
{
    _blendDistanceOutside = Math::Max(0.0f, value);
}

void AudioVolumeBase::SetBlendDistanceInside(float value)
{
    _blendDistanceInside = Math::Max(0.0f, value);
}

AudioVolumeSample AudioVolumeBase::Evaluate(const Vector3& worldPosition) const
{
    AudioVolumeSample sample;
    const Vector3 localPos = _transform.WorldToLocal(worldPosition);

    switch (_shape)
    {
    case AudioVolumeShape::Box:
    {
        Vector3 half = _boxSize * 0.5f;
        Vector3 clamped = Vector3::Clamp(localPos, -half, half);
        Vector3 delta = localPos - clamped;
        float outDist = (float)delta.Length();

        if (outDist > 0.0001f)
        {
            sample.IsInside = false;
            sample.SignedDistance = outDist;
            sample.ClosestPoint = _transform.LocalToWorld(clamped);
        }
        else
        {
            sample.IsInside = true;
            Vector3 d = half - Vector3::Abs(localPos);
            sample.SignedDistance = (float)-Math::Min(Math::Min(d.X, d.Y), d.Z);
            sample.ClosestPoint = worldPosition;
        }
        break;
    }
    case AudioVolumeShape::Sphere:
    {
        float dist = (float)localPos.Length();
        sample.SignedDistance = dist - _sphereRadius;
        sample.IsInside = sample.SignedDistance <= 0.0f;
        if (sample.IsInside)
        {
            sample.ClosestPoint = worldPosition;
        }
        else
        {
            Vector3 dir = dist > 0.0001f ? localPos / dist : Vector3::Forward;
            sample.ClosestPoint = _transform.LocalToWorld(dir * _sphereRadius);
        }
        break;
    }
    case AudioVolumeShape::Capsule:
    {
        Real halfH = (Real)(_capsuleHeight * 0.5f);
        Vector3 segPoint((Real)0.0, Math::Clamp(localPos.Y, -halfH, halfH), (Real)0.0);
        Vector3 delta = localPos - segPoint;
        float dist = (float)delta.Length();
        sample.SignedDistance = dist - _capsuleRadius;
        sample.IsInside = sample.SignedDistance <= 0.0f;
        if (sample.IsInside)
        {
            sample.ClosestPoint = worldPosition;
        }
        else
        {
            Vector3 dir = dist > 0.0001f ? delta / dist : Vector3::Forward;
            sample.ClosestPoint = _transform.LocalToWorld(segPoint + dir * _capsuleRadius);
        }
        break;
    }
    }

    // Blend weight calculation
    if (sample.IsInside)
    {
        if (_blendDistanceInside > 0.0001f)
            sample.Weight = Math::Saturate(-sample.SignedDistance / _blendDistanceInside);
        else
            sample.Weight = 1.0f;
    }
    else
    {
        if (_blendDistanceOutside > 0.0001f)
            sample.Weight = Math::Saturate(1.0f - sample.SignedDistance / _blendDistanceOutside);
        else
            sample.Weight = 0.0f;
    }

    return sample;
}

void AudioVolumeBase::UpdateBounds()
{
    switch (_shape)
    {
    case AudioVolumeShape::Box:
    {
        OrientedBoundingBox obb(Vector3::Zero, _boxSize * 0.5f);
        obb.Transform(_transform);
        obb.GetBoundingBox(_box);
        BoundingSphere::FromBox(_box, _sphere);
        break;
    }
    case AudioVolumeShape::Sphere:
    {
        _sphere = BoundingSphere(GetPosition(), _sphereRadius * (float)_transform.Scale.MaxValue());
        _sphere.GetBoundingBox(_box);
        break;
    }
    case AudioVolumeShape::Capsule:
    {
        float maxScale = (float)_transform.Scale.MaxValue();
        _sphere = BoundingSphere(GetPosition(), (_capsuleRadius + _capsuleHeight * 0.5f) * maxScale);
        _sphere.GetBoundingBox(_box);
        break;
    }
    }
}

bool AudioVolumeBase::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

#if USE_EDITOR
BoundingBox AudioVolumeBase::GetEditorBox() const
{
    return _box;
}

void AudioVolumeBase::OnDebugDraw()
{
    switch (_shape)
    {
    case AudioVolumeShape::Box:
    {
        OrientedBoundingBox obb(Vector3::Zero, _boxSize * 0.5f);
        obb.Transform(_transform);
        DEBUG_DRAW_WIRE_BOX(obb, Color::Teal, 0, true);
        break;
    }
    case AudioVolumeShape::Sphere:
    {
        DEBUG_DRAW_WIRE_SPHERE(BoundingSphere(GetPosition(), _sphereRadius), Color::Teal, 0, true);
        break;
    }
    case AudioVolumeShape::Capsule:
    {
        DEBUG_DRAW_WIRE_CAPSULE(GetPosition(), GetOrientation(), _capsuleRadius, _capsuleHeight, Color::Teal, 0, true);
        break;
    }
    }
}

void AudioVolumeBase::OnDebugDrawSelected()
{
    OnDebugDraw();
    Actor::OnDebugDrawSelected();
}
#endif

void AudioVolumeBase::OnEnable()
{
    UpdateBounds();
    AudioWorld::Register(this);

#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    Actor::OnEnable();
}

void AudioVolumeBase::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif

    AudioWorld::Unregister(this);

    Actor::OnDisable();
}

void AudioVolumeBase::OnTransformChanged()
{
    Actor::OnTransformChanged();
    UpdateBounds();
}

void AudioVolumeBase::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioVolumeBase);

    SERIALIZE_MEMBER(Shape, _shape);
    SERIALIZE_MEMBER(BoxSize, _boxSize);
    SERIALIZE_MEMBER(SphereRadius, _sphereRadius);
    SERIALIZE_MEMBER(CapsuleRadius, _capsuleRadius);
    SERIALIZE_MEMBER(CapsuleHeight, _capsuleHeight);
    SERIALIZE_MEMBER(Priority, _priority);
    SERIALIZE_MEMBER(BlendDistanceOutside, _blendDistanceOutside);
    SERIALIZE_MEMBER(BlendDistanceInside, _blendDistanceInside);
    SERIALIZE_MEMBER(ListenerMask, _listenerMask);
    SERIALIZE_MEMBER(BlendMode, _blendMode);
    SERIALIZE_MEMBER(BlendGroup, _blendGroup);
}

void AudioVolumeBase::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE_MEMBER(Shape, _shape);
    DESERIALIZE_MEMBER(BoxSize, _boxSize);
    DESERIALIZE_MEMBER(SphereRadius, _sphereRadius);
    DESERIALIZE_MEMBER(CapsuleRadius, _capsuleRadius);
    DESERIALIZE_MEMBER(CapsuleHeight, _capsuleHeight);
    DESERIALIZE_MEMBER(Priority, _priority);
    DESERIALIZE_MEMBER(BlendDistanceOutside, _blendDistanceOutside);
    DESERIALIZE_MEMBER(BlendDistanceInside, _blendDistanceInside);
    DESERIALIZE_MEMBER(ListenerMask, _listenerMask);
    DESERIALIZE_MEMBER(BlendMode, _blendMode);
    DESERIALIZE_MEMBER(BlendGroup, _blendGroup);

    UpdateBounds();
}
