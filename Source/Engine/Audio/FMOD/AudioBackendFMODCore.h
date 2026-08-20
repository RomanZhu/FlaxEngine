// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/AudioBackend.h"
#include "Engine/Core/Collections/Array.h"
#include <fmod.hpp>

/// <summary>
/// Native AudioClip backend implemented on the Core system owned by FMOD Studio.
/// It never creates, initializes, updates, or releases a second FMOD system.
/// </summary>
class AudioBackendFMODCore final : public AudioBackend
{
private:
    struct BufferSlot
    {
        FMOD::Sound* Sound = nullptr;
        AudioDataInfo Info = {};
        bool Allocated = false;
    };

    struct SourceSlot
    {
        FMOD::Channel* Channel = nullptr;
        Array<uint32, InlinedAllocation<4>> Queue;
        uint32 CurrentBuffer = 0;
        int32 ProcessedBuffers = 0;
        Vector3 Position = Vector3::Zero;
        Vector3 Velocity = Vector3::Zero;
        float Volume = 1.0f;
        float Pitch = 1.0f;
        float Pan = 0.0f;
        float MinDistance = 100.0f;
        float MaxDistance = 10000.0f;
        bool Loop = false;
        bool Spatial = false;
        bool Paused = false;
        bool Playing = false;
        bool Allocated = false;
    };

    FMOD::System* _system;
    FMOD::ChannelGroup* _channelGroup = nullptr;
    Array<BufferSlot> _buffers;
    Array<SourceSlot> _sources;
    float _masterVolume = 1.0f;

public:
    explicit AudioBackendFMODCore(FMOD::System* sharedSystem)
        : _system(sharedSystem)
    {
    }

private:
    BufferSlot* GetBuffer(uint32 id);
    SourceSlot* GetSource(uint32 id);
    void ApplySource(SourceSlot& source);
    bool PlayBuffer(SourceSlot& source, uint32 bufferId);

    // [AudioBackend]
    void Listener_Reset() override;
    void Listener_VelocityChanged(const Vector3& velocity) override;
    void Listener_TransformChanged(const Vector3& position, const Quaternion& orientation) override;
    void Listener_ReinitializeAll() override;
    uint32 Source_Add(const AudioDataInfo& format, const Vector3& position, const Quaternion& orientation, float volume, float pitch, float pan, bool loop, bool spatial, float attenuation, float minDistance, float doppler) override;
    void Source_Remove(uint32 sourceID) override;
    void Source_VelocityChanged(uint32 sourceID, const Vector3& velocity) override;
    void Source_TransformChanged(uint32 sourceID, const Vector3& position, const Quaternion& orientation) override;
    void Source_VolumeChanged(uint32 sourceID, float volume) override;
    void Source_PitchChanged(uint32 sourceID, float pitch) override;
    void Source_PanChanged(uint32 sourceID, float pan) override;
    void Source_IsLoopingChanged(uint32 sourceID, bool loop) override;
    void Source_SpatialSetupChanged(uint32 sourceID, bool spatial, float attenuation, float minDistance, float doppler) override;
    void Source_Play(uint32 sourceID) override;
    void Source_Pause(uint32 sourceID) override;
    void Source_Stop(uint32 sourceID) override;
    void Source_SetCurrentBufferTime(uint32 sourceID, float value) override;
    float Source_GetCurrentBufferTime(uint32 sourceID) override;
    void Source_SetNonStreamingBuffer(uint32 sourceID, uint32 bufferID) override;
    void Source_GetProcessedBuffersCount(uint32 sourceID, int32& processedBuffersCount) override;
    void Source_GetQueuedBuffersCount(uint32 sourceID, int32& queuedBuffersCount) override;
    void Source_QueueBuffer(uint32 sourceID, uint32 bufferID) override;
    void Source_DequeueProcessedBuffers(uint32 sourceID) override;
    uint32 Buffer_Create() override;
    void Buffer_Delete(uint32 bufferID) override;
    void Buffer_Write(uint32 bufferID, byte* samples, const AudioDataInfo& info) override;
    const Char* Base_Name() override;
    FeatureFlags Base_Features() override;
    void Base_OnActiveDeviceChanged() override;
    void Base_SetDopplerFactor(float value) override;
    void Base_SetVolume(float value) override;
    bool Base_Init() override;
    void Base_Update() override;
    void Base_Dispose() override;
};

#endif
