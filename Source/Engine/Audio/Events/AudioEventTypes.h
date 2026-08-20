// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Types/Guid.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Math/Quaternion.h"
#include "Engine/Core/Math/Transform.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/String.h"
#include "Engine/Core/Types/StringView.h"
#include "Engine/Scripting/ScriptingType.h"
#include "AudioEventHandle.h"

/// <summary>
/// Audio event backend implementation type.
/// </summary>
API_ENUM() enum class AudioEventBackendType : uint8
{
    /// <summary>
    /// Null/no-op backend.
    /// </summary>
    None = 0,

    /// <summary>
    /// FMOD Studio backend.
    /// </summary>
    FMODStudio = 1,
};

/// <summary>
/// Operational mode for legacy AudioClip playback.
/// </summary>
API_ENUM() enum class NativeAudioClipMode : uint8
{
    /// <summary>
    /// Native clips are enabled alongside the event backend.
    /// </summary>
    Enabled = 0,

    /// <summary>
    /// Native clips are disabled when an event backend (e.g. FMOD) is active.
    /// </summary>
    DisabledWhenEventBackendActive = 1,

    /// <summary>
    /// Native clips are always disabled.
    /// </summary>
    Disabled = 2,
};

/// <summary>
/// Defines which audio subsystem owns the primary audio hardware device and master output.
/// </summary>
API_ENUM() enum class AudioOutputOwner : uint8
{
    /// <summary>
    /// Legacy native clip backend owns device output.
    /// </summary>
    NativeClipBackend = 0,

    /// <summary>
    /// Event backend (e.g. FMOD Studio) owns device output.
    /// </summary>
    EventBackend = 1,
};

/// <summary>
/// Backend-neutral description of a physical audio output device.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioOutputDeviceInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioOutputDeviceInfo);

    /// <summary>Display name reported by the output backend.</summary>
    API_FIELD() String Name;

    /// <summary>Stable backend identifier used to select this device.</summary>
    API_FIELD() String StableId;

    /// <summary>Output sample rate when known.</summary>
    API_FIELD() int32 SampleRate = 0;

    /// <summary>Output channel count when known.</summary>
    API_FIELD() int32 Channels = 0;
};

/// <summary>
/// Stop policy for audio event instances.
/// </summary>
API_ENUM() enum class AudioStopMode : uint8
{
    /// <summary>
    /// Allow natural fade-out or release curve authored in the middleware.
    /// </summary>
    AllowFadeOut = 0,

    /// <summary>
    /// Stop immediately and cut off sound voices.
    /// </summary>
    Immediate = 1,
};

/// <summary>
/// Playback state of an audio event instance.
/// </summary>
API_ENUM() enum class AudioEventPlaybackState : uint8
{
    /// <summary>
    /// The event is stopped.
    /// </summary>
    Stopped = 0,

    /// <summary>
    /// The event is currently playing.
    /// </summary>
    Playing = 1,

    /// <summary>
    /// The event is sustaining at a loop point or sustain marker.
    /// </summary>
    Sustaining = 2,

    /// <summary>
    /// The event is paused.
    /// </summary>
    Paused = 3,

    /// <summary>
    /// The event is stopping (e.g. executing fade-out).
    /// </summary>
    Stopping = 4,
};

/// <summary>
/// Runtime loading state of a middleware sound bank.
/// </summary>
API_ENUM() enum class AudioBankState : uint8
{
    Unloaded = 0,
    Loading = 1,
    Loaded = 2,
    Error = 3,
};

/// <summary>Observed middleware state for a numeric parameter.</summary>
API_STRUCT() struct FLAXENGINE_API AudioParameterState
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioParameterState);

    API_FIELD() float Value = 0.0f;
    API_FIELD() float FinalValue = 0.0f;
    API_FIELD() bool IsValid = false;
};

/// <summary>Observed runtime state for a loaded middleware bank.</summary>
API_STRUCT() struct FLAXENGINE_API AudioBankRuntimeState
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioBankRuntimeState);

    API_FIELD() AudioBankState State = AudioBankState::Unloaded;
    API_FIELD() Guid AssetId = Guid::Empty;
    API_FIELD() bool SampleDataLoaded = false;
    API_FIELD() int32 RefCount = 0;
    API_FIELD() String Name;
    API_FIELD() String Path;
    API_FIELD() uint64 FileRevision = 0;
    API_FIELD() int32 LastResult = 0;
};

#include "Engine/Core/ISerializable.h"

