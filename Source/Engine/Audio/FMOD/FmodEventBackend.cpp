// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodEventBackend.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
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
        // A slot can have been released and reused since FMOD emitted the callback.
        // Generation validation is the final guard before exposing it to game code.
        if (!_handles.Validate(record.Handle))
            continue;

        AudioEventCallback callback;
        callback.Handle = record.Handle;
        callback.Type = record.Type;
        callback.TimelinePositionMs = record.TimelinePositionMs;
        callback.Bar = record.Bar;
        callback.Beat = record.Beat;
        callback.Tempo = record.Tempo;
        callback.TimeSignatureUpper = record.TimeSignatureUpper;
        callback.TimeSignatureLower = record.TimeSignatureLower;
        if (record.MarkerNameAnsi[0] != 0)
            callback.Marker = String(record.MarkerNameAnsi);
        AudioEventSystem::DispatchEventCallback(callback);

        // Internally-owned one-shots retain a handle until the STOPPED callback,
        // avoiding a per-frame registry sweep and keeping callback lifetime safe.
        if (record.Type == AudioEventCallbackType::Stopped && _handles.IsOneShot(record.Handle))
            ReleaseInstance(record.Handle);
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
        ReleaseCallbackContexts();
    }
}

bool FmodEventBackend::ConfigureInstanceCallback(FMOD::Studio::EventInstance* instance, AudioEventHandle handle)
{
    auto* context = New<FmodInstanceContext>();
    context->Backend = this;
    context->Handle = handle;
    if (!_handles.SetCallbackContext(handle, context))
    {
        Delete(context);
        return false;
    }

    // Contexts are retained until the Studio system shuts down. FMOD may invoke a
    // callback concurrently with a release, so reclaiming them per-slot is unsafe.
    _callbackContexts.Add(context);
    if (instance->setUserData(context) != FMOD_OK)
        return false;

    constexpr FMOD_STUDIO_EVENT_CALLBACK_TYPE callbackMask =
        FMOD_STUDIO_EVENT_CALLBACK_STARTING |
        FMOD_STUDIO_EVENT_CALLBACK_STARTED |
        FMOD_STUDIO_EVENT_CALLBACK_STOPPED |
        FMOD_STUDIO_EVENT_CALLBACK_RESTARTED |
        FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER |
        FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_BEAT |
        FMOD_STUDIO_EVENT_CALLBACK_REAL_TO_VIRTUAL |
        FMOD_STUDIO_EVENT_CALLBACK_VIRTUAL_TO_REAL |
        FMOD_STUDIO_EVENT_CALLBACK_START_FAILED;
    return instance->setCallback(&OnEventCallback, callbackMask) == FMOD_OK;
}

void FmodEventBackend::ReleaseCallbackContexts()
{
    for (auto* context : _callbackContexts)
        Delete(context);
    _callbackContexts.Clear();
}

void FmodEventBackend::EnqueueCallback(const FmodCallbackRecord& record)
{
    _callbacks.Enqueue(record);
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
    const int32 index = Audio::GetActiveDeviceIndex();
    if (index >= 0 && index < Audio::Devices.Count())
        SetOutputDevice(String(Audio::Devices[index].InternalName));
}

void FmodEventBackend::EnumerateOutputDevices(Array<AudioOutputDeviceInfo>& result) const
{
    result.Clear();
    if (!_coreSystem)
        return;

    int32 count = 0;
    if (_coreSystem->getNumDrivers(&count) != FMOD_OK)
        return;

    result.EnsureCapacity(count);
    for (int32 i = 0; i < count; i++)
    {
        char name[256] = {};
        FMOD_GUID guid = {};
        int32 sampleRate = 0;
        FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_DEFAULT;
        int32 channels = 0;
        if (_coreSystem->getDriverInfo(i, name, sizeof(name), &guid, &sampleRate, &speakerMode, &channels) != FMOD_OK)
            continue;

        AudioOutputDeviceInfo& device = result.AddOne();
        device.Name = String(name);
        device.StableId = FmodConvert::FromFmodGuid(guid).ToString();
        device.SampleRate = sampleRate;
        device.Channels = channels;
    }
}

