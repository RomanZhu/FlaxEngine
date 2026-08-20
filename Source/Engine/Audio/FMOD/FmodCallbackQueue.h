// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventCallbacks.h"
#include <atomic>

/// <summary>
/// Record of an asynchronous callback event emitted by the FMOD Studio runtime.
/// </summary>
struct FmodCallbackRecord
{
    AudioEventHandle Handle;
    AudioEventCallbackType Type = AudioEventCallbackType::Started;
    int32 TimelinePositionMs = 0;
    int32 Bar = 0;
    int32 Beat = 0;
    float Tempo = 0.0f;
    int32 TimeSignatureUpper = 0;
    int32 TimeSignatureLower = 0;
    char MarkerNameAnsi[96] = { 0 };
};

/// <summary>
/// Bounded multi-producer single-consumer callback queue transferring FMOD events to the main game thread.
/// </summary>
class FmodCallbackQueue
{
private:
    static constexpr uint64 Capacity = 1024;
    static constexpr uint64 CapacityMask = Capacity - 1;

    struct Cell
    {
        std::atomic<uint64> Sequence;
        FmodCallbackRecord Record;
    };

    Cell _cells[Capacity];
    std::atomic<uint64> _enqueuePosition;
    std::atomic<uint64> _dequeuePosition;
    std::atomic<uint64> _totalEnqueued;
    std::atomic<uint64> _totalDropped;

public:
    FmodCallbackQueue();

    bool Enqueue(const FmodCallbackRecord& record);
    bool Dequeue(FmodCallbackRecord& outRecord);
    void Clear();
    uint64 GetTotalEnqueued() const;
    uint64 GetTotalDropped() const;
    uint32 GetApproximateDepth() const;
};

#endif
