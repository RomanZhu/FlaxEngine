// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioBackendFMODCore.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Core/Log.h"

AudioBackendFMODCore::BufferSlot* AudioBackendFMODCore::GetBuffer(uint32 id)
{
    return id && id <= (uint32)_buffers.Count() && _buffers[id - 1].Allocated ? &_buffers[id - 1] : nullptr;
}

AudioBackendFMODCore::SourceSlot* AudioBackendFMODCore::GetSource(uint32 id)
{
    return id && id <= (uint32)_sources.Count() && _sources[id - 1].Allocated ? &_sources[id - 1] : nullptr;
}

bool AudioBackendFMODCore::ValidateChannel(SourceSlot& source, bool* outPlaying)
{
    if (!source.Channel)
        return false;
    bool playing = false;
    if (source.Channel->isPlaying(&playing) != FMOD_OK)
    {
        // FMOD may recycle Core channels when the shared Studio voice budget is
        // saturated. Raw Channel pointers become invalid at that point; retire
        // the slot immediately so subsequent property updates cannot flood the
        // error callback with operations on the stale handle.
        source.Channel = nullptr;
        source.Playing = false;
        if (outPlaying)
            *outPlaying = false;
        return false;
    }
    if (outPlaying)
        *outPlaying = playing;
    return true;
}

void AudioBackendFMODCore::ApplySource(SourceSlot& source)
{
    if (!ValidateChannel(source))
        return;
    source.Channel->setVolume(source.Volume);
    source.Channel->setPitch(source.Pitch);
    source.Channel->setPan(source.Pan);
    source.Channel->setMode((source.Spatial ? FMOD_3D : FMOD_2D) | (source.Loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF));
    if (source.Spatial)
    {
        const FMOD_VECTOR position = FmodConvert::ToFmodPositionMeters(source.Position);
        const FMOD_VECTOR velocity = FmodConvert::ToFmodVelocityMetersPerSecond(source.Velocity);
        source.Channel->set3DAttributes(&position, &velocity);
        source.Channel->set3DMinMaxDistance(source.MinDistance * 0.01f, source.MaxDistance * 0.01f);
    }
}

bool AudioBackendFMODCore::PlayBuffer(SourceSlot& source, uint32 bufferId)
{
    BufferSlot* buffer = GetBuffer(bufferId);
    if (!_system || !buffer || !buffer->Sound)
        return false;
    if (_system->playSound(buffer->Sound, _channelGroup, true, &source.Channel) != FMOD_OK || !source.Channel)
        return false;
    source.CurrentBuffer = bufferId;
    ApplySource(source);
    source.Channel->setPaused(source.Paused);
    source.Playing = true;
    return true;
}

void AudioBackendFMODCore::Listener_Reset()
{
    const FMOD_VECTOR zero = {};
    FMOD_VECTOR forward = { 0.0f, 0.0f, 1.0f };
    FMOD_VECTOR up = { 0.0f, 1.0f, 0.0f };
    if (_system)
        _system->set3DListenerAttributes(0, &zero, &zero, &forward, &up);
}

void AudioBackendFMODCore::Listener_VelocityChanged(const Vector3& velocity)
{
    if (!_system || Audio::Listeners.IsEmpty())
        return;
    auto* listener = Audio::Listeners[0];
    Listener_TransformChanged(listener->GetPosition(), listener->GetOrientation());
}

void AudioBackendFMODCore::Listener_TransformChanged(const Vector3& position, const Quaternion& orientation)
{
    if (!_system)
        return;
    const Vector3 velocity = Audio::Listeners.HasItems() ? Audio::Listeners[0]->GetVelocity() : Vector3::Zero;
    const FMOD_VECTOR p = FmodConvert::ToFmodPositionMeters(position);
    const FMOD_VECTOR v = FmodConvert::ToFmodVelocityMetersPerSecond(velocity);
    const FMOD_VECTOR f = FmodConvert::ToFmodDirection(Vector3::Transform(Vector3::Forward, orientation));
    const FMOD_VECTOR u = FmodConvert::ToFmodDirection(Vector3::Transform(Vector3::Up, orientation));
    _system->set3DListenerAttributes(0, &p, &v, &f, &u);
}

void AudioBackendFMODCore::Listener_ReinitializeAll()
{
    if (Audio::Listeners.HasItems())
        Listener_TransformChanged(Audio::Listeners[0]->GetPosition(), Audio::Listeners[0]->GetOrientation());
    else
        Listener_Reset();
}

