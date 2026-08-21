// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/ISerializable.h"
#include "Engine/Level/Actor.h"
#include "Engine/Engine/Time.h"

/// <summary>Reusable activation events shared by audio-event scene behaviors.</summary>
API_ENUM() enum class AudioActivationEvent : uint8
{
    None,
    BeginPlay,
    EndPlay,
    ActorEnable,
    ActorDisable,
    TriggerEnter,
    TriggerExit,
    CollisionEnter,
    CollisionExit,
    PointerEnter,
    PointerExit,
    PointerDown,
    PointerUp,
    ManualSignal,
};

/// <summary>Serialized filtering and deterministic repeat policy for an audio action.</summary>
API_STRUCT() struct FLAXENGINE_API AudioActivationBinding : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioActivationBinding);

    API_FIELD(Attributes="EditorOrder(0)") AudioActivationEvent Event = AudioActivationEvent::None;
    API_FIELD(Attributes="EditorOrder(10)") Actor* SourceActor = nullptr;
    API_FIELD(Attributes="EditorOrder(20)") Actor* TargetActor = nullptr;
    API_FIELD(Attributes="EditorOrder(30)") uint32 LayerMask = MAX_uint32;
    API_FIELD(Attributes="EditorOrder(40)") Tag RequiredTag;
    API_FIELD(Attributes="EditorOrder(50)") Actor* ListenerOrPlayer = nullptr;
    API_FIELD(Attributes="EditorOrder(60)") bool TriggerOnce = false;
    API_FIELD(Attributes="EditorOrder(70)") float Cooldown = 0.0f;
    API_FIELD(Attributes="EditorOrder(80)") bool RearmOnExit = true;
    API_FIELD(Attributes="EditorOrder(90)") bool AllowOverlappingActors = true;

    bool Matches(AudioActivationEvent event, Actor* source, Actor* target = nullptr) const
    {
        if (Event == AudioActivationEvent::None || Event != event)
            return false;
        if (SourceActor && SourceActor != source)
            return false;
        if (TargetActor && TargetActor != target)
            return false;
        if (source)
        {
            if ((LayerMask & (1u << Math::Clamp(source->GetLayer(), 0, 31))) == 0)
                return false;
            if (RequiredTag && !source->HasTag(RequiredTag))
                return false;
            if (ListenerOrPlayer && ListenerOrPlayer != source && ListenerOrPlayer != target)
                return false;
        }
        return true;
    }
};

/// <summary>Non-serialized deterministic state for one activation binding.</summary>
class AudioActivationState
{
    bool _triggered = false;
    int32 _overlapCount = 0;
    double _lastActivationTime = -MAX_double;

public:
    bool TryActivate(const AudioActivationBinding& binding, AudioActivationEvent event, Actor* source, Actor* target = nullptr)
    {
        if (!binding.Matches(event, source, target))
            return false;
        const double now = Time::Update.UnscaledTime.GetTotalSeconds();
        if (binding.TriggerOnce && _triggered)
            return false;
        if (binding.Cooldown > 0.0f && now - _lastActivationTime < binding.Cooldown)
            return false;
        if (!binding.AllowOverlappingActors && _overlapCount > 0 && (event == AudioActivationEvent::TriggerEnter || event == AudioActivationEvent::CollisionEnter))
            return false;
        if (event == AudioActivationEvent::TriggerEnter || event == AudioActivationEvent::CollisionEnter || event == AudioActivationEvent::PointerEnter)
            _overlapCount++;
        else if (event == AudioActivationEvent::TriggerExit || event == AudioActivationEvent::CollisionExit || event == AudioActivationEvent::PointerExit)
            _overlapCount = Math::Max(0, _overlapCount - 1);
        _triggered = true;
        _lastActivationTime = now;
        return true;
    }

    void NotifyExit(const AudioActivationBinding& binding)
    {
        _overlapCount = Math::Max(0, _overlapCount - 1);
        if (binding.RearmOnExit && _overlapCount == 0)
            _triggered = false;
    }

    void Reset()
    {
        _triggered = false;
        _overlapCount = 0;
        _lastActivationTime = -MAX_double;
    }
};
