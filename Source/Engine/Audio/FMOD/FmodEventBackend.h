// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include <fmod.hpp>
#include <fmod_studio.hpp>
#include <atomic>
#include "Engine/Audio/Events/IAudioEventBackend.h"
#include "FmodHandleRegistry.h"
#include "FmodBankRegistry.h"
#include "FmodCallbackQueue.h"

/// <summary>
/// FMOD Studio implementation of the audio event backend.
/// </summary>
class FLAXENGINE_API FmodEventBackend : public IAudioEventBackend
{
private:
    FMOD::Studio::System* _studioSystem = nullptr;
    FMOD::System* _coreSystem = nullptr;
    FmodHandleRegistry _handles;
    FmodBankRegistry _banks;
    FmodCallbackQueue _callbacks;
    Array<FmodInstanceContext*> _callbackContexts;
    Array<FmodInstanceContext*> _retiredCallbackContexts;
    float _oneShotSweepTimer = 0.0f;
    uint64 _totalInstancesCreated = 0;
    uint64 _totalPlays = 0;
    uint64 _totalStopped = 0;
    int32 _peakActiveInstances = 0;

    float _masterVolume = 1.0f;
    float _masterPitch = 1.0f;
    bool _isPaused = false;
    bool _isMuted = false;
    std::atomic<bool> _outputDevicesDirty { false };

public:
    FmodEventBackend();
    ~FmodEventBackend() override;

    // [IAudioEventBackend]
    const Char* GetName() const override;
    AudioEventBackendType GetType() const override;
    bool Init() override;
    void Update(float dt) override;
    void Dispose() override;

    void SetMasterVolume(float volume) override;
    void SetMasterPitch(float pitch) override;
    void SetPaused(bool paused) override;
    void SetMuted(bool muted) override;
    void SetDopplerFactor(float factor) override;
    void SetDistanceFactor(float factor) override;
    void OnActiveDeviceChanged() override;
    void EnumerateOutputDevices(Array<AudioOutputDeviceInfo>& result) const override;
    bool SetOutputDevice(const StringView& stableId) override;
    String GetOutputDevice() const override;

    void UpdateListeners(const Span<AudioListenerState>& listeners) override;

    bool LoadBank(const Guid& bankId, const StringView& path, bool nonBlocking) override;
    bool UnloadBank(const Guid& bankId, const StringView& path) override;
    bool UnloadAllBanks() override;
    bool IsBankLoaded(const Guid& bankId) const override;
    bool LoadBankSampleData(const Guid& bankId) override;
    void UnloadBankSampleData(const Guid& bankId) override;
    AudioBankState GetBankState(const Guid& bankId) const override;
    bool QueryBank(const Guid& bankId, const StringView& path, AudioBankRuntimeState& outState) const override;

    AudioEventHandle CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options) override;
    bool Play(AudioEventHandle handle) override;
    bool Pause(AudioEventHandle handle) override;
    bool KeyOff(AudioEventHandle handle) override;
    bool Stop(AudioEventHandle handle, AudioStopMode stopMode) override;
    bool StopAll(AudioStopMode stopMode) override;
    bool ReleaseInstance(AudioEventHandle handle) override;
    bool PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume = 1.0f, float pitch = 1.0f) override;

    bool Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes) override;
    bool SetVolume(AudioEventHandle handle, float volume) override;
    bool SetPitch(AudioEventHandle handle, float pitch) override;
    bool SetTimelinePosition(AudioEventHandle handle, int32 milliseconds) override;
    bool SetListenerMask(AudioEventHandle handle, uint32 listenerMask) override;
    bool ResolveParameterId(const Guid& eventId, const StringView& eventPath, const StringView& name, AudioParameterId& id) override;
    bool SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) override;
    bool SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed = false) override;
    bool SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) override;
    bool SetProgrammerSound(AudioEventHandle handle, const AudioProgrammerSoundData& data) override;

    bool SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed = false) override;
    bool SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed = false) override;

    bool QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState) const override;
    bool GetParameter(AudioEventHandle handle, const AudioParameterId& id, AudioParameterState& outState) const override;
    bool GetGlobalParameter(const AudioParameterId& id, AudioParameterState& outState) const override;

    bool SetSnapshotWeight(AudioEventHandle handle, float weight) override;
    bool SetBusVolume(const Guid& busId, const StringView& path, float volume) override;
    bool SetBusMute(const Guid& busId, const StringView& path, bool mute) override;
    bool SetBusPaused(const Guid& busId, const StringView& path, bool paused) override;
    bool StopBusEvents(const Guid& busId, const StringView& path, AudioStopMode stopMode) override;
    bool GetBusVolume(const Guid& busId, const StringView& path, float& outVolume, float& outFinalVolume) const override;
    bool GetBusMute(const Guid& busId, const StringView& path, bool& outMuted) const override;
    bool GetBusPaused(const Guid& busId, const StringView& path, bool& outPaused) const override;
    bool SetVCAVolume(const Guid& vcaId, const StringView& path, float volume) override;
    bool GetVCAVolume(const Guid& vcaId, const StringView& path, float& outVolume, float& outFinalVolume) const override;

    void CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot) override;

    /// <summary>
    /// Queues a callback record for main-thread dispatch. Called only by the FMOD callback bridge.
    /// </summary>
    void EnqueueCallback(const FmodCallbackRecord& record);

    FMOD::Studio::System* GetStudioSystem() const { return _studioSystem; }
    FMOD::System* GetCoreSystem() const { return _coreSystem; }

private:
    bool ConfigureInstanceCallback(FMOD::Studio::EventInstance* instance, AudioEventHandle handle);
    void ReleaseCallbackContexts();
    FMOD::Studio::EventDescription* GetEventDescription(const Guid& eventId, const StringView& path);
    FMOD::Studio::Bus* GetBus(const Guid& busId, const StringView& path);
    FMOD::Studio::VCA* GetVCA(const Guid& vcaId, const StringView& path);
    void RefreshOutputDevices();
    static FMOD_RESULT F_CALL OnSystemCallback(FMOD_SYSTEM* system, FMOD_SYSTEM_CALLBACK_TYPE type, void* commandData1, void* commandData2, void* userData);
    static FMOD_RESULT F_CALL OnEventCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters);
};

#endif
