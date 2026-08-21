// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioListener.h"
#include "Engine/Engine/Time.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Core/Log.h"
#include "AudioBackend.h"
#include "Audio.h"
#include "Engine/Serialization/Serialization.h"

AudioListener::AudioListener(const SpawnParams& params)
    : Actor(params)
    , _velocity(Vector3::Zero)
    , _prevPos(Vector3::Zero)
{
}

bool AudioListener::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

void AudioListener::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);
    SERIALIZE_GET_OTHER_OBJ(AudioListener);
    SERIALIZE(ListenerIndex);
    SERIALIZE(ListenerWeight);
    SERIALIZE(AttenuationActor);
    SERIALIZE(MaximumInferredVelocity);
}

void AudioListener::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);
    DESERIALIZE(ListenerIndex);
    DESERIALIZE(ListenerWeight);
    DESERIALIZE(AttenuationActor);
    DESERIALIZE(MaximumInferredVelocity);
    ListenerIndex = Math::Clamp(ListenerIndex, 0, AUDIO_MAX_LISTENERS - 1);
    ListenerWeight = Math::Saturate(ListenerWeight);
    MaximumInferredVelocity = Math::Max(0.0f, MaximumInferredVelocity);
}

void AudioListener::Update()
{
    // Update the velocity
    const Vector3 pos = GetPosition();
    const float dt = Math::Max(Time::Update.UnscaledDeltaTime.GetTotalSeconds(), 0.00001f);
    const auto prevVelocity = _velocity;
    _velocity = (pos - _prevPos) / dt;
    // A teleport or a debugger/frame-time discontinuity must not produce an
    // unbounded Doppler impulse. 100 m/s is deliberately generous for games.
    const float maxVelocity = Math::Max(0.0f, MaximumInferredVelocity);
    const float velocityLength = (float)_velocity.Length();
    if (maxVelocity <= 0.0f)
        _velocity = Vector3::Zero;
    else if (velocityLength > maxVelocity)
        _velocity *= maxVelocity / velocityLength;
    _prevPos = pos;
    if (_velocity != prevVelocity && !_velocity.IsNanOrInfinity())
    {
        AudioBackend::Listener::VelocityChanged(_velocity);
    }
}

void AudioListener::OnEnable()
{
    _prevPos = GetPosition();
    _velocity = Vector3::Zero;

    ASSERT(!Audio::Listeners.Contains(this));
    if (Audio::Listeners.Count() >= AUDIO_MAX_LISTENERS)
    {
        if IF_CONSTEXPR (AUDIO_MAX_LISTENERS == 1)
            LOG(Warning, "There is more than one Audio Listener active. Please make sure only exactly one is active at any given time.");
        else
            LOG(Warning, "Too many Audio Listener active.");
    }
    else
    {
        Audio::Listeners.Add(this);
        AudioBackend::Listener::Reset();
        AudioBackend::Listener::TransformChanged(GetPosition(), GetOrientation());
        // Camera rigs commonly finalize their transforms in LateUpdate. Sample
        // velocity after those scripts so position and velocity describe the
        // same frame when the late audio spatial update submits them.
        GetScene()->Ticking.LateUpdate.AddTick<AudioListener, &AudioListener::Update>(this);
    }
#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    // Base
    Actor::OnEnable();
}

void AudioListener::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif
    if (Audio::Listeners.Remove(this))
    {
        GetScene()->Ticking.LateUpdate.RemoveTick(this);
        AudioBackend::Listener::Reset();
    }

    // Base
    Actor::OnDisable();
}

void AudioListener::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);

    if (IsActiveInHierarchy() && IsDuringPlay())
    {
        AudioBackend::Listener::TransformChanged(GetPosition(), GetOrientation());
    }
}
