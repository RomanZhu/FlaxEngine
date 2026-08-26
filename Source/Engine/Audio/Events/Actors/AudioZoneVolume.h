// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioVolumeBase.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/Assets/AudioSnapshot.h"
#include "Engine/Audio/Events/AudioZoneMixer.h"

/// <summary>
/// Audio zone volume that applies mixer snapshots and parameter adjustments when the listener enters the region.
/// </summary>
API_CLASS(Attributes="ActorContextMenu(\"New/Audio/Audio Zone Volume\"), ActorToolbox(\"Other\")")
class FLAXENGINE_API AudioZoneVolume : public AudioVolumeBase
{
    DECLARE_SCENE_OBJECT(AudioZoneVolume);

private:
    AudioEventHandle _snapshotHandle;
    AudioParameterId _resolvedWeightParameter;
    bool _weightParameterResolved = false;
    float _mixerWeight = 0.0f;
    float _currentRawWeight = 0.0f;
    float _currentResolvedWeight = 0.0f;

public:
    /// <summary>
    /// The mixer snapshot to engage while inside or near this zone.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"Zone\")")
    JsonAssetReference<AudioSnapshot> Snapshot;

    /// <summary>
    /// Explicit snapshot path override.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(10), EditorDisplay(\"Zone\"), HideInEditor")
    String SnapshotPath;

    /// <summary>
    /// Optional per-zone override for the snapshot weight parameter.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Zone\", \"Snapshot Weight Parameter\")")
    AudioParameterId SnapshotWeightParameter;

    /// <summary>Target type for non-snapshot zone mixer contributions.</summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Zone\", \"Target Type\")")
    AudioZoneTargetType TargetType = AudioZoneTargetType::Snapshot;

    /// <summary>Backend GUID for bus or VCA targets.</summary>
    API_FIELD(Attributes="EditorOrder(31), EditorDisplay(\"Zone\", \"Target ID\")")
    Guid TargetId = Guid::Empty;

    /// <summary>Backend path for bus or VCA targets.</summary>
    API_FIELD(Attributes="EditorOrder(32), EditorDisplay(\"Zone\", \"Target Path\")")
    String TargetPath;

    /// <summary>Global parameter identifier for parameter targets.</summary>
    API_FIELD(Attributes="EditorOrder(33), EditorDisplay(\"Zone\", \"Target Parameter\")")
    AudioParameterId TargetParameter;

public:
    /// <summary>
    /// Updates snapshot intensity based on listener sample.
    /// </summary>
    void UpdateListenerPosition(const Vector3& listenerPosition) override;
    void ApplyResolvedSample(const Vector3& listenerPosition, const AudioVolumeSample& rawSample, float resolvedWeight) override;

    API_PROPERTY(Attributes="HideInEditor, NoSerialize") FORCE_INLINE AudioEventHandle GetHandle() const { return _snapshotHandle; }
    API_PROPERTY(Attributes="HideInEditor, NoSerialize") FORCE_INLINE float GetRawWeight() const { return _currentRawWeight; }
    API_PROPERTY(Attributes="HideInEditor, NoSerialize") FORCE_INLINE float GetResolvedWeight() const { return _currentResolvedWeight; }
    API_PROPERTY(Attributes="HideInEditor, NoSerialize") FORCE_INLINE bool GetIsSnapshotActive() const { return _snapshotHandle.IsValid(); }

    /// <summary>Returns the current listener contribution used by AudioZoneMixer.</summary>
    float GetMixerWeight() const { return _mixerWeight; }

    /// <summary>Applies one deterministic final weight for this zone target.</summary>
    void ApplyMixerWeight(float weight);
    bool EnsureMixerInstance(float initialWeight);
    /// <summary>Releases this zone's snapshot when another zone becomes canonical.</summary>
    void ReleaseMixerInstance(AudioStopMode stopMode = AudioStopMode::AllowFadeOut);
    AudioZoneTargetType GetTargetType() const { return TargetType; }
    const Guid& GetTargetId() const { return TargetId; }
    const String& GetTargetPath() const { return TargetPath; }
    const AudioParameterId& GetTargetParameter() const { return TargetParameter; }

    /// <summary>Returns the stable authored target key used for aggregation.</summary>
    String GetMixerTargetKey() const;

    // [Actor]
    void Serialize(SerializeStream& stream, const void* otherObj) override;
    void Deserialize(DeserializeStream& stream, ISerializeModifier* modifier) override;

protected:
    // [Actor]
    void OnEnable() override;
    void OnDisable() override;
};
