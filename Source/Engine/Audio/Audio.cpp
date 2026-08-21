// Copyright (c) Wojciech Figat. All rights reserved.

#include "Audio.h"
#include "AudioBackend.h"
#include "AudioSettings.h"
#include "Engine/Scripting/ScriptingType.h"
#include "Engine/Scripting/BinaryModule.h"
#include "Engine/Level/Level.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Engine/CommandLine.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/EngineService.h"
#if AUDIO_API_NONE
#include "None/AudioBackendNone.h"
#endif
#if AUDIO_API_PS4
#include "Platforms/PS4/Engine/Audio/AudioBackendPS4.h"
#endif
#if AUDIO_API_PS5
#include "Platforms/PS5/Engine/Audio/AudioBackendPS5.h"
#endif
#if AUDIO_API_SWITCH
#include "Platforms/Switch/Engine/Audio/AudioBackendSwitch.h"
#endif
#if AUDIO_API_OPENAL
#include "OpenAL/AudioBackendOAL.h"
#endif
#if AUDIO_API_XAUDIO2
#include "XAudio2/AudioBackendXAudio2.h"
#endif

#include "Events/AudioEventSystem.h"
#include "Events/AudioEventBackendNone.h"
#include "Events/AudioWorld.h"
#include "Events/AudioEventCatalog.h"
#include "Events/Surface/AudioSurfaceLibrary.h"
#include "Events/Surface/AudioPhysicsInteractionSystem.h"
#include "AudioListener.h"
#include "Engine/Engine/Time.h"
#if AUDIO_EVENT_API_FMOD
#include "FMOD/FmodEventBackend.h"
#include "FMOD/AudioBackendFMODCore.h"
#endif

float AudioDataInfo::GetLength() const
{
    return (float)NumSamples / (float)Math::Max(1U, SampleRate * NumChannels);
}

Array<AudioListener*> Audio::Listeners;
Array<AudioSource*> Audio::Sources;
Array<AudioDevice> Audio::Devices;
Action Audio::DevicesChanged;
Action Audio::ActiveDeviceChanged;
AudioBackend* AudioBackend::Instance = nullptr;

namespace
{
    float MasterVolume = 1.0f;
    float Volume = 1.0f;
    int32 ActiveDeviceIndex = -1;
    bool MuteOnFocusLoss = true;
    bool EnableHRTF = true;
}

class AudioService : public EngineService
{
private:
    bool _wasPlayMode = false;

public:

    AudioService()
        : EngineService(TEXT("Audio"), -50)
    {
    }

    bool Init() override;
    void Update() override;
    void Dispose() override;
};

AudioService AudioServiceInstance;

// Spatial attributes must be submitted after scene LateUpdate. Camera rigs and
// gameplay scripts often finalize their transforms there, while the main audio
// service intentionally initializes and performs non-spatial work much earlier.
class AudioSpatialService : public EngineService
{
public:
    AudioSpatialService()
        : EngineService(TEXT("Audio Spatial"), 300)
    {
    }

    void LateUpdate() override;
};

AudioSpatialService AudioSpatialServiceInstance;

namespace
{
    void OnEnginePause()
    {
        AudioBackend::SetVolume(0.0f);
#if COMPILE_WITH_AUDIO_EVENTS
        AudioEventSystem::SetPaused(true);
#endif
    }

    void OnEngineUnpause()
    {
        AudioBackend::SetVolume(Volume);
#if COMPILE_WITH_AUDIO_EVENTS
        AudioEventSystem::SetPaused(!Engine::IsPlayMode());
#endif
    }

#if COMPILE_WITH_AUDIO_EVENTS
    bool LoadConfiguredBank(const JsonAssetReference<AudioBank>& reference, bool preloadSampleData, bool forceBlocking = false)
    {
        if (!reference)
            return true;
        reference->WaitForLoaded();
        const AudioBank* bank = reference->GetInstance<AudioBank>();
        if (!bank)
        {
            LOG(Error, "Configured startup audio bank '{0}' is not a valid AudioBank asset.", reference->GetPath());
            return false;
        }
        AudioEventCatalog::RegisterBank(bank);
        if (!AudioEventSystem::LoadBank(bank->BackendId, bank->Path, forceBlocking ? false : bank->NonBlocking))
        {
            LOG(Error, "Failed to load configured audio bank '{0}' ({1}).", reference->GetPath(), bank->Path);
            return false;
        }
        if (preloadSampleData && bank->BackendId.IsValid() && !AudioEventSystem::LoadBankSampleData(bank->BackendId))
        {
            LOG(Error, "Failed to preload sample data for audio bank '{0}'.", reference->GetPath());
            return false;
        }
        return true;
    }
#endif
}

