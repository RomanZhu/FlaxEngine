// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioEventSystem.h"
#include "AudioEventCatalog.h"
#include "Assets/AudioEvent.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Level/Actor.h"
#include "Engine/Scripting/ScriptingObjectReference.h"

namespace
{
    struct TrackedAudioEventInstance
    {
        Guid EventAssetId = Guid::Empty;
        Guid OwnerId = Guid::Empty;
        ScriptingObjectReference<Actor> Owner;
        AudioEventHandle Handle;
        Vector3 PreviousPosition = Vector3::Zero;
        bool FollowOwner = false;
    };

    Array<TrackedAudioEventInstance> TrackedInstances;

    int32 FindTrackedInstance(const AudioEvent* audioEvent, const Actor* owner)
    {
        if (!audioEvent || !owner)
            return -1;
        const Guid eventAssetId = audioEvent->GetID();
        const Guid ownerId = owner->GetID();
        for (int32 i = 0; i < TrackedInstances.Count(); i++)
        {
            const auto& instance = TrackedInstances[i];
            if (instance.FollowOwner && instance.EventAssetId == eventAssetId && instance.OwnerId == ownerId)
                return i;
        }
        return -1;
    }

    void AddInitialParameters(AudioEventCreateOptions& options, const Span<AudioParameterValue>& initialParameters)
    {
        if (initialParameters.Length() != 0)
            options.InitialParameters.Add(initialParameters.Get(), initialParameters.Length());
    }
}

IAudioEventBackend* AudioEventSystem::_backend = nullptr;
Delegate<const AudioEventCallback&> AudioEventSystem::EventCallback;

IAudioEventBackend* AudioEventSystem::GetBackend()
{
    return _backend;
}

void AudioEventSystem::SetBackend(IAudioEventBackend* backend)
{
    if (_backend != backend)
        TrackedInstances.Clear();
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

AudioEventHandle AudioEventSystem::Play(AudioEvent* audioEvent, Actor* owner)
{
    return Play(audioEvent, owner, Span<AudioParameterValue>());
}

AudioEventHandle AudioEventSystem::Play(AudioEvent* audioEvent, Actor* owner, const Array<AudioParameterValue>& initialParameters)
{
    return Play(audioEvent, owner, Span<AudioParameterValue>(initialParameters.Get(), initialParameters.Count()));
}

AudioEventHandle AudioEventSystem::Play(AudioEvent* audioEvent, Actor* owner, const Span<AudioParameterValue>& initialParameters)
{
    if (!audioEvent || !owner || !owner->IsDuringPlay() || !Engine::IsPlayMode())
        return AudioEventHandle();

    const int32 existingIndex = FindTrackedInstance(audioEvent, owner);
    if (existingIndex != -1)
    {
        auto& existing = TrackedInstances[existingIndex];
        AudioEventInstanceState state;
        if (QueryInstance(existing.Handle, state))
        {
            const Vector3 position = owner->GetPosition();
            existing.PreviousPosition = position;
            Set3DAttributes(existing.Handle, Audio3DAttributes(owner->GetTransform()));
            if (initialParameters.Length() == 0 || SetParameters(existing.Handle, initialParameters, true))
            {
                if (AudioEventSystem::Play(existing.Handle))
                    return existing.Handle;
            }
            const AudioEventHandle staleHandle = existing.Handle;
            TrackedInstances.RemoveAt(existingIndex);
            StopAndRelease(staleHandle, AudioStopMode::Immediate);
        }
        else
        {
            TrackedInstances.RemoveAt(existingIndex);
        }
    }

    AudioEventCreateOptions options;
    options.Attributes = Audio3DAttributes(owner->GetTransform());
    options.OwnerId = owner->GetID();
    AddInitialParameters(options, initialParameters);
    const AudioEventHandle handle = CreateInstanceFromAsset(audioEvent, options);
    if (!handle.IsValid())
        return AudioEventHandle();
    if (!AudioEventSystem::Play(handle))
    {
        ReleaseInstance(handle);
        return AudioEventHandle();
    }

    TrackedAudioEventInstance instance;
    instance.EventAssetId = audioEvent->GetID();
    instance.OwnerId = owner->GetID();
    instance.Owner = owner;
    instance.Handle = handle;
    instance.PreviousPosition = owner->GetPosition();
    instance.FollowOwner = true;
    TrackedInstances.Add(MoveTemp(instance));
    return handle;
}

AudioEventHandle AudioEventSystem::PlayAt(AudioEvent* audioEvent, const Vector3& position)
{
    return PlayAt(audioEvent, position, Span<AudioParameterValue>());
}

AudioEventHandle AudioEventSystem::PlayAt(AudioEvent* audioEvent, const Vector3& position, const Array<AudioParameterValue>& initialParameters)
{
    return PlayAt(audioEvent, position, Span<AudioParameterValue>(initialParameters.Get(), initialParameters.Count()));
}

AudioEventHandle AudioEventSystem::PlayAt(AudioEvent* audioEvent, const Vector3& position, const Span<AudioParameterValue>& initialParameters)
{
    if (!audioEvent || !Engine::IsPlayMode())
        return AudioEventHandle();

    AudioEventCreateOptions options;
    options.Attributes = Audio3DAttributes(position, Vector3::Zero, Vector3::Forward, Vector3::Up);
    AddInitialParameters(options, initialParameters);
    const AudioEventHandle handle = CreateInstanceFromAsset(audioEvent, options);
    if (!handle.IsValid())
        return AudioEventHandle();
    if (!AudioEventSystem::Play(handle))
    {
        ReleaseInstance(handle);
        return AudioEventHandle();
    }

    TrackedAudioEventInstance instance;
    instance.EventAssetId = audioEvent->GetID();
    instance.Handle = handle;
    instance.PreviousPosition = position;
    TrackedInstances.Add(MoveTemp(instance));
    return handle;
}

#if USE_EDITOR
AudioEventHandle AudioEventSystem::CreatePreviewInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    // Edit mode deliberately suspends and unloads the runtime bank set. A preview
    // is an explicit request to wake the backend; otherwise FMOD can report the
    // instance as Playing while the global Studio system remains paused.
    if (_backend && !Engine::IsPlayMode())
        _backend->SetPaused(false);
    return _backend ? _backend->CreateInstance(eventId, path, options) : AudioEventHandle();
}

