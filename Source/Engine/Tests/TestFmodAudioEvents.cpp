// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Config.h"
#include <ThirdParty/catch2/catch.hpp>

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/FMOD/FmodCallbackQueue.h"
#include "Engine/Audio/FMOD/FmodHandleRegistry.h"

TEST_CASE("FMOD callback queue is bounded and reports overflow")
{
    FmodCallbackQueue queue;
    FmodCallbackRecord record;
    record.Handle = AudioEventHandle(1, 1);
    int32 accepted = 0;
    while (queue.Enqueue(record))
        accepted++;
    CHECK(accepted == 1024);
    CHECK(queue.GetTotalDropped() == 1);
    CHECK(queue.GetApproximateDepth() == 1024);

    FmodCallbackRecord output;
    int32 drained = 0;
    while (queue.Dequeue(output))
        drained++;
    CHECK(drained == accepted);
    CHECK(queue.GetApproximateDepth() == 0);
}

TEST_CASE("FMOD stale generation cannot resolve a reused event slot")
{
    FmodHandleRegistry registry;
    auto* firstInstance = reinterpret_cast<FMOD::Studio::EventInstance*>(1);
    const AudioEventHandle first = registry.Allocate(firstInstance, Guid::New(), Guid::Empty);
    FmodInstanceContext context;
    context.Handle = first;
    CHECK(registry.SetCallbackContext(first, &context));

    FMOD::Studio::EventInstance* releasedInstance = nullptr;
    FmodInstanceContext* releasedContext = nullptr;
    CHECK(registry.Free(first, releasedInstance, releasedContext));
    CHECK(releasedInstance == firstInstance);
    CHECK(releasedContext == &context);
    CHECK_FALSE(registry.Validate(first));

    auto* secondInstance = reinterpret_cast<FMOD::Studio::EventInstance*>(2);
    const AudioEventHandle second = registry.Allocate(secondInstance, Guid::New(), Guid::Empty);
    CHECK(second.Index == first.Index);
    CHECK(second.Generation != first.Generation);
    CHECK(registry.Validate(second));
    CHECK(registry.Get(first) == nullptr);
    CHECK(registry.Get(second) == secondInstance);
}

#endif
