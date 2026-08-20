// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodHandleRegistry.h"

#if AUDIO_EVENT_API_FMOD

AudioEventHandle FmodHandleRegistry::Allocate(FMOD::Studio::EventInstance* instance, const Guid& eventId, const Guid& ownerId, bool oneShot)
{
    uint32 index = 0;
    if (_freeIndices.HasItems())
    {
        index = _freeIndices.Pop();
    }
    else
    {
        index = (uint32)_slots.Count();
        _slots.AddDefault(1);
    }

    auto& slot = _slots[index];
    slot.Generation++;
    if (slot.Generation == 0)
        slot.Generation = 1;

    slot.InUse = true;
    slot.Instance = instance;
    slot.CallbackContext = nullptr;
    slot.EventId = eventId;
    slot.OwnerId = ownerId;
    slot.OneShot = oneShot;

    return AudioEventHandle(index, slot.Generation);
}

bool FmodHandleRegistry::SetCallbackContext(AudioEventHandle handle, FmodInstanceContext* context)
{
    if (!Validate(handle))
        return false;

    _slots[handle.Index].CallbackContext = context;
    return true;
}

bool FmodHandleRegistry::Free(AudioEventHandle handle, FMOD::Studio::EventInstance*& outInstance, FmodInstanceContext*& outContext)
{
    outInstance = nullptr;
    outContext = nullptr;
    if (!Validate(handle))
        return false;

    auto& slot = _slots[handle.Index];
    outInstance = slot.Instance;
    outContext = slot.CallbackContext;

    slot.Generation++;
    if (slot.Generation == 0)
        slot.Generation = 1;

    slot.InUse = false;
    slot.Instance = nullptr;
    slot.CallbackContext = nullptr;
    slot.EventId = Guid::Empty;
    slot.OwnerId = Guid::Empty;
    slot.OneShot = false;
    _freeIndices.Add(handle.Index);

    return true;
}

FMOD::Studio::EventInstance* FmodHandleRegistry::Get(AudioEventHandle handle) const
{
    if (Validate(handle))
        return _slots[handle.Index].Instance;
    return nullptr;
}

bool FmodHandleRegistry::Validate(AudioEventHandle handle) const
{
    return handle.IsValid() && (int32)handle.Index < _slots.Count() && _slots[handle.Index].InUse && _slots[handle.Index].Generation == handle.Generation;
}

bool FmodHandleRegistry::IsOneShot(AudioEventHandle handle) const
{
    return Validate(handle) && _slots[handle.Index].OneShot;
}

void FmodHandleRegistry::Clear()
{
    _slots.Clear();
    _freeIndices.Clear();
}

int32 FmodHandleRegistry::GetActiveCount() const
{
    int32 count = 0;
    for (int32 i = 0; i < _slots.Count(); i++)
    {
        if (_slots[i].InUse)
            count++;
    }
    return count;
}

#endif
