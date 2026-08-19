// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioEventTypes.h"
#include "IAudioEventBackend.h"
#include "Engine/Scripting/ScriptingType.h"

/// <summary>
/// Static engine-facing entry point for playing and managing audio events, snapshots, banks, and parameters.
/// </summary>
API_CLASS(Static) class FLAXENGINE_API AudioEventSystem
{
    DECLARE_SCRIPTING_TYPE_NO_SPAWN(AudioEventSystem);
    friend class AudioService;

private:
    static IAudioEventBackend* _backend;

public:
    /// <summary>
    /// Gets the currently active audio event backend instance.
    /// </summary>
    static IAudioEventBackend* GetBackend();

    /// <summary>
    /// Sets the active audio event backend instance.
    /// </summary>
    static void SetBackend(IAudioEventBackend* backend);

    /// <summary>
    /// Gets the backend type currently active.
    /// </summary>
    API_PROPERTY() static AudioEventBackendType GetBackendType();

    /// <summary>
    /// Gets the name of the active audio event backend.
    /// </summary>
    API_PROPERTY() static String GetBackendName();

public:
    /// <summary>
    /// Sets the master volume for audio event playback.
    /// </summary>
    /// <param name="volume">Volume multiplier in range [0, 1].</param>
    API_FUNCTION() static void SetMasterVolume(float volume);

    /// <summary>
    /// Sets the master pitch multiplier for audio event playback.
    /// </summary>
    /// <param name="pitch">Pitch multiplier.</param>
    API_FUNCTION() static void SetMasterPitch(float pitch);

    /// <summary>
    /// Sets whether audio event playback is paused globally.
    /// </summary>
    /// <param name="paused">True to pause, false to resume.</param>
    API_FUNCTION() static void SetPaused(bool paused);

    /// <summary>
    /// Sets whether audio event playback is muted globally.
    /// </summary>
    /// <param name="muted">True to mute, false to unmute.</param>
    API_FUNCTION() static void SetMuted(bool muted);

    /// <summary>
    /// Sets the global Doppler scale factor.
    /// </summary>
    /// <param name="factor">Doppler multiplier.</param>
    API_FUNCTION() static void SetDopplerFactor(float factor);

    /// <summary>
    /// Sets the global 3D distance scale factor.
    /// </summary>
    /// <param name="factor">Distance scaling factor.</param>
    API_FUNCTION() static void SetDistanceFactor(float factor);

    /// <summary>
    /// Loads a sound bank into memory.
    /// </summary>
    /// <param name="bankId">The bank asset ID or GUID.</param>
    /// <param name="path">Optional direct file path to the bank.</param>
    /// <param name="nonBlocking">If true, loads asynchronously in background.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool LoadBank(const Guid& bankId, const StringView& path = StringView::Empty, bool nonBlocking = false);

    /// <summary>
    /// Unloads a sound bank from memory.
    /// </summary>
    /// <param name="bankId">The bank asset ID or GUID.</param>
    /// <param name="path">Optional bank file path used when the bank was loaded without a GUID.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool UnloadBank(const Guid& bankId, const StringView& path = StringView::Empty);

    /// <summary>
    /// Unloads all currently loaded sound banks.
    /// </summary>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool UnloadAllBanks();

    /// <summary>
    /// Checks if a sound bank is loaded.
    /// </summary>
    /// <param name="bankId">The bank asset ID or GUID.</param>
    /// <returns>True if loaded, false otherwise.</returns>
    API_FUNCTION() static bool IsBankLoaded(const Guid& bankId);

    /// <summary>
    /// Creates a playback instance for an audio event.
    /// </summary>
    /// <param name="eventId">The event asset ID or GUID.</param>
    /// <param name="path">Optional path to event.</param>
    /// <param name="options">Playback options.</param>
    /// <returns>Valid handle on success, invalid on failure.</returns>
    API_FUNCTION() static AudioEventHandle CreateInstance(const Guid& eventId, const StringView& path = StringView::Empty, const AudioEventCreateOptions& options = AudioEventCreateOptions());

    /// <summary>
    /// Starts playback of an audio event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool Play(AudioEventHandle handle);

    /// <summary>
    /// Pauses playback of an audio event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool Pause(AudioEventHandle handle);

    /// <summary>
    /// Stops playback of an audio event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="stopMode">Stop mode.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool Stop(AudioEventHandle handle, AudioStopMode stopMode = AudioStopMode::AllowFadeOut);

    /// <summary>
    /// Stops all active audio event instances, including one-shots owned by the backend.
    /// </summary>
    /// <param name="stopMode">Stop mode.</param>
    /// <returns>True on success, false if no backend is active.</returns>
    API_FUNCTION() static bool StopAll(AudioStopMode stopMode = AudioStopMode::AllowFadeOut);

    /// <summary>
    /// Releases an audio event instance back to the pool.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool ReleaseInstance(AudioEventHandle handle);

    /// <summary>
    /// Plays an audio event one-shot in 3D or 2D space.
    /// </summary>
    /// <param name="eventId">Event ID or GUID.</param>
    /// <param name="path">Optional event path.</param>
    /// <param name="attributes">3D spatial attributes.</param>
    /// <param name="volume">Volume multiplier.</param>
    /// <param name="pitch">Pitch multiplier.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool PlayOneShot(const Guid& eventId, const StringView& path = StringView::Empty, const Audio3DAttributes& attributes = Audio3DAttributes(), float volume = 1.0f, float pitch = 1.0f);

    /// <summary>
    /// Sets 3D spatial positioning and orientation attributes on an event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="attributes">3D spatial attributes.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes);

    /// <summary>
    /// Sets instance volume.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="volume">Volume multiplier.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetVolume(AudioEventHandle handle, float volume);

    /// <summary>
    /// Sets instance pitch.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="pitch">Pitch multiplier.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetPitch(AudioEventHandle handle, float pitch);

    /// <summary>
    /// Sets instance timeline playback position.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="milliseconds">Timeline position in milliseconds.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetTimelinePosition(AudioEventHandle handle, int32 milliseconds);

    /// <summary>
    /// Sets the listener mask bitfield for an event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="listenerMask">Mask of listeners.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetListenerMask(AudioEventHandle handle, uint32 listenerMask);

    /// <summary>
    /// Sets a numeric parameter value on an event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="id">Parameter identifier.</param>
    /// <param name="value">Numeric parameter value.</param>
    /// <param name="ignoreSeekSpeed">If true, sets value instantly ignoring seek speed.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Sets a labeled parameter value on an event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="id">Parameter identifier.</param>
    /// <param name="label">Label name.</param>
    /// <param name="ignoreSeekSpeed">If true, sets value instantly ignoring seek speed.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Sets a global numeric parameter across all event instances.
    /// </summary>
    /// <param name="id">Parameter identifier.</param>
    /// <param name="value">Numeric parameter value.</param>
    /// <param name="ignoreSeekSpeed">If true, sets value instantly ignoring seek speed.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Sets a global labeled parameter across all event instances.
    /// </summary>
    /// <param name="id">Parameter identifier.</param>
    /// <param name="label">Label name.</param>
    /// <param name="ignoreSeekSpeed">If true, sets value instantly ignoring seek speed.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false);

    /// <summary>
    /// Queries current playback state of an event instance.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="outState">Result state.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool QueryInstance(AudioEventHandle handle, API_PARAM(Out) AudioEventInstanceState& outState);

    /// <summary>
    /// Sets snapshot evaluation blend weight.
    /// </summary>
    /// <param name="handle">Instance handle.</param>
    /// <param name="weight">Weight multiplier in range [0, 1].</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetSnapshotWeight(AudioEventHandle handle, float weight);

    /// <summary>
    /// Sets volume on a mixer bus.
    /// </summary>
    /// <param name="busId">Bus GUID.</param>
    /// <param name="path">Bus path string.</param>
    /// <param name="volume">Volume multiplier.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetBusVolume(const Guid& busId, const StringView& path, float volume);

    /// <summary>
    /// Sets mute state on a mixer bus.
    /// </summary>
    /// <param name="busId">Bus GUID.</param>
    /// <param name="path">Bus path string.</param>
    /// <param name="mute">True to mute, false to unmute.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetBusMute(const Guid& busId, const StringView& path, bool mute);

    /// <summary>
    /// Sets pause state on a mixer bus.
    /// </summary>
    /// <param name="busId">Bus GUID.</param>
    /// <param name="path">Bus path string.</param>
    /// <param name="paused">True to pause, false to resume.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetBusPaused(const Guid& busId, const StringView& path, bool paused);

    /// <summary>
    /// Sets volume on a Voltage-Controlled Amplifier (VCA).
    /// </summary>
    /// <param name="vcaId">VCA GUID.</param>
    /// <param name="path">VCA path string.</param>
    /// <param name="volume">Volume multiplier.</param>
    /// <returns>True on success, false on failure.</returns>
    API_FUNCTION() static bool SetVCAVolume(const Guid& vcaId, const StringView& path, float volume);

    /// <summary>
    /// Captures a point-in-time diagnostics telemetry snapshot from the active backend.
    /// </summary>
    /// <param name="outSnapshot">Result diagnostics snapshot.</param>
    API_FUNCTION() static void CaptureDiagnostics(API_PARAM(Out) AudioDiagnosticsSnapshot& outSnapshot);
};
