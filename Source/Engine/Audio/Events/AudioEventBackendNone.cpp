// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEventBackendNone.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Core/Log.h"

AudioEventBackendNone::AudioEventBackendNone()
{
}

AudioEventBackendNone::~AudioEventBackendNone()
{
    Dispose();
}

const Char* AudioEventBackendNone::GetName() const
{
    return TEXT("Null");
}

AudioEventBackendType AudioEventBackendNone::GetType() const
{
    return AudioEventBackendType::None;
}

bool AudioEventBackendNone::Init()
{
    _slots.Clear();
    _freeIndices.Clear();
    _loadedBanks.Clear();
    _loadedBankPaths.Clear();
    _loadedBankPathsById.Clear();
    _loadedBankIdsByPath.Clear();
    _globalParameters.Clear();
    _masterVolume = 1.0f;
    _masterPitch = 1.0f;
    _isPaused = false;
    _isMuted = false;
    _dopplerFactor = 1.0f;
    _distanceFactor = 0.01f;
    return false;
}

void AudioEventBackendNone::Update(float dt)
{
    // Advance timeline for playing slots
    if (!_isPaused)
    {
        for (int32 i = 0; i < _slots.Count(); i++)
        {
            auto& slot = _slots[i];
            if (slot.InUse && slot.State.PlaybackState == AudioEventPlaybackState::Playing && !slot.State.IsPaused)
            {
                slot.State.TimelinePosition += (int32)(dt * 1000.0f * slot.State.Pitch * _masterPitch);
            }
        }
    }
}

void AudioEventBackendNone::Dispose()
{
    StopAll(AudioStopMode::Immediate);
    _slots.Clear();
    _freeIndices.Clear();
    _loadedBanks.Clear();
    _loadedBankPaths.Clear();
    _loadedBankPathsById.Clear();
    _loadedBankIdsByPath.Clear();
    _globalParameters.Clear();
}

void AudioEventBackendNone::SetMasterVolume(float volume)
{
    _masterVolume = Math::Saturate(volume);
}

void AudioEventBackendNone::SetMasterPitch(float pitch)
{
    _masterPitch = Math::Clamp(pitch, 0.0f, 10.0f);
}

void AudioEventBackendNone::SetPaused(bool paused)
{
    _isPaused = paused;
}

void AudioEventBackendNone::SetMuted(bool muted)
{
    _isMuted = muted;
}

void AudioEventBackendNone::SetDopplerFactor(float factor)
{
    _dopplerFactor = Math::Max(0.0f, factor);
}

void AudioEventBackendNone::SetDistanceFactor(float factor)
{
    _distanceFactor = Math::Max(0.0001f, factor);
}

void AudioEventBackendNone::OnActiveDeviceChanged()
{
}

void AudioEventBackendNone::EnumerateOutputDevices(Array<AudioOutputDeviceInfo>& result) const
{
    result.Clear();
}

bool AudioEventBackendNone::SetOutputDevice(const StringView& stableId)
{
    return stableId.IsEmpty();
}

String AudioEventBackendNone::GetOutputDevice() const
{
    return String::Empty;
}

void AudioEventBackendNone::UpdateListeners(const Span<AudioListenerState>& listeners)
{
}

bool AudioEventBackendNone::LoadBank(const Guid& bankId, const StringView& path, bool nonBlocking)
{
    bool loaded = false;
    if (bankId.IsValid())
    {
        _loadedBanks.Add(bankId);
        loaded = true;
    }
    if (path.HasChars())
    {
        String pathString(path);
        _loadedBankPaths.Add(pathString);
        if (bankId.IsValid())
        {
            String* previousPath = _loadedBankPathsById.TryGet(bankId);
            if (previousPath && *previousPath != pathString)
            {
                _loadedBankPaths.Remove(*previousPath);
                _loadedBankIdsByPath.Remove(*previousPath);
            }
            _loadedBankPathsById[bankId] = pathString;
            _loadedBankIdsByPath[pathString] = bankId;
        }
        loaded = true;
    }
    return loaded;
}

