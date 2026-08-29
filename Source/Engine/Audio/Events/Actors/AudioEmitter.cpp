// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioWorld.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
#include "Engine/Engine/Time.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Debug/DebugDraw.h"
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

void AudioEmitter::SetListenerMask(uint32 value)
{
    _listenerMask = value != 0 ? value : 1u;
    if (_handle.IsValid())
        AudioEventSystem::SetListenerMask(_handle, _listenerMask);
}

void AudioEmitter::Play()
{
#if USE_EDITOR
    if (!IsDuringPlay())
    {
        _lastPlayError = TEXT("Emitter is not in a runtime scene.");
        return;
    }
#endif

    if (_handle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_handle, state))
        {
            if (state.PlaybackState == AudioEventPlaybackState::Playing || state.PlaybackState == AudioEventPlaybackState::Sustaining)
            {
                _lastPlayError.Clear();
                return;
            }
            if (AudioEventSystem::Play(_handle))
            {
                _lastPlayError.Clear();
                return;
            }

            // FMOD can retain a stopped instance whose resources were invalidated
            // by a bank/play-mode transition. A failed restart must not strand the
            // emitter on that handle forever; release and create a fresh instance.
            LOG(Warning, "AudioEmitter '{0}' could not restart its stopped event instance; recreating it.", GetName());
            AudioEventSystem::ReleaseInstance(_handle);
            _handle = AudioEventHandle();
        }
        else
        {
            // The backend may have invalidated this handle during a global stop (for example
            // while leaving Editor play mode). Drop it before creating a fresh instance.
            AudioEventSystem::ReleaseInstance(_handle);
            _handle = AudioEventHandle();
        }
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
    }

    if (!hasTypedEvent)
        path = EventPath;

    if (!eventId.IsValid() && path.IsEmpty())
    {
        _lastPlayError = TEXT("No valid typed event or legacy event path.");
        LOG(Error, "AudioEmitter '{0}' cannot play because it has no valid typed event or legacy event path.", GetName());
        return;
    }

    if (Event)
    {
        const auto* eventData = Event->GetInstance<AudioEvent>();
        if (eventData && !AudioEventCatalog::EnsureDependenciesLoaded(eventData))
        {
            _lastPlayError = String::Format(TEXT("Dependencies for '{0}' are unavailable."), eventData->Path);
            LOG(Error, "AudioEmitter '{0}' could not load dependencies for event '{1}'.", GetName(), eventData->Path);
            return;
        }
    }

    AudioEventCreateOptions options;
    // Configure the entire initial state before start. Parameter-driven FMOD events
    // can have a deliberately silent default state and must not be started before
    // their authored/runtime parameters have been applied.
    options.AutoPlay = false;
    options.Attributes = Audio3DAttributes(GetTransform(), _velocity);
    options.OwnerId = GetID();
    options.ListenerMask = _listenerMask;
    for (const auto& value : InitialParameters)
    {
        AudioParameterId resolved;
        if (value.Id.Name.HasChars() && ResolveParameter(value.Id.Name, resolved))
        {
            AudioParameterValue parameter = value;
            parameter.Id = resolved;
            options.InitialParameters.Add(parameter);
        }
        else if (!value.Id.Name.HasChars() && value.Id.IsValid())
        {
            options.InitialParameters.Add(value);
        }
    }

    _handle = AudioEventSystem::CreateInstance(eventId, path, options);
    if (_handle.IsValid())
    {
        if (!AudioEventSystem::SetVolume(_handle, _volume))
            LOG(Warning, "AudioEmitter '{0}' could not apply volume to event '{1}'.", GetName(), path);
        if (!AudioEventSystem::SetPitch(_handle, _pitch))
            LOG(Warning, "AudioEmitter '{0}' could not apply pitch to event '{1}'.", GetName(), path);
        if (!AudioEventSystem::Play(_handle))
        {
            _lastPlayError = String::Format(TEXT("Backend refused to start '{0}'."), path);
            LOG(Error, "AudioEmitter '{0}' created event '{1}' but the backend refused to start it.", GetName(), path);
            AudioEventSystem::ReleaseInstance(_handle);
            _handle = AudioEventHandle();
        }
        else
        {
            _lastPlayError.Clear();
        }
    }
    else
    {
        _lastPlayError = String::Format(TEXT("Could not create event instance '{0}'."), path);
        LOG(Error, "AudioEmitter '{0}' could not create event instance '{1}' ({2}).", GetName(), path, eventId);
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
        AudioEventSystem::StopAndRelease(_handle, _stopMode);
        _handle = AudioEventHandle();
    }
}

bool AudioEmitter::SignalActivation(AudioActivationEvent activationEvent, Actor* source, Actor* target)
{
    bool handled = false;
    if (_playActivationState.TryActivate(PlayActivation, activationEvent, source, target))
    {
        Play();
        handled = true;
    }
    if (_stopActivationState.TryActivate(StopActivation, activationEvent, source, target))
    {
        Stop();
        handled = true;
    }
    if (activationEvent == AudioActivationEvent::TriggerExit || activationEvent == AudioActivationEvent::CollisionExit || activationEvent == AudioActivationEvent::PointerExit)
    {
        _playActivationState.NotifyExit(PlayActivation);
        _stopActivationState.NotifyExit(StopActivation);
    }
    return handled;
}

