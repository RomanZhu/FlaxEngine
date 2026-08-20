// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioVolumeBase.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/Assets/AudioSnapshot.h"

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
    float _mixerWeight = 0.0f;

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

public:
    /// <summary>
    /// Updates snapshot intensity based on listener sample.
    /// </summary>
    void UpdateListenerPosition(const Vector3& listenerPosition) override;

    /// <summary>Returns the current listener contribution used by AudioZoneMixer.</summary>
    float GetMixerWeight() const { return _mixerWeight; }

    /// <summary>Applies one deterministic final weight for this zone target.</summary>
    void ApplyMixerWeight(float weight);

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