void AudioSettings::Apply()
{
    ::MuteOnFocusLoss = MuteOnFocusLoss;
    if (AudioBackend::Instance != nullptr)
    {
        Audio::SetDopplerFactor(DopplerFactor);
        Audio::SetEnableHRTF(EnableHRTF);
    }
#if COMPILE_WITH_AUDIO_EVENTS
    AudioEventSystem::SetDopplerFactor(DopplerFactor);
#endif
}

AudioDevice* Audio::GetActiveDevice()
{
    return ActiveDeviceIndex >= 0 && ActiveDeviceIndex < Devices.Count() ? &Devices[ActiveDeviceIndex] : nullptr;
}

int32 Audio::GetActiveDeviceIndex()
{
    return ActiveDeviceIndex;
}

void Audio::SetActiveDeviceIndex(int32 index)
{
    index = Math::Clamp(index, -1, Devices.Count() - 1);
    if (ActiveDeviceIndex == index)
        return;

    ActiveDeviceIndex = index;

    const bool eventOwnsOutput = AudioSettings::Get()->OutputOwner == AudioOutputOwner::EventBackend && AudioEventSystem::GetBackend() && AudioEventSystem::GetBackend()->GetType() != AudioEventBackendType::None;
    if (!eventOwnsOutput)
        AudioBackend::OnActiveDeviceChanged();
#if COMPILE_WITH_AUDIO_EVENTS
    if (eventOwnsOutput)
        AudioEventSystem::GetBackend()->OnActiveDeviceChanged();
#endif

    ActiveDeviceChanged();
}

float Audio::GetMasterVolume()
{
    return MasterVolume;
}

void Audio::SetMasterVolume(float value)
{
    MasterVolume = Math::Saturate(value);
#if COMPILE_WITH_AUDIO_EVENTS
    AudioEventSystem::SetMasterVolume(MasterVolume);
#endif
}

float Audio::GetVolume()
{
    return Volume;
}

void Audio::SetDopplerFactor(float value)
{
    value = Math::Max(0.0f, value);
    AudioBackend::SetDopplerFactor(value);
#if COMPILE_WITH_AUDIO_EVENTS
    AudioEventSystem::SetDopplerFactor(value);
#endif
}

bool Audio::GetEnableHRTF()
{
    return EnableHRTF;
}

void Audio::SetEnableHRTF(bool value)
{
    if (EnableHRTF == value)
        return;
    if (value && EnumHasNoneFlags(AudioBackend::Features(), AudioBackend::FeatureFlags::HRTF))
    {
        LOG(Warning, "HRTF audio is not supported.");
        return;
    }
    EnableHRTF = value;
    AudioBackend::Listener::ReinitializeAll();
}

