// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodCallbackQueue.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Platform/Platform.h"

bool FmodCallbackQueue::Enqueue(const FmodCallbackRecord& record)
{
    ScopeLock lock(_lock);
    int32 nextHead = (_head + 1) % Capacity;
    if (nextHead == _tail)
    {
        // Queue full — drop callback safely without blocking mixer thread
        return false;
    }
    _records[_head] = record;
    _head = nextHead;
    return true;
}

bool FmodCallbackQueue::Dequeue(FmodCallbackRecord& outRecord)
{
    ScopeLock lock(_lock);
    if (_head == _tail)
        return false;

    outRecord = _records[_tail];
    _tail = (_tail + 1) % Capacity;
    return true;
}

void FmodCallbackQueue::Clear()
{
    ScopeLock lock(_lock);
    _head = 0;
    _tail = 0;
}

#endif
