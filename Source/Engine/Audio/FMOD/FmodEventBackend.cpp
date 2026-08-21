// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodEventBackend.h"

#if AUDIO_EVENT_API_FMOD

#include "FmodConvert.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioSettings.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Utilities/StringConverter.h"
#include "Engine/Platform/Platform.h"
#include <cmath>
#include <cstring>

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

    // All positions and velocities crossing this boundary are explicitly converted
    // to meters, so FMOD receives one distance unit per meter. Attenuation values
    // authored by Studio therefore remain unchanged.
    _coreSystem->set3DSettings(1.0f, 1.0f, 1.0f);
    _coreSystem->setUserData(this);
    _coreSystem->setCallback(&OnSystemCallback, FMOD_SYSTEM_CALLBACK_DEVICELISTCHANGED | FMOD_SYSTEM_CALLBACK_DEVICELOST | FMOD_SYSTEM_CALLBACK_ERROR);

    const auto settings = AudioSettings::Get();
    if (settings->FmodOutputType >= 0)
        FmodConvert::CheckResult(_coreSystem->setOutput((FMOD_OUTPUTTYPE)settings->FmodOutputType), "Core::System::setOutput");
    if (settings->OutputOwner == AudioOutputOwner::NativeClipBackend)
    {
        // The event backend remains usable for deterministic timeline/gameplay behavior,
        // but it must not compete with the native clip backend for a hardware endpoint.
        result = _coreSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
        if (!FmodConvert::CheckResult(result, "Core::System::setOutput(NOSOUND)"))
        {
            Dispose();
            return true;
        }
    }
    FMOD_ADVANCEDSETTINGS coreAdvanced = {};
    coreAdvanced.cbSize = sizeof(coreAdvanced);
    coreAdvanced.profilePort = settings->LiveUpdatePort;
    coreAdvanced.maxVorbisCodecs = Math::Clamp(settings->FmodMaxVorbisCodecs, 0, 1024);
    String encryptionKey;
    StringAnsi encryptionKeyAnsi;
    if (settings->EncryptionKeyEnvironmentVariable.HasChars())
    {
        if (Platform::GetEnvironmentVariable(settings->EncryptionKeyEnvironmentVariable, encryptionKey) && encryptionKey.HasChars())
        {
            encryptionKeyAnsi.Set(encryptionKey.Get(), encryptionKey.Length());
        }
        else
            LOG(Error, "FMOD encryption-key environment variable '{0}' is missing or empty.", settings->EncryptionKeyEnvironmentVariable);
    }
    FmodConvert::CheckResult(_coreSystem->setAdvancedSettings(&coreAdvanced), "Core::System::setAdvancedSettings");
    FMOD_STUDIO_ADVANCEDSETTINGS studioAdvanced = {};
    studioAdvanced.cbsize = sizeof(studioAdvanced);
    // FMOD otherwise processes Studio commands at 20 ms (50 Hz), which makes
    // listener rotation visibly outrun 3D panning at common game frame rates.
    // A shorter period keeps spatial attributes aligned with camera motion. FMOD
    // quantizes this value to the platform mixer block duration.
    studioAdvanced.studioupdateperiod = Math::Clamp(settings->FmodStudioUpdatePeriod, 1, 1000);
    studioAdvanced.encryptionkey = encryptionKeyAnsi.HasChars() ? encryptionKeyAnsi.Get() : nullptr;
    FmodConvert::CheckResult(_studioSystem->setAdvancedSettings(&studioAdvanced), "Studio::System::setAdvancedSettings");
    if (settings->FmodSampleRate > 0 || settings->FmodSpeakerMode > 0)
        FmodConvert::CheckResult(_coreSystem->setSoftwareFormat(Math::Max(0, settings->FmodSampleRate), (FMOD_SPEAKERMODE)Math::Max(0, settings->FmodSpeakerMode), 0), "Core::System::setSoftwareFormat");
    // Studio's initialize count is the virtual-event budget. The number of
    // voices that actually reach the mixer is a separate Core setting. Keeping
    // these independent lets large scenes retain cheap virtual timelines while
    // enforcing a bounded, predictable real mixing cost.
    FmodConvert::CheckResult(_coreSystem->setSoftwareChannels(Math::Clamp(settings->FmodRealChannels, 1, 256)), "Core::System::setSoftwareChannels");
    if (settings->FmodDspBufferLength > 0 && settings->FmodDspBufferCount > 0)
        FmodConvert::CheckResult(_coreSystem->setDSPBufferSize(settings->FmodDspBufferLength, settings->FmodDspBufferCount), "Core::System::setDSPBufferSize");

    FMOD_STUDIO_INITFLAGS studioFlags = FMOD_STUDIO_INIT_NORMAL;
#if USE_EDITOR || !BUILD_RELEASE
    if (settings->EnableLiveUpdate)
        studioFlags |= FMOD_STUDIO_INIT_LIVEUPDATE;
#endif

    result = _studioSystem->initialize(Math::Clamp(settings->FmodMaxChannels, 32, 4096), studioFlags, FMOD_INIT_NORMAL, nullptr);
    _liveUpdateActive = (studioFlags & FMOD_STUDIO_INIT_LIVEUPDATE) != 0;
    if (result != FMOD_OK && _liveUpdateActive && settings->AllowLiveUpdateFallback)
    {
        LOG(Warning, "FMOD Live Update initialization failed on port {0}: ({1}) {2}. Retrying without Live Update.", settings->LiveUpdatePort, (int32)result, String(FMOD_ErrorString(result)));
        studioFlags = (FMOD_STUDIO_INITFLAGS)(studioFlags & ~FMOD_STUDIO_INIT_LIVEUPDATE);
        result = _studioSystem->initialize(Math::Clamp(settings->FmodMaxChannels, 32, 4096), studioFlags, FMOD_INIT_NORMAL, nullptr);
        _liveUpdateActive = false;
    }
    if (!FmodConvert::CheckResult(result, "Studio::System::initialize"))
    {
        Dispose();
        return true;
    }

    FMOD::ChannelGroup* masterGroup = nullptr;
    if (FmodConvert::CheckResult(_coreSystem->getMasterChannelGroup(&masterGroup), "Core::System::getMasterChannelGroup") && masterGroup)
    {
        result = masterGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &_masterMeterDsp);
        if (FmodConvert::CheckResult(result, "MasterChannelGroup::getDSP") && _masterMeterDsp)
            FmodConvert::CheckResult(_masterMeterDsp->setMeteringEnabled(false, true), "MasterDSP::setMeteringEnabled");
    }

    _banks.Init(_studioSystem);
    // A previous editor play session can leave a diagnostic error latched even
    // though the Studio system has just been recreated successfully.
    _lastErrorCode.store(0, std::memory_order_relaxed);
    _channelReuseNotifications.store(0, std::memory_order_relaxed);
    _handles.Clear();
    _releasedInstances.Clear();
    _nextReleasedDiagnosticIndex = 0x80000000u;
    _callbacks.Clear();
    _totalInstancesCreated = 0;
    _totalPlays = 0;
    _totalStopped = 0;
    _peakActiveInstances = 0;

    return false;
}