bool AudioService::Init()
{
    PROFILE_CPU_NAMED("Audio.Init");
    PROFILE_MEM(Audio);
    const auto settings = AudioSettings::Get();
    const bool muteAll = CommandLine::Options.Mute.IsTrue() || settings->DisableAudio;
    bool eventBackendRequested = false;
#if AUDIO_EVENT_API_FMOD
    eventBackendRequested = !muteAll && settings->EventBackend == AudioEventBackendType::FMODStudio;
#endif

#if COMPILE_WITH_AUDIO_EVENTS
    // Studio must be initialized before selecting a shared FMOD Core AudioClip
    // backend. This guarantees a single FMOD system and one physical output.
    IAudioEventBackend* eventBackend = nullptr;
#if AUDIO_EVENT_API_FMOD
    if (eventBackendRequested)
        eventBackend = New<FmodEventBackend>();
#endif
    if (!eventBackend)
        eventBackend = New<AudioEventBackendNone>();
    if (eventBackend->Init())
    {
        LOG(Warning, "Failed to initialize audio event backend '{0}'. Falling back to Null.", eventBackend->GetName());
        Delete(eventBackend);
        eventBackend = New<AudioEventBackendNone>();
        if (eventBackend->Init())
            LOG(Error, "Failed to initialize the Null audio event backend.");
    }
    AudioEventSystem::SetBackend(eventBackend);
#endif

    bool enableNativeClips = !muteAll && settings->NativeClips == NativeAudioClipMode::Enabled;
    if (!muteAll && settings->NativeClips == NativeAudioClipMode::DisabledWhenEventBackendActive)
        enableNativeClips = !eventBackendRequested;

    if (settings->OutputOwner == AudioOutputOwner::EventBackend && !eventBackendRequested && !muteAll)
        LOG(Warning, "Audio output owner is set to Event Backend, but the selected event backend is unavailable. Falling back to the native clip backend when enabled.");
    if (eventBackendRequested && enableNativeClips && settings->OutputOwner != AudioOutputOwner::EventBackend)
        LOG(Warning, "Native clips and the FMOD event backend are both enabled with native output ownership. This migration mode opens two mixer/device paths.");

    // Pick a backend to use
    AudioBackend* backend = nullptr;
#if AUDIO_EVENT_API_FMOD
    if (enableNativeClips && settings->OutputOwner == AudioOutputOwner::EventBackend && eventBackend && eventBackend->GetType() == AudioEventBackendType::FMODStudio)
        backend = New<AudioBackendFMODCore>(static_cast<FmodEventBackend*>(eventBackend)->GetCoreSystem());
#endif
#if AUDIO_API_NONE
    if (!enableNativeClips)
        backend = New<AudioBackendNone>();
#endif
#if AUDIO_API_PS4
    if (!backend)
        backend = New<AudioBackendPS4>();
#endif
#if AUDIO_API_PS5
    if (!backend)
        backend = New<AudioBackendPS5>();
#endif
#if AUDIO_API_SWITCH
    if (!backend)
        backend = New<AudioBackendSwitch>();
#endif
#if AUDIO_API_OPENAL
    if (!backend)
        backend = New<AudioBackendOAL>();
#endif
#if AUDIO_API_XAUDIO2
	if (!backend)
		backend = New<AudioBackendXAudio2>();
#endif
#if AUDIO_API_NONE
    if (!backend)
        backend = New<AudioBackendNone>();
#else
    if (!enableNativeClips)
        LOG(Warning, "Cannot disable the native clip backend because the Null Audio Backend is unavailable on this platform.");
#endif
    if (backend == nullptr)
    {
        LOG(Error, "Failed to create audio backend.");
        return true;
    }
    AudioBackend::Instance = backend;

    LOG(Info, "Audio system initialization... (backend: {0})", AudioBackend::Name());

    EnableHRTF = settings->EnableHRTF;
    if (AudioBackend::Init())
    {
        LOG(Warning, "Failed to initialize audio backend.");
    }
    if (EnableHRTF && EnumHasNoneFlags(AudioBackend::Features(), AudioBackend::FeatureFlags::HRTF))
    {
        LOG(Warning, "HRTF audio is not supported.");
        EnableHRTF = false;
    }

#if COMPILE_WITH_AUDIO_EVENTS
    if (settings->OutputOwner == AudioOutputOwner::EventBackend && eventBackend->GetType() != AudioEventBackendType::None)
    {
        Array<AudioOutputDeviceInfo> outputDevices;
        eventBackend->EnumerateOutputDevices(outputDevices);
        Audio::Devices.Resize(outputDevices.Count());
        for (int32 i = 0; i < outputDevices.Count(); i++)
        {
            auto& destination = Audio::Devices[i];
            const auto& source = outputDevices[i];
            destination.Name = source.Name;
            destination.InternalName = StringAnsi(source.StableId);
            destination.BackendName = StringAnsi(eventBackend->GetName());
            destination.BackendIndex = i;
        }
        ActiveDeviceIndex = Audio::Devices.HasItems() ? 0 : -1;
        if (ActiveDeviceIndex >= 0)
            eventBackend->SetOutputDevice(String(Audio::Devices[ActiveDeviceIndex].InternalName));
        Audio::DevicesChanged();
    }
    // Deterministic bank initialization: strings, master, then authored startup order.
    if (eventBackend->GetType() != AudioEventBackendType::None)
    {
        AudioEventCatalog::Clear();
        if (settings->MasterStringsBank && !settings->MasterStringsBank->WaitForLoaded()) AudioEventCatalog::RegisterBank(settings->MasterStringsBank->GetInstance<AudioBank>());
        if (settings->MasterBank && !settings->MasterBank->WaitForLoaded()) AudioEventCatalog::RegisterBank(settings->MasterBank->GetInstance<AudioBank>());
        for (const auto& bank : settings->StartupBanks)
            if (bank && !bank->WaitForLoaded()) AudioEventCatalog::RegisterBank(bank->GetInstance<AudioBank>());
        LoadConfiguredBank(settings->MasterStringsBank, false, true);
        LoadConfiguredBank(settings->MasterBank, false, true);
        for (const auto& bank : settings->StartupBanks)
            // Startup banks are part of the scene's runtime contract. Complete
            // their metadata load before BeginPlay so emitters cannot race an
            // FMOD non-blocking load during their first OnEnable/OnBeginPlay.
            LoadConfiguredBank(bank, settings->PreloadStartupBankSampleData, true);
    }
    AudioSurfaceLibrary* surfaceLibrary = nullptr;
    if (settings->SurfaceLibrary && !settings->SurfaceLibrary->WaitForLoaded())
        surfaceLibrary = settings->SurfaceLibrary->GetInstance<AudioSurfaceLibrary>();
    AudioWorld::SetSurfaceLibrary(surfaceLibrary);
    _wasPlayMode = Engine::IsPlayMode();
    AudioEventSystem::SetPaused(!_wasPlayMode);
    LOG(Info, "Audio event system initialization... (backend: {0})", AudioEventSystem::GetBackendName());
#endif

    Engine::Pause.Bind(&OnEnginePause);
    Engine::Unpause.Bind(&OnEngineUnpause);

    return false;
}

