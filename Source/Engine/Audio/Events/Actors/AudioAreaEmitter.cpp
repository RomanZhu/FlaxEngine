// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioAreaEmitter.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Engine/Time.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"

AudioAreaEmitter::AudioAreaEmitter(const SpawnParams& params)
    : AudioVolumeBase(params)
{
}

void AudioAreaEmitter::SetVolume(float value)
{
    _volume = Math::Saturate(value);

    if (_handle.IsValid())
    {
        // Keep the current listener-dependent attenuation when applying a volume change immediately.
        float weight = 1.0f;
        for (int32 i = 0; i < Audio::Listeners.Count(); i++)
        {
            auto* listener = Audio::Listeners[i];
            if (listener && listener->IsActiveInHierarchy() && listener->IsDuringPlay())
            {
                weight = Evaluate(listener->GetPosition()).Weight;
                break;
            }
        }
        AudioEventSystem::SetVolume(_handle, _volume * weight);
    }
}

void AudioAreaEmitter::SetPitch(float value)
{
    _pitch = Math::Clamp(value, 0.5f, 2.0f);
    if (_handle.IsValid())
        AudioEventSystem::SetPitch(_handle, _pitch);
}

void AudioAreaEmitter::Play()
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
            if (state.PlaybackState != AudioEventPlaybackState::Playing && state.PlaybackState != AudioEventPlaybackState::Sustaining)
                AudioEventSystem::Play(_handle);
            return;
        }
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
    options.Attributes = Audio3DAttributes(GetPosition(), Vector3::Zero, Vector3::Forward, Vector3::Up);
    options.OwnerId = GetID();

    _handle = AudioEventSystem::CreateInstance(eventId, path, options);
    if (_handle.IsValid())
    {
        AudioEventSystem::SetVolume(_handle, _volume);
        AudioEventSystem::SetPitch(_handle, _pitch);
    }
}

void AudioAreaEmitter::Stop()
{
    if (_handle.IsValid())
    {
        AudioEventSystem::Stop(_handle, AudioStopMode::AllowFadeOut);
        AudioEventSystem::ReleaseInstance(_handle);
        _handle = AudioEventHandle();
    }
    _belowStopDuration = 0.0f;
}

void AudioAreaEmitter::UpdateListenerPosition(const Vector3& listenerPosition)
{
    if (!IsDuringPlay())
        return;

    AudioVolumeSample sample = Evaluate(listenerPosition);

    if (_handle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_handle, state))
        {
            Vector3 velocity = Vector3::Zero;
            if (!FollowListenerInside)
            {
                const float dt = (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
                if (dt > 0.00001f)
                    velocity = (sample.ClosestPoint - _previousSourcePosition) / dt;
            }
            _previousSourcePosition = sample.ClosestPoint;
            Audio3DAttributes attrs(sample.ClosestPoint, velocity, Vector3::Forward, Vector3::Up);
            AudioEventSystem::Set3DAttributes(_handle, attrs);
            AudioEventSystem::SetVolume(_handle, _volume * sample.Weight);

            if (sample.Weight <= StopThreshold)
            {
                _belowStopDuration += (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
                if (_belowStopDuration >= StopDelay)
                    Stop();
            }
            else
            {
                _belowStopDuration = 0.0f;
            }
        }
        else
        {
            _handle = AudioEventHandle();
        }
    }

    if (!_handle.IsValid() && sample.Weight >= StartThreshold && _playOnStart)
    {
        Play();
    }
}

void AudioAreaEmitter::OnEnable()
{
    AudioVolumeBase::OnEnable();
}

void AudioAreaEmitter::OnDisable()
{
    Stop();
    AudioVolumeBase::OnDisable();
}

void AudioAreaEmitter::BeginPlay(SceneBeginData* data)
{
    AudioVolumeBase::BeginPlay(data);

    // Startup follows the same threshold as ongoing automatic playback; creating
    // an instance outside the volume only to stop it next update wastes voices.
    if (_playOnStart && IsDuringPlay())
    {
        for (int32 i = 0; i < Audio::Listeners.Count(); i++)
        {
            auto* listener = Audio::Listeners[i];
            if (listener && listener->IsActiveInHierarchy() && listener->IsDuringPlay() && Evaluate(listener->GetPosition()).Weight >= StartThreshold)
            {
                Play();
                break;
            }
        }
    }
}

void AudioAreaEmitter::Serialize(SerializeStream& stream, const void* otherObj)
{
    AudioVolumeBase::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioAreaEmitter);

    SERIALIZE(Event);
    SERIALIZE(EventPath);
    SERIALIZE_MEMBER(Volume, _volume);
    SERIALIZE_MEMBER(Pitch, _pitch);
    SERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    SERIALIZE(StartThreshold);
    SERIALIZE(StopThreshold);
    SERIALIZE(StopDelay);
    SERIALIZE(FollowListenerInside);
}

void AudioAreaEmitter::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    AudioVolumeBase::Deserialize(stream, modifier);

    DESERIALIZE(Event);
    DESERIALIZE(EventPath);
    DESERIALIZE_MEMBER(Volume, _volume);
    DESERIALIZE_MEMBER(Pitch, _pitch);
    DESERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    DESERIALIZE(StartThreshold);
    DESERIALIZE(StopThreshold);
    DESERIALIZE(StopDelay);
    DESERIALIZE(FollowListenerInside);
    StartThreshold = Math::Saturate(StartThreshold);
    StopThreshold = Math::Saturate(StopThreshold);
    StopDelay = Math::Max(0.0f, StopDelay);
}