uint32 AudioBackendFMODCore::Source_Add(const AudioDataInfo&, const Vector3& position, const Quaternion&, float volume, float pitch, float pan, bool loop, bool spatial, float attenuation, float minDistance, float)
{
    int32 index = 0;
    for (; index < _sources.Count(); index++)
        if (!_sources[index].Allocated)
            break;
    if (index == _sources.Count())
        _sources.Add(SourceSlot());
    SourceSlot& source = _sources[index];
    source = SourceSlot();
    source.Allocated = true;
    source.Position = position;
    source.Volume = volume;
    source.Pitch = pitch;
    source.Pan = pan;
    source.Loop = loop;
    source.Spatial = spatial;
    source.MinDistance = minDistance;
    source.MaxDistance = Math::Max(minDistance, minDistance + Math::Max(1.0f, attenuation) * 10000.0f);
    return index + 1;
}

void AudioBackendFMODCore::Source_Remove(uint32 sourceID)
{
    SourceSlot* source = GetSource(sourceID);
    if (!source)
        return;
    if (ValidateChannel(*source))
        source->Channel->stop();
    *source = SourceSlot();
}

void AudioBackendFMODCore::Source_VelocityChanged(uint32 sourceID, const Vector3& velocity) { if (auto* s = GetSource(sourceID)) { s->Velocity = velocity; ApplySource(*s); } }
void AudioBackendFMODCore::Source_TransformChanged(uint32 sourceID, const Vector3& position, const Quaternion&) { if (auto* s = GetSource(sourceID)) { s->Position = position; ApplySource(*s); } }
void AudioBackendFMODCore::Source_VolumeChanged(uint32 sourceID, float volume) { if (auto* s = GetSource(sourceID)) { s->Volume = volume; ApplySource(*s); } }
void AudioBackendFMODCore::Source_PitchChanged(uint32 sourceID, float pitch) { if (auto* s = GetSource(sourceID)) { s->Pitch = pitch; ApplySource(*s); } }
void AudioBackendFMODCore::Source_PanChanged(uint32 sourceID, float pan) { if (auto* s = GetSource(sourceID)) { s->Pan = pan; ApplySource(*s); } }
void AudioBackendFMODCore::Source_IsLoopingChanged(uint32 sourceID, bool loop) { if (auto* s = GetSource(sourceID)) { s->Loop = loop; ApplySource(*s); } }

void AudioBackendFMODCore::Source_SpatialSetupChanged(uint32 sourceID, bool spatial, float attenuation, float minDistance, float)
{
    if (auto* s = GetSource(sourceID))
    {
        s->Spatial = spatial;
        s->MinDistance = minDistance;
        s->MaxDistance = Math::Max(minDistance, minDistance + Math::Max(1.0f, attenuation) * 10000.0f);
        ApplySource(*s);
    }
}

void AudioBackendFMODCore::Source_Play(uint32 sourceID)
{
    SourceSlot* source = GetSource(sourceID);
    if (!source)
        return;
    source->Paused = false;
    if (source->Playing && ValidateChannel(*source) && source->Channel->setPaused(false) == FMOD_OK)
        return;
    if (source->CurrentBuffer)
        PlayBuffer(*source, source->CurrentBuffer);
    else if (source->Queue.HasItems())
        PlayBuffer(*source, source->Queue[0]);
}

void AudioBackendFMODCore::Source_Pause(uint32 sourceID) { if (auto* s = GetSource(sourceID)) { s->Paused = true; if (ValidateChannel(*s)) s->Channel->setPaused(true); } }
void AudioBackendFMODCore::Source_Stop(uint32 sourceID) { if (auto* s = GetSource(sourceID)) { if (ValidateChannel(*s)) s->Channel->stop(); s->Channel = nullptr; s->Playing = false; s->ProcessedBuffers = 0; } }

void AudioBackendFMODCore::Source_SetCurrentBufferTime(uint32 sourceID, float value)
{
    if (auto* s = GetSource(sourceID))
        if (ValidateChannel(*s))
            s->Channel->setPosition((uint32)(Math::Max(0.0f, value) * 1000.0f), FMOD_TIMEUNIT_MS);
}

float AudioBackendFMODCore::Source_GetCurrentBufferTime(uint32 sourceID)
{
    uint32 position = 0;
    if (auto* s = GetSource(sourceID))
        if (ValidateChannel(*s) && s->Channel->getPosition(&position, FMOD_TIMEUNIT_MS) != FMOD_OK)
        {
            s->Channel = nullptr;
            s->Playing = false;
        }
    return position * 0.001f;
}

void AudioBackendFMODCore::Source_SetNonStreamingBuffer(uint32 sourceID, uint32 bufferID) { if (auto* s = GetSource(sourceID)) { s->Queue.Clear(); s->CurrentBuffer = bufferID; } }
void AudioBackendFMODCore::Source_GetProcessedBuffersCount(uint32 sourceID, int32& count) { auto* s = GetSource(sourceID); count = s ? s->ProcessedBuffers : 0; }
void AudioBackendFMODCore::Source_GetQueuedBuffersCount(uint32 sourceID, int32& count) { auto* s = GetSource(sourceID); count = s ? s->Queue.Count() : 0; }
void AudioBackendFMODCore::Source_QueueBuffer(uint32 sourceID, uint32 bufferID) { if (auto* s = GetSource(sourceID)) if (GetBuffer(bufferID) && !s->Queue.Contains(bufferID)) s->Queue.Add(bufferID); }