/// <summary>
/// Parameter identifier used to target instance or global event parameters.
/// </summary>
API_STRUCT() struct FLAXENGINE_API AudioParameterId : ISerializable
{
    API_AUTO_SERIALIZATION();
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioParameterId);

    /// <summary>
    /// Unique GUID of the parameter from the audio middleware (if available).
    /// </summary>
    API_FIELD() Guid ID = Guid::Empty;

    /// <summary>
    /// Backend-neutral low word of a resolved parameter identifier. Together with Data2 this maps
    /// directly to FMOD Studio's two-word parameter identifier without exposing FMOD SDK types.
    /// </summary>
    API_FIELD() uint32 Data1 = 0;

    /// <summary>
    /// Backend-neutral high word of a resolved parameter identifier.
    /// </summary>
    API_FIELD() uint32 Data2 = 0;

    /// <summary>
    /// Name of the parameter.
    /// </summary>
    API_FIELD() String Name;

    AudioParameterId() = default;

    explicit AudioParameterId(const StringView& name)
        : Name(name)
    {
    }

    explicit AudioParameterId(const Guid& id, const StringView& name = StringView::Empty)
        : ID(id)
        , Name(name)
    {
    }

    FORCE_INLINE bool IsValid() const
    {
        return ID.IsValid() || Data1 != 0 || Data2 != 0 || Name.HasChars();
    }

    FORCE_INLINE bool operator==(const AudioParameterId& other) const
    {
        if (Data1 != 0 || Data2 != 0 || other.Data1 != 0 || other.Data2 != 0)
            return Data1 == other.Data1 && Data2 == other.Data2;
        if (ID.IsValid() && other.ID.IsValid())
            return ID == other.ID;
        return Name == other.Name;
    }

    FORCE_INLINE bool operator!=(const AudioParameterId& other) const
    {
        return !(*this == other);
    }
};

inline uint32 GetHash(const AudioParameterId& key)
{
    if (key.Data1 != 0 || key.Data2 != 0)
        return GetHash(((uint64)key.Data2 << 32) | key.Data1);
    return key.ID.IsValid() ? GetHash(key.ID) : GetHash(key.Name);
}

/// <summary>
/// A numeric parameter update used by batched event calls.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioParameterValue
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioParameterValue);

    API_FIELD() AudioParameterId Id;
    API_FIELD() float Value = 0.0f;
};

/// <summary>
/// 3D spatial transformation attributes for audio emitters and listeners.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API Audio3DAttributes
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(Audio3DAttributes);

    /// <summary>
    /// Position in 3D world space (centimeters).
    /// </summary>
    API_FIELD() Vector3 Position = Vector3::Zero;

    /// <summary>
    /// Velocity in 3D world space (centimeters per second).
    /// </summary>
    API_FIELD() Vector3 Velocity = Vector3::Zero;

    /// <summary>
    /// Forward orientation unit vector.
    /// </summary>
    API_FIELD() Vector3 Forward = Vector3::Forward;

    /// <summary>
    /// Up orientation unit vector.
    /// </summary>
    API_FIELD() Vector3 Up = Vector3::Up;

    Audio3DAttributes() = default;

    Audio3DAttributes(const Vector3& position, const Vector3& velocity, const Vector3& forward, const Vector3& up)
        : Position(position)
        , Velocity(velocity)
        , Forward(forward)
        , Up(up)
    {
    }

    Audio3DAttributes(const Transform& transform, const Vector3& velocity = Vector3::Zero)
        : Position(transform.Translation)
        , Velocity(velocity)
        , Forward(transform.GetForward())
        , Up(transform.GetUp())
    {
    }
};

/// <summary>
/// Snapshot of listener state passed to the event backend.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioListenerState
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioListenerState);

    /// <summary>
    /// Unique ID of the listener actor.
    /// </summary>
    API_FIELD() Guid ActorId = Guid::Empty;

    /// <summary>
    /// Spatial attributes of the listener.
    /// </summary>
    API_FIELD() Audio3DAttributes Attributes;

    /// <summary>
    /// Attenuation weighting for this listener.
    /// </summary>
    API_FIELD() float Weight = 1.0f;

    AudioListenerState() = default;

    AudioListenerState(const Guid& actorId, const Audio3DAttributes& attributes, float weight = 1.0f)
        : ActorId(actorId)
        , Attributes(attributes)
        , Weight(weight)
    {
    }
};

/// <summary>
/// Detailed runtime state of an audio event instance.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioEventInstanceState
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioEventInstanceState);

    /// <summary>
    /// Current playback state.
    /// </summary>
    API_FIELD() AudioEventPlaybackState PlaybackState = AudioEventPlaybackState::Stopped;

    /// <summary>
    /// Timeline position in milliseconds.
    /// </summary>
    API_FIELD() int32 TimelinePosition = 0;

    /// <summary>
    /// Current pitch multiplier.
    /// </summary>
    API_FIELD() float Pitch = 1.0f;

    /// <summary>
    /// Current volume multiplier.
    /// </summary>
    API_FIELD() float Volume = 1.0f;

    /// <summary>
    /// True if the instance is currently paused.
    /// </summary>
    API_FIELD() bool IsPaused = false;
};

/// <summary>
/// Options passed when creating an audio event instance.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioEventCreateOptions
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioEventCreateOptions);

    /// <summary>
    /// If true, the event starts playback immediately upon creation.
    /// </summary>
    API_FIELD() bool AutoPlay = false;

    /// <summary>
    /// Initial 3D spatial attributes.
    /// </summary>
    API_FIELD() Audio3DAttributes Attributes;

    /// <summary>
    /// Bitmask of listeners that can hear this instance.
    /// </summary>
    API_FIELD() uint32 ListenerMask = MAX_uint32;

    /// <summary>
    /// Optional owner actor ID for lifetime association.
    /// </summary>
    API_FIELD() Guid OwnerId = Guid::Empty;
};

