// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioTrigger.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Engine/Time.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"

AudioTrigger::AudioTrigger(const SpawnParams& params)
    : AudioVolumeBase(params)
{
}

void AudioTrigger::SetCooldown(float value)
{
    _cooldown = Math::Max(0.0f, value);
}

void AudioTrigger::SetVolume(float value)
{
    _volume = Math::Saturate(value);
    if (_handle.IsValid())
        AudioEventSystem::SetVolume(_handle, _volume);
}

void AudioTrigger::SetPitch(float value)
{
    _pitch = Math::Clamp(value, 0.5f, 2.0f);
    if (_handle.IsValid())
        AudioEventSystem::SetPitch(_handle, _pitch);
}

bool AudioTrigger::ShouldTrigger(bool entered) const
{
    switch (_activationMode)
    {
    case AudioTriggerActivationMode::ListenerEnter:
        return entered;
    case AudioTriggerActivationMode::ListenerExit:
        return !entered;
    case AudioTriggerActivationMode::ListenerEnterAndExit:
        return true;
    default:
        return false;
    }
}

bool AudioTrigger::StartEvent()
{
    if (_triggerOnce && _hasTriggered)
        return false;
    if (_cooldownRemaining > 0.0f)
        return false;

    // A persistent instance owns the boundary-crossing lifetime. Never create a
    // second instance while the previous one is still retained by this actor.
    if (_handle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_handle, state))
        {
            if (state.PlaybackState != AudioEventPlaybackState::Stopped)
                return false;

            if (AudioEventSystem::Play(_handle))
            {
                AudioEventSystem::SetVolume(_handle, _volume);
                AudioEventSystem::SetPitch(_handle, _pitch);
                _hasTriggered = true;
                _cooldownRemaining = _cooldown;
                return true;
            }

            AudioEventSystem::ReleaseInstance(_handle);
        }

        // Backends can invalidate all instances (for example during a bank or
        // device reset). Do not leave the trigger permanently wedged on a stale
        // generation-safe handle.
        _handle = AudioEventHandle();
    }

    Guid eventId = Guid::Empty;
    String path;
    bool hasTypedEvent = false;
    bool oneShot = false;

    if (Event)
    {
        Event->WaitForLoaded();
        const auto* data = Event->GetInstance<AudioEvent>();
        if (data)
        {
            hasTypedEvent = true;
            eventId = data->BackendId;
            path = data->Path;
            oneShot = data->IsOneShot;
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
            auto itOneShot = node.FindMember("IsOneShot");
            if (itOneShot != node.MemberEnd() && itOneShot->value.IsBool())
                oneShot = itOneShot->value.GetBool();
        }
    }

    if (!hasTypedEvent)
        path = EventPath;

    if (!eventId.IsValid() && path.IsEmpty())
        return false;

    if (Event)
    {
        const auto* eventData = Event->GetInstance<AudioEvent>();
        if (eventData && !AudioEventCatalog::EnsureDependenciesLoaded(eventData))
            return false;
    }

    const Audio3DAttributes attributes(GetPosition(), Vector3::Zero, Vector3::Forward, Vector3::Up);
    bool started = false;
    AudioEventCreateOptions options;
    options.AutoPlay = true;
    options.Attributes = attributes;
    options.OwnerId = GetID();
    options.ListenerMask = GetListenerMask();
    if (oneShot)
    {
        // Use an explicit instance so the trigger's listener mask is applied.
        // Releasing an FMOD one-shot relinquishes ownership while authored
        // playback continues to completion.
        const AudioEventHandle oneShotHandle = AudioEventSystem::CreateInstance(eventId, path, options);
        started = oneShotHandle.IsValid();
        if (started)
        {
            AudioEventSystem::SetVolume(oneShotHandle, _volume);
            AudioEventSystem::SetPitch(oneShotHandle, _pitch);
            AudioEventSystem::ReleaseInstance(oneShotHandle);
        }
    }
    else
    {
        _handle = AudioEventSystem::CreateInstance(eventId, path, options);
        started = _handle.IsValid();
        if (started)
        {
            AudioEventSystem::SetVolume(_handle, _volume);
            AudioEventSystem::SetPitch(_handle, _pitch);
        }
    }

    if (started)
    {
        _hasTriggered = true;
        _cooldownRemaining = _cooldown;
    }
    return started;
}

bool AudioTrigger::ExecuteAction()
{
    switch (_action)
    {
    case AudioTriggerActionType::StopEvent:
    case AudioTriggerActionType::StopSnapshot:
        Stop();
        return true;
    case AudioTriggerActionType::SetGlobalParameter:
        return ActionParameter.IsValid() && AudioEventSystem::SetGlobalParameter(ActionParameter, ActionValue);
    case AudioTriggerActionType::SetBusVolume:
        return AudioEventSystem::SetBusVolume(Guid::Empty, MixerPath, Math::Saturate(ActionValue));
    case AudioTriggerActionType::SetVCAVolume:
        return AudioEventSystem::SetVCAVolume(Guid::Empty, MixerPath, Math::Saturate(ActionValue));
    case AudioTriggerActionType::MuteBus:
        return AudioEventSystem::SetBusMute(Guid::Empty, MixerPath, ActionValue >= 0.5f);
    case AudioTriggerActionType::PauseBus:
        return AudioEventSystem::SetBusPaused(Guid::Empty, MixerPath, ActionValue >= 0.5f);
    case AudioTriggerActionType::PlayOneShot:
    case AudioTriggerActionType::StartPersistentEvent:
    case AudioTriggerActionType::StartSnapshot:
    default:
        return StartEvent();
    }
}