bool AudioEmitter::SetParameter(const StringView& name, float value, bool ignoreSeekSpeed)
{
    AudioParameterId id;
    if (!ResolveParameter(name, id))
        return false;
    bool found = false;
    for (auto& parameter : InitialParameters)
    {
        if (parameter.Id == id)
        {
            parameter.Value = value;
            found = true;
            break;
        }
    }
    if (!found)
    {
        auto& parameter = InitialParameters.AddOne();
        parameter.Id = id;
        parameter.Value = value;
    }

    if (_handle.IsValid())
        return AudioEventSystem::SetParameter(_handle, id, value, ignoreSeekSpeed);
    return true;
}

bool AudioEmitter::SetParameterLabel(const StringView& name, const StringView& label, bool ignoreSeekSpeed)
{
    AudioParameterId id;
    if (_handle.IsValid() && ResolveParameter(name, id))
        return AudioEventSystem::SetParameterLabel(_handle, id, label, ignoreSeekSpeed);
    return false;
}

bool AudioEmitter::ResolveParameter(const StringView& name, AudioParameterId& result) const
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
        const float maxVelocity = 10000.0f;
        const float velocityLength = (float)_velocity.Length();
        if (velocityLength > maxVelocity)
            _velocity *= maxVelocity / velocityLength;
        if (_velocity.IsNanOrInfinity())
            _velocity = Vector3::Zero;
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

void AudioEmitter::HandleEventCallback(const AudioEventCallback& callback)
{
    switch (callback.Type)
    {
    case AudioEventCallbackType::Started:
    case AudioEventCallbackType::Restarted:
        Started();
        break;
    case AudioEventCallbackType::Stopped:
        Stopped();
        break;
    case AudioEventCallbackType::TimelineMarker:
        TimelineMarker(callback.Marker, callback.TimelinePositionMs);
        break;
    case AudioEventCallbackType::TimelineBeat:
    {
        AudioTimelineBeat beat;
        beat.PositionMs = callback.TimelinePositionMs;
        beat.Bar = callback.Bar;
        beat.Beat = callback.Beat;
        beat.Tempo = callback.Tempo;
        beat.TimeSignatureUpper = callback.TimeSignatureUpper;
        beat.TimeSignatureLower = callback.TimeSignatureLower;
        TimelineBeat(beat);
        break;
    }
    default:
        break;
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
    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorEnable, this, this);
}

void AudioEmitter::OnDisable()
{
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif

    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorDisable, this, this);
    if (_stopOnDisable)
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

void AudioEmitter::FlushDeferredBeginPlayAudio()
{
    if (!_beginPlayAudioPending)
        return;

    _beginPlayAudioPending = false;
    if (_playOnStart && IsDuringPlay())
        Play();
    SignalActivation(AudioActivationEvent::BeginPlay, this, this);
}

void AudioEmitter::BeginPlay(SceneBeginData* data)
{
    Actor::BeginPlay(data);

    _playActivationState.Reset();
    _stopActivationState.Reset();
    // AudioService reloads startup banks in its first play-mode Update. Defer
    // begin-play audio until AudioWorld's late update so event descriptions are
    // available before either PlayOnStart or a BeginPlay activation creates one.
    _beginPlayAudioPending = IsDuringPlay();
}

void AudioEmitter::EndPlay()
{
    _beginPlayAudioPending = false;
    SignalActivation(AudioActivationEvent::EndPlay, this, this);
    Actor::EndPlay();
}

void AudioEmitter::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioEmitter);

    SERIALIZE(Event);
    SERIALIZE(EventPath);
    SERIALIZE(InitialParameters);
    SERIALIZE(PlayActivation);
    SERIALIZE(StopActivation);
    SERIALIZE_MEMBER(Volume, _volume);
    SERIALIZE_MEMBER(Pitch, _pitch);
    SERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    SERIALIZE_MEMBER(StopMode, _stopMode);
    SERIALIZE_MEMBER(StopOnDisable, _stopOnDisable);
    SERIALIZE_MEMBER(ListenerMask, _listenerMask);
    SERIALIZE(Occlusion);
    SERIALIZE(OcclusionPriority);
}

void AudioEmitter::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);

    DESERIALIZE(Event);
    DESERIALIZE(EventPath);
    DESERIALIZE(InitialParameters);
    DESERIALIZE(PlayActivation);
    DESERIALIZE(StopActivation);
    DESERIALIZE_MEMBER(Volume, _volume);
    DESERIALIZE_MEMBER(Pitch, _pitch);
    DESERIALIZE_MEMBER(PlayOnStart, _playOnStart);
    DESERIALIZE_MEMBER(StopMode, _stopMode);
    DESERIALIZE_MEMBER(StopOnDisable, _stopOnDisable);
    DESERIALIZE_MEMBER(ListenerMask, _listenerMask);
    DESERIALIZE(Occlusion);
    DESERIALIZE(OcclusionPriority);
    Occlusion.Sanitize();
}
