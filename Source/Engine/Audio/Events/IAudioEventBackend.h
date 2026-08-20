// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioEventTypes.h"
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
    virtual bool SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) = 0;

    // Global parameters
    virtual bool SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) = 0;
    virtual bool SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) = 0;

    // Instance state query
    virtual bool QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState) const = 0;

    // Snapshots, buses, and VCAs
    virtual bool SetSnapshotWeight(AudioEventHandle handle, float weight) = 0;
    virtual bool SetBusVolume(const Guid& busId, const StringView& path, float volume) = 0;
    virtual bool SetBusMute(const Guid& busId, const StringView& path, bool mute) = 0;
    virtual bool SetBusPaused(const Guid& busId, const StringView& path, bool paused) = 0;
    virtual bool SetVCAVolume(const Guid& vcaId, const StringView& path, float volume) = 0;

    // Diagnostics
    virtual void CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot) = 0;
};
