// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioZoneVolume.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
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
            AudioEventSystem::SetSnapshotWeight(_snapshotHandle, sample.Weight);
        }
    }
    else if (_snapshotHandle.IsValid())
    {
        AudioEventSystem::Stop(_snapshotHandle, AudioStopMode::AllowFadeOut);
        AudioEventSystem::ReleaseInstance(_snapshotHandle);
        _snapshotHandle = AudioEventHandle();
    }
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
    }
    AudioVolumeBase::OnDisable();
}

void AudioZoneVolume::Serialize(SerializeStream& stream, const void* otherObj)
{
    AudioVolumeBase::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(AudioZoneVolume);

    SERIALIZE(Snapshot);
    SERIALIZE(SnapshotPath);
}

void AudioZoneVolume::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    AudioVolumeBase::Deserialize(stream, modifier);

    DESERIALIZE(Snapshot);
    DESERIALIZE(SnapshotPath);
}
