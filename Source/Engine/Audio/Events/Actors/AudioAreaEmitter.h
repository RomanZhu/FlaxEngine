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
