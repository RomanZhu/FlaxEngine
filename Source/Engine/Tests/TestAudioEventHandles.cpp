// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Audio/Events/AudioEventBackendNone.h"
#include <ThirdParty/catch2/catch.hpp>

TEST_CASE("AudioEventHandles")
{
    SECTION("Default and Invalid Handles")
    {
        AudioEventHandle defaultHandle;
        CHECK(!defaultHandle.IsValid());
        CHECK(!defaultHandle);
        CHECK(defaultHandle.Index == 0);
        CHECK(defaultHandle.Generation == 0);

        AudioEventHandle zeroGenHandle(5, 0);
        CHECK(!zeroGenHandle.IsValid());
    }

    SECTION("Valid Handle Equality and Raw Conversion")
    {
        AudioEventHandle h1(12, 42);
        CHECK(h1.IsValid());
        CHECK(h1.Index == 12);
        CHECK(h1.Generation == 42);

        AudioEventHandle h2(12, 42);
        CHECK(h1 == h2);

        AudioEventHandle h3(12, 43);
        CHECK(h1 != h3);

        uint64 raw = h1.Raw();
        AudioEventHandle fromRaw = AudioEventHandle::FromRaw(raw);
        CHECK(fromRaw == h1);
        CHECK(fromRaw.Index == 12);
        CHECK(fromRaw.Generation == 42);
    }

    SECTION("Handle Slot Allocation and Stale Invalidation")
    {
        AudioEventBackendNone backend;
        CHECK(!backend.Init());

        Guid testGuid = Guid::New();
        AudioEventCreateOptions options;
        options.AutoPlay = true;

        AudioEventHandle h1 = backend.CreateInstance(testGuid, StringView::Empty, options);
        CHECK(h1.IsValid());
        CHECK(h1.Index == 0);
        CHECK(h1.Generation == 1);

        AudioEventInstanceState state;
        CHECK(backend.QueryInstance(h1, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Playing);

        // Release h1
        CHECK(backend.ReleaseInstance(h1));

        // Stale handle h1 should no longer be queryable or operable
        AudioEventInstanceState staleState;
        CHECK(!backend.QueryInstance(h1, staleState));
        CHECK(!backend.Play(h1));
        CHECK(!backend.Stop(h1, AudioStopMode::Immediate));
        CHECK(!backend.ReleaseInstance(h1)); // Second release fails

        // Next allocation should reuse slot 0 with incremented generation
        AudioEventHandle h2 = backend.CreateInstance(testGuid, StringView::Empty, options);
        CHECK(h2.IsValid());
        CHECK(h2.Index == 0);
        CHECK(h2.Generation > h1.Generation);
        CHECK(h1 != h2);

        // h2 is valid, h1 remains invalid
        CHECK(backend.QueryInstance(h2, state));
        CHECK(!backend.QueryInstance(h1, staleState));

        // A backend-wide stop invalidates all live handles so callers cannot operate on stale instances.
        AudioEventHandle h3 = backend.CreateInstance(testGuid, StringView::Empty, options);
        AudioEventHandle h4 = backend.CreateInstance(testGuid, StringView::Empty, options);
        CHECK(h3.IsValid());
        CHECK(h4.IsValid());
        CHECK(backend.StopAll(AudioStopMode::Immediate));
        CHECK(!backend.QueryInstance(h2, staleState));
        CHECK(!backend.QueryInstance(h3, staleState));
        CHECK(!backend.QueryInstance(h4, staleState));
        CHECK(!backend.Play(h3));

        backend.Dispose();
    }
}