bool FmodEventBackend::SetOutputDevice(const StringView& stableId)
{
    if (!_coreSystem || stableId.IsEmpty())
        return false;

    const String id(stableId);
    int32 count = 0;
    if (_coreSystem->getNumDrivers(&count) != FMOD_OK)
        return false;
    for (int32 i = 0; i < count; i++)
    {
        FMOD_GUID guid = {};
        if (_coreSystem->getDriverInfo(i, nullptr, 0, &guid, nullptr, nullptr, nullptr) == FMOD_OK && FmodConvert::FromFmodGuid(guid).ToString() == id)
            return _coreSystem->setDriver(i) == FMOD_OK;
    }
    return false;
}

String FmodEventBackend::GetOutputDevice() const
{
    if (!_coreSystem)
        return String::Empty;
    int32 driver = -1;
    if (_coreSystem->getDriver(&driver) != FMOD_OK || driver < 0)
        return String::Empty;
    FMOD_GUID guid = {};
    return _coreSystem->getDriverInfo(driver, nullptr, 0, &guid, nullptr, nullptr, nullptr) == FMOD_OK ? FmodConvert::FromFmodGuid(guid).ToString() : String::Empty;
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

bool FmodEventBackend::LoadBankSampleData(const Guid& bankId)
{
    return _banks.LoadSampleData(bankId);
}

void FmodEventBackend::UnloadBankSampleData(const Guid& bankId)
{
    _banks.UnloadSampleData(bankId);
}

AudioBankState FmodEventBackend::GetBankState(const Guid& bankId) const
{
    return _banks.GetState(bankId);
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
    if (!ConfigureInstanceCallback(inst, handle))
    {
        FmodInstanceContext* context;
        _handles.Free(handle, inst, context);
        inst->setUserData(nullptr);
        inst->release();
        return AudioEventHandle();
    }

    // Apply attributes
    FMOD_3D_ATTRIBUTES attrs = FmodConvert::ToFmodAttributes(options.Attributes);
    inst->set3DAttributes(&attrs);
    inst->setListenerMask(options.ListenerMask);

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

bool FmodEventBackend::KeyOff(AudioEventHandle handle)
{
    auto* inst = _handles.Get(handle);
    return inst ? inst->keyOff() == FMOD_OK : false;
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

        ReleaseInstance(handle);
    }

    return true;
}

bool FmodEventBackend::ReleaseInstance(AudioEventHandle handle)
{
    FMOD::Studio::EventInstance* inst = nullptr;
    FmodInstanceContext* context = nullptr;
    if (_handles.Free(handle, inst, context) && inst)
    {
        // Prevent any later FMOD callback from resolving a released handle. The
        // stable context remains allocated until backend shutdown for in-flight
        // callback safety.
        inst->setUserData(nullptr);
        inst->setCallback(nullptr, FMOD_STUDIO_EVENT_CALLBACK_ALL);
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

    // Keep a private handle until STOPPED so the callback bridge owns cleanup.
    const AudioEventHandle handle = _handles.Allocate(inst, eventId, Guid::Empty, true);
    if (!ConfigureInstanceCallback(inst, handle))
    {
        FmodInstanceContext* context;
        _handles.Free(handle, inst, context);
        inst->setUserData(nullptr);
        inst->release();
        return false;
    }

    FMOD_3D_ATTRIBUTES fattrs = FmodConvert::ToFmodAttributes(attributes);
    inst->set3DAttributes(&fattrs);
    inst->setVolume(Math::Saturate(volume));
    inst->setPitch(Math::Clamp(pitch, 0.0f, 10.0f));
    if (inst->start() == FMOD_OK)
        return true;

    ReleaseInstance(handle);
    return false;
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

    if (id.Data1 != 0 || id.Data2 != 0)
    {
        FMOD_STUDIO_PARAMETER_ID parameterId;
        parameterId.data1 = id.Data1;
        parameterId.data2 = id.Data2;
        return inst->setParameterByID(parameterId, value, ignoreSeekSpeed) == FMOD_OK;
    }

    if (id.Name.HasChars())
    {
        StringAnsi nameAnsi(id.Name);
        return inst->setParameterByName(nameAnsi.Get(), value, ignoreSeekSpeed) == FMOD_OK;
    }

    return false;
}

bool FmodEventBackend::SetParameters(AudioEventHandle handle, const Span<AudioParameterValue>& values, bool ignoreSeekSpeed)
{
    auto* inst = _handles.Get(handle);
    if (!inst || values.Length() == 0)
        return false;

    Array<FMOD_STUDIO_PARAMETER_ID, InlinedAllocation<16>> ids;
    Array<float, InlinedAllocation<16>> numericValues;
    for (const auto& value : values)
    {
        if (value.Id.Data1 == 0 && value.Id.Data2 == 0)
        {
            // Name-only IDs cannot be batched by FMOD; preserve generic semantics.
            bool result = true;
            for (const auto& item : values)
                result &= SetParameter(handle, item.Id, item.Value, ignoreSeekSpeed);
            return result;
        }
        FMOD_STUDIO_PARAMETER_ID id;
        id.data1 = value.Id.Data1;
        id.data2 = value.Id.Data2;
        ids.Add(id);
        numericValues.Add(value.Value);
    }
    return inst->setParametersByIDs(ids.Get(), numericValues.Get(), ids.Count(), ignoreSeekSpeed) == FMOD_OK;
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

    if (id.Data1 != 0 || id.Data2 != 0)
    {
        FMOD_STUDIO_PARAMETER_ID parameterId;
        parameterId.data1 = id.Data1;
        parameterId.data2 = id.Data2;
        return _studioSystem->setParameterByID(parameterId, value, ignoreSeekSpeed) == FMOD_OK;
    }

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

bool FmodEventBackend::SetSnapshotWeight(AudioEventHandle /*handle*/, float /*weight*/)
{
    // FMOD Studio snapshots have no universal runtime blend-weight API. Projects
    // that need continuous blending must author and reference a parameter through
    // AudioSnapshot::WeightParameter (or an AudioZoneVolume override).
    return false;
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
    outSnapshot.BackendName = GetName();
    outSnapshot.Initialized = _studioSystem != nullptr;
    outSnapshot.CallbackQueueDepth = _callbacks.GetApproximateDepth();
    outSnapshot.DroppedCallbacks = _callbacks.GetTotalDropped();
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
    auto* instance = reinterpret_cast<FMOD::Studio::EventInstance*>(event);
    if (!instance)
        return FMOD_OK;

    void* userData = nullptr;
    if (instance->getUserData(&userData) != FMOD_OK || !userData)
        return FMOD_OK;

    const auto* context = static_cast<FmodInstanceContext*>(userData);
    if (!context->Backend)
        return FMOD_OK;

    FmodCallbackRecord record;
    record.Handle = context->Handle;
    switch (type)
    {
    case FMOD_STUDIO_EVENT_CALLBACK_STARTING:
        record.Type = AudioEventCallbackType::Starting;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_STARTED:
        record.Type = AudioEventCallbackType::Started;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_STOPPED:
        record.Type = AudioEventCallbackType::Stopped;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_RESTARTED:
        record.Type = AudioEventCallbackType::Restarted;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_REAL_TO_VIRTUAL:
        record.Type = AudioEventCallbackType::RealToVirtual;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_VIRTUAL_TO_REAL:
        record.Type = AudioEventCallbackType::VirtualToReal;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_START_FAILED:
        record.Type = AudioEventCallbackType::StartFailed;
        break;
    case FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER:
    {
        record.Type = AudioEventCallbackType::TimelineMarker;
        const auto* marker = static_cast<FMOD_STUDIO_TIMELINE_MARKER_PROPERTIES*>(parameters);
        if (marker)
        {
            record.TimelinePositionMs = marker->position;
            if (marker->name)
            {
                int32 i = 0;
                for (; i < (int32)sizeof(record.MarkerNameAnsi) - 1 && marker->name[i] != 0; i++)
                    record.MarkerNameAnsi[i] = marker->name[i];
                record.MarkerNameAnsi[i] = 0;
            }
        }
        break;
    }
    case FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_BEAT:
    {
        record.Type = AudioEventCallbackType::TimelineBeat;
        const auto* beat = static_cast<FMOD_STUDIO_TIMELINE_BEAT_PROPERTIES*>(parameters);
        if (beat)
        {
            record.TimelinePositionMs = beat->position;
            record.Bar = beat->bar;
            record.Beat = beat->beat;
            record.Tempo = beat->tempo;
            record.TimeSignatureUpper = beat->timesignatureupper;
            record.TimeSignatureLower = beat->timesignaturelower;
        }
        break;
    }
    default:
        return FMOD_OK;
    }

    context->Backend->EnqueueCallback(record);
    return FMOD_OK;
}

#endif