void AudioTrigger::UpdateListenerPosition(const Vector3& listenerPosition)
{
    if (!IsDuringPlay())
        return;

    ProcessSample(Evaluate(listenerPosition));
}

void AudioTrigger::UpdateTarget(Actor* target)
{
    Array<Actor*> targets;
    if (target)
        targets.Add(target);
    UpdateTargets(targets);
}

void AudioTrigger::UpdateTargets(const Array<Actor*>& targets)
{
    if (!IsDuringPlay())
        return;

    AudioVolumeSample aggregate;
    bool anyInside = false;
    for (int32 i = 0; i < targets.Count(); i++)
    {
        auto* target = targets[i];
        if (!target)
            continue;
        bool accepted = target->IsActiveInHierarchy();
        switch (_targetMode)
        {
        case AudioTriggerTargetMode::Listener:
            accepted &= dynamic_cast<AudioListener*>(target) != nullptr && i < 32 && (GetListenerMask() & (1u << i)) != 0;
            break;
        case AudioTriggerTargetMode::Actor:
            accepted &= !TargetActor || TargetActor == target;
            break;
        case AudioTriggerTargetMode::LayerMask:
            accepted &= TargetLayerMask == 0 || (static_cast<uint32>(target->GetLayerMask()) & TargetLayerMask) != 0;
            break;
        case AudioTriggerTargetMode::Tag:
            accepted &= !TargetTag || target->HasTag(TargetTag);
            break;
        default:
            break;
        }

        if (!accepted)
            continue;
        const AudioVolumeSample sample = Evaluate(target->GetPosition());
        if (sample.IsInside)
            anyInside = true;
        if (sample.Weight > aggregate.Weight)
            aggregate = sample;
    }
    aggregate.IsInside = anyInside;
    ProcessSample(aggregate);
}

void AudioTrigger::ProcessSample(const AudioVolumeSample& sample)
{
    if (!IsDuringPlay())
        return;

    if (_cooldownRemaining > 0.0f)
        _cooldownRemaining = Math::Max(0.0f, _cooldownRemaining - Time::GetDeltaTime());

    const bool entered = sample.IsInside && (!_hasSample || !_isInside);
    const bool exited = _hasSample && _isInside && !sample.IsInside;

    if (exited && _stopOnExit)
        Stop();

    if ((entered || exited) && ShouldTrigger(entered))
        ExecuteAction();

    if (exited && _rearmOnExit)
        _hasTriggered = false;

    _isInside = sample.IsInside;
    _hasSample = true;
}

void AudioTrigger::Stop()
{
    if (_handle.IsValid())
    {
        AudioEventSystem::StopAndRelease(_handle, _stopMode);
        _handle = AudioEventHandle();
    }
}

void AudioTrigger::OnEnable()
{
    AudioVolumeBase::OnEnable();
}

void AudioTrigger::OnDisable()
{
    Stop();
    _hasSample = false;
    _isInside = false;
    AudioVolumeBase::OnDisable();
}

void AudioTrigger::BeginPlay(SceneBeginData* data)
{
    Stop();
    AudioVolumeBase::BeginPlay(data);
    _hasSample = false;
    _isInside = false;
    _hasTriggered = false;
    _cooldownRemaining = 0.0f;
}

void AudioTrigger::Serialize(SerializeStream& stream, const void* otherObj)
{
    AudioVolumeBase::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioTrigger);

    SERIALIZE(Event);
    SERIALIZE(EventPath);
    SERIALIZE_MEMBER(ActivationMode, _activationMode);
    SERIALIZE_MEMBER(StopOnExit, _stopOnExit);
    SERIALIZE_MEMBER(TriggerOnce, _triggerOnce);
    SERIALIZE_MEMBER(RearmOnExit, _rearmOnExit);
    SERIALIZE_MEMBER(TargetMode, _targetMode);
    SERIALIZE(TargetActor);
    SERIALIZE(TargetLayerMask);
    SERIALIZE(TargetTag);
    SERIALIZE_MEMBER(Action, _action);
    SERIALIZE(ActionParameter);
    SERIALIZE(ActionValue);
    SERIALIZE(MixerPath);
    SERIALIZE_MEMBER(Cooldown, _cooldown);
    SERIALIZE_MEMBER(Volume, _volume);
    SERIALIZE_MEMBER(Pitch, _pitch);
    SERIALIZE_MEMBER(StopMode, _stopMode);
}

void AudioTrigger::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    AudioVolumeBase::Deserialize(stream, modifier);

    DESERIALIZE(Event);
    DESERIALIZE(EventPath);
    DESERIALIZE_MEMBER(ActivationMode, _activationMode);
    DESERIALIZE_MEMBER(StopOnExit, _stopOnExit);
    DESERIALIZE_MEMBER(TriggerOnce, _triggerOnce);
    DESERIALIZE_MEMBER(RearmOnExit, _rearmOnExit);
    DESERIALIZE_MEMBER(TargetMode, _targetMode);
    DESERIALIZE(TargetActor);
    DESERIALIZE(TargetLayerMask);
    DESERIALIZE(TargetTag);
    DESERIALIZE_MEMBER(Action, _action);
    DESERIALIZE(ActionParameter);
    DESERIALIZE(ActionValue);
    DESERIALIZE(MixerPath);
    DESERIALIZE_MEMBER(Cooldown, _cooldown);
    DESERIALIZE_MEMBER(Volume, _volume);
    DESERIALIZE_MEMBER(Pitch, _pitch);
    DESERIALIZE_MEMBER(StopMode, _stopMode);

    _cooldown = Math::Max(0.0f, _cooldown);
    _volume = Math::Saturate(_volume);
    _pitch = Math::Clamp(_pitch, 0.5f, 2.0f);
}
