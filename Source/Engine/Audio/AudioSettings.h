// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Config/Settings.h"
#include "Engine/Content/JsonAssetReference.h"
#include "Events/AudioEventTypes.h"
#include "Events/Assets/AudioBank.h"
#include "Events/Surface/AudioSurfaceLibrary.h"

/// <summary>
/// Audio settings container.
/// </summary>
API_CLASS(sealed, Namespace="FlaxEditor.Content.Settings") class FLAXENGINE_API AudioSettings : public SettingsBase
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AudioSettings);
    API_AUTO_SERIALIZATION();

public:
    /// <summary>
    /// If checked, audio playback will be disabled in build game. Can be used if game uses custom audio playback engine.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(0), EditorDisplay(\"General\")")
    bool DisableAudio = false;

    /// <summary>
    /// The audio event backend to use (e.g. None, FMOD Studio).
    /// </summary>
    API_FIELD(Attributes="EditorOrder(20), EditorDisplay(\"Backend\")")
    AudioEventBackendType EventBackend = AudioEventBackendType::None;

    /// <summary>
    /// Mode for legacy AudioClip / AudioSource playback when an event backend is active.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(30), EditorDisplay(\"Backend\", \"Native Clips\")")
    NativeAudioClipMode NativeClips = NativeAudioClipMode::DisabledWhenEventBackendActive;

    /// <summary>
    /// Selects which audio subsystem owns the primary audio device and master output.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(40), EditorDisplay(\"Backend\", \"Output Owner\")")
    AudioOutputOwner OutputOwner = AudioOutputOwner::NativeClipBackend;

    /// <summary>Master strings bank. Loaded before the master and startup banks.</summary>
    API_FIELD(Attributes="EditorOrder(50), EditorDisplay(\"FMOD Banks\")")
    JsonAssetReference<AudioBank> MasterStringsBank;

    /// <summary>Master bank. Loaded before all dependent startup banks.</summary>
    API_FIELD(Attributes="EditorOrder(60), EditorDisplay(\"FMOD Banks\")")
    JsonAssetReference<AudioBank> MasterBank;

    /// <summary>Banks loaded at audio service startup in deterministic list order.</summary>
    API_FIELD(Attributes="EditorOrder(70), EditorDisplay(\"FMOD Banks\")")
    Array<JsonAssetReference<AudioBank>> StartupBanks;

    /// <summary>Loads sample data for startup banks after their metadata is loaded.</summary>
    API_FIELD(Attributes="EditorOrder(80), EditorDisplay(\"FMOD Banks\")")
    bool PreloadStartupBankSampleData = false;

    /// <summary>Locale used to select localized variants from the cooked manifest.</summary>
    API_FIELD(Attributes="EditorOrder(90), EditorDisplay(\"FMOD Banks\")")
    String AudioLocale = TEXT("default");

    /// <summary>Maximum FMOD Studio virtual channels (active event timelines).</summary>
    API_FIELD(Attributes="EditorOrder(100), EditorDisplay(\"FMOD Runtime\")")
    int32 FmodMaxChannels = 1024;

    /// <summary>Maximum FMOD Core real software channels mixed at once.</summary>
    API_FIELD(Attributes="EditorOrder(101), EditorDisplay(\"FMOD Runtime\"), Limit(1, 256)")
    int32 FmodRealChannels = 128;

    /// <summary>Enables FMOD Studio Live Update in Editor and Development builds.</summary>
    API_FIELD(Attributes="EditorOrder(110), EditorDisplay(\"FMOD Runtime\")")
    bool EnableLiveUpdate = false;

    /// <summary>
    /// FMOD Studio asynchronous update period in milliseconds. Lower values make listener and emitter motion more responsive at the cost of additional audio-thread work.
    /// The effective period is quantized to the platform mixer block duration.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(111), EditorDisplay(\"FMOD Runtime\"), Limit(1, 1000)")
    int32 FmodStudioUpdatePeriod = 5;

    /// <summary>FMOD profiler and Live Update port.</summary>
    API_FIELD(Attributes="EditorOrder(120), EditorDisplay(\"FMOD Runtime\")")
    uint16 LiveUpdatePort = 9264;

    /// <summary>Retries without Live Update when its port or profiler initialization fails.</summary>
    API_FIELD(Attributes="EditorOrder(121), EditorDisplay(\"FMOD Runtime\")")
    bool AllowLiveUpdateFallback = true;

    /// <summary>FMOD output type numeric value, or -1 to use the platform default.</summary>
    API_FIELD(Attributes="EditorOrder(122), EditorDisplay(\"FMOD Platform\")")
    int32 FmodOutputType = -1;

    /// <summary>Output sample rate, or zero to use the device default.</summary>
    API_FIELD(Attributes="EditorOrder(123), EditorDisplay(\"FMOD Platform\")")
    int32 FmodSampleRate = 0;

    /// <summary>FMOD speaker mode numeric value, or zero for default.</summary>
    API_FIELD(Attributes="EditorOrder(124), EditorDisplay(\"FMOD Platform\")")
    int32 FmodSpeakerMode = 0;

    API_FIELD(Attributes="EditorOrder(125), EditorDisplay(\"FMOD Platform\")")
    uint32 FmodDspBufferLength = 0;

    API_FIELD(Attributes="EditorOrder(126), EditorDisplay(\"FMOD Platform\")")
    int32 FmodDspBufferCount = 0;

    /// <summary>Maximum simultaneously decoded Vorbis streams.</summary>
    API_FIELD(Attributes="EditorOrder(127), EditorDisplay(\"FMOD Platform\")")
    int32 FmodMaxVorbisCodecs = 32;

    /// <summary>Name of an environment variable containing the FMOD bank encryption key. The secret is never serialized.</summary>
    API_FIELD(Attributes="EditorOrder(128), EditorDisplay(\"FMOD Security\")")
    String EncryptionKeyEnvironmentVariable;

    /// <summary>Reloads affected banks after a successful Studio build.</summary>
    API_FIELD(Attributes="EditorOrder(140), EditorDisplay(\"FMOD Workflow\")")
    bool AutoReloadBanksOnBuild = true;

    /// <summary>Synchronizes metadata after a successful Studio bank build.</summary>
    API_FIELD(Attributes="EditorOrder(150), EditorDisplay(\"FMOD Workflow\")")
    bool AutoSyncMetadataOnBankBuild = true;

    /// <summary>Allowlisted FMOD DSP plugin base names deployed by the audio cooker.</summary>
    API_FIELD(Attributes="EditorOrder(160), EditorDisplay(\"FMOD Cooking\")")
    Array<String> RuntimePluginAllowlist;

    /// <summary>Default material-to-event library used by automatic physics interaction audio.</summary>
    API_FIELD(Attributes="EditorOrder(170), EditorDisplay(\"Acoustics\")")
    JsonAssetReference<AudioSurfaceLibrary> SurfaceLibrary;

    /// <summary>
    /// The doppler effect factor. Scale for source and listener velocities. Default is 1.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(200), EditorDisplay(\"General\")")
    float DopplerFactor = 1.0f;

    /// <summary>
    /// True if all audio playback should be muted when the game has no user focus.
    /// </summary>
    API_FIELD(Attributes="EditorOrder(210), EditorDisplay(\"General\", \"Mute On Focus Loss\")")
    bool MuteOnFocusLoss = false;

    /// <summary>
    /// Enables or disables HRTF audio for in-engine processing of 3D audio (if supported by platform).
    /// If enabled, the user should be using two-channel/headphones audio output and have all other surround virtualization disabled (Atmos, DTS:X, vendor specific, etc.)
    /// </summary>
    API_FIELD(Attributes="EditorOrder(220), EditorDisplay(\"Spatial Audio\")")
    bool EnableHRTF = false;

public:
    /// <summary>
    /// Gets the instance of the settings asset (default value if missing). Object returned by this method is always loaded with valid data to use.
    /// </summary>
    static AudioSettings* Get();

    // [SettingsBase]
    void Apply() override;
};
