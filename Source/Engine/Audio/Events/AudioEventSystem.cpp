// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEventSystem.h"
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

AudioEventHandle AudioEventSystem::CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    if (!Engine::IsPlayMode())
        return AudioEventHandle();
    return _backend ? _backend->CreateInstance(eventId, path, options) : AudioEventHandle();
}

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

bool AudioEventSystem::PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume, float pitch)
{
    if (!Engine::IsPlayMode())
        return false;
    return _backend ? _backend->PlayOneShot(eventId, path, attributes, volume, pitch) : false;
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

bool AudioEventSystem::SetVCAVolume(const Guid& vcaId, const StringView& path, float volume)
{
    return _backend ? _backend->SetVCAVolume(vcaId, path, volume) : false;
}

void AudioEventSystem::CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot)
{
    if (_backend)
        _backend->CaptureDiagnostics(outSnapshot);
    else
        outSnapshot = AudioDiagnosticsSnapshot();
}
