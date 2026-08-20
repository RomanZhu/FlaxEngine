// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Level/Actor.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "Engine/Audio/Events/AudioEventCallbacks.h"
#include "Engine/Audio/Events/Occlusion/AudioOcclusionTypes.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"

/// <summary>
/// Scene actor that plays and spatializes audio events from an audio event middleware (e.g. FMOD Studio).
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Emitter\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioEmitter : public Actor
{
    DECLARE_SCENE_OBJECT(AudioEmitter);
    friend class AudioWorld;

private:
    Vector3 _velocity = Vector3::Zero;
    Vector3 _prevPos = Vector3::Zero;
    float _volume = 1.0f;
    float _pitch = 1.0f;
    float _minDistance = 1000.0f;
    float _attenuation = 1.0f;
    float _dopplerFactor = 1.0f;
    bool _playOnStart = false;
    bool _allowSpatialization = true;
    AudioStopMode _stopMode = AudioStopMode::AllowFadeOut;
    bool _stopOnDisable = true;
    uint32 _listenerMask = 1;
    AudioEventHandle _handle;

public:
    /// <summary>
    /// The audio event asset to play.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Audio Emitter\")")
    JsonAssetReference<AudioEvent> Event;

    /// <summary>
    /// Explicit event path override if not using an asset reference.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Audio Emitter\"), HideInEditor")
    String EventPath;

    /// <summary>
    /// Gets the volume of the played audio event (in range 0-1).
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(20), DefaultValue(1.0f), Limit(0, 1, 0.01f), EditorDisplay(\"Audio Emitter\")")
    FORCE_INLINE float GetVolume() const { return _volume; }

    /// <summary>
    /// Sets the volume of the played audio event (in range 0-1).
    /// </summary>
    API_PROPERTY() void SetVolume(float value);

    /// <summary>
    /// Gets the pitch multiplier of the played audio event.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(30), DefaultValue(1.0f), Limit(0.5f, 2.0f, 0.01f), EditorDisplay(\"Audio Emitter\")")
    FORCE_INLINE float GetPitch() const { return _pitch; }

    /// <summary>
    /// Sets the pitch multiplier of the played audio event.
    /// </summary>
    API_PROPERTY() void SetPitch(float value);

    /// <summary>
    /// Determines whether the audio event should autoplay on scene start.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(40), DefaultValue(false), EditorDisplay(\"Audio Emitter\", \"Play On Start\")")
    FORCE_INLINE bool GetPlayOnStart() const { return _playOnStart; }

    /// <summary>
    /// Determines whether the audio event should autoplay on scene start.
    /// </summary>
    API_PROPERTY() void SetPlayOnStart(bool value);

    /// <summary>
    /// Gets the stop mode applied when stopping playback.
    /// </summary>
    API_PROPERTY(Attributes="EditorOrder(50), DefaultValue(AudioStopMode.AllowFadeOut), EditorDisplay(\"Audio Emitter\", \"Stop Mode\")")
    FORCE_INLINE AudioStopMode GetStopMode() const { return _stopMode; }

    /// <summary>
    /// Sets the stop mode applied when stopping playback.
    /// </summary>
    API_PROPERTY() void SetStopMode(AudioStopMode value);

    /// <summary>
    /// Gets the velocity of the emitter for Doppler shift calculation.
    /// </summary>
    API_PROPERTY() FORCE_INLINE const Vector3& GetVelocity() const { return _velocity; }

    /// <summary>
    /// Gets the active event handle.
    /// </summary>
    API_PROPERTY() FORCE_INLINE AudioEventHandle GetHandle() const { return _handle; }

public:
    /// <summary>
    /// Starts playing the audio event.
    /// </summary>
    API_FUNCTION() void Play();

    /// <summary>
    /// Pauses audio event playback.
    /// </summary>
    API_FUNCTION() void Pause();

    /// <summary>
    /// Stops audio event playback.
    /// </summary>
    API_FUNCTION() void Stop();

    /// <summary>
    /// Sets an instance parameter value by name.
    /// </summary>
    API_FUNCTION() bool SetParameter(const StringView& name, float value, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Sets an instance parameter label by name.
    /// </summary>
    API_FUNCTION() bool SetParameterLabel(const StringView& name, const StringView& label, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Gets the current playback state of the active event instance.
    /// </summary>
    API_PROPERTY() AudioEventPlaybackState GetPlaybackState() const;

    /// <summary>
    /// Returns true if the emitter is currently playing an active sound instance.
    /// </summary>
    API_PROPERTY() bool IsActuallyPlaying() const;

    /// <summary>Raised when this emitter starts a playback instance.</summary>
    API_EVENT() Action Started;

    /// <summary>Raised when this emitter stops or releases its playback instance.</summary>
    API_EVENT() Action Stopped;

    /// <summary>Raised for a backend timeline marker owned by this emitter.</summary>
    API_EVENT() Delegate<String, int32> TimelineMarker;

    /// <summary>Raised for a backend musical timeline beat owned by this emitter.</summary>
    API_EVENT() Delegate<AudioTimelineBeat> TimelineBeat;

    /// <summary>Listener mask applied to newly created instances.</summary>
    API_PROPERTY(Attributes="EditorOrder(60), EditorDisplay(\"Audio Emitter\", \"Listener Mask\")")
    FORCE_INLINE uint32 GetListenerMask() const { return _listenerMask; }

    /// <summary>Sets the listener mask applied to this emitter.</summary>
    API_PROPERTY() void SetListenerMask(uint32 value);

    /// <summary>Whether the instance is stopped when the actor is disabled.</summary>
    API_PROPERTY(Attributes="EditorOrder(70), DefaultValue(true), EditorDisplay(\"Audio Emitter\", \"Stop On Disable\")")
    FORCE_INLINE bool GetStopOnDisable() const { return _stopOnDisable; }

    /// <summary>Sets whether the instance is stopped when the actor is disabled.</summary>
    API_PROPERTY() void SetStopOnDisable(bool value) { _stopOnDisable = value; }

    /// <summary>Per-emitter scalable acoustic obstruction settings.</summary>
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"Audio Emitter\", \"Occlusion\")")
    AudioOcclusionSettings Occlusion;

    /// <summary>Priority used when the global occlusion query budget is oversubscribed.</summary>
    API_FIELD(Attributes="EditorOrder(81), EditorDisplay(\"Audio Emitter\", \"Occlusion Priority\")")
    int32 OcclusionPriority = 0;

    /// <summary>Returns the sanitized occlusion settings used by the world scheduler.</summary>
    const AudioOcclusionSettings& GetOcclusionSettings() const { return Occlusion; }
    int32 GetOcclusionPriority() const { return OcclusionPriority; }

private:
    void UpdateVelocity(float dt);
    void Push3DAttributes();
    void HandleEventCallback(const AudioEventCallback& callback);

public:
    // [Actor]
#if USE_EDITOR
    BoundingBox GetEditorBox() const override
    {
        const Vector3 size(50.0f);
        return BoundingBox(_transform.Translation - size, _transform.Translation + size);
    }
    void OnDebugDrawSelected() override;
#endif
    bool IntersectsItself(const Ray& ray, Real& distance, Vector3& normal) override;
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
    void OnTransformChanged() override;
    void BeginPlay(SceneBeginData* data) override;
};
