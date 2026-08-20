// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioVolumeBase.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"

/// <summary>
/// Spatial area audio emitter that positions its virtual source at the closest point to the listener and modulates volume based on zone distance.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Area Emitter\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioAreaEmitter : public AudioVolumeBase
{
    DECLARE_SCENE_OBJECT(AudioAreaEmitter);

private:
    float _volume = 1.0f;
    float _pitch = 1.0f;
    bool _playOnStart = true;
    Vector3 _previousSourcePosition = Vector3::Zero;
    float _belowStopDuration = 0.0f;
    AudioEventHandle _handle;

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

    /// <summary>
    /// Updates spatial positioning relative to listener position.
    /// </summary>
    void UpdateListenerPosition(const Vector3& listenerPosition) override;

    // [Actor]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void BeginPlay(SceneBeginData* data) override;
};
