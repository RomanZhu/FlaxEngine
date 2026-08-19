// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/OrientedBoundingBox.h"
#include "Engine/Core/Math/BoundingSphere.h"
#include "Engine/Core/Math/BoundingBox.h"
#include "Engine/Core/Math/Ray.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Geometric shape representation for audio volumes.
/// </summary>
API_ENUM() enum class AudioVolumeShape : uint8
{
    /// <summary>
    /// Oriented box volume shape.
    /// </summary>
    Box = 0,

    /// <summary>
    /// Sphere volume shape.
    /// </summary>
    Sphere = 1,

    /// <summary>
    /// Capsule volume shape.
    /// </summary>
    Capsule = 2,
};

/// <summary>
/// Sample result from querying an audio volume at a 3D world position.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioVolumeSample
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioVolumeSample);

    /// <summary>
    /// True if the sample position is inside the volume boundary.
    /// </summary>
    API_FIELD() bool IsInside = false;

    /// <summary>
    /// Signed distance to the volume boundary (negative when inside, positive when outside).
    /// </summary>
    API_FIELD() float SignedDistance = MAX_float;

    /// <summary>
    /// Evaluated blend weight in range [0, 1].
    /// </summary>
    API_FIELD() float Weight = 0.0f;

    /// <summary>
    /// Closest point on or within the volume to the sample point.
    /// </summary>
    API_FIELD() Vector3 ClosestPoint = Vector3::Zero;
};

/// <summary>
/// Base actor class for spatial audio zones and area emitters.
/// </summary>
API_CLASS(Abstract) class FLAXENGINE_API AudioVolumeBase : public Actor
{
    DECLARE_SCENE_OBJECT(AudioVolumeBase);

protected:
    AudioVolumeShape _shape = AudioVolumeShape::Box;
    Vector3 _boxSize = Vector3(1000.0f);
    float _sphereRadius = 500.0f;
    float _capsuleRadius = 200.0f;
    float _capsuleHeight = 600.0f;
    int32 _priority = 0;
    float _blendDistanceOutside = 200.0f;
    float _blendDistanceInside = 100.0f;
    uint32 _listenerMask = MAX_uint32;

public:
    /// <summary>
    /// Gets the volume shape type.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(0), EditorDisplay(\"Volume\")")
    FORCE_INLINE AudioVolumeShape GetShape() const { return _shape; }

    /// <summary>
    /// Sets the volume shape type.
    /// </summary>
    API_PROPERTY() void SetShape(AudioVolumeShape value);

    /// <summary>
    /// Gets the box dimensions (when Shape is Box).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(10), EditorDisplay(\"Volume\"), VisibleIf(nameof(IsBox))")
    FORCE_INLINE Vector3 GetBoxSize() const { return _boxSize; }

    /// <summary>
    /// Sets the box dimensions (when Shape is Box).
    /// </summary>
    API_PROPERTY() void SetBoxSize(const Vector3& value);

    /// <summary>
    /// Gets the sphere radius (when Shape is Sphere).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(20), EditorDisplay(\"Volume\"), VisibleIf(nameof(IsSphere))")
    FORCE_INLINE float GetSphereRadius() const { return _sphereRadius; }

    /// <summary>
    /// Sets the sphere radius (when Shape is Sphere).
    /// </summary>
    API_PROPERTY() void SetSphereRadius(float value);

    /// <summary>
    /// Gets the capsule radius (when Shape is Capsule).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(30), EditorDisplay(\"Volume\"), VisibleIf(nameof(IsCapsule))")
    FORCE_INLINE float GetCapsuleRadius() const { return _capsuleRadius; }

    /// <summary>
    /// Sets the capsule radius (when Shape is Capsule).
    /// </summary>
    API_PROPERTY() void SetCapsuleRadius(float value);

    /// <summary>
    /// Gets the capsule height (when Shape is Capsule).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(40), EditorDisplay(\"Volume\"), VisibleIf(nameof(IsCapsule))")
    FORCE_INLINE float GetCapsuleHeight() const { return _capsuleHeight; }

    /// <summary>
    /// Sets the capsule height (when Shape is Capsule).
    /// </summary>
    API_PROPERTY() void SetCapsuleHeight(float value);

    /// <summary>
    /// Gets the volume evaluation priority (higher priority overrides lower).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(50), EditorDisplay(\"Volume\")")
    FORCE_INLINE int32 GetPriority() const { return _priority; }

    /// <summary>
    /// Sets the volume evaluation priority.
    /// </summary>
    API_PROPERTY() void SetPriority(int32 value) { _priority = value; }

    /// <summary>
    /// Gets the fade-in distance outside the volume boundary.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(60), EditorDisplay(\"Blending\")")
    FORCE_INLINE float GetBlendDistanceOutside() const { return _blendDistanceOutside; }

    /// <summary>
    /// Sets the fade-in distance outside the volume boundary.
    /// </summary>
    API_PROPERTY() void SetBlendDistanceOutside(float value);

    /// <summary>
    /// Gets the full-weight blend depth inside the volume boundary.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(70), EditorDisplay(\"Blending\")")
    FORCE_INLINE float GetBlendDistanceInside() const { return _blendDistanceInside; }

    /// <summary>
    /// Sets the full-weight blend depth inside the volume boundary.
    /// </summary>
    API_PROPERTY() void SetBlendDistanceInside(float value);

    /// <summary>
    /// Returns true if the volume shape is a box.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsBox() const { return _shape == AudioVolumeShape::Box; }

    /// <summary>
    /// Returns true if the volume shape is a sphere.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsSphere() const { return _shape == AudioVolumeShape::Sphere; }

    /// <summary>
    /// Returns true if the volume shape is a capsule.
    /// </summary>
    API_PROPERTY() FORCE_INLINE bool IsCapsule() const { return _shape == AudioVolumeShape::Capsule; }

public:
    /// <summary>
    /// Evaluates the volume's signed distance and weight at a world position.
    /// </summary>
    API_FUNCTION() AudioVolumeSample Evaluate(const Vector3& worldPosition) const;

    /// <summary>
    /// Updates this volume from the active audio listener position during play.
    /// </summary>
    virtual void UpdateListenerPosition(const Vector3& listenerPosition)
    {
    }

protected:
    void UpdateBounds();

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override;
    void OnDebugDraw() override;
    void OnDebugDrawSelected() override;
#endif
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
};
