// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Scripting/Script.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "AudioInteractionTypes.h"
#include "Engine/Level/Tags.h"
#include "Engine/Physics/Collisions.h"

class PhysicsColliderActor;

API_ENUM() enum class AudioPhysicsContactType : uint8
{
    Collision,
    Trigger,
    CollisionOrTrigger,
};

API_ENUM() enum class AudioPhysicsIntensitySource : uint8
{
    RelativeSpeed,
    NormalSpeed,
    Impulse,
    KineticEnergyApproximation,
    TangentialSpeed,
};

/// <summary>Reusable include/exclude and numeric contact conditions.</summary>
API_STRUCT() struct FLAXENGINE_API AudioPhysicsCondition : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioPhysicsCondition);

    API_FIELD() Actor* IncludeActor = nullptr;
    API_FIELD() Actor* ExcludeActor = nullptr;
    API_FIELD() Tag IncludeTag;
    API_FIELD() Tag ExcludeTag;
    API_FIELD() uint32 IncludeLayers = MAX_uint32;
    API_FIELD() uint32 ExcludeLayers = 0;
    API_FIELD() float MinimumRelativeSpeed = 0.0f;
    API_FIELD() float MaximumRelativeSpeed = MAX_float;
    API_FIELD() float MinimumImpulse = 0.0f;
    API_FIELD() float MaximumImpulse = MAX_float;
};

/// <summary>One ordered impact, friction, or exit mapping.</summary>
API_STRUCT() struct FLAXENGINE_API AudioPhysicsRule : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioPhysicsRule);

    // Stable asset IDs avoid retaining managed/live JsonAsset objects inside the
    // value-type rule array. Keeping object references here made scene reloads
    // capable of resolving a partially marshalled GUID and crashing ChangeID.
    API_FIELD(Attributes="EditorDisplay(\"Event\", \"Event Assets\")") Array<Guid> Events;
    API_FIELD() AudioPhysicsContactType PlayOn = AudioPhysicsContactType::CollisionOrTrigger;
    API_FIELD() AudioPhysicsCondition Condition;
    API_FIELD() AudioPhysicsIntensitySource IntensitySource = AudioPhysicsIntensitySource::RelativeSpeed;
    API_FIELD() float MinimumIntensity = 0.0f;
    API_FIELD() float MaximumIntensity = 1000.0f;
    API_FIELD() float Cooldown = 0.05f;
    API_FIELD() AudioParameterId IntensityParameter = AudioParameterId(TEXT("Intensity"));
    API_FIELD() uint32 ListenerMask = 1;
    API_FIELD() int32 Priority = 0;
    API_FIELD() int32 MaximumVoices = 4;
    API_FIELD() float Attack = 12.0f;
    API_FIELD() float Release = 6.0f;
    API_FIELD() float StartThreshold = 1.0f;
    API_FIELD() float StopThreshold = 0.5f;
    API_FIELD() AudioStopMode StopMode = AudioStopMode::AllowFadeOut;
};

/// <summary>Attachable, rule-driven impact/friction/exit audio for ordinary physics Actors.</summary>
API_CLASS(Attributes="Category(\"Audio Events\")") class FLAXENGINE_API AudioPhysics : public Script
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE(AudioPhysics);

    struct ContactVoice
    {
        uint64 PairKey = 0;
        PhysicsColliderActor* Other = nullptr;
        AudioImpactContext Context;
        AudioEventHandle FrictionHandle;
        int32 FrictionRule = -1;
        AudioParameterId ResolvedIntensityParameter;
        bool HasIntensityParameter = false;
        float SmoothedIntensity = 0.0f;
        float LastImpactTime = -MAX_float;
    };

    Array<PhysicsColliderActor*> _colliders;
    Array<ContactVoice> _contacts;

public:
    API_FIELD(Attributes="EditorDisplay(\"Impact\")") bool EnableImpact = true;
    API_FIELD(Attributes="EditorDisplay(\"Impact\")") Array<AudioPhysicsRule> ImpactRules;
    API_FIELD(Attributes="EditorDisplay(\"Friction\")") bool EnableFriction = true;
    API_FIELD(Attributes="EditorDisplay(\"Friction\")") Array<AudioPhysicsRule> FrictionRules;
    API_FIELD(Attributes="EditorDisplay(\"Exit\")") bool EnableExit = true;
    API_FIELD(Attributes="EditorDisplay(\"Exit\")") Array<AudioPhysicsRule> ExitRules;
    API_FIELD(Attributes="EditorDisplay(\"Voice Policy\"), Limit(1, 64)") int32 MaximumFrictionVoices = 8;

    API_FIELD(Attributes="ReadOnly, NoSerialize, EditorDisplay(\"Runtime\")") String LastExplanation;
    API_FIELD(Attributes="ReadOnly, NoSerialize, EditorDisplay(\"Runtime\")") String LastSelectedEvent;
    API_FIELD(Attributes="ReadOnly, NoSerialize, EditorDisplay(\"Runtime\")") float LastIntensity = 0.0f;
    API_FIELD(Attributes="ReadOnly, NoSerialize, EditorDisplay(\"Runtime\")") int32 ActiveFrictionVoices = 0;

    API_FUNCTION() bool Validate(API_PARAM(Out) String& result) const;
    API_FUNCTION() bool SimulateImpact(float relativeSpeed, float normalSpeed, float impulse, bool trigger = false);

    void OnEnable() override;
    void OnDisable() override;
    void OnDestroy() override;
    void OnUpdate() override;

private:
    void BindColliders();
    void UnbindColliders();
    void OnCollisionEnter(const Collision& collision);
    void OnCollisionExit(const Collision& collision);
    void OnTriggerEnter(PhysicsColliderActor* other);
    void OnTriggerExit(PhysicsColliderActor* other);
    void ProcessEnter(const AudioImpactContext& context, PhysicsColliderActor* other, bool trigger);
    void ProcessExit(PhysicsColliderActor* other, bool trigger);
    bool PlayRule(const AudioPhysicsRule& rule, const AudioImpactContext& context, uint64 pairKey, bool persistent, AudioEventHandle& handle, AudioParameterId* resolvedIntensityParameter = nullptr);
    int32 FindRule(const Array<AudioPhysicsRule>& rules, const AudioImpactContext& context, PhysicsColliderActor* other, bool trigger, String& rejection) const;
    static float ComputeIntensity(const AudioPhysicsRule& rule, const AudioImpactContext& context);
    static uint64 MakePairKey(const ScriptingObject* a, const ScriptingObject* b);
    ContactVoice* FindContact(uint64 pairKey);
    void StopContact(ContactVoice& contact);
};
