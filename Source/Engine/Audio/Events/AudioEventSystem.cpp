// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEventSystem.h"
#include "AudioEventCatalog.h"
#include "Assets/AudioEvent.h"
#include "Engine/Engine/Engine.h"

IAudioEventBackend* AudioEventSystem::_backend = nullptr;
Delegate<const AudioEventCallback&> AudioEventSystem::EventCallback;

IAudioEventBackend* AudioEventSystem::GetBackend()
{
    return _backend;
}

void AudioEventSystem::SetBackend(IAudioEventBackend* backend)
{
    _backend = backend;
}

void AudioEventSystem::DispatchEventCallback(const AudioEventCallback& callback)
{
    EventCallback(callback);
}

AudioEventBackendType AudioEventSystem::GetBackendType()
{
    return _backend ? _backend->GetType() : AudioEventBackendType::None;
}

String AudioEventSystem::GetBackendName()
{
    return _backend ? String(_backend->GetName()) : String(TEXT("None"));
}

void AudioEventSystem::SetMasterVolume(float volume)
{
    if (_backend)
        _backend->SetMasterVolume(volume);
}

void AudioEventSystem::SetMasterPitch(float pitch)
{
    if (_backend)
        _backend->SetMasterPitch(pitch);
}

void AudioEventSystem::SetPaused(bool paused)
{
    if (_backend)
        _backend->SetPaused(paused);
}

void AudioEventSystem::SetMuted(bool muted)
{
    if (_backend)
        _backend->SetMuted(muted);
}

void AudioEventSystem::SetDopplerFactor(float factor)
{
    if (_backend)
        _backend->SetDopplerFactor(factor);
}

void AudioEventSystem::SetDistanceFactor(float factor)
{
    if (_backend)
        _backend->SetDistanceFactor(factor);
}

bool AudioEventSystem::LoadBank(const Guid& bankId, const StringView& path, bool nonBlocking)
{
    return _backend ? _backend->LoadBank(bankId, path, nonBlocking) : false;
}

bool AudioEventSystem::UnloadBank(const Guid& bankId, const StringView& path)
{
    return _backend ? _backend->UnloadBank(bankId, path) : false;
}

bool AudioEventSystem::UnloadAllBanks()
{
    return _backend ? _backend->UnloadAllBanks() : false;
}

bool AudioEventSystem::IsBankLoaded(const Guid& bankId)
{
    return _backend ? _backend->IsBankLoaded(bankId) : false;
}

bool AudioEventSystem::LoadBankSampleData(const Guid& bankId)
{
    return _backend ? _backend->LoadBankSampleData(bankId) : false;
}

void AudioEventSystem::UnloadBankSampleData(const Guid& bankId)
{
    if (_backend)
        _backend->UnloadBankSampleData(bankId);
}

AudioBankState AudioEventSystem::GetBankState(const Guid& bankId)
{
    return _backend ? _backend->GetBankState(bankId) : AudioBankState::Unloaded;
}

bool AudioEventSystem::QueryBank(const Guid& bankId, const StringView& path, AudioBankRuntimeState& outState)
{
    outState = AudioBankRuntimeState();
    return _backend ? _backend->QueryBank(bankId, path, outState) : false;
}

AudioEventHandle AudioEventSystem::CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    if (!Engine::IsPlayMode())
        return AudioEventHandle();
    return _backend ? _backend->CreateInstance(eventId, path, options) : AudioEventHandle();
}

AudioEventHandle AudioEventSystem::CreateInstanceFromAsset(AudioEvent* audioEvent, const AudioEventCreateOptions& options)
{
    if (!audioEvent || !AudioEventCatalog::EnsureDependenciesLoaded(audioEvent))
        return AudioEventHandle();
    return CreateInstance(audioEvent->BackendId, audioEvent->Path, options);
}

#if USE_EDITOR
AudioEventHandle AudioEventSystem::CreatePreviewInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    return _backend ? _backend->CreateInstance(eventId, path, options) : AudioEventHandle();
}

bool AudioEventSystem::PlayPreview(AudioEventHandle handle)
{
    return _backend ? _backend->Play(handle) : false;
}
#endif

bool AudioEventSystem::Play(AudioEventHandle handle)
{
    if (!Engine::IsPlayMode())
        return false;
    return _backend ? _backend->Play(handle) : false;
}

bool AudioEventSystem::Pause(AudioEventHandle handle)
{
    return _backend ? _backend->Pause(handle) : false;
}

bool AudioEventSystem::KeyOff(AudioEventHandle handle)
{
    return _backend ? _backend->KeyOff(handle) : false;
}

bool AudioEventSystem::Stop(AudioEventHandle handle, AudioStopMode stopMode)
{
    return _backend ? _backend->Stop(handle, stopMode) : false;
}

bool AudioEventSystem::StopAll(AudioStopMode stopMode)
{
    return _backend ? _backend->StopAll(stopMode) : false;
}

bool AudioEventSystem::ReleaseInstance(AudioEventHandle handle)
{
    return _backend ? _backend->ReleaseInstance(handle) : false;
}

bool AudioEventSystem::StopAndRelease(AudioEventHandle handle, AudioStopMode stopMode)
{
    if (!_backend || !handle.IsValid())
        return false;
    const bool stopped = _backend->Stop(handle, stopMode);
    if (stopped)
    {
        AudioEventCallback callback;
        callback.Handle = handle;
        callback.Type = AudioEventCallbackType::Stopped;
        DispatchEventCallback(callback);
    }
    const bool released = _backend->ReleaseInstance(handle);
    return stopped && released;
}