bool AudioEventBackendNone::UnloadBank(const Guid& bankId, const StringView& path)
{
    bool removed = bankId.IsValid() && _loadedBanks.Remove(bankId);
    if (bankId.IsValid())
    {
        String* mappedPath = _loadedBankPathsById.TryGet(bankId);
        if (mappedPath)
        {
            removed |= _loadedBankPaths.Remove(*mappedPath);
            _loadedBankIdsByPath.Remove(*mappedPath);
            _loadedBankPathsById.Remove(bankId);
        }
    }
    if (path.HasChars())
    {
        String pathString(path);
        removed |= _loadedBankPaths.Remove(pathString);
        Guid* mappedId = _loadedBankIdsByPath.TryGet(pathString);
        if (mappedId)
        {
            removed |= _loadedBanks.Remove(*mappedId);
            _loadedBankPathsById.Remove(*mappedId);
            _loadedBankIdsByPath.Remove(pathString);
        }
    }
    return removed;
}

bool AudioEventBackendNone::UnloadAllBanks()
{
    _loadedBanks.Clear();
    _loadedBankPaths.Clear();
    _loadedBankPathsById.Clear();
    _loadedBankIdsByPath.Clear();
    return true;
}

bool AudioEventBackendNone::IsBankLoaded(const Guid& bankId) const
{
    return _loadedBanks.Contains(bankId);
}

bool AudioEventBackendNone::LoadBankSampleData(const Guid& bankId)
{
    return _loadedBanks.Contains(bankId);
}

void AudioEventBackendNone::UnloadBankSampleData(const Guid& bankId)
{
}

AudioBankState AudioEventBackendNone::GetBankState(const Guid& bankId) const
{
    return _loadedBanks.Contains(bankId) ? AudioBankState::Loaded : AudioBankState::Unloaded;
}

AudioEventHandle AudioEventBackendNone::CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    uint32 index = 0;
    if (_freeIndices.HasItems())
    {
        index = _freeIndices.Pop();
    }
    else
    {
        index = (uint32)_slots.Count();
        _slots.AddDefault(1);
    }

    auto& slot = _slots[index];
    slot.Generation++;
    if (slot.Generation == 0)
        slot.Generation = 1; // 0 reserved for invalid handles

    slot.InUse = true;
    slot.EventId = eventId;
    slot.State.PlaybackState = options.AutoPlay ? AudioEventPlaybackState::Playing : AudioEventPlaybackState::Stopped;
    slot.State.TimelinePosition = 0;
    slot.State.Pitch = 1.0f;
    slot.State.Volume = 1.0f;
    slot.State.IsPaused = false;
    slot.Attributes = options.Attributes;
    slot.Parameters.Clear();

    return AudioEventHandle(index, slot.Generation);
}

bool AudioEventBackendNone::Play(AudioEventHandle handle)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.PlaybackState = AudioEventPlaybackState::Playing;
    slot->State.IsPaused = false;
    return true;
}

bool AudioEventBackendNone::Pause(AudioEventHandle handle)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.IsPaused = true;
    slot->State.PlaybackState = AudioEventPlaybackState::Paused;
    return true;
}

bool AudioEventBackendNone::KeyOff(AudioEventHandle handle)
{
    return Stop(handle, AudioStopMode::AllowFadeOut);
}

bool AudioEventBackendNone::Stop(AudioEventHandle handle, AudioStopMode stopMode)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.PlaybackState = AudioEventPlaybackState::Stopped;
    slot->State.IsPaused = false;
    return true;
}

bool AudioEventBackendNone::StopAll(AudioStopMode stopMode)
{
    (void)stopMode;
    for (int32 i = 0; i < _slots.Count(); i++)
    {
        auto& slot = _slots[i];
        if (!slot.InUse)
            continue;

        // Invalidate every existing handle while retaining generations for safe slot reuse.
        slot.Generation++;
        if (slot.Generation == 0)
            slot.Generation = 1;
        slot.InUse = false;
        slot.EventId = Guid::Empty;
        slot.State = AudioEventInstanceState();
        slot.Parameters.Clear();
        _freeIndices.Add((uint32)i);
    }
    return true;
}

bool AudioEventBackendNone::ReleaseInstance(AudioEventHandle handle)
{
    if (!ValidateHandle(handle))
        return false;

    auto& slot = _slots[handle.Index];
    slot.Generation++;
    if (slot.Generation == 0)
        slot.Generation = 1;

    slot.InUse = false;
    slot.EventId = Guid::Empty;
    slot.State = AudioEventInstanceState();
    slot.Parameters.Clear();
    _freeIndices.Add(handle.Index);
    return true;
}