void AudioBackendFMODCore::Source_DequeueProcessedBuffers(uint32 sourceID)
{
    if (auto* s = GetSource(sourceID))
    {
        const int32 count = Math::Min(s->ProcessedBuffers, s->Queue.Count());
        for (int32 i = 0; i < count; i++)
            s->Queue.RemoveAtKeepOrder(0);
        s->ProcessedBuffers = 0;
    }
}

uint32 AudioBackendFMODCore::Buffer_Create()
{
    int32 index = 0;
    for (; index < _buffers.Count(); index++)
        if (!_buffers[index].Allocated)
            break;
    if (index == _buffers.Count())
        _buffers.Add(BufferSlot());
    _buffers[index].Allocated = true;
    return index + 1;
}

void AudioBackendFMODCore::Buffer_Delete(uint32 bufferID)
{
    if (auto* buffer = GetBuffer(bufferID))
    {
        if (buffer->Sound)
            buffer->Sound->release();
        *buffer = BufferSlot();
    }
}

void AudioBackendFMODCore::Buffer_Write(uint32 bufferID, byte* samples, const AudioDataInfo& info)
{
    BufferSlot* buffer = GetBuffer(bufferID);
    if (!buffer || !_system || !samples)
        return;
    if (buffer->Sound)
        buffer->Sound->release();
    FMOD_CREATESOUNDEXINFO ex = {};
    ex.cbsize = sizeof(ex);
    ex.length = info.NumSamples * (info.BitDepth / 8);
    ex.numchannels = info.NumChannels;
    ex.defaultfrequency = info.SampleRate;
    ex.format = info.BitDepth == 8 ? FMOD_SOUND_FORMAT_PCM8 : info.BitDepth == 24 ? FMOD_SOUND_FORMAT_PCM24 : info.BitDepth == 32 ? FMOD_SOUND_FORMAT_PCM32 : FMOD_SOUND_FORMAT_PCM16;
    const FMOD_RESULT result = _system->createSound((const char*)samples, FMOD_OPENMEMORY | FMOD_OPENRAW | FMOD_CREATESAMPLE, &ex, &buffer->Sound);
    if (!FmodConvert::CheckResult(result, "Core::createSound(AudioClip)"))
        buffer->Sound = nullptr;
    buffer->Info = info;
}

const Char* AudioBackendFMODCore::Base_Name() { return TEXT("FMOD Core (Studio shared)"); }
AudioBackend::FeatureFlags AudioBackendFMODCore::Base_Features() { return FeatureFlags::SpatialMultiChannel; }
void AudioBackendFMODCore::Base_OnActiveDeviceChanged() { }
void AudioBackendFMODCore::Base_SetDopplerFactor(float value) { if (_system) _system->set3DSettings(Math::Max(0.0f, value), 1.0f, 1.0f); }
void AudioBackendFMODCore::Base_SetVolume(float value) { _masterVolume = Math::Saturate(value); if (_channelGroup) _channelGroup->setVolume(_masterVolume); }
bool AudioBackendFMODCore::Base_Init()
{
    if (!_system || _system->createChannelGroup("Flax Native Clips", &_channelGroup) != FMOD_OK || !_channelGroup)
        return true;
    FMOD::ChannelGroup* masterGroup = nullptr;
    if (_system->getMasterChannelGroup(&masterGroup) != FMOD_OK || !masterGroup || masterGroup->addGroup(_channelGroup) != FMOD_OK)
    {
        _channelGroup->release();
        _channelGroup = nullptr;
        return true;
    }
    _channelGroup->setVolume(_masterVolume);
    return false;
}

void AudioBackendFMODCore::Base_Update()
{
    for (SourceSlot& source : _sources)
    {
        if (!source.Allocated || !source.Playing || !source.Channel || source.Paused)
            continue;
        bool playing = false;
        if (!ValidateChannel(source, &playing))
            continue;
        if (playing)
            continue;
        source.Channel = nullptr;
        source.Playing = false;
        if (source.Queue.HasItems())
        {
            source.ProcessedBuffers++;
            const int32 next = source.ProcessedBuffers;
            if (next < source.Queue.Count())
                PlayBuffer(source, source.Queue[next]);
        }
    }
}

void AudioBackendFMODCore::Base_Dispose()
{
    for (SourceSlot& source : _sources)
        if (ValidateChannel(source))
            source.Channel->stop();
    for (BufferSlot& buffer : _buffers)
        if (buffer.Sound)
            buffer.Sound->release();
    if (_channelGroup)
    {
        _channelGroup->release();
        _channelGroup = nullptr;
    }
    _sources.Clear();
    _buffers.Clear();
    _system = nullptr;
}

#endif
