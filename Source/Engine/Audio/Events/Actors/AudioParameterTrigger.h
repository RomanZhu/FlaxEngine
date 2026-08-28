// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Audio/Events/AudioActivation.h"
#include "Engine/Audio/Events/AudioEventTypes.h"

class AudioEmitter;

/// <summary>One metadata-aware local or global audio parameter action.</summary>
API_STRUCT() struct FLAXENGINE_API AudioParameterAction : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioParameterAction);

    API_FIELD(Attributes="EditorOrder(0)") AudioParameterId Parameter;
    API_FIELD(Attributes="EditorOrder(10)") float Value = 0.0f;
    API_FIELD(Attributes="EditorOrder(20)") String Label;
    API_FIELD(Attributes="EditorOrder(30)") bool UseLabel = false;
    API_FIELD(Attributes="EditorOrder(40)") bool IgnoreSeekSpeed = false;
};

/// <summary>Searchable scene Actor that applies one or more parameters to stable emitter references.</summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio Events/Audio Parameter Trigger\"), ActorToolbox(\"Audio Events\")")
class FLAXENGINE_API AudioParameterTrigger : public Actor
{
    DECLARE_SCENE_OBJECT(AudioParameterTrigger);

    AudioActivationState _activationState;

public:
    /// <summary>Target emitters for local parameters. Empty is invalid unless Global is enabled.</summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Parameter Trigger\")")
    Array<AudioEmitter*> Targets;

    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Parameter Trigger\")")
    Array<AudioParameterAction> Parameters;

    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Parameter Trigger\", \"Global Parameter\")")
    bool Global = false;

    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Activation\", \"Trigger Event\")")
    AudioActivationBinding Activation;

    /// <summary>Applies all configured parameters and reports false when any target or parameter is invalid.</summary>
    API_FUNCTION() bool Apply();

    API_FUNCTION() bool SignalActivation(AudioActivationEvent activationEvent, Actor* source = nullptr, Actor* target = nullptr);

    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    void OnEnable() override;
    void OnDisable() override;
    void BeginPlay(SceneBeginData* data) override;
    void EndPlay() override;
};
