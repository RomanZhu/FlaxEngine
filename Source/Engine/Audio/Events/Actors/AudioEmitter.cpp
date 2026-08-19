// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioWorld.h"
#include "Engine/Engine/Time.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Debug/DebugDraw.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Core/Log.h"

AudioEmitter::AudioEmitter(const SpawnParams& params)
    : Actor(params)
{
}

void AudioEmitter::SetVolume(float value)
{
    _volume = Math::Saturate(value);
    if (_handle.IsValid())
        AudioEventSystem::SetVolume(_handle, _volume);
}

void AudioEmitter::SetPitch(float value)
{
    _pitch = Math::Clamp(value, 0.5f, 2.0f);
    if (_handle.IsValid())
        AudioEventSystem::SetPitch(_handle, _pitch);
}

void AudioEmitter::SetPlayOnStart(bool value)
{
    _playOnStart = value;
}

void AudioEmitter::SetStopMode(AudioStopMode value)
{
    _stopMode = value;
}

void AudioEmitter::Play()
{
#if USE_EDITOR
    if (!IsDuringPlay())
        return;
#endif

    if (_handle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_handle, state))
        {
            if (state.PlaybackState == AudioEventPlaybackState::Playing || state.PlaybackState == AudioEventPlaybackState::Sustaining)
                return;
            AudioEventSystem::Play(_handle);
            return;
        }

        // The backend may have invalidated this handle during a global stop (for example
        // while leaving Editor play mode). Drop it before creating a fresh instance.
        AudioEventSystem::ReleaseInstance(_handle);
        _handle = AudioEventHandle();
    }

    Guid eventId = Guid::Empty;
    String path;
    bool hasTypedEvent = false;

    if (Event)
    {
        Event->WaitForLoaded();
        const auto* data = Event->GetInstance<AudioEvent>();
        if (data)
        {
            hasTypedEvent = true;
            eventId = data->BackendId;
            path = data->Path;
        }
        else if (Event->Data && Event->DataTypeName == TEXT("FlaxEngine.AudioEvent"))
        {
            hasTypedEvent = true;
            auto& node = *Event->Data;
            auto itBackend = node.FindMember("BackendId");
            if (itBackend != node.MemberEnd() && itBackend->value.IsString())
                JsonTools::GetGuid(eventId, node, "BackendId");
            auto itPath = node.FindMember("Path");
            if (itPath != node.MemberEnd() && itPath->value.IsString())
                path = itPath->value.GetString();
        }
    }

    if (!hasTypedEvent)
        path = EventPath;

    if (!eventId.IsValid() && path.IsEmpty())
        return;

    AudioEventCreateOptions options;
    options.AutoPlay = true;
    options.Attributes = Audio3DAttributes(GetTransform(), _velocity);
    options.OwnerId = GetID();

    _handle = AudioEventSystem::CreateInstance(eventId, path, options);
    if (_handle.IsValid())
    {
        AudioEventSystem::SetVolume(_handle, _volume);
        AudioEventSystem::SetPitch(_handle, _pitch);
    }
}

void AudioEmitter::Pause()
{
    if (_handle.IsValid())
        AudioEventSystem::Pause(_handle);
}

void AudioEmitter::Stop()
{
    if (_handle.IsValid())
    {
        AudioEventSystem::Stop(_handle, _stopMode);
        AudioEventSystem::ReleaseInstance(_handle);
        _handle = AudioEventHandle();
    }
}

bool AudioEmitter::SetParameter(const StringView& name, float value, bool ignoreSeekSpeed)
{
    if (_handle.IsValid())
        return AudioEventSystem::SetParameter(_handle, AudioParameterId(name), value, ignoreSeekSpeed);
    return false;
}

bool AudioEmitter::SetParameterLabel(const StringView& name, const StringView& label, bool ignoreSeekSpeed)
{
    if (_handle.IsValid())
        return AudioEventSystem::SetParameterLabel(_handle, AudioParameterId(name), label, ignoreSeekSpeed);
    return false;
}

AudioEventPlaybackState AudioEmitter::GetPlaybackState() const
{
    if (_handle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_handle, state))
            return state.PlaybackState;
    }
    return AudioEventPlaybackState::Stopped;
}

bool AudioEmitter::IsActuallyPlaying() const
{
    auto state = GetPlaybackState();
    return state == AudioEventPlaybackState::Playing || state == AudioEventPlaybackState::Sustaining;
}

void AudioEmitter::UpdateVelocity(float dt)
{
    const Vector3 pos = GetPosition();
    if (dt > 0.00001f)
    {
        _velocity = (pos - _prevPos) / dt;
    }
    _prevPos = pos;
}

void AudioEmitter::Push3DAttributes()
{
    if (_handle.IsValid())
    {
        Audio3DAttributes attrs(GetTransform(), _velocity);
        AudioEventSystem::Set3DAttributes(_handle, attrs);
    }
}

bool AudioEmitter::IntersectsItself(const Ray& ray, Real& distance, Vector3& normal)
{
    return false;
}

#if USE_EDITOR
void AudioEmitter::OnDebugDrawSelected()
{
    DEBUG_DRAW_SPHERE(BoundingSphere(GetPosition(), 100.0f), Color::DeepSkyBlue, 0, true);
    Actor::OnDebugDrawSelected();
}
#endif

void AudioEmitter::OnEnable()
{
    _prevPos = GetPosition();
    _velocity = Vector3::Zero;

    AudioWorld::Register(this);

#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif

    Actor::OnEnable();
}

void AudioEmitter::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif

    Stop();
    AudioWorld::Unregister(this);

    Actor::OnDisable();
}

void AudioEmitter::OnTransformChanged()
{
    Actor::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);

    Push3DAttributes();
}

void AudioEmitter::BeginPlay(SceneBeginData* data)
{
    Actor::BeginPlay(data);

    if (_playOnStart && IsDuringPlay())
        Play();
}

void AudioEmitter::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioEmitter);

    SERIALIZE(Event);
    SERIALIZE(EventPath);
    SERIALIZE_MEMBER(Volume, _volume);
    SERIALIZE_MEMBER(Pitch, _pitch);
    SERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    SERIALIZE_MEMBER(StopMode, _stopMode);
}

void AudioEmitter::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE(Event);
    DESERIALIZE(EventPath);
    DESERIALIZE_MEMBER(Volume, _volume);
    DESERIALIZE_MEMBER(Pitch, _pitch);
    DESERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    DESERIALIZE_MEMBER(StopMode, _stopMode);
}
