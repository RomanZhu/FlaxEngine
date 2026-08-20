// Copyright (c) Wojciech Figat. All rights reserved.

#include "FmodCallbackQueue.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Core/Math/Math.h"

FmodCallbackQueue::FmodCallbackQueue()
    : _enqueuePosition(0)
    , _dequeuePosition(0)
    , _totalEnqueued(0)
    , _totalDropped(0)
{
    for (uint64 i = 0; i < Capacity; i++)
        _cells[i].Sequence.store(i, std::memory_order_relaxed);
}

bool FmodCallbackQueue::Enqueue(const FmodCallbackRecord& record)
{
    // Bounded Vyukov-style queue. Producers never allocate or take a game-thread lock.
    Cell* cell;
    uint64 position = _enqueuePosition.load(std::memory_order_relaxed);
    for (;;)
    {
        cell = &_cells[position & CapacityMask];
        const uint64 sequence = cell->Sequence.load(std::memory_order_acquire);
        const intptr difference = (intptr)(sequence - position);
        if (difference == 0)
        {
            if (_enqueuePosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                break;
        }
        else if (difference < 0)
        {
            _totalDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        else
        {
            position = _enqueuePosition.load(std::memory_order_relaxed);
        }
    }

    cell->Record = record;
    cell->Sequence.store(position + 1, std::memory_order_release);
    _totalEnqueued.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool FmodCallbackQueue::Dequeue(FmodCallbackRecord& outRecord)
{
    const uint64 position = _dequeuePosition.load(std::memory_order_relaxed);
    Cell& cell = _cells[position & CapacityMask];
    if (cell.Sequence.load(std::memory_order_acquire) != position + 1)
        return false;

    outRecord = cell.Record;
    _dequeuePosition.store(position + 1, std::memory_order_relaxed);
    cell.Sequence.store(position + Capacity, std::memory_order_release);
    return true;
}

void FmodCallbackQueue::Clear()
{
    // Call only after FMOD has stopped producing callbacks (initialization/disposal).
    _enqueuePosition.store(0, std::memory_order_relaxed);
    _dequeuePosition.store(0, std::memory_order_relaxed);
    _totalEnqueued.store(0, std::memory_order_relaxed);
    _totalDropped.store(0, std::memory_order_relaxed);
    for (uint64 i = 0; i < Capacity; i++)
        _cells[i].Sequence.store(i, std::memory_order_relaxed);
}

uint64 FmodCallbackQueue::GetTotalEnqueued() const
{
    return _totalEnqueued.load(std::memory_order_relaxed);
}

uint64 FmodCallbackQueue::GetTotalDropped() const
{
    return _totalDropped.load(std::memory_order_relaxed);
}

uint32 FmodCallbackQueue::GetApproximateDepth() const
{
    const uint64 enqueued = _enqueuePosition.load(std::memory_order_relaxed);
    const uint64 dequeued = _dequeuePosition.load(std::memory_order_relaxed);
    return (uint32)Math::Min<uint64>(enqueued - dequeued, Capacity);
}

#endif
