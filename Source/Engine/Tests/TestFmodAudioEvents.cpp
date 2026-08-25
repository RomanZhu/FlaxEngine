// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Config.h"
#include <ThirdParty/catch2/catch.hpp>

#if AUDIO_EVENT_API_FMOD

#include "Engine/Audio/FMOD/FmodCallbackQueue.h"
#include "Engine/Audio/FMOD/FmodHandleRegistry.h"
#include "Engine/Audio/FMOD/FmodConvert.h"

TEST_CASE("FMOD GUID conversion preserves Flax asset identifiers")
{
    const Guid source(0x34f54f36, 0x2a904a3c, 0xa280e096, 0x32cf8257);
    const FMOD_GUID converted = FmodConvert::ToFmodGuid(source);
    CHECK(converted.Data1 == 0x34f54f36);
    CHECK(converted.Data2 == 0x4a3c);
    CHECK(converted.Data3 == 0x2a90);
    CHECK(converted.Data4[0] == 0x96);
    CHECK(converted.Data4[3] == 0xa2);
    CHECK(converted.Data4[4] == 0x57);
    CHECK(converted.Data4[7] == 0x32);
    CHECK(FmodConvert::FromFmodGuid(converted) == source);
}

TEST_CASE("FMOD Studio GUID conversion preserves canonical catalog identifiers")
{
    const Guid source(0x803ab263, 0x79c70f6e, 0x2817920e, 0xee88b297);
    const FMOD_GUID converted = FmodConvert::ToFmodStudioGuid(source);
    CHECK(converted.Data1 == 0x803ab263);
    CHECK(converted.Data2 == 0x79c7);
    CHECK(converted.Data3 == 0x0f6e);
    CHECK(converted.Data4[0] == 0x28);
    CHECK(converted.Data4[3] == 0x0e);
    CHECK(converted.Data4[4] == 0xee);
    CHECK(converted.Data4[7] == 0x97);
    CHECK(FmodConvert::FromFmodStudioGuid(converted) == source);
}

TEST_CASE("FMOD spatial conversion uses meters and preserves orientation")
{
    Audio3DAttributes source;
    source.Position = Vector3(100.0, -250.0, 25.0);
    source.Velocity = Vector3(300.0, 0.0, -50.0);
    source.Forward = Vector3::Forward;
    source.Up = Vector3::Up;
    const FMOD_3D_ATTRIBUTES converted = FmodConvert::ToFmodAttributes(source);
    CHECK(Math::NearEqual(converted.position.x, 1.0f));
    CHECK(Math::NearEqual(converted.position.y, -2.5f));
    CHECK(Math::NearEqual(converted.position.z, 0.25f));
    CHECK(Math::NearEqual(converted.velocity.x, 3.0f));
    CHECK(Math::NearEqual(converted.velocity.z, -0.5f));
    CHECK(Math::NearEqual(converted.forward.x, (float)source.Forward.X));
    CHECK(Math::NearEqual(converted.forward.y, (float)source.Forward.Y));
    CHECK(Math::NearEqual(converted.forward.z, (float)source.Forward.Z));
    CHECK(FmodConvert::FromFmodAttributes(converted).Position == source.Position);
    CHECK(FmodConvert::FromFmodAttributes(converted).Velocity == source.Velocity);
}

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
    // The registry's public Get additionally asks FMOD whether the native
    // instance is still valid. The sentinel pointer used by this unit test is
    // deliberately not a real FMOD object, so inspect the validated slot here
    // instead of invoking middleware through that sentinel.
    CHECK(registry.GetSlots()[second.Index].Instance == secondInstance);
}

#endif
