// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodEventBackend.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Math/Math.h"

FmodEventBackend::FmodEventBackend()
{
}

FmodEventBackend::~FmodEventBackend()
{
    Dispose();
}

const Char* FmodEventBackend::GetName() const
{
    return TEXT("FMOD Studio");
}

AudioEventBackendType FmodEventBackend::GetType() const
{
    return AudioEventBackendType::FMODStudio;
}

bool FmodEventBackend::Init()
{
    if (_studioSystem)
        return false;

    FMOD_RESULT result = FMOD::Studio::System::create(&_studioSystem);
    if (!FmodConvert::CheckResult(result, "Studio::System::create") || !_studioSystem)
    {
        _studioSystem = nullptr;
        return true;
    }

    result = _studioSystem->getCoreSystem(&_coreSystem);
    if (!FmodConvert::CheckResult(result, "getCoreSystem") || !_coreSystem)
    {
        Dispose();
        return true;
    }

    // Configure 3D settings with 0.01 distance factor for Flax centimeters
    _coreSystem->set3DSettings(1.0f, 0.01f, 1.0f);

    FMOD_STUDIO_INITFLAGS studioFlags = FMOD_STUDIO_INIT_NORMAL;
#if USE_EDITOR || !BUILD_RELEASE
    studioFlags |= FMOD_STUDIO_INIT_LIVEUPDATE;
#endif

    result = _studioSystem->initialize(256, studioFlags, FMOD_INIT_NORMAL, nullptr);
    if (!FmodConvert::CheckResult(result, "Studio::System::initialize"))
    {
        Dispose();
        return true;
    }

    _banks.Init(_studioSystem);
    _handles.Clear();
    _callbacks.Clear();

    return false;
}

void FmodEventBackend::Update(float dt)
{
    if (!_studioSystem)
        return;

    // Drain callback queue
    FmodCallbackRecord record;
    while (_callbacks.Dequeue(record))
    {
        // Dispatched on game thread
    }

    _studioSystem->update();
}

void FmodEventBackend::Dispose()
{
    if (_studioSystem)
    {
        StopAll(AudioStopMode::Immediate);
        _banks.Dispose();
        _handles.Clear();
        _callbacks.Clear();

        _studioSystem->unloadAll();
        _studioSystem->release();
        _studioSystem = nullptr;
        _coreSystem = nullptr;
    }
}

