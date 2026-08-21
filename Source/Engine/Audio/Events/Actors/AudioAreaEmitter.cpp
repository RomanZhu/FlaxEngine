// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioAreaEmitter.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
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
    for (auto& voice : _listenerVoices)
        if (voice.Handle.IsValid())
            AudioEventSystem::SetVolume(voice.Handle, _volume * voice.Weight);
    if (_handle.IsValid())
        AudioEventSystem::SetVolume(_handle, _volume * _currentWeight);
}

void AudioAreaEmitter::SetPitch(float value)
{
    _pitch = Math::Clamp(value, 0.5f, 2.0f);
    for (auto& voice : _listenerVoices)
        if (voice.Handle.IsValid())
            AudioEventSystem::SetPitch(voice.Handle, _pitch);
    if (_handle.IsValid())
        AudioEventSystem::SetPitch(_handle, _pitch);
}

AudioEventHandle AudioAreaEmitter::CreateVoice(const Vector3& position, uint32 listenerMask)
{
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
        return AudioEventHandle();
    if (Event)
    {
        const auto* eventData = Event->GetInstance<AudioEvent>();
        if (eventData && !AudioEventCatalog::EnsureDependenciesLoaded(eventData))
            return AudioEventHandle();
    }
    AudioEventCreateOptions options;
    options.AutoPlay = true;
    options.Attributes = Audio3DAttributes(position, Vector3::Zero, Vector3::Forward, Vector3::Up);
    options.OwnerId = GetID();
    options.ListenerMask = listenerMask;
    const AudioEventHandle handle = AudioEventSystem::CreateInstance(eventId, path, options);
    if (handle.IsValid())
    {
        AudioEventSystem::SetVolume(handle, _volume);
        AudioEventSystem::SetPitch(handle, _pitch);
    }
    return handle;
}

void AudioAreaEmitter::Play()
{
#if USE_EDITOR
    if (!IsDuringPlay())
        return;
#endif
    // Manual playback uses the same listener-scoped ownership as automatic
    // area playback. Keep the request armed so PlayOnStart=false still creates
    // voices when an eligible listener enters, without a transient all-listener
    // instance that would be cut or leaked on the next AudioWorld pass.
    _manualPlayRequested = true;
    UpdateAllListeners();
}

void AudioAreaEmitter::Stop()
{
    _manualPlayRequested = false;
    AudioEventHandle manualHandle = _handle;
    _handle = AudioEventHandle();
    bool manualIsVoice = false;
    for (const auto& voice : _listenerVoices)
        manualIsVoice |= voice.Handle == manualHandle;
    for (auto& voice : _listenerVoices)
        StopVoice(voice);
    _listenerVoices.Clear();
    if (manualHandle.IsValid() && !manualIsVoice)
    {
        AudioEventSystem::StopAndRelease(manualHandle, AudioStopMode::AllowFadeOut);
    }
    _belowStopDuration = 0.0f;
}

void AudioAreaEmitter::StopVoice(ListenerVoice& voice, AudioStopMode stopMode)
{
    if (voice.Handle.IsValid())
    {
        AudioEventSystem::StopAndRelease(voice.Handle, stopMode);
        voice.Handle = AudioEventHandle();
    }
    voice.BelowStopDuration = 0.0f;
}

void AudioAreaEmitter::UpdateListenerPosition(const Vector3& listenerPosition)
{
    const AudioVolumeSample sample = Evaluate(listenerPosition);
    UpdateVoice(nullptr, GetListenerMask(), listenerPosition, sample);
}

void AudioAreaEmitter::ApplyResolvedSample(const Vector3& listenerPosition, const AudioVolumeSample& sample, float resolvedWeight)
{
    if (!IsDuringPlay())
        return;

    AudioVolumeSample resolved = sample;
    resolved.Weight = resolvedWeight;
    UpdateVoice(nullptr, GetListenerMask(), listenerPosition, resolved);
}

void AudioAreaEmitter::BeginListenerUpdate()
{
    bool handleBelongsToVoice = false;
    for (const auto& voice : _listenerVoices)
        handleBelongsToVoice |= voice.Handle == _handle;
    if (_handle.IsValid() && !handleBelongsToVoice)
    {
        AudioEventSystem::StopAndRelease(_handle, AudioStopMode::AllowFadeOut);
        _handle = AudioEventHandle();
    }
    _currentWeight = 0.0f;
    for (auto& voice : _listenerVoices)
        voice.Seen = false;
}

void AudioAreaEmitter::ApplyResolvedListenerSample(AudioListener* listener, int32 listenerIndex, const AudioVolumeSample& sample, float resolvedWeight)
{
    if (!listener || listenerIndex < 0 || listenerIndex >= 32)
        return;
    AudioVolumeSample resolved = sample;
    resolved.Weight = Math::Saturate(resolvedWeight);
    UpdateVoice(listener, 1u << listenerIndex, listener->GetPosition(), resolved);
    for (auto& voice : _listenerVoices)
        if (voice.Listener == listener)
        {
            voice.Seen = true;
            break;
        }
}

void AudioAreaEmitter::EndListenerUpdate()
{
    for (int32 i = _listenerVoices.Count() - 1; i >= 0; i--)
    {
        if (_listenerVoices[i].Seen)
            continue;
        StopVoice(_listenerVoices[i]);
        _listenerVoices.RemoveAt(i);
    }
    _handle = _listenerVoices.HasItems() ? _listenerVoices[0].Handle : AudioEventHandle();
}