bool AudioEventSystem::PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume, float pitch)
{
    if (!Engine::IsPlayMode())
        return false;
    return _backend ? _backend->PlayOneShot(eventId, path, attributes, volume, pitch) : false;
}

bool AudioEventSystem::PlayOneShotFromAsset(AudioEvent* audioEvent, const Audio3DAttributes& attributes, float volume, float pitch)
{
    return audioEvent && AudioEventCatalog::EnsureDependenciesLoaded(audioEvent) && PlayOneShot(audioEvent->BackendId, audioEvent->Path, attributes, volume, pitch);
}

bool AudioEventSystem::Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes)
{
    return _backend ? _backend->Set3DAttributes(handle, attributes) : false;
}

bool AudioEventSystem::SetVolume(AudioEventHandle handle, float volume)
{
    return _backend ? _backend->SetVolume(handle, volume) : false;
}

bool AudioEventSystem::SetPitch(AudioEventHandle handle, float pitch)
{
    return _backend ? _backend->SetPitch(handle, pitch) : false;
}

bool AudioEventSystem::SetTimelinePosition(AudioEventHandle handle, int32 milliseconds)
{
    return _backend ? _backend->SetTimelinePosition(handle, milliseconds) : false;
}

bool AudioEventSystem::SetListenerMask(AudioEventHandle handle, uint32 listenerMask)
{
    return _backend ? _backend->SetListenerMask(handle, listenerMask) : false;
}

bool AudioEventSystem::ResolveParameterId(const Guid& eventId, const StringView& eventPath, const StringView& name, AudioParameterId& id)
{
    id = AudioParameterId(name);
    return _backend ? _backend->ResolveParameterId(eventId, eventPath, name, id) : false;
}

bool AudioEventSystem::SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetParameter(handle, id, value, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetParameters(handle, values, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetParameterLabel(handle, id, label, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetProgrammerSound(AudioEventHandle handle, const StringView& key, AudioProgrammerSoundProvider* provider)
{
    if (!_backend || !provider)
        return false;
    AudioProgrammerSoundData data;
    return provider->Resolve(key, data) && data.Path.HasChars() && _backend->SetProgrammerSound(handle, data);
}

bool AudioEventSystem::SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetGlobalParameter(id, value, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetGlobalParameterLabel(id, label, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState)
{
    return _backend ? _backend->QueryInstance(handle, outState) : false;
}

bool AudioEventSystem::GetParameter(AudioEventHandle handle, const AudioParameterId& id, AudioParameterState& outState)
{
    outState = AudioParameterState();
    return _backend ? _backend->GetParameter(handle, id, outState) : false;
}

bool AudioEventSystem::GetGlobalParameter(const AudioParameterId& id, AudioParameterState& outState)
{
    outState = AudioParameterState();
    return _backend ? _backend->GetGlobalParameter(id, outState) : false;
}

bool AudioEventSystem::SetSnapshotWeight(AudioEventHandle handle, float weight)
{
    return _backend ? _backend->SetSnapshotWeight(handle, weight) : false;
}

bool AudioEventSystem::SetBusVolume(const Guid& busId, const StringView& path, float volume)
{
    return _backend ? _backend->SetBusVolume(busId, path, volume) : false;
}

bool AudioEventSystem::SetBusMute(const Guid& busId, const StringView& path, bool mute)
{
    return _backend ? _backend->SetBusMute(busId, path, mute) : false;
}

bool AudioEventSystem::SetBusPaused(const Guid& busId, const StringView& path, bool paused)
{
    return _backend ? _backend->SetBusPaused(busId, path, paused) : false;
}

bool AudioEventSystem::StopBusEvents(const Guid& busId, const StringView& path, AudioStopMode stopMode)
{
    return _backend ? _backend->StopBusEvents(busId, path, stopMode) : false;
}

bool AudioEventSystem::GetBusVolume(const Guid& busId, const StringView& path, float& outVolume, float& outFinalVolume)
{
    outVolume = 0.0f;
    outFinalVolume = 0.0f;
    return _backend ? _backend->GetBusVolume(busId, path, outVolume, outFinalVolume) : false;
}

bool AudioEventSystem::GetBusMute(const Guid& busId, const StringView& path, bool& outMuted)
{
    outMuted = false;
    return _backend ? _backend->GetBusMute(busId, path, outMuted) : false;
}

bool AudioEventSystem::GetBusPaused(const Guid& busId, const StringView& path, bool& outPaused)
{
    outPaused = false;
    return _backend ? _backend->GetBusPaused(busId, path, outPaused) : false;
}

bool AudioEventSystem::SetVCAVolume(const Guid& vcaId, const StringView& path, float volume)
{
    return _backend ? _backend->SetVCAVolume(vcaId, path, volume) : false;
}

bool AudioEventSystem::GetVCAVolume(const Guid& vcaId, const StringView& path, float& outVolume, float& outFinalVolume)
{
    outVolume = 0.0f;
    outFinalVolume = 0.0f;
    return _backend ? _backend->GetVCAVolume(vcaId, path, outVolume, outFinalVolume) : false;
}

void AudioEventSystem::CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot)
{
    if (_backend)
        _backend->CaptureDiagnostics(outSnapshot);
    else
        outSnapshot = AudioDiagnosticsSnapshot();
}