bool AudioEventBackendNone::PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume, float pitch)
{
    return true;
}

bool AudioEventBackendNone::Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->Attributes = attributes;
    return true;
}

bool AudioEventBackendNone::SetVolume(AudioEventHandle handle, float volume)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.Volume = Math::Saturate(volume);
    return true;
}

bool AudioEventBackendNone::SetPitch(AudioEventHandle handle, float pitch)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.Pitch = Math::Clamp(pitch, 0.0f, 10.0f);
    return true;
}

bool AudioEventBackendNone::SetTimelinePosition(AudioEventHandle handle, int32 milliseconds)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->State.TimelinePosition = Math::Max(0, milliseconds);
    return true;
}

bool AudioEventBackendNone::SetListenerMask(AudioEventHandle handle, uint32 listenerMask)
{
    return ValidateHandle(handle);
}

bool AudioEventBackendNone::ResolveParameterId(const Guid& eventId, const StringView& eventPath, const StringView& name, AudioParameterId& id)
{
    id = AudioParameterId(name);
    return name.HasChars();
}

bool AudioEventBackendNone::SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    slot->Parameters[id] = value;
    return true;
}

bool AudioEventBackendNone::SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed)
{
    bool result = true;
    for (const auto& value : values)
        result &= SetParameter(handle, value.Id, value.Value, ignoreSeekSpeed);
    return result;
}

bool AudioEventBackendNone::SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    return ValidateHandle(handle);
}

bool AudioEventBackendNone::SetProgrammerSound(AudioEventHandle handle, const AudioProgrammerSoundData& data)
{
    return ValidateHandle(handle) && data.Path.HasChars();
}

bool AudioEventBackendNone::SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    _globalParameters[id] = value;
    return true;
}

bool AudioEventBackendNone::SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    return true;
}

bool AudioEventBackendNone::QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState) const
{
    const auto* slot = GetSlot(handle);
    if (!slot)
        return false;

    outState = slot->State;
    return true;
}

bool AudioEventBackendNone::SetSnapshotWeight(AudioEventHandle handle, float weight)
{
    return ValidateHandle(handle);
}

bool AudioEventBackendNone::SetBusVolume(const Guid& busId, const StringView& path, float volume)
{
    return true;
}

bool AudioEventBackendNone::SetBusMute(const Guid& busId, const StringView& path, bool mute)
{
    return true;
}

bool AudioEventBackendNone::SetBusPaused(const Guid& busId, const StringView& path, bool paused)
{
    return true;
}

bool AudioEventBackendNone::SetVCAVolume(const Guid& vcaId, const StringView& path, float volume)
{
    return true;
}

void AudioEventBackendNone::CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot)
{
    outSnapshot = AudioDiagnosticsSnapshot();
    outSnapshot.BackendName = GetName();
    outSnapshot.Initialized = true;
    outSnapshot.CpuUsage = 0.0f;
    outSnapshot.MemoryAllocated = 0;
    // Count path-only banks in addition to GUID-backed entries without double-counting
    // a bank tracked by both its GUID and load path.
    outSnapshot.LoadedBanks = _loadedBanks.Count() + Math::Max(0, _loadedBankPaths.Count() - _loadedBankIdsByPath.Count());

    int32 active = 0;
    for (int32 i = 0; i < _slots.Count(); i++)
    {
        if (_slots[i].InUse)
            active++;
    }
    outSnapshot.ActiveInstances = active;
}

bool AudioEventBackendNone::ValidateHandle(AudioEventHandle handle) const
{
    return handle.IsValid() && (int32)handle.Index < _slots.Count() && _slots[handle.Index].InUse && _slots[handle.Index].Generation == handle.Generation;
}

AudioEventBackendNone::Slot* AudioEventBackendNone::GetSlot(AudioEventHandle handle)
{
    if (ValidateHandle(handle))
        return &_slots[handle.Index];
    return nullptr;
}

const AudioEventBackendNone::Slot* AudioEventBackendNone::GetSlot(AudioEventHandle handle) const
{
    if (ValidateHandle(handle))
        return &_slots[handle.Index];
    return nullptr;
}