bool AudioEventSystem::PlayPreview(AudioEventHandle handle)
{
    return _backend ? _backend->Play(handle) : false;
}

void AudioEventSystem::SetPreviewListener(const Audio3DAttributes& attributes)
{
    if (!_backend || Engine::IsPlayMode())
        return;
    const AudioListenerState listener(Guid::Empty, attributes, 1.0f, 0);
    _backend->UpdateListeners(Span<AudioListenerState>(&listener, 1));
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

bool AudioEventSystem::Stop(AudioEvent* audioEvent, Actor* owner, AudioStopMode stopMode)
{
    const int32 index = FindTrackedInstance(audioEvent, owner);
    if (index == -1)
        return false;
    const AudioEventHandle handle = TrackedInstances[index].Handle;
    TrackedInstances.RemoveAt(index);
    return StopAndRelease(handle, stopMode);
}

bool AudioEventSystem::StopAll(AudioStopMode stopMode)
{
    const bool result = _backend ? _backend->StopAll(stopMode) : false;
    TrackedInstances.Clear();
    return result;
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

bool AudioEventSystem::GetEventParameters(const Guid& eventId, const StringView& eventPath, Array<AudioParameterDescription>& result)
{
    result.Clear();
    return _backend ? _backend->GetEventParameters(eventId, eventPath, result) : false;
}

bool AudioEventSystem::SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetParameter(handle, id, value, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetParameter(AudioEvent* audioEvent, Actor* owner, const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    const int32 index = FindTrackedInstance(audioEvent, owner);
    return index != -1 && SetParameter(TrackedInstances[index].Handle, id, value, ignoreSeekSpeed);
}

bool AudioEventSystem::SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed)
{
    return _backend ? _backend->SetParameters(handle, values, ignoreSeekSpeed) : false;
}

bool AudioEventSystem::SetParameters(AudioEventHandle handle, const Array<AudioParameterValue>& values, bool ignoreSeekSpeed)
{
    return SetParameters(handle, Span<AudioParameterValue>(values.Get(), values.Count()), ignoreSeekSpeed);
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

void AudioEventSystem::UpdateTrackedInstances(float dt)
{
    for (int32 i = TrackedInstances.Count() - 1; i >= 0; i--)
    {
        auto& instance = TrackedInstances[i];
        AudioEventInstanceState state;
        if (!QueryInstance(instance.Handle, state))
        {
            TrackedInstances.RemoveAt(i);
            continue;
        }
        if (state.PlaybackState == AudioEventPlaybackState::Stopped)
        {
            ReleaseInstance(instance.Handle);
            TrackedInstances.RemoveAt(i);
            continue;
        }
        if (!instance.FollowOwner)
            continue;

        Actor* owner = instance.Owner;
        if (!owner || !owner->IsDuringPlay())
        {
            const AudioEventHandle handle = instance.Handle;
            TrackedInstances.RemoveAt(i);
            StopAndRelease(handle, AudioStopMode::Immediate);
            continue;
        }

        const Vector3 position = owner->GetPosition();
        Vector3 velocity = Vector3::Zero;
        if (dt > 0.00001f)
        {
            velocity = (position - instance.PreviousPosition) / dt;
            const float maxVelocity = 10000.0f;
            const float velocityLength = (float)velocity.Length();
            if (velocityLength > maxVelocity)
                velocity *= maxVelocity / velocityLength;
            if (velocity.IsNanOrInfinity())
                velocity = Vector3::Zero;
        }
        instance.PreviousPosition = position;
        if (!Set3DAttributes(instance.Handle, Audio3DAttributes(owner->GetTransform(), velocity)))
        {
            const AudioEventHandle handle = instance.Handle;
            TrackedInstances.RemoveAt(i);
            StopAndRelease(handle, AudioStopMode::Immediate);
        }
    }
}
