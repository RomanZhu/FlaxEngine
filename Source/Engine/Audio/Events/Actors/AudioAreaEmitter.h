// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioVolumeBase.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"

class AudioListener;

/// <summary>
/// Spatial area audio emitter that positions its virtual source at the closest point to the listener and modulates volume based on zone distance.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Area Emitter\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioAreaEmitter : public AudioVolumeBase
{
    DECLARE_SCENE_OBJECT(AudioAreaEmitter);

private:
    struct ListenerVoice
    {
        AudioListener* Listener = nullptr;
        AudioEventHandle Handle;
        Vector3 PreviousSourcePosition = Vector3::Zero;
        float BelowStopDuration = 0.0f;
        float Weight = 0.0f;
        bool Seen = false;
    };

    float _volume = 1.0f;
    float _pitch = 1.0f;
    bool _playOnStart = true;
    bool _manualPlayRequested = false;
    Vector3 _previousSourcePosition = Vector3::Zero;
    float _belowStopDuration = 0.0f;
    AudioEventHandle _handle;
    Array<ListenerVoice> _listenerVoices;
    float _currentWeight = 0.0f;
    Vector3 _currentClosestPoint = Vector3::Zero;

public:
    /// <summary>
    /// The audio event to play.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Area Emitter\")")
    JsonAssetReference<AudioEvent> Event;

    /// <summary>
    /// Explicit event path override.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Area Emitter\"), HideInEditor")
    String EventPath;

    /// <summary>
    /// Base volume multiplier.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(20), DefaultValue(1.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Area Emitter\")")
    FORCE_INLINE float GetVolume() const { return _volume; }

    /// <summary>
    /// Sets base volume multiplier.
    /// </summary>
    API_PROPERTY() void SetVolume(float value);

    /// <summary>
    /// Base pitch multiplier.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(30), DefaultValue(1.0f), Limit(0.5f, 2.0f, 0.01f), EditorDisplay(\"Area Emitter\")")
    FORCE_INLINE float GetPitch() const { return _pitch; }

    /// <summary>
    /// Sets base pitch multiplier.
    /// </summary>
    API_PROPERTY() void SetPitch(float value);

    /// <summary>
    /// Weight required before automatic ambient playback starts.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Area Emitter\", \"Start Threshold\"), Limit(0, 1, 0.001f)")
    float StartThreshold = 0.01f;

    /// <summary>
    /// Weight below which a playing ambient event becomes eligible to stop.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"Area Emitter\", \"Stop Threshold\"), Limit(0, 1, 0.001f)")
    float StopThreshold = 0.001f;

    /// <summary>
    /// Time that the weight must remain below Stop Threshold before playback is released.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"Area Emitter\", \"Stop Delay\"), Limit(0, 30, 0.01f)")
    float StopDelay = 1.0f;

    /// <summary>Uses a zero-velocity virtual source while the listener is inside the volume.</summary>
    API_FIELD(Attributes="EditorOrder(70), DefaultValue(true), EditorDisplay(\"Area Emitter\", \"Follow Listener Inside\")")
    bool FollowListenerInside = true;

public:
    /// <summary>
    /// Starts area ambient playback.
    /// </summary>
    API_FUNCTION() void Play();

    /// <summary>
    /// Stops area ambient playback.
    /// </summary>
    API_FUNCTION() void Stop();

    /// <summary>Gets the owned event instance for runtime diagnostics.</summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    FORCE_INLINE AudioEventHandle GetHandle() const { return _handle; }

    /// <summary>Gets the final, group-resolved listener contribution.</summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    FORCE_INLINE float GetCurrentWeight() const { return _currentWeight; }

    /// <summary>Gets the closest virtual source point from the latest sample.</summary>
    API_PROPERTY(Attributes="HideInEditor, NoSerialize")
    FORCE_INLINE Vector3 GetCurrentClosestPoint() const { return _currentClosestPoint; }

    API_FUNCTION() bool SetParameter(const StringView& name, float value, bool ignoreSeekSpeed = false);
    API_FUNCTION() bool SetParameterLabel(const StringView& name, const StringView& label, bool ignoreSeekSpeed = false);
    API_PROPERTY(Attributes="HideInEditor, NoSerialize") AudioEventPlaybackState GetPlaybackState() const;

    /// <summary>
    /// Updates spatial positioning relative to listener position.
    /// </summary>
    void UpdateListenerPosition(const Vector3& listenerPosition) override;
    void ApplyResolvedSample(const Vector3& listenerPosition, const AudioVolumeSample& rawSample, float resolvedWeight) override;

    /// <summary>Begins one AudioWorld multi-listener arbitration pass.</summary>
    void BeginListenerUpdate();
    /// <summary>Applies the group-resolved contribution for one compact listener.</summary>
    void ApplyResolvedListenerSample(AudioListener* listener, int32 listenerIndex, const AudioVolumeSample& rawSample, float resolvedWeight);
    /// <summary>Releases per-listener voices whose listeners disappeared or were masked out.</summary>
    void EndListenerUpdate();

    // [Actor]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

private:
    AudioEventHandle CreateVoice(const Vector3& position, uint32 listenerMask);
    void UpdateVoice(AudioListener* listener, uint32 listenerMask, const Vector3& listenerPosition, const AudioVolumeSample& sample);
    void UpdateAllListeners();
    void StopVoice(ListenerVoice& voice, AudioStopMode stopMode = AudioStopMode::AllowFadeOut);

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void BeginPlay(SceneBeginData* data) override;
};
