// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioVolumeBase.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"

/// <summary>
/// Listener boundary transition that activates an audio trigger.
/// </summary>
API_ENUM() enum class AudioTriggerActivationMode : uint8
{
    /// <summary>
    /// Activate when the listener enters the volume.
    /// </summary>
    ListenerEnter = 0,

    /// <summary>
    /// Activate when the listener exits the volume.
    /// </summary>
    ListenerExit = 1,

    /// <summary>
    /// Activate on both listener entry and exit.
    /// </summary>
    ListenerEnterAndExit = 2,
};

API_ENUM() enum class AudioTriggerTargetMode : uint8
{
    Listener = 0,
    Actor = 1,
    LayerMask = 2,
    Tag = 3,
};

API_ENUM() enum class AudioTriggerActionType : uint8
{
    PlayOneShot = 0,
    StartPersistentEvent = 1,
    StopEvent = 2,
    StartSnapshot = 3,
    StopSnapshot = 4,
    SetGlobalParameter = 5,
    SetBusVolume = 6,
    SetVCAVolume = 7,
    MuteBus = 8,
    PauseBus = 9,
};

/// <summary>
/// Listener volume that starts an audio event when the listener crosses its boundary.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Trigger\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioTrigger : public AudioVolumeBase
{
    DECLARE_SCENE_OBJECT(AudioTrigger);

private:
    AudioTriggerActivationMode _activationMode = AudioTriggerActivationMode::ListenerEnter;
    bool _stopOnExit = false;
    bool _triggerOnce = false;
    bool _rearmOnExit = true;
    AudioTriggerTargetMode _targetMode = AudioTriggerTargetMode::Listener;
    AudioTriggerActionType _action = AudioTriggerActionType::PlayOneShot;
    float _cooldown = 0.0f;
    float _volume = 1.0f;
    float _pitch = 1.0f;
    AudioStopMode _stopMode = AudioStopMode::AllowFadeOut;

    AudioEventHandle _handle;
    bool _hasSample = false;
    bool _isInside = false;
    bool _hasTriggered = false;
    float _cooldownRemaining = 0.0f;

public:
    /// <summary>
    /// The audio event to start when the listener crosses the trigger boundary.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Audio Trigger\")")
    JsonAssetReference<AudioEvent> Event;

    /// <summary>
    /// Explicit event path override used when no event asset is assigned.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Audio Trigger\"), HideInEditor")
    String EventPath;

    /// <summary>
    /// Boundary transition that starts the event.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(20), EditorDisplay(\"Audio Trigger\")")
    FORCE_INLINE AudioTriggerActivationMode GetActivationMode() const { return _activationMode; }

    /// <summary>
    /// Sets the boundary transition that starts the event.
    /// </summary>
    API_PROPERTY() void SetActivationMode(AudioTriggerActivationMode value) { _activationMode = value; }

    /// <summary>
    /// Stops and releases a persistent event when the listener exits the volume.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(30), EditorDisplay(\"Audio Trigger\", \"Stop On Exit\")")
    FORCE_INLINE bool GetStopOnExit() const { return _stopOnExit; }

    /// <summary>
    /// Sets whether a persistent event is stopped when the listener exits the volume.
    /// </summary>
    API_PROPERTY() void SetStopOnExit(bool value) { _stopOnExit = value; }

    /// <summary>
    /// If true, the trigger can start its event only once per play session.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(40), EditorDisplay(\"Audio Trigger\", \"Trigger Once\")")
    FORCE_INLINE bool GetTriggerOnce() const { return _triggerOnce; }

    /// <summary>
    /// Sets whether the trigger can start its event only once per play session.
    /// </summary>
    API_PROPERTY() void SetTriggerOnce(bool value) { _triggerOnce = value; }

    API_PROPERTY(Attributes="EditorOrder(45), EditorDisplay(\"Audio Trigger\", \"Target Mode\")")
    FORCE_INLINE AudioTriggerTargetMode GetTargetMode() const { return _targetMode; }
    API_PROPERTY() void SetTargetMode(AudioTriggerTargetMode value) { _targetMode = value; }

    API_PROPERTY(Attributes="EditorOrder(46), DefaultValue(true), EditorDisplay(\"Audio Trigger\", \"Rearm On Exit\")")
    FORCE_INLINE bool GetRearmOnExit() const { return _rearmOnExit; }
    API_PROPERTY() void SetRearmOnExit(bool value) { _rearmOnExit = value; }

    API_PROPERTY(Attributes="EditorOrder(47), EditorDisplay(\"Audio Trigger\", \"Action\")")
    FORCE_INLINE AudioTriggerActionType GetAction() const { return _action; }
    API_PROPERTY() void SetAction(AudioTriggerActionType value) { _action = value; }

    API_FIELD(Attributes="EditorOrder(48), EditorDisplay(\"Audio Trigger\", \"Action Parameter\")")
    AudioParameterId ActionParameter;
    API_FIELD(Attributes="EditorOrder(49), EditorDisplay(\"Audio Trigger\", \"Action Value\")")
    float ActionValue = 1.0f;
    API_FIELD(Attributes="EditorOrder(49), EditorDisplay(\"Audio Trigger\", \"Mixer Path\")")
    String MixerPath;

    /// <summary>
    /// Minimum time in seconds between event starts.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(50), DefaultValue(0.0f), Limit(0, 3600, 0.1f), EditorDisplay(\"Audio Trigger\")")
    FORCE_INLINE float GetCooldown() const { return _cooldown; }

    /// <summary>
    /// Sets the minimum time in seconds between event starts.
    /// </summary>
    API_PROPERTY() void SetCooldown(float value);

    /// <summary>
    /// Volume multiplier applied to newly started events.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(60), DefaultValue(1.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Audio Trigger\")")
    FORCE_INLINE float GetVolume() const { return _volume; }

    /// <summary>
    /// Sets the volume multiplier applied to newly started events.
    /// </summary>
    API_PROPERTY() void SetVolume(float value);

    /// <summary>
    /// Pitch multiplier applied to newly started events.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(70), DefaultValue(1.0f), Limit(0.5f, 2.0f, 0.01f), EditorDisplay(\"Audio Trigger\")")
    FORCE_INLINE float GetPitch() const { return _pitch; }

    /// <summary>
    /// Sets the pitch multiplier applied to newly started events.
    /// </summary>
    API_PROPERTY() void SetPitch(float value);

    /// <summary>
    /// Stop policy used when stopping the active persistent event.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(80), EditorDisplay(\"Audio Trigger\", \"Stop Mode\")")
    FORCE_INLINE AudioStopMode GetStopMode() const { return _stopMode; }

    /// <summary>
    /// Sets the stop policy used when stopping the active persistent event.
    /// </summary>
    API_PROPERTY() void SetStopMode(AudioStopMode value) { _stopMode = value; }

    /// <summary>
    /// Gets the active persistent event handle, if any.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    FORCE_INLINE AudioEventHandle GetHandle() const { return _handle; }

    /// <summary>
    /// Returns true if the most recently sampled listener position was inside the volume.
    /// </summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    FORCE_INLINE bool GetIsInside() const { return _isInside; }

public:
    /// <summary>
    /// Updates listener occupancy and processes boundary transitions.
    /// </summary>
    void UpdateListenerPosition(const Vector3& listenerPosition) override;

    /// <summary>
    /// Stops and releases the active persistent event.
    /// </summary>
    API_FUNCTION() void Stop();

    // [Actor]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void BeginPlay(SceneBeginData* data) override;

private:
    bool ShouldTrigger(bool entered) const;
    bool StartEvent();
    bool ExecuteAction();
};
