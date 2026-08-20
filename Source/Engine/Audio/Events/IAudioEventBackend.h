// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioEventTypes.h"
#include "AudioProgrammerSoundProvider.h"
#include "Engine/Core/Types/Span.h"

/// <summary>
/// Interface for audio event middleware backends (e.g. Null, FMOD Studio).
/// </summary>
class FLAXENGINE_API IAudioEventBackend
{
public:
    virtual ~IAudioEventBackend() = default;

    /// <summary>
    /// Gets the human-readable display name of the audio event backend.
    /// </summary>
    virtual const Char* GetName() const = 0;

    /// <summary>
    /// Gets the backend type.
    /// </summary>
    virtual AudioEventBackendType GetType() const = 0;

    /// <summary>
    /// Initializes the backend.
    /// </summary>
    /// <returns>True on error, false on success.</returns>
    virtual bool Init() = 0;

    /// <summary>
    /// Updates the backend each frame.
    /// </summary>
    /// <param name="dt">Delta time in seconds.</param>
    virtual void Update(float dt) = 0;

    /// <summary>
    /// Disposes and cleans up all resources allocated by the backend.
    /// </summary>
    virtual void Dispose() = 0;

public:
    // Master controls
    virtual void SetMasterVolume(float volume) = 0;
    virtual void SetMasterPitch(float pitch) = 0;
    virtual void SetPaused(bool paused) = 0;
    virtual void SetMuted(bool muted) = 0;
    virtual void SetDopplerFactor(float factor) = 0;
    virtual void SetDistanceFactor(float factor) = 0;
    virtual void OnActiveDeviceChanged() = 0;

    // Output devices. These are meaningful only when this backend owns output.
    virtual void EnumerateOutputDevices(Array<AudioOutputDeviceInfo>& result) const = 0;
    virtual bool SetOutputDevice(const StringView& stableId) = 0;
    virtual String GetOutputDevice() const = 0;

    // Listeners
    virtual void UpdateListeners(const Span<AudioListenerState>& listeners) = 0;

    // Banks
    virtual bool LoadBank(const Guid& bankId, const StringView& path, bool nonBlocking) = 0;
    virtual bool UnloadBank(const Guid& bankId, const StringView& path) = 0;
    virtual bool UnloadAllBanks() = 0;
    virtual bool IsBankLoaded(const Guid& bankId) const = 0;
    virtual bool LoadBankSampleData(const Guid& bankId) = 0;
    virtual void UnloadBankSampleData(const Guid& bankId) = 0;
    virtual AudioBankState GetBankState(const Guid& bankId) const = 0;
    virtual bool QueryBank(const Guid& bankId, const StringView& path, AudioBankRuntimeState& outState) const { outState = AudioBankRuntimeState(); return false; }

    // Event instance playback
    virtual AudioEventHandle CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options) = 0;
    virtual bool Play(AudioEventHandle handle) = 0;
    virtual bool Pause(AudioEventHandle handle) = 0;
    virtual bool KeyOff(AudioEventHandle handle) = 0;
    virtual bool Stop(AudioEventHandle handle, AudioStopMode stopMode) = 0;
    virtual bool StopAll(AudioStopMode stopMode) = 0;
    virtual bool ReleaseInstance(AudioEventHandle handle) = 0;
    virtual bool PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume = 1.0f, float pitch = 1.0f) = 0;

    // Instance attributes & parameters
    virtual bool Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes) = 0;
    virtual bool SetVolume(AudioEventHandle handle, float volume) = 0;
    virtual bool SetPitch(AudioEventHandle handle, float pitch) = 0;
    virtual bool SetTimelinePosition(AudioEventHandle handle, int32 milliseconds) = 0;
    virtual bool SetListenerMask(AudioEventHandle handle, uint32 listenerMask) = 0;
    virtual bool ResolveParameterId(const Guid& eventId, const StringView& eventPath, const StringView& name, AudioParameterId& id) = 0;
    virtual bool SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetProgrammerSound(AudioEventHandle handle, const AudioProgrammerSoundData& data) = 0;

    // Global parameters
    virtual bool SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) = 0;

    // Instance state query
    virtual bool QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState) const = 0;
    virtual bool GetParameter(AudioEventHandle handle, const AudioParameterId& id, AudioParameterState& outState) const { outState = AudioParameterState(); return false; }
    virtual bool GetGlobalParameter(const AudioParameterId& id, AudioParameterState& outState) const { outState = AudioParameterState(); return false; }

    // Snapshots, buses, and VCAs
    virtual bool SetSnapshotWeight(AudioEventHandle handle, float weight) = 0;
    virtual bool SetBusVolume(const Guid& busId, const StringView& path, float volume) = 0;
    virtual bool SetBusMute(const Guid& busId, const StringView& path, bool mute) = 0;
    virtual bool SetBusPaused(const Guid& busId, const StringView& path, bool paused) = 0;
    virtual bool StopBusEvents(const Guid& busId, const StringView& path, AudioStopMode stopMode) { return false; }
    virtual bool GetBusVolume(const Guid& busId, const StringView& path, float& outVolume, float& outFinalVolume) const { outVolume = 0.0f; outFinalVolume = 0.0f; return false; }
    virtual bool GetBusMute(const Guid& busId, const StringView& path, bool& outMuted) const { outMuted = false; return false; }
    virtual bool GetBusPaused(const Guid& busId, const StringView& path, bool& outPaused) const { outPaused = false; return false; }
    virtual bool SetVCAVolume(const Guid& vcaId, const StringView& path, float volume) = 0;
    virtual bool GetVCAVolume(const Guid& vcaId, const StringView& path, float& outVolume, float& outFinalVolume) const { outVolume = 0.0f; outFinalVolume = 0.0f; return false; }

    // Diagnostics
    virtual void CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot) = 0;
};
