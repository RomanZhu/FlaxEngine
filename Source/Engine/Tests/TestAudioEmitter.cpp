// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Events/AudioEventBackendNone.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/AudioWorld.h"
#include "Engine/Audio/Events/Actors/AudioEmitter.h"
#include "Engine/Audio/Events/Actors/AudioBankLoader.h"
#include "Engine/Scripting/ScriptingObject.h"
#if USE_EDITOR
#include "Editor/Editor.h"
#endif
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR
namespace
{
    struct AudioEmitterPlayModeScope
    {
        bool Previous;

        explicit AudioEmitterPlayModeScope(bool value)
            : Previous(Editor::IsPlayMode)
        {
            Editor::IsPlayMode = value;
        }

        ~AudioEmitterPlayModeScope()
        {
            Editor::IsPlayMode = Previous;
        }
    };
}
#endif

TEST_CASE("AudioEmitter")
{
    SECTION("Emitter Operations and Lifecycle")
    {
#if USE_EDITOR
        AudioEmitterPlayModeScope playMode(true);
#endif
        AudioEventBackendNone backend;
        CHECK(!backend.Init());
        AudioEventSystem::SetBackend(&backend);

        Guid eventGuid = Guid::New();

        AudioEmitter* emitter = New<AudioEmitter>(ScriptingObject::SpawnParams(Guid::New(), AudioEmitter::TypeInitializer));
        emitter->EventPath = TEXT("event:/Test/Shoot");
        emitter->Flags |= ObjectFlags::IsDuringPlay;
        emitter->SetVolume(0.8f);
        emitter->SetPitch(1.1f);

        CHECK(!emitter->IsActuallyPlaying());
        CHECK(emitter->GetPlaybackState() == AudioEventPlaybackState::Stopped);

        emitter->Play();
        CHECK(emitter->GetHandle().IsValid());
        CHECK(emitter->IsActuallyPlaying());
        CHECK(emitter->GetPlaybackState() == AudioEventPlaybackState::Playing);

        // Parameters
        CHECK(emitter->SetParameter(TEXT("PitchShift"), 2.0f));

        emitter->Pause();
        CHECK(emitter->GetPlaybackState() == AudioEventPlaybackState::Paused);

        emitter->Stop();
        CHECK(!emitter->GetHandle().IsValid());
        CHECK(!emitter->IsActuallyPlaying());

#if USE_EDITOR
        Editor::IsPlayMode = false;
        emitter->Play();
        CHECK(!emitter->GetHandle().IsValid());
#endif

        Delete(emitter);
        AudioEventSystem::SetBackend(nullptr);
        backend.Dispose();
    }
}