void FmodEventBackend::SetMasterVolume(float volume)
{
    _masterVolume = Math::Saturate(volume);
    if (_studioSystem)
    {
        FMOD::Studio::Bus* masterBus = nullptr;
        if (_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
        {
            masterBus->setVolume(_masterVolume);
        }
    }
}

void FmodEventBackend::SetMasterPitch(float pitch)
{
    _masterPitch = Math::Clamp(pitch, 0.0f, 10.0f);
    if (_coreSystem)
    {
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (_coreSystem->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup)
        {
            masterGroup->setPitch(_masterPitch);
        }
    }
}

void FmodEventBackend::SetPaused(bool paused)
{
    _isPaused = paused;
    if (_studioSystem)
    {
        FMOD::Studio::Bus* masterBus = nullptr;
        if (_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
        {
            masterBus->setPaused(_isPaused);
        }
    }
}

void FmodEventBackend::SetMuted(bool muted)
{
    _isMuted = muted;
    if (_studioSystem)
    {
        FMOD::Studio::Bus* masterBus = nullptr;
        if (_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
        {
            masterBus->setMute(_isMuted);
        }
    }
}

void FmodEventBackend::SetDopplerFactor(float factor)
{
    if (_coreSystem)
    {
        float doppler, distance, rolloff;
        if (_coreSystem->get3DSettings(&doppler, &distance, &rolloff) == FMOD_OK)
        {
            _coreSystem->set3DSettings(Math::Max(0.0f, factor), distance, rolloff);
        }
    }
}

void FmodEventBackend::SetDistanceFactor(float factor)
{
    if (_coreSystem)
    {
        float doppler, distance, rolloff;
        if (_coreSystem->get3DSettings(&doppler, &distance, &rolloff) == FMOD_OK)
        {
            _coreSystem->set3DSettings(doppler, Math::Max(0.0001f, factor), rolloff);
        }
    }
}

void FmodEventBackend::OnActiveDeviceChanged()
{
}

void FmodEventBackend::UpdateListeners(const Span<AudioListenerState>& listeners)
{
    if (!_studioSystem)
        return;

    if (listeners.Length() == 0)
    {
        // FMOD requires at least one listener. Reset the retained listener and mute
        // its contribution so disabling the last Flax listener cannot leave a stale
        // camera transform driving 3D spatialization.
        _studioSystem->setNumListeners(1);
        const FMOD_3D_ATTRIBUTES attributes = FmodConvert::ToFmodAttributes(Audio3DAttributes());
        _studioSystem->setListenerAttributes(0, &attributes);
        _studioSystem->setListenerWeight(0, 0.0f);
        return;
    }

    int32 count = Math::Min(listeners.Length(), (int32)FMOD_MAX_LISTENERS);
    _studioSystem->setNumListeners(Math::Max(1, count));

    for (int32 i = 0; i < count; i++)
    {
        const auto& l = listeners[i];
        FMOD_3D_ATTRIBUTES attrs = FmodConvert::ToFmodAttributes(l.Attributes);
        _studioSystem->setListenerAttributes(i, &attrs);
        _studioSystem->setListenerWeight(i, Math::Saturate(l.Weight));
    }
}

bool FmodEventBackend::LoadBank(const Guid& bankId, const StringView& path, bool nonBlocking)
{
    return _banks.Load(bankId, path, nonBlocking);
}

bool FmodEventBackend::UnloadBank(const Guid& bankId, const StringView& path)
{
    return _banks.Unload(bankId, path);
}

bool FmodEventBackend::UnloadAllBanks()
{
    return _banks.UnloadAll();
}

bool FmodEventBackend::IsBankLoaded(const Guid& bankId) const
{
    return _banks.IsLoaded(bankId);
}

FMOD::Studio::EventDescription* FmodEventBackend::GetEventDescription(const Guid& eventId, const StringView& path)
{
    if (!_studioSystem)
        return nullptr;

    FMOD::Studio::EventDescription* desc = nullptr;
    if (eventId.IsValid())
    {
        FMOD_GUID fg = FmodConvert::ToFmodGuid(eventId);
        if (_studioSystem->getEventByID(&fg, &desc) == FMOD_OK && desc)
            return desc;
    }

    if (path.HasChars())
    {
        StringAnsi pathAnsi(path);
        if (_studioSystem->getEvent(pathAnsi.Get(), &desc) == FMOD_OK && desc)
            return desc;
    }

    return nullptr;
}

FMOD::Studio::Bus* FmodEventBackend::GetBus(const Guid& busId, const StringView& path)
{
    if (!_studioSystem)
        return nullptr;

    FMOD::Studio::Bus* bus = nullptr;
    if (busId.IsValid())
    {
        FMOD_GUID fg = FmodConvert::ToFmodGuid(busId);
        if (_studioSystem->getBusByID(&fg, &bus) == FMOD_OK && bus)
            return bus;
    }

    if (path.HasChars())
    {
        StringAnsi pathAnsi(path);
        if (_studioSystem->getBus(pathAnsi.Get(), &bus) == FMOD_OK && bus)
            return bus;
    }

    return nullptr;
}

FMOD::Studio::VCA* FmodEventBackend::GetVCA(const Guid& vcaId, const StringView& path)
{
    if (!_studioSystem)
        return nullptr;

    FMOD::Studio::VCA* vca = nullptr;
    if (vcaId.IsValid())
    {
        FMOD_GUID fg = FmodConvert::ToFmodGuid(vcaId);
        if (_studioSystem->getVCAByID(&fg, &vca) == FMOD_OK && vca)
            return vca;
    }

    if (path.HasChars())
    {
        StringAnsi pathAnsi(path);
        if (_studioSystem->getVCA(pathAnsi.Get(), &vca) == FMOD_OK && vca)
            return vca;
    }

    return nullptr;
}

AudioEventHandle FmodEventBackend::CreateInstance(const Guid& eventId, const StringView& path, const AudioEventCreateOptions& options)
{
    auto* desc = GetEventDescription(eventId, path);
    if (!desc)
        return AudioEventHandle();

    FMOD::Studio::EventInstance* inst = nullptr;
    if (desc->createInstance(&inst) != FMOD_OK || !inst)
        return AudioEventHandle();

    AudioEventHandle handle = _handles.Allocate(inst, eventId, options.OwnerId);

    // Apply attributes
    FMOD_3D_ATTRIBUTES attrs = FmodConvert::ToFmodAttributes(options.Attributes);
    inst->set3DAttributes(&attrs);
    inst->setListenerMask(options.ListenerMask);

    // Hook callback
    inst->setCallback(&OnEventCallback, FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER | FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_BEAT | FMOD_STUDIO_EVENT_CALLBACK_STOPPED);

    if (options.AutoPlay)
        inst->start();

    return handle;
}

bool FmodEventBackend::Play(AudioEventHandle handle)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->start() == FMOD_OK : false;
}

bool FmodEventBackend::Pause(AudioEventHandle handle)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->setPaused(true) == FMOD_OK : false;
}

bool FmodEventBackend::Stop(AudioEventHandle handle, AudioStopMode stopMode)
{
    auto* inst = _handles.Get(handle);
    if (!inst)
        return false;

    FMOD_STUDIO_STOP_MODE sm = (stopMode == AudioStopMode::Immediate) ? FMOD_STUDIO_STOP_IMMEDIATE : FMOD_STUDIO_STOP_ALLOWFADEOUT;
    return inst->stop(sm) == FMOD_OK;
}

bool FmodEventBackend::StopAll(AudioStopMode stopMode)
{
    const FMOD_STUDIO_STOP_MODE sm = (stopMode == AudioStopMode::Immediate) ? FMOD_STUDIO_STOP_IMMEDIATE : FMOD_STUDIO_STOP_ALLOWFADEOUT;

    // Stopping the master bus also catches one-shots that were released immediately after start.
    if (_studioSystem)
    {
        FMOD::Studio::Bus* masterBus = nullptr;
        if (_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
            masterBus->stopAllEvents(sm);
    }

    // Explicitly stop, release, and invalidate every handle owned by the registry.
    const auto& slots = _handles.GetSlots();
    for (int32 i = 0; i < slots.Count(); i++)
    {
        if (!slots[i].InUse)
            continue;

        const AudioEventHandle handle((uint32)i, slots[i].Generation);
        if (auto* inst = _handles.Get(handle))
            inst->stop(sm);

        FMOD::Studio::EventInstance* instance = nullptr;
        if (_handles.Free(handle, instance) && instance)
            instance->release();
    }

    return true;
}

bool FmodEventBackend::ReleaseInstance(AudioEventHandle handle)
{
    FMOD::Studio::EventInstance* inst = nullptr;
    if (_handles.Free(handle, inst) && inst)
    {
        inst->release();
        return true;
    }
    return false;
}

bool FmodEventBackend::PlayOneShot(const Guid& eventId, const StringView& path, const Audio3DAttributes& attributes, float volume, float pitch)
{
    auto* desc = GetEventDescription(eventId, path);
    if (!desc)
        return false;

    FMOD::Studio::EventInstance* inst = nullptr;
    if (desc->createInstance(&inst) != FMOD_OK || !inst)
        return false;

    FMOD_3D_ATTRIBUTES fattrs = FmodConvert::ToFmodAttributes(attributes);
    inst->set3DAttributes(&fattrs);
    inst->setVolume(Math::Saturate(volume));
    inst->setPitch(Math::Clamp(pitch, 0.0f, 10.0f));
    inst->start();
    inst->release();
    return true;
}

bool FmodEventBackend::Set3DAttributes(AudioEventHandle handle, const Audio3DAttributes& attributes)
{
    auto* inst = _handles.Get(handle);
    if (!inst)
        return false;

    FMOD_3D_ATTRIBUTES fa = FmodConvert::ToFmodAttributes(attributes);
    return inst->set3DAttributes(&fa) == FMOD_OK;
}

bool FmodEventBackend::SetVolume(AudioEventHandle handle, float volume)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->setVolume(Math::Saturate(volume)) == FMOD_OK : false;
}

bool FmodEventBackend::SetPitch(AudioEventHandle handle, float pitch)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->setPitch(Math::Clamp(pitch, 0.0f, 10.0f)) == FMOD_OK : false;
}