void FmodEventBackend::Update(float dt)
{
    if (!_studioSystem)
        return;

    PROFILE_CPU_NAMED("FMOD.EventBackend.Update");

    if (_outputDevicesDirty.exchange(false, std::memory_order_acq_rel))
        RefreshOutputDevices();

    // Drain callback queue
    {
        PROFILE_CPU_NAMED("FMOD.Callbacks");
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

            if (record.Type == AudioEventCallbackType::Stopped)
                _totalStopped++;
        }
    }

    {
        PROFILE_CPU_NAMED("FMOD.Studio.Update");
        _studioSystem->update();
    }
    UpdateReleasedInstances();
    {
        PROFILE_CPU_NAMED("FMOD.OutputMeter");
        UpdateOutputMeter(dt);
    }

    // STOPPED can be dropped by an intentionally bounded callback queue. Keep a
    // low-frequency defensive recovery sweep in every configuration without
    // returning to per-frame polling. Queue pressure must never become a leak.
    _oneShotSweepTimer += dt;
    if (_oneShotSweepTimer >= 1.0f)
    {
        _oneShotSweepTimer = 0.0f;
        Array<AudioEventHandle, InlinedAllocation<16>> stopped;
        const auto& slots = _handles.GetSlots();
        for (int32 i = 0; i < slots.Count(); i++)
        {
            if (!slots[i].InUse || !slots[i].OneShot || !slots[i].Instance)
                continue;
            FMOD_STUDIO_PLAYBACK_STATE state;
            if (!slots[i].Instance->isValid() ||
                (slots[i].Instance->getPlaybackState(&state) == FMOD_OK && state == FMOD_STUDIO_PLAYBACK_STOPPED))
                stopped.Add(AudioEventHandle((uint32)i, slots[i].Generation));
        }
        for (const auto handle : stopped)
            ReleaseInstance(handle);
    }

}

void FmodEventBackend::Dispose()
{
    if (_studioSystem)
    {
        StopAll(AudioStopMode::Immediate);
        // Stop every callback producer before clearing the bounded MPSC queue.
        // FmodCallbackQueue::Clear is consumer-only and is not safe while the
        // Studio update thread or Core device callback can still enqueue.
        if (_coreSystem)
        {
            _coreSystem->setCallback(nullptr, 0);
            _coreSystem->setUserData(nullptr);
        }
        _studioSystem->flushCommands();
        _banks.Dispose();
        _handles.Clear();
        _releasedInstances.Clear();

        _studioSystem->unloadAll();
        _studioSystem->flushCommands();
        _studioSystem->release();
        _studioSystem = nullptr;
        _coreSystem = nullptr;
        ReleaseCallbackContexts();
        _callbacks.Clear();
    }
    _masterMeterDsp = nullptr;
    _listeners.Clear();
    _outputPeak.Clear();
    _outputRms.Clear();
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
        FMOD_STUDIO_EVENT_CALLBACK_START_FAILED |
        FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND |
        FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND;
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
    // Studio's root bus does not exist until the master bank is loaded. Audio
    // settings are applied before startup banks during initialization, so an
    // unconditional lookup here produces a real FMOD ERR_EVENT_NOTFOUND and
    // poisons diagnostics even though startup succeeds moments later. The
    // cached value is applied again after banks load by AudioEventSystem.
    if (_studioSystem && _banks.GetLoadedCount() > 0)
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
    if (_studioSystem && _banks.GetLoadedCount() > 0)
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
    if (_studioSystem && _banks.GetLoadedCount() > 0)
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

void FmodEventBackend::RefreshOutputDevices()
{
    const String selected = GetOutputDevice();
    Array<AudioOutputDeviceInfo> devices;
    EnumerateOutputDevices(devices);
    Audio::Devices.Resize(devices.Count());
    for (int32 i = 0; i < devices.Count(); i++)
    {
        Audio::Devices[i].Name = devices[i].Name;
        Audio::Devices[i].InternalName = StringAnsi(devices[i].StableId);
        Audio::Devices[i].BackendName = "FMOD Studio";
        Audio::Devices[i].BackendIndex = i;
    }
    int32 activeIndex = -1;
    if (selected.HasChars())
    {
        for (int32 i = 0; i < devices.Count(); i++)
        {
            if (devices[i].StableId == selected)
            {
                activeIndex = i;
                break;
            }
        }
    }
    if (activeIndex < 0 && devices.HasItems())
        activeIndex = 0;
    Audio::SetActiveDeviceIndex(activeIndex);
    Audio::DevicesChanged();
}

FMOD_RESULT F_CALL FmodEventBackend::OnSystemCallback(FMOD_SYSTEM*, FMOD_SYSTEM_CALLBACK_TYPE type, void* commandData1, void*, void* userData)
{
    auto* backend = static_cast<FmodEventBackend*>(userData);
    if (backend && (type & (FMOD_SYSTEM_CALLBACK_DEVICELISTCHANGED | FMOD_SYSTEM_CALLBACK_DEVICELOST)) != 0)
        backend->_outputDevicesDirty.store(true, std::memory_order_release);
    if (backend && (type & FMOD_SYSTEM_CALLBACK_ERROR) != 0)
    {
        const auto* error = static_cast<FMOD_ERRORCALLBACK_INFO*>(commandData1);
        if (error)
        {
            // Voice stealing is FMOD's normal polyphony-budget behavior. Keep
            // it visible as benchmark telemetry without poisoning LastError or
            // flooding the console as though it were an integration failure.
            if (error->result == FMOD_ERR_CHANNEL_STOLEN)
            {
                backend->_channelReuseNotifications.fetch_add(1, std::memory_order_relaxed);
                return FMOD_OK;
            }
            // The shared Core backend retires raw channel slots as soon as FMOD
            // recycles them. A race can still report one invalid-handle result
            // from the Core Channel/ChannelControl API during that handoff; it
            // is normal voice-budget behavior, not an Event backend failure.
            if (error->result == FMOD_ERR_INVALID_HANDLE && error->functionname &&
                (std::strstr(error->functionname, "Channel::") != nullptr ||
                 std::strstr(error->functionname, "ChannelControl::") != nullptr))
            {
                backend->_channelReuseNotifications.fetch_add(1, std::memory_order_relaxed);
                return FMOD_OK;
            }
            // Catalog discovery and typed-reference fallback intentionally probe
            // IDs and paths. A miss is returned to the caller and surfaced with
            // context there; it is not a backend fault and must not flood the
            // console through FMOD's synchronous error callback.
            if (error->result == FMOD_ERR_EVENT_NOTFOUND && error->functionname)
            {
                const char* function = error->functionname;
                if (std::strstr(function, "getParameterDescriptionByName") != nullptr ||
                    std::strstr(function, "getEvent") != nullptr ||
                    std::strstr(function, "getBus") != nullptr ||
                    std::strstr(function, "getVCA") != nullptr)
                    return FMOD_OK;
            }
            backend->_lastErrorCode.store((int32)error->result, std::memory_order_relaxed);
            LOG(Error, "[FMOD] {0}({1}) failed for instance {2}: ({3}) {4}",
                String(error->functionname ? error->functionname : "unknown"),
                String(error->functionparams ? error->functionparams : ""),
                (uint64)(uintptr)error->instance,
                (int32)error->result,
                String(FMOD_ErrorString(error->result)));
        }
    }
    return FMOD_OK;
}

void FmodEventBackend::UpdateOutputMeter(float dt)
{
    _combinedOutputPeak = 0.0f;
    _combinedOutputRms = 0.0f;
    _combinedOutputDbfs = -120.0f;
    _outputClipping = false;
    _outputPeak.Clear();
    _outputRms.Clear();
    if (!_masterMeterDsp && _coreSystem)
    {
        // Output-device and Play-mode transitions may rebuild FMOD's master
        // channel group while the Studio system remains alive. Reacquire the
        // built-in head DSP instead of retaining a stale native handle.
        FMOD::ChannelGroup* masterGroup = nullptr;
        if (_coreSystem->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup &&
            masterGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &_masterMeterDsp) == FMOD_OK && _masterMeterDsp)
            _masterMeterDsp->setMeteringEnabled(false, true);
    }
    if (!_masterMeterDsp)
        return;

    FMOD_DSP_METERING_INFO output = {};
    const FMOD_RESULT result = _masterMeterDsp->getMeteringInfo(nullptr, &output);
    if (result != FMOD_OK)
    {
        if (result == FMOD_ERR_INVALID_HANDLE)
        {
            _masterMeterDsp = nullptr;
            return;
        }
        _lastErrorCode.store((int32)result, std::memory_order_relaxed);
        return;
    }
    const int32 channels = Math::Clamp((int32)output.numchannels, 0, 32);
    float rmsSquares = 0.0f;
    _outputPeak.Resize(channels);
    _outputRms.Resize(channels);
    for (int32 i = 0; i < channels; i++)
    {
        _outputPeak[i] = output.peaklevel[i];
        _outputRms[i] = output.rmslevel[i];
        _combinedOutputPeak = Math::Max(_combinedOutputPeak, output.peaklevel[i]);
        rmsSquares += output.rmslevel[i] * output.rmslevel[i];
    }
    if (channels > 0)
        _combinedOutputRms = Math::Sqrt(rmsSquares / channels);
    if (_combinedOutputRms > 0.000001f)
        _combinedOutputDbfs = Math::Max(-120.0f, 20.0f * (float)std::log10(_combinedOutputRms));
    _outputClipping = _combinedOutputPeak >= 0.999f;
    if (_combinedOutputRms > 0.00001f)
        _secondsSinceNonSilentOutput = 0.0f;
    else if (_secondsSinceNonSilentOutput >= 0.0f)
        _secondsSinceNonSilentOutput += Math::Max(0.0f, dt);
}

