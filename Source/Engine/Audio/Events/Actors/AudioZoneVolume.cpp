// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioZoneVolume.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Serialization/JsonTools.h"
#include "Engine/Serialization/Serialization.h"

AudioZoneVolume::AudioZoneVolume(const SpawnParams& params)
    : AudioVolumeBase(params)
{
}

void AudioZoneVolume::UpdateListenerPosition(const Vector3& listenerPosition)
{
    if (!IsDuringPlay())
        return;

    AudioVolumeSample sample = Evaluate(listenerPosition);
    _mixerWeight = sample.Weight;
    AudioParameterId weightParameter = SnapshotWeightParameter.IsValid() ? SnapshotWeightParameter : _resolvedWeightParameter;

    if (sample.Weight > 0.001f)
    {
        if (_snapshotHandle.IsValid())
        {
            AudioEventInstanceState state;
            if (!AudioEventSystem::QueryInstance(_snapshotHandle, state))
                _snapshotHandle = AudioEventHandle();
        }

        if (!_snapshotHandle.IsValid())
        {
            Guid snapId = Guid::Empty;
            String path;
            bool hasTypedSnapshot = false;
            if (Snapshot)
            {
                Snapshot->WaitForLoaded();
                const auto* data = Snapshot->GetInstance<AudioSnapshot>();
                if (data)
                {
                    hasTypedSnapshot = true;
                    snapId = data->BackendId;
                    path = data->Path;
                    if (!SnapshotWeightParameter.IsValid())
                    {
                        weightParameter = data->WeightParameter;
                        _resolvedWeightParameter = weightParameter;
                    }
                }
                else if (Snapshot->Data && Snapshot->DataTypeName == TEXT("FlaxEngine.AudioSnapshot"))
                {
                    hasTypedSnapshot = true;
                    auto& node = *Snapshot->Data;
                    auto itBackend = node.FindMember("BackendId");
                    if (itBackend != node.MemberEnd() && itBackend->value.IsString())
                        JsonTools::GetGuid(snapId, node, "BackendId");
                    auto itPath = node.FindMember("Path");
                    if (itPath != node.MemberEnd() && itPath->value.IsString())
                        path = itPath->value.GetString();
                }
            }

            if (!hasTypedSnapshot)
                path = SnapshotPath;

            if (snapId.IsValid() || path.HasChars())
            {
                AudioEventCreateOptions options;
                options.AutoPlay = true;
                _snapshotHandle = AudioEventSystem::CreateInstance(snapId, path, options);
            }
        }

        if (_snapshotHandle.IsValid())
        {
            // AudioZoneMixer applies the final value after all zones have been sampled.
        }
    }
    else if (_snapshotHandle.IsValid())
    {
        AudioEventSystem::Stop(_snapshotHandle, AudioStopMode::AllowFadeOut);
        AudioEventSystem::ReleaseInstance(_snapshotHandle);
        _snapshotHandle = AudioEventHandle();
        _resolvedWeightParameter = AudioParameterId();
    }
}

void AudioZoneVolume::ApplyMixerWeight(float weight)
{
    if (!_snapshotHandle.IsValid())
        return;
    const AudioParameterId parameter = SnapshotWeightParameter.IsValid() ? SnapshotWeightParameter : _resolvedWeightParameter;
    if (parameter.IsValid())
        AudioEventSystem::SetParameter(_snapshotHandle, parameter, Math::Saturate(weight));
    else
        AudioEventSystem::SetSnapshotWeight(_snapshotHandle, Math::Saturate(weight));
}

String AudioZoneVolume::GetMixerTargetKey() const
{
    if (Snapshot)
    {
        const auto* data = Snapshot->GetInstance<AudioSnapshot>();
        if (data)
        {
            if (data->BackendId.IsValid())
                return data->BackendId.ToString();
            return data->Path;
        }
    }
    return SnapshotPath;
}

void AudioZoneVolume::OnEnable()
{
    AudioVolumeBase::OnEnable();
}

void AudioZoneVolume::OnDisable()
{
    if (_snapshotHandle.IsValid())
    {
        AudioEventSystem::Stop(_snapshotHandle, AudioStopMode::Immediate);
        AudioEventSystem::ReleaseInstance(_snapshotHandle);
        _snapshotHandle = AudioEventHandle();
        _resolvedWeightParameter = AudioParameterId();
        _mixerWeight = 0.0f;
    }
    AudioVolumeBase::OnDisable();
}

void AudioZoneVolume::Serialize(SerializeStream& stream, const void* otherObj)
{
    AudioVolumeBase::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioZoneVolume);

    SERIALIZE(Snapshot);
    SERIALIZE(SnapshotPath);
    SERIALIZE(SnapshotWeightParameter);
}

void AudioZoneVolume::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    AudioVolumeBase::Deserialize(stream, modifier);

    DESERIALIZE(Snapshot);
    DESERIALIZE(SnapshotPath);
    DESERIALIZE(SnapshotWeightParameter);
}
