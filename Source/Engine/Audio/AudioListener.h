// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"

/// <summary>
/// Represents a listener that hears audio sources. For spatial audio the volume and pitch of played audio is determined by the distance, orientation and velocity differences between the source and the listener.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Listener\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioListener : public Actor
{
    DECLARE_SCENE_OBJECT(AudioListener);
private:
    Vector3 _velocity;
    Vector3 _prevPos;

public:
    /// <summary>Explicit listener index used by event listener masks.</summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Audio Listener\"), Limit(0, 7)")
    int32 ListenerIndex = 0;

    /// <summary>Attenuation contribution for this listener.</summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Audio Listener\"), Limit(0, 1, 0.01f)")
    float ListenerWeight = 1.0f;

    /// <summary>Optional Actor whose position is used for attenuation while this Actor supplies orientation.</summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Audio Listener\")")
    Actor* AttenuationActor = nullptr;

    /// <summary>Maximum inferred listener velocity in centimeters per second. Set to zero to disable inferred listener velocity and Doppler from listener motion.</summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Audio Listener\"), Limit(0, 1000000)")
    float MaximumInferredVelocity = 10000.0f;

    /// <summary>
    /// Gets the velocity of the listener. Determines pitch in relation to AudioListener's position.
    /// </summary>
    API_PROPERTY() FORCE_INLINE const Vector3& GetVelocity() const
    {
        return _velocity;
    }

    FORCE_INLINE Vector3 GetAttenuationPosition() const { return AttenuationActor ? AttenuationActor->GetPosition() : GetPosition(); }

private:
    void Update();

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(50);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
#endif
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actors]
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
};
