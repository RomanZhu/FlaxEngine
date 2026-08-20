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
    const AudioVolumeSample sample = Evaluate(listenerPosition);
    ApplyResolvedSample(listenerPosition, sample, sample.Weight);
}

void AudioZoneVolume::ApplyResolvedSample(const Vector3& listenerPosition, const AudioVolumeSample& sample, float resolvedWeight)
{
    if (!IsDuringPlay())
        return;

    _currentRawWeight = sample.Weight;
    _currentResolvedWeight = Math::Saturate(resolvedWeight);
    _mixerWeight = _currentResolvedWeight;
    if (_currentResolvedWeight > 0.001f)
    {
        // AudioZoneMixer owns creation and applies one target instance per snapshot key.
    }
    else if (_snapshotHandle.IsValid())
    {
        AudioEventSystem::StopAndRelease(_snapshotHandle, AudioStopMode::AllowFadeOut);
        _snapshotHandle = AudioEventHandle();
        _resolvedWeightParameter = AudioParameterId();
    }
}

bool AudioZoneVolume::EnsureMixerInstance()
{
    if (TargetType != AudioZoneTargetType::Snapshot)
        return true;
    if (_snapshotHandle.IsValid())
    {
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(_snapshotHandle, state))
            return true;
        _snapshotHandle = AudioEventHandle();
    }
    Guid snapId = Guid::Empty;
    String path;
    if (Snapshot)
    {
        Snapshot->WaitForLoaded();
        const auto* data = Snapshot->GetInstance<AudioSnapshot>();
        if (data)
        {
            snapId = data->BackendId;
            path = data->Path;
            if (!SnapshotWeightParameter.IsValid())
                _resolvedWeightParameter = data->WeightParameter;
        }
    }
    if (!snapId.IsValid() && path.IsEmpty())
        path = SnapshotPath;
    if (!snapId.IsValid() && path.IsEmpty())
        return false;
    AudioEventCreateOptions options;
    options.AutoPlay = true;
    _snapshotHandle = AudioEventSystem::CreateInstance(snapId, path, options);
    return _snapshotHandle.IsValid();
}

void AudioZoneVolume::ApplyMixerWeight(float weight)
{
    const float value = Math::Saturate(weight);
    if (TargetType == AudioZoneTargetType::BusVolume)
    {
        AudioEventSystem::SetBusVolume(TargetId, TargetPath, value);
        return;
    }
    if (TargetType == AudioZoneTargetType::BusMute)
    {
        AudioEventSystem::SetBusMute(TargetId, TargetPath, value >= 0.5f);
        return;
    }
    if (TargetType == AudioZoneTargetType::VCAVolume)
    {
        AudioEventSystem::SetVCAVolume(TargetId, TargetPath, value);
        return;
    }
    if (TargetType == AudioZoneTargetType::GlobalParameter)
    {
        if (TargetParameter.IsValid())
            AudioEventSystem::SetGlobalParameter(TargetParameter, value);
        return;
    }
    if (!_snapshotHandle.IsValid())
        return;
    const AudioParameterId parameter = SnapshotWeightParameter.IsValid() ? SnapshotWeightParameter : _resolvedWeightParameter;
    if (parameter.IsValid())
        AudioEventSystem::SetParameter(_snapshotHandle, parameter, value);
    else
        AudioEventSystem::SetSnapshotWeight(_snapshotHandle, value);
}

void AudioZoneVolume::ReleaseMixerInstance(AudioStopMode stopMode)
{
    if (_snapshotHandle.IsValid())
    {
        AudioEventSystem::StopAndRelease(_snapshotHandle, stopMode);
        _snapshotHandle = AudioEventHandle();
    }
    _resolvedWeightParameter = AudioParameterId();
}

String AudioZoneVolume::GetMixerTargetKey() const
{
    if (TargetType != AudioZoneTargetType::Snapshot)
    {
        if (TargetType == AudioZoneTargetType::GlobalParameter)
            return TEXT("Global:") + TargetParameter.Name;
        const String prefix = TargetType == AudioZoneTargetType::BusVolume ? TEXT("BusVolume:") :
            (TargetType == AudioZoneTargetType::BusMute ? TEXT("BusMute:") : TEXT("VCA:"));
        return prefix + TargetPath + TargetId.ToString();
    }
    if (Snapshot)
    {
        const auto* data = Snapshot->GetInstance<AudioSnapshot>();
        if (data)
        {
            if (data->BackendId.IsValid())
                return TEXT("Snapshot:") + data->BackendId.ToString();
            return TEXT("Snapshot:") + data->Path;
        }
    }
    return TEXT("Snapshot:") + SnapshotPath;
}

void AudioZoneVolume::OnEnable()
{
    AudioVolumeBase::OnEnable();
}

void AudioZoneVolume::OnDisable()
{
    if (_snapshotHandle.IsValid())
    {
        ReleaseMixerInstance(AudioStopMode::Immediate);
        _mixerWeight = 0.0f;
        _currentRawWeight = 0.0f;
        _currentResolvedWeight = 0.0f;
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
    SERIALIZE(TargetType);
    SERIALIZE(TargetId);
    SERIALIZE(TargetPath);
    SERIALIZE(TargetParameter);
}

void AudioZoneVolume::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    AudioVolumeBase::Deserialize(stream, modifier);

    DESERIALIZE(Snapshot);
    DESERIALIZE(SnapshotPath);
    DESERIALIZE(SnapshotWeightParameter);
    DESERIALIZE(TargetType);
    DESERIALIZE(TargetId);
    DESERIALIZE(TargetPath);
    DESERIALIZE(TargetParameter);
}