bool FmodEventBackend::SetTimelinePosition(AudioEventHandle handle, int32 milliseconds)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->setTimelinePosition(Math::Max(0, milliseconds)) == FMOD_OK : false;
}

bool FmodEventBackend::SetListenerMask(AudioEventHandle handle, uint32 listenerMask)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->setListenerMask(listenerMask) == FMOD_OK : false;
}

bool FmodEventBackend::SetParameter(AudioEventHandle handle, const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    auto* inst = _handles.Get(handle);
    if (!inst)
        return false;

    if (id.Name.HasChars())
    {
        StringAnsi nameAnsi(id.Name);
        return inst->setParameterByName(nameAnsi.Get(), value, ignoreSeekSpeed) == FMOD_OK;
    }

    return false;
}

bool FmodEventBackend::SetParameterLabel(AudioEventHandle handle, const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    auto* inst = _handles.Get(handle);
    if (!inst || id.Name.IsEmpty())
        return false;

    StringAnsi nameAnsi(id.Name);
    StringAnsi labelAnsi(label);
    return inst->setParameterByNameWithLabel(nameAnsi.Get(), labelAnsi.Get(), ignoreSeekSpeed) == FMOD_OK;
}

bool FmodEventBackend::SetGlobalParameter(const AudioParameterId& id, float value, bool ignoreSeekSpeed)
{
    if (!_studioSystem)
        return false;

    if (id.Name.HasChars())
    {
        StringAnsi nameAnsi(id.Name);
        return _studioSystem->setParameterByName(nameAnsi.Get(), value, ignoreSeekSpeed) == FMOD_OK;
    }

    return false;
}

