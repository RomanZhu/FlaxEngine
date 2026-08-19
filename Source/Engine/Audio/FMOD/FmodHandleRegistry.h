// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventTypes.h"
#include "Engine/Core/Collections/Array.h"
#include <fmod_studio.hpp>

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
        Guid EventId = Guid::Empty;
        Guid OwnerId = Guid::Empty;
        bool InUse = false;
    };

private:
    Array<Slot> _slots;
    Array<uint32> _freeIndices;

public:
    FmodHandleRegistry() = default;
    ~FmodHandleRegistry() = default;

    AudioEventHandle Allocate(FMOD::Studio::EventInstance* instance, const Guid& eventId, const Guid& ownerId);
    bool Free(AudioEventHandle handle, FMOD::Studio::EventInstance*& outInstance);
    FMOD::Studio::EventInstance* Get(AudioEventHandle handle) const;
    bool Validate(AudioEventHandle handle) const;
    void Clear();

    int32 GetActiveCount() const;
    const Array<Slot>& GetSlots() const { return _slots; }
};

#endif