void AudioService::Update()
{
    PROFILE_CPU_NAMED("Audio.Update");
    PROFILE_MEM(Audio);

    // Update the master volume
    float masterVolume = MasterVolume;
    if (MuteOnFocusLoss && !Engine::HasFocus)
    {
        // Mute audio if app has no user focus
        masterVolume = 0.0f;
    }
    if (Math::NotNearEqual(Volume, masterVolume))
    {
        Volume = masterVolume;
        AudioBackend::SetVolume(masterVolume);
#if COMPILE_WITH_AUDIO_EVENTS
        AudioEventSystem::SetMasterVolume(masterVolume);
#endif
    }

#if COMPILE_WITH_AUDIO_EVENTS
    const bool playMode = Engine::IsPlayMode();
    if (playMode != _wasPlayMode)
    {
        if (!playMode)
        {
            AudioEventSystem::StopAll(AudioStopMode::Immediate);
            AudioEventSystem::SetPaused(true);
            AudioEventSystem::UnloadAllBanks();
        }
        else
        {
            const auto settings = AudioSettings::Get();
            LoadConfiguredBank(settings->MasterStringsBank, false, true);
            LoadConfiguredBank(settings->MasterBank, false, true);
            for (const auto& bank : settings->StartupBanks)
                LoadConfiguredBank(bank, settings->PreloadStartupBankSampleData, true);
            AudioEventSystem::SetPaused(false);
        }
        _wasPlayMode = playMode;
    }
#endif

}

void AudioSpatialService::LateUpdate()
{
    PROFILE_CPU_NAMED("Audio.SpatialUpdate");
    PROFILE_MEM(Audio);

    // Native backends also consume listener/source state here (for example,
    // XAudio2 calculates its spatial mix during this call).
    AudioBackend::Update();

#if COMPILE_WITH_AUDIO_EVENTS
    if (AudioEventSystem::GetBackend())
    {
        Array<AudioListenerState, InlinedAllocation<AUDIO_MAX_LISTENERS>> listenerStates;
        const bool playMode = Engine::IsPlayMode();
        const float dt = (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
        if (playMode)
        {
            for (int32 i = 0; i < Audio::Listeners.Count(); i++)
            {
                auto* listener = Audio::Listeners[i];
                if (listener && listener->IsActiveInHierarchy() && listener->IsDuringPlay())
                {
                    Audio3DAttributes attrs(listener->GetAttenuationPosition(), listener->GetVelocity(), listener->GetForward(), listener->GetTransform().GetUp());
                    listenerStates.Add(AudioListenerState(listener->GetID(), attrs, listener->ListenerWeight, listener->ListenerIndex));
                }
            }
            AudioEventSystem::GetBackend()->UpdateListeners(Span<AudioListenerState>(listenerStates.Get(), listenerStates.Count()));

            AudioWorld::Update(dt);
        }
        AudioEventSystem::GetBackend()->Update(dt);
    }
#endif
}

void AudioService::Dispose()
{
    ASSERT(Audio::Sources.IsEmpty() && Audio::Listeners.IsEmpty());

    // Dispose the native backend first because FMOD Core may share the Studio
    // system and must release all sounds/channels before Studio is released.
    Audio::Devices.Resize(0);
    if (AudioBackend::Instance)
    {
        AudioBackend::Dispose();
        Delete(AudioBackend::Instance);
        AudioBackend::Instance = nullptr;
    }
#if COMPILE_WITH_AUDIO_EVENTS
    if (AudioEventSystem::GetBackend())
    {
        AudioEventSystem::GetBackend()->Dispose();
        Delete(AudioEventSystem::GetBackend());
        AudioEventSystem::SetBackend(nullptr);
    }
#endif
    ActiveDeviceIndex = -1;
    AudioWorld::SetSurfaceLibrary(nullptr);
    AudioWorld::GetSurfaceInteractions().Clear();
    AudioEventCatalog::Clear();
}
