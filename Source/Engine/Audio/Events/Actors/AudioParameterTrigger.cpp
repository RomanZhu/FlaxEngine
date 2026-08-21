// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioParameterTrigger.h"
#include "AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Serialization/Serialization.h"
#include "Engine/Level/Scene/Scene.h"

AudioParameterTrigger::AudioParameterTrigger(const SpawnParams& params)
    : Actor(params)
{
}

bool AudioParameterTrigger::Apply()
{
    bool success = Parameters.HasItems() && (Global || Targets.HasItems());
    for (const auto& action : Parameters)
    {
        if (action.Parameter.Name.IsEmpty() && action.Parameter.ID == Guid::Empty && action.Parameter.Data1 == 0 && action.Parameter.Data2 == 0)
        {
            LOG(Error, "AudioParameterTrigger '{0}' contains an empty parameter reference.", GetName());
            success = false;
            continue;
        }
        if (Global)
        {
            const bool applied = action.UseLabel
                ? AudioEventSystem::SetGlobalParameterLabel(action.Parameter, action.Label, action.IgnoreSeekSpeed)
                : AudioEventSystem::SetGlobalParameter(action.Parameter, action.Value, action.IgnoreSeekSpeed);
            success &= applied;
            if (!applied)
                LOG(Error, "AudioParameterTrigger '{0}' failed to set global parameter '{1}'.", GetName(), action.Parameter.Name);
            continue;
        }
        for (auto* emitter : Targets)
        {
            if (!emitter)
            {
                success = false;
                continue;
            }
            const bool applied = action.UseLabel
                ? emitter->SetParameterLabel(action.Parameter.Name, action.Label, action.IgnoreSeekSpeed)
                : emitter->SetParameter(action.Parameter.Name, action.Value, action.IgnoreSeekSpeed);
            success &= applied;
            if (!applied)
                LOG(Error, "AudioParameterTrigger '{0}' could not update emitter '{1}' parameter '{2}' because its instance is invalid.", GetName(), emitter->GetName(), action.Parameter.Name);
        }
    }
    return success;
}

bool AudioParameterTrigger::SignalActivation(AudioActivationEvent activationEvent, Actor* source, Actor* target)
{
    if (_activationState.TryActivate(Activation, activationEvent, source, target))
        return Apply();
    if (activationEvent == AudioActivationEvent::TriggerExit || activationEvent == AudioActivationEvent::CollisionExit || activationEvent == AudioActivationEvent::PointerExit)
        _activationState.NotifyExit(Activation);
    return false;
}

bool AudioParameterTrigger::IntersectsItself(const Ray&, Real&, Vector3&)
{
    return false;
}

void AudioParameterTrigger::OnEnable()
{
#if USE_EDITOR
    GetSceneRendering()->AddViewportIcon(this);
#endif
    Actor::OnEnable();
    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorEnable, this, this);
}

void AudioParameterTrigger::OnDisable()
{
    if (IsDuringPlay())
        SignalActivation(AudioActivationEvent::ActorDisable, this, this);
#if USE_EDITOR
    GetSceneRendering()->RemoveViewportIcon(this);
#endif
    Actor::OnDisable();
}

void AudioParameterTrigger::BeginPlay(SceneBeginData* data)
{
    Actor::BeginPlay(data);
    _activationState.Reset();
    SignalActivation(AudioActivationEvent::BeginPlay, this, this);
}

void AudioParameterTrigger::EndPlay()
{
    SignalActivation(AudioActivationEvent::EndPlay, this, this);
    Actor::EndPlay();
}

void AudioParameterTrigger::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);
    SERIALIZE_GET_OTHER_OBJ(AudioParameterTrigger);
    SERIALIZE(Targets);
    SERIALIZE(Parameters);
    SERIALIZE(Global);
    SERIALIZE(Activation);
}

void AudioParameterTrigger::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);
    DESERIALIZE(Targets);
    DESERIALIZE(Parameters);
    DESERIALIZE(Global);
    DESERIALIZE(Activation);
}