void AudioAreaEmitter::UpdateVoice(AudioListener* listener, uint32 listenerMask, const Vector3& listenerPosition, const AudioVolumeSample& sample)
{
    if (!IsDuringPlay())
        return;

    int32 voiceIndex = -1;
    for (int32 i = 0; i < _listenerVoices.Count(); i++)
        if (_listenerVoices[i].Listener == listener)
        {
            voiceIndex = i;
            break;
        }
    if (voiceIndex < 0)
    {
        ListenerVoice voice;
        voice.Listener = listener;
        _listenerVoices.Add(voice);
        voiceIndex = _listenerVoices.Count() - 1;
    }
    auto& voice = _listenerVoices[voiceIndex];
    const float weight = Math::Saturate(sample.Weight);
    voice.Weight = weight;
    if (voice.Handle.IsValid())
    {
        AudioEventInstanceState state;
        if (!AudioEventSystem::QueryInstance(voice.Handle, state))
            voice.Handle = AudioEventHandle();
    }
    if (!voice.Handle.IsValid() && weight >= StartThreshold && (_playOnStart || _manualPlayRequested))
        voice.Handle = CreateVoice(sample.ClosestPoint, listenerMask);
    if (voice.Handle.IsValid())
    {
        Vector3 velocity = Vector3::Zero;
        if (!FollowListenerInside)
        {
            const float dt = (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
            if (dt > 0.00001f)
                velocity = (sample.ClosestPoint - voice.PreviousSourcePosition) / dt;
        }
        voice.PreviousSourcePosition = sample.ClosestPoint;
        AudioEventSystem::Set3DAttributes(voice.Handle, Audio3DAttributes(sample.ClosestPoint, velocity, Vector3::Forward, Vector3::Up));
        AudioEventSystem::SetListenerMask(voice.Handle, listenerMask);
        AudioEventSystem::SetVolume(voice.Handle, _volume * weight);
        if (weight <= StopThreshold)
        {
            voice.BelowStopDuration += (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
            if (voice.BelowStopDuration >= StopDelay)
                StopVoice(voice);
        }
        else
            voice.BelowStopDuration = 0.0f;
    }
    if (weight > _currentWeight)
    {
        _currentWeight = weight;
        _currentClosestPoint = sample.ClosestPoint;
    }
    _handle = voice.Handle;
}

void AudioAreaEmitter::UpdateAllListeners()
{
    bool handleBelongsToVoice = false;
    for (const auto& voice : _listenerVoices)
        handleBelongsToVoice |= voice.Handle == _handle;
    if (_handle.IsValid() && !handleBelongsToVoice)
    {
        AudioEventSystem::StopAndRelease(_handle, AudioStopMode::AllowFadeOut);
        _handle = AudioEventHandle();
    }
    _currentWeight = 0.0f;
    Array<AudioListener*> seen;
    int32 compactIndex = 0;
    for (auto* listener : Audio::Listeners)
    {
        if (!listener || !listener->IsActiveInHierarchy() || !listener->IsDuringPlay())
            continue;
        const uint32 listenerBit = compactIndex < 32 ? (1u << compactIndex) : 0u;
        compactIndex++;
        if ((GetListenerMask() & listenerBit) == 0)
            continue;
        seen.Add(listener);
        UpdateVoice(listener, listenerBit, listener->GetPosition(), Evaluate(listener->GetPosition()));
    }
    for (int32 i = _listenerVoices.Count() - 1; i >= 0; i--)
    {
        if (!seen.Contains(_listenerVoices[i].Listener))
        {
            StopVoice(_listenerVoices[i]);
            _listenerVoices.RemoveAt(i);
        }
    }
    if (_listenerVoices.IsEmpty())
        _handle = AudioEventHandle();
}

bool AudioAreaEmitter::SetParameter(const StringView& name, float value, bool ignoreSeekSpeed)
{
    AudioParameterId id;
    if (!ResolveParameter(name, id))
        return false;
    bool result = false;
    for (auto& voice : _listenerVoices)
        if (voice.Handle.IsValid())
            result |= AudioEventSystem::SetParameter(voice.Handle, id, value, ignoreSeekSpeed);
    return result || (_handle.IsValid() && AudioEventSystem::SetParameter(_handle, id, value, ignoreSeekSpeed));
}

bool AudioAreaEmitter::SetParameterLabel(const StringView& name, const StringView& label, bool ignoreSeekSpeed)
{
    AudioParameterId id;
    if (!ResolveParameter(name, id))
        return false;
    bool result = false;
    for (auto& voice : _listenerVoices)
        if (voice.Handle.IsValid())
            result |= AudioEventSystem::SetParameterLabel(voice.Handle, id, label, ignoreSeekSpeed);
    return result || (_handle.IsValid() && AudioEventSystem::SetParameterLabel(_handle, id, label, ignoreSeekSpeed));
}

bool AudioAreaEmitter::ResolveParameter(const StringView& name, AudioParameterId& result) const
{
    if (name.IsEmpty())
        return false;
    Guid eventId = Guid::Empty;
    StringView eventPath = EventPath;
    if (Event)
    {
        Event->WaitForLoaded();
        if (const auto* data = Event->GetInstance<AudioEvent>())
        {
            eventId = data->BackendId;
            eventPath = data->Path;
        }
    }
    return AudioEventSystem::ResolveParameterId(eventId, eventPath, name, result);
}

AudioEventPlaybackState AudioAreaEmitter::GetPlaybackState() const
{
    AudioEventInstanceState state;
    return _handle.IsValid() && AudioEventSystem::QueryInstance(_handle, state) ? state.PlaybackState : AudioEventPlaybackState::Stopped;
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
    _manualPlayRequested = false;

    // Voices are created on the first world sample, once compact listener
    // indices and listener masks are known.
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