bool FmodEventBackend::SetGlobalParameterLabel(const AudioParameterId& id, const StringView& label, bool ignoreSeekSpeed)
{
    if (!_studioSystem || id.Name.IsEmpty())
        return false;

    StringAnsi nameAnsi(id.Name);
    StringAnsi labelAnsi(label);
    return _studioSystem->setParameterByNameWithLabel(nameAnsi.Get(), labelAnsi.Get(), ignoreSeekSpeed) == FMOD_OK;
}

bool FmodEventBackend::QueryInstance(AudioEventHandle handle, AudioEventInstanceState& outState) const
{
    auto* inst = _handles.Get(handle);
    if (!inst)
        return false;

    FMOD_STUDIO_PLAYBACK_STATE pbState;
    if (inst->getPlaybackState(&pbState) == FMOD_OK)
    {
        switch (pbState)
        {
        case FMOD_STUDIO_PLAYBACK_PLAYING:
            outState.PlaybackState = AudioEventPlaybackState::Playing;
            break;
        case FMOD_STUDIO_PLAYBACK_SUSTAINING:
            outState.PlaybackState = AudioEventPlaybackState::Sustaining;
            break;
        case FMOD_STUDIO_PLAYBACK_STOPPED:
            outState.PlaybackState = AudioEventPlaybackState::Stopped;
            break;
        case FMOD_STUDIO_PLAYBACK_STOPPING:
            outState.PlaybackState = AudioEventPlaybackState::Stopping;
            break;
        default:
            outState.PlaybackState = AudioEventPlaybackState::Stopped;
            break;
        }
    }

    inst->getTimelinePosition(&outState.TimelinePosition);
    inst->getPitch(&outState.Pitch);
    inst->getVolume(&outState.Volume);
    inst->getPaused(&outState.IsPaused);
    return true;
}

bool FmodEventBackend::SetSnapshotWeight(AudioEventHandle handle, float weight)
{
    auto* inst = _handles.Get(handle);
    if (!inst)
        return false;

    return inst->setParameterByName("Intensity", Math::Saturate(weight) * 100.0f) == FMOD_OK;
}

bool FmodEventBackend::SetBusVolume(const Guid& busId, const StringView& path, float volume)
{
    auto* bus = GetBus(busId, path);
    return bus ? bus->setVolume(Math::Saturate(volume)) == FMOD_OK : false;
}

bool FmodEventBackend::SetBusMute(const Guid& busId, const StringView& path, bool mute)
{
    auto* bus = GetBus(busId, path);
    return bus ? bus->setMute(mute) == FMOD_OK : false;
}

bool FmodEventBackend::SetBusPaused(const Guid& busId, const StringView& path, bool paused)
{
    auto* bus = GetBus(busId, path);
    return bus ? bus->setPaused(paused) == FMOD_OK : false;
}

bool FmodEventBackend::SetVCAVolume(const Guid& vcaId, const StringView& path, float volume)
{
    auto* vca = GetVCA(vcaId, path);
    return vca ? vca->setVolume(Math::Saturate(volume)) == FMOD_OK : false;
}

void FmodEventBackend::CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot)
{
    outSnapshot = AudioDiagnosticsSnapshot();
    if (_studioSystem)
    {
        FMOD_STUDIO_CPU_USAGE studioCpu;
        FMOD_CPU_USAGE coreCpu;
        _studioSystem->getCPUUsage(&studioCpu, &coreCpu);
        outSnapshot.CpuUsage = studioCpu.update + coreCpu.dsp;

        int32 currentAlloc, maxAlloc;
        FMOD::Memory_GetStats(&currentAlloc, &maxAlloc);
        outSnapshot.MemoryAllocated = (uint64)currentAlloc;
        outSnapshot.LoadedBanks = _banks.GetLoadedCount();
        outSnapshot.ActiveInstances = _handles.GetActiveCount();
    }
}

FMOD_RESULT F_CALL FmodEventBackend::OnEventCallback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters)
{
    // FMOD background/mixer callback
    return FMOD_OK;
}

#endif