void FmodEventBackend::UpdateListeners(const Span<AudioListenerState>& listeners)
{
    if (!_studioSystem)
        return;

    if (listeners.Length() == 0)
    {
        const bool lostActiveListener = _listeners.HasItems();
        _listeners.Clear();
        if (lostActiveListener && !_missingListenerWarned)
        {
            LOG(Warning, "FMOD has no active AudioListener. 3D events can start but cannot become audible.");
            _missingListenerWarned = true;
        }
        // FMOD requires at least one listener. Reset the retained listener and mute
        // its contribution so disabling the last Flax listener cannot leave a stale
        // camera transform driving 3D spatialization.
        _studioSystem->setNumListeners(1);
        const FMOD_3D_ATTRIBUTES attributes = FmodConvert::ToFmodAttributes(Audio3DAttributes());
        _studioSystem->setListenerAttributes(0, &attributes);
        _studioSystem->setListenerWeight(0, 0.0f);
        return;
    }

    _missingListenerWarned = false;

    int32 count = Math::Min(listeners.Length(), (int32)FMOD_MAX_LISTENERS);
    int32 middlewareCount = 1;
    for (int32 i = 0; i < count; i++)
        middlewareCount = Math::Max(middlewareCount, Math::Clamp(listeners[i].Index + 1, 1, (int32)FMOD_MAX_LISTENERS));
    _listeners.Resize(middlewareCount);
    // Grow/shrink the Studio listener table before touching weights. FMOD rejects
    // setListenerWeight for an index that is outside the current listener count.
    _studioSystem->setNumListeners(middlewareCount);
    for (int32 i = 0; i < middlewareCount; i++)
    {
        _listeners[i] = AudioListenerState();
        _listeners[i].Index = i;
        _studioSystem->setListenerWeight(i, 0.0f);
    }

    for (int32 i = 0; i < count; i++)
    {
        const auto& l = listeners[i];
        const int32 listenerIndex = Math::Clamp(l.Index, 0, middlewareCount - 1);
        if (_listeners[listenerIndex].ActorId.IsValid() && !_duplicateListenerWarned)
        {
            LOG(Warning, "Multiple AudioListeners use listener index {0}; the last listener wins.", listenerIndex);
            _duplicateListenerWarned = true;
        }
        _listeners[listenerIndex] = l;
        FMOD_3D_ATTRIBUTES attrs = FmodConvert::ToFmodAttributes(l.Attributes);
        _studioSystem->setListenerAttributes(listenerIndex, &attrs);
        _studioSystem->setListenerWeight(listenerIndex, Math::Saturate(l.Weight));
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

bool FmodEventBackend::QueryBank(const Guid& bankId, const StringView& path, AudioBankRuntimeState& outState) const
{
    return _banks.Query(bankId, path, outState);
}

FMOD::Studio::EventDescription* FmodEventBackend::GetEventDescription(const Guid& eventId, const StringView& path)
{
    if (!_studioSystem || _banks.GetLoadedCount() == 0)
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
    if (!_studioSystem || _banks.GetLoadedCount() == 0)
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
    if (!_studioSystem || _banks.GetLoadedCount() == 0)
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
    _totalInstancesCreated++;
    _peakActiveInstances = Math::Max(_peakActiveInstances, _handles.GetActiveCount());
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
    // FMOD rejects a zero mask. Treat it as the primary listener so malformed or
    // legacy content cannot turn every created instance into an error callback.
    inst->setListenerMask(options.ListenerMask != 0 ? options.ListenerMask : 1u);

    // Apply initial parameters before start so parameter-triggered instruments do
    // not enter FMOD virtualization in their silent default state.
    for (const auto& parameter : options.InitialParameters)
    {
        if (!SetParameter(handle, parameter.Id, parameter.Value, true))
        {
            FmodInstanceContext* context;
            _handles.Free(handle, inst, context);
            inst->setUserData(nullptr);
            inst->release();
            return AudioEventHandle();
        }
    }

    if (options.AutoPlay && inst->start() == FMOD_OK)
    {
        _handles.MarkPlayed(handle);
        _totalPlays++;
    }

    return handle;
}

bool FmodEventBackend::Play(AudioEventHandle handle)
{
    auto* inst = _handles.Get(handle);
    if (!inst || inst->start() != FMOD_OK)
        return false;
    _handles.MarkPlayed(handle);
    _totalPlays++;
    return true;
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
    if (_studioSystem && _banks.GetLoadedCount() > 0)
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

    // These instances have already transferred ownership to FMOD via release().
    // The master-bus stop above owns their shutdown; discard only our observer records.
    _releasedInstances.Clear();

    return true;
}

bool FmodEventBackend::ReleaseInstance(AudioEventHandle handle)
{
    ReleasedInstanceDiagnostic diagnostic;
    bool retainDiagnostic = false;
    if (_handles.Validate(handle))
    {
        const auto& slot = _handles.GetSlots()[handle.Index];
        FMOD_STUDIO_PLAYBACK_STATE state = FMOD_STUDIO_PLAYBACK_STOPPED;
        retainDiagnostic = slot.Instance && slot.PlayCount > 0 && slot.Instance->isValid() &&
                           slot.Instance->getPlaybackState(&state) == FMOD_OK && state != FMOD_STUDIO_PLAYBACK_STOPPED;
        if (retainDiagnostic)
        {
            diagnostic.Instance = slot.Instance;
            diagnostic.Handle = handle;
            diagnostic.EventId = slot.EventId;
            diagnostic.OwnerId = slot.OwnerId;
            diagnostic.PlayCount = slot.PlayCount;
            diagnostic.IsOneShot = true;
            FMOD::Studio::EventDescription* description = nullptr;
            if (slot.Instance->getDescription(&description) == FMOD_OK && description)
            {
                char path[512] = {};
                int32 retrieved = 0;
                if (description->getPath(path, ARRAY_COUNT(path), &retrieved) == FMOD_OK)
                    diagnostic.Path = String(StringAnsi(path));
            }
        }
    }

    FMOD::Studio::EventInstance* inst = nullptr;
    FmodInstanceContext* context = nullptr;
    if (_handles.Free(handle, inst, context) && inst)
    {
        // Prevent any later FMOD callback from resolving a released handle. The
        // stable context remains allocated until backend shutdown for in-flight
        // callback safety.
        if (inst->isValid())
        {
            inst->setUserData(nullptr);
            inst->setCallback(nullptr, FMOD_STUDIO_EVENT_CALLBACK_ALL);
            inst->release();
        }
        if (retainDiagnostic)
        {
            _releasedInstances.Add(MoveTemp(diagnostic));
            _peakActiveInstances = Math::Max(_peakActiveInstances, _handles.GetActiveCount() + _releasedInstances.Count());
        }
        // FMOD can invoke callbacks from its Studio thread while a release is in
        // flight. Keep the tiny context allocation until backend shutdown rather
        // than putting a synchronous flushCommands barrier in the frame loop.
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

    // Fire-and-forget instances need no callback bridge or stable handle. FMOD
    // retains the event until playback ends after release(), which is the same
    // non-blocking ownership model used by the official integration. Avoiding a
    // callback allocation here keeps rapid gunfire off the command-flush path.
    _totalInstancesCreated++;

    FMOD_3D_ATTRIBUTES fattrs = FmodConvert::ToFmodAttributes(attributes);
    inst->set3DAttributes(&fattrs);
    inst->setVolume(Math::Saturate(volume));
    inst->setPitch(Math::Clamp(pitch, 0.0f, 10.0f));
    const FMOD_RESULT startResult = inst->start();
    inst->release();
    if (startResult == FMOD_OK)
    {
        _totalPlays++;
        TrackReleasedInstance(inst, AudioEventHandle(_nextReleasedDiagnosticIndex++, 1), eventId, Guid::Empty, path, 1, true);
        _peakActiveInstances = Math::Max(_peakActiveInstances, _handles.GetActiveCount() + _releasedInstances.Count());
        return true;
    }

    return false;
}

void FmodEventBackend::TrackReleasedInstance(FMOD::Studio::EventInstance* instance, const AudioEventHandle& handle, const Guid& eventId, const Guid& ownerId, const StringView& path, int32 playCount, bool isOneShot)
{
    if (!instance)
        return;
    ReleasedInstanceDiagnostic& diagnostic = _releasedInstances.AddOne();
    diagnostic.Instance = instance;
    diagnostic.Handle = handle;
    diagnostic.EventId = eventId;
    diagnostic.OwnerId = ownerId;
    diagnostic.Path = path;
    diagnostic.PlayCount = playCount;
    diagnostic.IsOneShot = isOneShot;
}

void FmodEventBackend::UpdateReleasedInstances()
{
    for (int32 i = _releasedInstances.Count() - 1; i >= 0; i--)
    {
        auto* instance = _releasedInstances[i].Instance;
        FMOD_STUDIO_PLAYBACK_STATE state = FMOD_STUDIO_PLAYBACK_STOPPED;
        if (!instance || !instance->isValid() || instance->getPlaybackState(&state) != FMOD_OK || state == FMOD_STUDIO_PLAYBACK_STOPPED)
        {
            _releasedInstances.RemoveAt(i);
            _totalStopped++;
        }
    }
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

bool FmodEventBackend::ResolveParameterId(const Guid& eventId, const StringView& eventPath, const StringView& name, AudioParameterId& id)
{
    auto* description = GetEventDescription(eventId, eventPath);
    if (!description || name.IsEmpty())
        return false;
    FMOD_STUDIO_PARAMETER_DESCRIPTION parameter;
    StringAsANSI<> nameAnsi(name.Get(), name.Length());
    if (description->getParameterDescriptionByName(nameAnsi.Get(), &parameter) != FMOD_OK)
        return false;
    id = AudioParameterId(name);
    id.Data1 = parameter.id.data1;
    id.Data2 = parameter.id.data2;
    return true;
}

bool FmodEventBackend::GetEventParameters(const Guid& eventId, const StringView& eventPath, Array<AudioParameterDescription>& result)
{
    result.Clear();
    auto* description = GetEventDescription(eventId, eventPath);
    if (!description)
        return false;
    int count = 0;
    if (description->getParameterDescriptionCount(&count) != FMOD_OK)
        return false;
    result.EnsureCapacity(count);
    for (int i = 0; i < count; i++)
    {
        FMOD_STUDIO_PARAMETER_DESCRIPTION parameter;
        if (description->getParameterDescriptionByIndex(i, &parameter) != FMOD_OK)
            continue;
        AudioParameterDescription value;
        value.Id = AudioParameterId(String(parameter.name));
        value.Id.Data1 = parameter.id.data1;
        value.Id.Data2 = parameter.id.data2;
        value.Minimum = parameter.minimum;
        value.Maximum = parameter.maximum;
        value.DefaultValue = parameter.defaultvalue;
        value.Type = (int32)parameter.type;
        value.Flags = (uint32)parameter.flags;
        result.Add(value);
    }
    return true;
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

bool FmodEventBackend::SetProgrammerSound(AudioEventHandle handle, const AudioProgrammerSoundData& data)
{
    auto* context = _handles.GetCallbackContext(handle);
    if (!context || data.Path.IsEmpty())
        return false;

    const StringAsANSI<512> pathAnsi(data.Path.Get(), data.Path.Length());
    int32 i = 0;
    for (; i < (int32)ARRAY_COUNT(context->ProgrammerSoundPath) - 1 && pathAnsi.Get()[i]; i++)
        context->ProgrammerSoundPath[i] = pathAnsi.Get()[i];
    context->ProgrammerSoundPath[i] = 0;
    context->ProgrammerSoundSubsound = data.SubsoundIndex;
    return i != 0;
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
    outState = AudioEventInstanceState();
    auto* inst = _handles.Get(handle);
    if (!inst || !inst->isValid())
        return false;

    FMOD_STUDIO_PLAYBACK_STATE pbState;
    if (inst->getPlaybackState(&pbState) != FMOD_OK)
        return false;
    else
    {
        switch (pbState)
        {
        case FMOD_STUDIO_PLAYBACK_STARTING:
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

    // A Studio instance may be invalidated by FMOD between any two calls (for
    // example when a stolen one-shot finishes on the mixer thread). Stop at the
    // first failed probe so diagnostics produce at most one useful stale-handle
    // report instead of amplifying it into a full property-query cascade.
    if (inst->getTimelinePosition(&outState.TimelinePosition) != FMOD_OK)
        return false;
    if (inst->getPitch(&outState.Pitch) != FMOD_OK)
        return false;
    if (inst->getVolume(&outState.Volume) != FMOD_OK)
        return false;
    if (inst->getPaused(&outState.IsPaused) != FMOD_OK)
        return false;
    return true;
}

bool FmodEventBackend::GetParameter(AudioEventHandle handle, const AudioParameterId& id, AudioParameterState& outState) const
{
    outState = AudioParameterState();
    auto* instance = _handles.Get(handle);
    if (!instance)
        return false;
    FMOD_RESULT result = FMOD_ERR_INVALID_PARAM;
    if (id.Data1 != 0 || id.Data2 != 0)
    {
        FMOD_STUDIO_PARAMETER_ID parameterId { id.Data1, id.Data2 };
        result = instance->getParameterByID(parameterId, &outState.Value, &outState.FinalValue);
    }
    else if (id.Name.HasChars())
    {
        StringAnsi name(id.Name);
        result = instance->getParameterByName(name.Get(), &outState.Value, &outState.FinalValue);
    }
    outState.IsValid = result == FMOD_OK;
    return outState.IsValid;
}

bool FmodEventBackend::GetGlobalParameter(const AudioParameterId& id, AudioParameterState& outState) const
{
    outState = AudioParameterState();
    if (!_studioSystem)
        return false;
    FMOD_RESULT result = FMOD_ERR_INVALID_PARAM;
    if (id.Data1 != 0 || id.Data2 != 0)
    {
        FMOD_STUDIO_PARAMETER_ID parameterId { id.Data1, id.Data2 };
        result = _studioSystem->getParameterByID(parameterId, &outState.Value, &outState.FinalValue);
    }
    else if (id.Name.HasChars())
    {
        StringAnsi name(id.Name);
        result = _studioSystem->getParameterByName(name.Get(), &outState.Value, &outState.FinalValue);
    }
    outState.IsValid = result == FMOD_OK;
    return outState.IsValid;
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

bool FmodEventBackend::StopBusEvents(const Guid& busId, const StringView& path, AudioStopMode stopMode)
{
    auto* bus = GetBus(busId, path);
    if (!bus)
        return false;
    const FMOD_STUDIO_STOP_MODE mode = stopMode == AudioStopMode::Immediate
        ? FMOD_STUDIO_STOP_IMMEDIATE
        : FMOD_STUDIO_STOP_ALLOWFADEOUT;
    return bus->stopAllEvents(mode) == FMOD_OK;
}

bool FmodEventBackend::GetBusVolume(const Guid& busId, const StringView& path, float& outVolume, float& outFinalVolume) const
{
    outVolume = 0.0f;
    outFinalVolume = 0.0f;
    auto* bus = const_cast<FmodEventBackend*>(this)->GetBus(busId, path);
    return bus && bus->getVolume(&outVolume, &outFinalVolume) == FMOD_OK;
}

bool FmodEventBackend::GetBusMute(const Guid& busId, const StringView& path, bool& outMuted) const
{
    outMuted = false;
    auto* bus = const_cast<FmodEventBackend*>(this)->GetBus(busId, path);
    return bus && bus->getMute(&outMuted) == FMOD_OK;
}

bool FmodEventBackend::GetBusPaused(const Guid& busId, const StringView& path, bool& outPaused) const
{
    outPaused = false;
    auto* bus = const_cast<FmodEventBackend*>(this)->GetBus(busId, path);
    return bus && bus->getPaused(&outPaused) == FMOD_OK;
}

bool FmodEventBackend::SetVCAVolume(const Guid& vcaId, const StringView& path, float volume)
{
    auto* vca = GetVCA(vcaId, path);
    return vca ? vca->setVolume(Math::Saturate(volume)) == FMOD_OK : false;
}

bool FmodEventBackend::GetVCAVolume(const Guid& vcaId, const StringView& path, float& outVolume, float& outFinalVolume) const
{
    outVolume = 0.0f;
    outFinalVolume = 0.0f;
    auto* vca = const_cast<FmodEventBackend*>(this)->GetVCA(vcaId, path);
    return vca ? vca->getVolume(&outVolume, &outFinalVolume) == FMOD_OK : false;
}

void FmodEventBackend::FillRuntimeInfo(FMOD::Studio::EventInstance* instance, const AudioEventHandle& handle, const Guid& eventId, const Guid& ownerId, const StringView& path, int32 playCount, bool isOneShot, AudioEventRuntimeInfo& eventInfo) const
{
    eventInfo.Handle = handle;
    eventInfo.EventId = eventId;
    eventInfo.OwnerId = ownerId;
    eventInfo.Path = path;
    eventInfo.IsOneShot = isOneShot;
    eventInfo.PlayCount = playCount;
    eventInfo.Started = playCount > 0;
    if (!instance || !instance->isValid())
        return;

    FMOD_STUDIO_PLAYBACK_STATE playbackState = FMOD_STUDIO_PLAYBACK_STOPPED;
    if (instance->getPlaybackState(&playbackState) != FMOD_OK)
        return;
    switch (playbackState)
    {
        case FMOD_STUDIO_PLAYBACK_STARTING:
        case FMOD_STUDIO_PLAYBACK_PLAYING:
            eventInfo.PlaybackState = AudioEventPlaybackState::Playing;
            break;
        case FMOD_STUDIO_PLAYBACK_SUSTAINING:
            eventInfo.PlaybackState = AudioEventPlaybackState::Sustaining;
            break;
        case FMOD_STUDIO_PLAYBACK_STOPPING:
            eventInfo.PlaybackState = AudioEventPlaybackState::Stopping;
            break;
        default:
            eventInfo.PlaybackState = AudioEventPlaybackState::Stopped;
            break;
    }
    if (instance->getTimelinePosition(&eventInfo.TimelinePosition) != FMOD_OK)
        return;
    if (instance->getVolume(&eventInfo.Volume, &eventInfo.FinalVolume) != FMOD_OK)
        return;
    if (instance->isVirtual(&eventInfo.IsVirtual) != FMOD_OK)
        return;
    if (instance->getListenerMask(&eventInfo.ListenerMask) != FMOD_OK)
        return;
    eventInfo.Playing = eventInfo.PlaybackState == AudioEventPlaybackState::Playing || eventInfo.PlaybackState == AudioEventPlaybackState::Sustaining;
    eventInfo.TimeSeconds = (float)eventInfo.TimelinePosition * 0.001f;
    eventInfo.RealVoices = eventInfo.IsVirtual || !eventInfo.Playing ? 0 : 1;
    eventInfo.VirtualVoices = eventInfo.IsVirtual && eventInfo.Playing ? 1 : 0;

    FMOD_3D_ATTRIBUTES eventAttributes = {};
    if (instance->get3DAttributes(&eventAttributes) != FMOD_OK)
        return;
    eventInfo.Has3DAttributes = true;
    eventInfo.SourcePositionCentimeters = FmodConvert::FromFmodPositionCentimeters(eventAttributes.position);
    eventInfo.SourcePositionMeters = Vector3(eventAttributes.position.x, eventAttributes.position.y, eventAttributes.position.z);
    if (_listeners.HasItems())
    {
        int32 listenerIndex = 0;
        for (int32 i = 0; i < _listeners.Count(); i++)
        {
            if ((eventInfo.ListenerMask & (1u << i)) != 0)
            {
                listenerIndex = i;
                break;
            }
        }
        eventInfo.ListenerPositionCentimeters = _listeners[listenerIndex].Attributes.Position;
        eventInfo.ListenerPositionMeters = _listeners[listenerIndex].Attributes.Position * FmodConvert::MetersPerFlaxUnit;
        eventInfo.DistanceMeters = (float)Vector3::Distance(eventInfo.SourcePositionMeters, eventInfo.ListenerPositionMeters);
    }

    FMOD::Studio::EventDescription* description = nullptr;
    if (instance->getDescription(&description) != FMOD_OK)
        return;
    if (description)
    {
        FMOD_STUDIO_LOADING_STATE sampleState;
        if (description->getSampleLoadingState(&sampleState) == FMOD_OK)
            eventInfo.SampleLoadingState = (int32)sampleState;
        if (eventInfo.Path.IsEmpty())
        {
            char resolvedPath[512] = {};
            int32 retrieved = 0;
            if (description->getPath(resolvedPath, ARRAY_COUNT(resolvedPath), &retrieved) == FMOD_OK)
                eventInfo.Path = String(StringAnsi(resolvedPath));
        }
        description->getMinMaxDistance(&eventInfo.MinimumDistanceMeters, &eventInfo.MaximumDistanceMeters);
    }

    eventInfo.Audibility = eventInfo.FinalVolume;
    eventInfo.ChannelCount = eventInfo.Playing && !eventInfo.IsVirtual ? 1 : 0;
    eventInfo.Audible = eventInfo.Playing && !eventInfo.IsVirtual && eventInfo.FinalVolume > 0.0001f;
    eventInfo.ReachingOutput = eventInfo.Audible && _combinedOutputRms > 0.00001f;
    if (!eventInfo.Started)
        eventInfo.SilenceCause = TEXT("instance has not started");
    else if (!eventInfo.Playing)
        eventInfo.SilenceCause = TEXT("timeline is not playing");
    else if (eventInfo.ListenerMask == 0)
        eventInfo.SilenceCause = TEXT("listener mask is zero");
    else if (_listeners.IsEmpty() && eventInfo.Has3DAttributes)
        eventInfo.SilenceCause = TEXT("no active listener");
    else if (eventInfo.IsVirtual)
        eventInfo.SilenceCause = TEXT("FMOD virtualized the instance");
    else if (eventInfo.FinalVolume <= 0.0001f)
        eventInfo.SilenceCause = TEXT("final event volume is zero");
    else if (_combinedOutputRms <= 0.00001f)
        eventInfo.SilenceCause = TEXT("signal is not reaching the master output");
}

void FmodEventBackend::CaptureDiagnostics(AudioDiagnosticsSnapshot& outSnapshot)
{
    // Bank unloads and middleware-owned one-shot completion can invalidate the
    // native handle before the queued STOPPED callback reaches this thread.
    // Evict those slots first so a diagnostic snapshot never amplifies one stale
    // instance into a cascade of failing property queries.
    Array<AudioEventHandle, InlinedAllocation<8>> staleHandles;
    const auto& diagnosticSlots = _handles.GetSlots();
    for (int32 i = 0; i < diagnosticSlots.Count(); i++)
    {
        const FmodHandleRegistry::Slot& slot = diagnosticSlots[i];
        if (slot.InUse && (!slot.Instance || !slot.Instance->isValid()))
            staleHandles.Add(AudioEventHandle((uint32)i, slot.Generation));
    }
    for (const AudioEventHandle handle : staleHandles)
        ReleaseInstance(handle);

    outSnapshot = AudioDiagnosticsSnapshot();
    outSnapshot.BackendName = GetName();
    outSnapshot.Initialized = _studioSystem != nullptr;
    const auto settings = AudioSettings::Get();
    outSnapshot.LiveUpdateEnabled = _liveUpdateActive;
    outSnapshot.CallbackQueueDepth = _callbacks.GetApproximateDepth();
    outSnapshot.DroppedCallbacks = _callbacks.GetTotalDropped();
    outSnapshot.ChannelReuseNotifications = _channelReuseNotifications.load(std::memory_order_relaxed);
    outSnapshot.ListenerCount = _listeners.Count();
    outSnapshot.CombinedOutputPeak = _combinedOutputPeak;
    outSnapshot.CombinedOutputRms = _combinedOutputRms;
    outSnapshot.CombinedOutputDbfs = _combinedOutputDbfs;
    outSnapshot.OutputClipping = _outputClipping;
    outSnapshot.SecondsSinceNonSilentOutput = _secondsSinceNonSilentOutput;
    outSnapshot.OutputPeak.Resize(_outputPeak.Count());
    outSnapshot.OutputRms.Resize(_outputRms.Count());
    for (int32 i = 0; i < _outputPeak.Count(); i++)
        outSnapshot.OutputPeak[i] = _outputPeak[i];
    for (int32 i = 0; i < _outputRms.Count(); i++)
        outSnapshot.OutputRms[i] = _outputRms[i];
    outSnapshot.LastErrorCode = _lastErrorCode.load(std::memory_order_relaxed);
    if (outSnapshot.LastErrorCode != 0)
        outSnapshot.LastError = String(FMOD_ErrorString((FMOD_RESULT)outSnapshot.LastErrorCode));
    if (_studioSystem)
    {
        FMOD_STUDIO_CPU_USAGE studioCpu;
        FMOD_CPU_USAGE coreCpu;
        _studioSystem->getCPUUsage(&studioCpu, &coreCpu);
        outSnapshot.CpuUsage = studioCpu.update + coreCpu.dsp;
        outSnapshot.StudioUpdateCpu = studioCpu.update;
        outSnapshot.MixerCpu = coreCpu.dsp;
        outSnapshot.StreamCpu = coreCpu.stream;

        int32 currentAlloc, maxAlloc;
        FMOD::Memory_GetStats(&currentAlloc, &maxAlloc, false);
        outSnapshot.MemoryAllocated = (uint64)currentAlloc;
        outSnapshot.MemoryPeak = (uint64)maxAlloc;
        outSnapshot.LoadedBanks = _banks.GetLoadedCount();
        outSnapshot.LoadedSampleDataBanks = _banks.GetSampleDataLoadedCount();
        outSnapshot.ActiveInstances = _handles.GetActiveCount() + _releasedInstances.Count();
        outSnapshot.TotalInstancesCreated = _totalInstancesCreated;
        outSnapshot.TotalPlays = _totalPlays;
        outSnapshot.TotalStopped = _totalStopped;
        outSnapshot.PeakActiveInstances = _peakActiveInstances;
        _banks.Capture(outSnapshot.Banks);
        for (const FmodHandleRegistry::Slot& slot : _handles.GetSlots())
        {
            if (!slot.InUse || !slot.Instance)
                continue;
            const AudioEventHandle diagnosticHandle = slot.CallbackContext ? slot.CallbackContext->Handle : AudioEventHandle();
            AudioEventInstanceState state;
            if (!QueryInstance(diagnosticHandle, state))
                continue;
            AudioEventRuntimeInfo& eventInfo = outSnapshot.Events.AddOne();
            eventInfo.Handle = diagnosticHandle;
            eventInfo.EventId = slot.EventId;
            eventInfo.OwnerId = slot.OwnerId;
            eventInfo.IsOneShot = slot.OneShot;
            eventInfo.PlaybackState = state.PlaybackState;
            eventInfo.TimelinePosition = state.TimelinePosition;
            eventInfo.Volume = state.Volume;
            if (slot.Instance->getVolume(&eventInfo.Volume, &eventInfo.FinalVolume) != FMOD_OK)
                continue;
            if (slot.Instance->isVirtual(&eventInfo.IsVirtual) != FMOD_OK)
                continue;
            eventInfo.RealVoices = eventInfo.IsVirtual || state.PlaybackState == AudioEventPlaybackState::Stopped ? 0 : 1;
            eventInfo.VirtualVoices = eventInfo.IsVirtual && state.PlaybackState != AudioEventPlaybackState::Stopped ? 1 : 0;
            eventInfo.PlayCount = slot.PlayCount;
            eventInfo.Started = slot.PlayCount > 0;
            eventInfo.Playing = state.PlaybackState == AudioEventPlaybackState::Playing || state.PlaybackState == AudioEventPlaybackState::Sustaining;
            eventInfo.TimeSeconds = (float)state.TimelinePosition * 0.001f;
            if (slot.Instance->getListenerMask(&eventInfo.ListenerMask) != FMOD_OK)
                continue;
            FMOD_3D_ATTRIBUTES eventAttributes = {};
            if (slot.Instance->get3DAttributes(&eventAttributes) != FMOD_OK)
                continue;
            eventInfo.Has3DAttributes = true;
            eventInfo.SourcePositionCentimeters = FmodConvert::FromFmodPositionCentimeters(eventAttributes.position);
            eventInfo.SourcePositionMeters = Vector3(eventAttributes.position.x, eventAttributes.position.y, eventAttributes.position.z);
            if (_listeners.HasItems())
            {
                int32 listenerIndex = 0;
                for (int32 i = 0; i < _listeners.Count(); i++)
                {
                    if ((eventInfo.ListenerMask & (1u << i)) != 0)
                    {
                        listenerIndex = i;
                        break;
                    }
                }
                eventInfo.ListenerPositionCentimeters = _listeners[listenerIndex].Attributes.Position;
                eventInfo.ListenerPositionMeters = _listeners[listenerIndex].Attributes.Position * FmodConvert::MetersPerFlaxUnit;
                eventInfo.DistanceMeters = (float)Vector3::Distance(eventInfo.SourcePositionMeters, eventInfo.ListenerPositionMeters);
            }
            FMOD::Studio::EventDescription* description = nullptr;
            if (slot.Instance->getDescription(&description) != FMOD_OK)
                continue;
            if (description)
            {
                FMOD_STUDIO_LOADING_STATE sampleState;
                if (description->getSampleLoadingState(&sampleState) == FMOD_OK)
                    eventInfo.SampleLoadingState = (int32)sampleState;
                char path[512] = {};
                int32 retrieved = 0;
                if (description->getPath(path, ARRAY_COUNT(path), &retrieved) == FMOD_OK)
                    eventInfo.Path = String(StringAnsi(path));
                description->getMinMaxDistance(&eventInfo.MinimumDistanceMeters, &eventInfo.MaximumDistanceMeters);
            }
            // getChannelGroup reports FMOD_ERR_STUDIO_NOT_LOADED for otherwise
            // valid instances whose sample/channel is still being prepared. The
            // global FMOD error callback logs that expected transient, so derive
            // the signal-path state from stable instance properties and the
            // measured master output instead of probing the optional group.
            eventInfo.Audibility = eventInfo.FinalVolume;
            eventInfo.ChannelCount = eventInfo.Playing && !eventInfo.IsVirtual ? 1 : 0;
            eventInfo.Audible = eventInfo.Playing && !eventInfo.IsVirtual && eventInfo.ChannelCount > 0 && eventInfo.FinalVolume > 0.0001f && eventInfo.Audibility > 0.0001f;
            eventInfo.ReachingOutput = eventInfo.Audible && _combinedOutputRms > 0.00001f;
            if (!eventInfo.Started)
                eventInfo.SilenceCause = TEXT("instance has not started");
            else if (!eventInfo.Playing)
                eventInfo.SilenceCause = TEXT("timeline is not playing");
            else if (eventInfo.ListenerMask == 0)
                eventInfo.SilenceCause = TEXT("listener mask is zero");
            else if (_listeners.IsEmpty() && eventInfo.Has3DAttributes)
                eventInfo.SilenceCause = TEXT("no active listener");
            else if (eventInfo.IsVirtual)
                eventInfo.SilenceCause = TEXT("FMOD virtualized the instance");
            else if (eventInfo.ChannelCount == 0)
                eventInfo.SilenceCause = TEXT("event has no real output channel");
            else if (eventInfo.FinalVolume <= 0.0001f)
                eventInfo.SilenceCause = TEXT("final event volume is zero");
            else if (eventInfo.Audibility <= 0.0001f)
                eventInfo.SilenceCause = eventInfo.MaximumDistanceMeters > 0.0f && eventInfo.DistanceMeters > eventInfo.MaximumDistanceMeters
                    ? TEXT("source is beyond the event maximum attenuation distance")
                    : TEXT("final channel-group audibility is zero");
            else if (_combinedOutputRms <= 0.00001f)
                eventInfo.SilenceCause = TEXT("signal is not reaching the master output");
            if (eventInfo.Path.StartsWith(TEXT("snapshot:/")))
            {
                AudioSnapshotRuntimeInfo& snapshotInfo = outSnapshot.Snapshots.AddOne();
                snapshotInfo.Handle = eventInfo.Handle;
                snapshotInfo.Path = eventInfo.Path;
                snapshotInfo.PlaybackState = eventInfo.PlaybackState;
            }
        }
        for (const ReleasedInstanceDiagnostic& diagnostic : _releasedInstances)
        {
            if (!diagnostic.Instance || !diagnostic.Instance->isValid())
                continue;
            AudioEventRuntimeInfo& eventInfo = outSnapshot.Events.AddOne();
            FillRuntimeInfo(diagnostic.Instance, diagnostic.Handle, diagnostic.EventId, diagnostic.OwnerId, diagnostic.Path, diagnostic.PlayCount, diagnostic.IsOneShot, eventInfo);
        }
        // The Studio system remains initialized in editor idle mode, but its
        // banks are intentionally unloaded on play-mode exit. Avoid querying a
        // master bus that cannot exist until the master bank is loaded again.
        if (_banks.GetLoadedCount() > 0)
        {
            if (FMOD::Studio::Bus* masterBus = GetBus(Guid::Empty, TEXT("bus:/")))
            {
                AudioBusRuntimeInfo& busInfo = outSnapshot.Buses.AddOne();
                busInfo.Path = TEXT("bus:/");
                masterBus->getVolume(&busInfo.Volume, &busInfo.FinalVolume);
                masterBus->getMute(&busInfo.Muted);
                masterBus->getPaused(&busInfo.Paused);
                if (busInfo.Muted || busInfo.Paused || busInfo.FinalVolume <= 0.0001f)
                {
                    for (auto& eventInfo : outSnapshot.Events)
                    {
                        eventInfo.ReachingOutput = false;
                        if (eventInfo.Playing)
                            eventInfo.SilenceCause = busInfo.Muted ? TEXT("master bus is muted") : busInfo.Paused ? TEXT("master bus is paused") : TEXT("master bus gain is zero");
                    }
                }
            }
        }
        outSnapshot.ActiveDevice = GetOutputDevice();
        int32 channels = 0;
        _coreSystem->getChannelsPlaying(&channels, &outSnapshot.RealVoices);
        outSnapshot.VirtualVoices = Math::Max(0, channels - outSnapshot.RealVoices);
        FMOD_SPEAKERMODE speakerMode;
        _coreSystem->getSoftwareFormat(&outSnapshot.OutputSampleRate, &speakerMode, &outSnapshot.OutputChannels);
        _coreSystem->getDSPBufferSize(&outSnapshot.DspBufferLength, &outSnapshot.DspBufferCount);
        uint32 version = 0;
        _coreSystem->getVersion(&version);
        outSnapshot.RuntimeVersion = String::Format(TEXT("{0}.{1:00}.{2:00}"), version >> 16, (version >> 8) & 0xff, version & 0xff);
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

    if (type == FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND)
    {
        auto* properties = static_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters);
        if (!properties || !context->ProgrammerSoundPath[0])
            return FMOD_ERR_EVENT_NOTFOUND;
        FMOD::Sound* sound = nullptr;
        const FMOD_RESULT result = context->Backend->_coreSystem->createSound(context->ProgrammerSoundPath, FMOD_CREATESTREAM, nullptr, &sound);
        if (result != FMOD_OK)
            return result;
        properties->sound = reinterpret_cast<FMOD_SOUND*>(sound);
        properties->subsoundIndex = context->ProgrammerSoundSubsound;
        return FMOD_OK;
    }
    if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND)
    {
        auto* properties = static_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters);
        auto* sound = properties ? reinterpret_cast<FMOD::Sound*>(properties->sound) : nullptr;
        if (sound)
            sound->release();
        if (properties)
            properties->sound = nullptr;
        return FMOD_OK;
    }

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
