// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Audio/Config.h"

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Core/Types/StringView.h"

/// <summary>
/// Record of an asynchronous callback event emitted by the FMOD Studio runtime.
/// </summary>
struct FmodCallbackRecord
{
    enum class Types : uint8
    {
        TimelineMarker = 0,
        TimelineBeat = 1,
        SoundStopped = 2,
    };

    Types Type = Types::TimelineMarker;
    AudioEventHandle Handle;
    int32 Position = 0;
    char Name[128] = { 0 };
};

/// <summary>
/// Bounded multi-producer single-consumer callback queue transferring FMOD events to the main game thread.
/// </summary>
class FmodCallbackQueue
{
private:
    static constexpr int32 Capacity = 256;
    FmodCallbackRecord _records[Capacity];
    int32 _head = 0;
    int32 _tail = 0;
    CriticalSection _lock;

public:
    FmodCallbackQueue() = default;

    bool Enqueue(const FmodCallbackRecord& record);
    bool Dequeue(FmodCallbackRecord& outRecord);
    void Clear();
};

#endif