/// <summary>Observed runtime state for a live event instance.</summary>
API_STRUCT() struct FLAXENGINE_API AudioEventRuntimeInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioEventRuntimeInfo);

    API_FIELD() AudioEventHandle Handle;
    API_FIELD() Guid EventId = Guid::Empty;
    API_FIELD() Guid OwnerId = Guid::Empty;
    API_FIELD() String Path;
    API_FIELD() AudioEventPlaybackState PlaybackState = AudioEventPlaybackState::Stopped;
    API_FIELD() int32 TimelinePosition = 0;
    API_FIELD() float Volume = 1.0f;
    API_FIELD() bool IsVirtual = false;
    API_FIELD() bool IsOneShot = false;
    /// <summary>Number of real mixer voices attributed to this event when reported by the backend.</summary>
    API_FIELD() int32 RealVoices = 0;
    /// <summary>Number of virtual voices attributed to this event when reported by the backend.</summary>
    API_FIELD() int32 VirtualVoices = 0;
    /// <summary>Observed starts for this event instance (diagnostics may report zero when unavailable).</summary>
    API_FIELD() int32 PlayCount = 0;
    /// <summary>Elapsed timeline time in seconds for display and profiling.</summary>
    API_FIELD() float TimeSeconds = 0.0f;
};

/// <summary>Observed runtime state for an audio bus.</summary>
API_STRUCT() struct FLAXENGINE_API AudioBusRuntimeInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioBusRuntimeInfo);

    API_FIELD() String Path;
    API_FIELD() float Volume = 1.0f;
    API_FIELD() float FinalVolume = 1.0f;
    API_FIELD() bool Muted = false;
    API_FIELD() bool Paused = false;
};

/// <summary>Observed runtime state for an active snapshot instance.</summary>
API_STRUCT() struct FLAXENGINE_API AudioSnapshotRuntimeInfo
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioSnapshotRuntimeInfo);

    API_FIELD() AudioEventHandle Handle;
    API_FIELD() String Path;
    API_FIELD() AudioEventPlaybackState PlaybackState = AudioEventPlaybackState::Stopped;
};

/// <summary>
/// Diagnostic metrics snapshot from the audio event backend.
/// </summary>
API_STRUCT(NoDefault) struct FLAXENGINE_API AudioDiagnosticsSnapshot
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioDiagnosticsSnapshot);

    /// <summary>
    /// Name of the backend that supplied this snapshot.
    /// </summary>
    API_FIELD() String BackendName;
    API_FIELD() String RuntimeVersion;
    API_FIELD() bool LiveUpdateEnabled = false;
    API_FIELD() String ActiveDevice;

    /// <summary>
    /// True when the backend is initialized and able to service event requests.
    /// </summary>
    API_FIELD() bool Initialized = false;

    /// <summary>
    /// Total CPU percentage consumed by the audio event DSP / update thread.
    /// </summary>
    API_FIELD() float CpuUsage = 0.0f;

    /// <summary>
    /// Total memory allocated by the audio event backend in bytes.
    /// </summary>
    API_FIELD() uint64 MemoryAllocated = 0;
    API_FIELD() uint64 MemoryPeak = 0;

    /// <summary>
    /// Current number of active event instances.
    /// </summary>
    API_FIELD() int32 ActiveInstances = 0;
    API_FIELD() int32 RealVoices = 0;
    API_FIELD() int32 VirtualVoices = 0;
    API_FIELD() uint64 TotalInstancesCreated = 0;
    API_FIELD() uint64 TotalPlays = 0;
    API_FIELD() uint64 TotalStopped = 0;
    API_FIELD() int32 PeakActiveInstances = 0;

    /// <summary>
    /// Current number of loaded sound banks.
    /// </summary>
    API_FIELD() int32 LoadedBanks = 0;
    API_FIELD() int32 LoadedSampleDataBanks = 0;
    API_FIELD() float StudioUpdateCpu = 0.0f;
    API_FIELD() float MixerCpu = 0.0f;
    API_FIELD() float StreamCpu = 0.0f;
    API_FIELD() int32 OutputSampleRate = 0;
    API_FIELD() int32 OutputChannels = 0;
    API_FIELD() uint32 DspBufferLength = 0;
    API_FIELD() int32 DspBufferCount = 0;
    API_FIELD() Array<AudioBankRuntimeState> Banks;
    API_FIELD() Array<AudioEventRuntimeInfo> Events;
    API_FIELD() Array<AudioBusRuntimeInfo> Buses;
    API_FIELD() Array<AudioSnapshotRuntimeInfo> Snapshots;

    /// <summary>
    /// Approximate count of callback records waiting for main-thread dispatch.
    /// </summary>
    API_FIELD() uint32 CallbackQueueDepth = 0;

    /// <summary>
    /// Number of callback records dropped because the bounded callback queue was full.
    /// </summary>
    API_FIELD() uint64 DroppedCallbacks = 0;
    API_FIELD() int32 OcclusionQueriesThisFrame = 0;
    API_FIELD() int32 OcclusionDeferred = 0;
};
