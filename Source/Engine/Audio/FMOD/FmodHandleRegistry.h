// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "Engine/Core/Collections/Array.h"
#include <fmod_studio.hpp>

class FmodEventBackend;

/// <summary>
/// Stable FMOD user-data payload. It is allocated independently of the registry's
/// resizable slot array, so a callback can never observe a moved slot address.
/// </summary>
struct FmodInstanceContext
{
    FmodEventBackend* Backend = nullptr;
    AudioEventHandle Handle;
    char ProgrammerSoundPath[512] = {};
    int32 ProgrammerSoundSubsound = -1;
};

/// <summary>
/// Slot-based generational handle registry for FMOD Studio event instances.
/// </summary>
class FmodHandleRegistry
{
public:
    struct Slot
    {
        uint32 Generation = 0;
        FMOD::Studio::EventInstance* Instance = nullptr;
        FmodInstanceContext* CallbackContext = nullptr;
        Guid EventId = Guid::Empty;
        Guid OwnerId = Guid::Empty;
        bool InUse = false;
        bool OneShot = false;
        int32 PlayCount = 0;
    };

private:
    Array<Slot> _slots;
    Array<uint32> _freeIndices;

public:
    FmodHandleRegistry() = default;
    ~FmodHandleRegistry() = default;

    AudioEventHandle Allocate(FMOD::Studio::EventInstance* instance, const Guid& eventId, const Guid& ownerId, bool oneShot = false);
    bool SetCallbackContext(AudioEventHandle handle, FmodInstanceContext* context);
    bool Free(AudioEventHandle handle, FMOD::Studio::EventInstance*& outInstance, FmodInstanceContext*& outContext);
    FMOD::Studio::EventInstance* Get(AudioEventHandle handle) const;
    FmodInstanceContext* GetCallbackContext(AudioEventHandle handle) const;
    bool Validate(AudioEventHandle handle) const;
    bool IsOneShot(AudioEventHandle handle) const;
    void MarkPlayed(AudioEventHandle handle);
    void Clear();

    int32 GetActiveCount() const;
    const Array<Slot>& GetSlots() const { return _slots; }
};

#endif
