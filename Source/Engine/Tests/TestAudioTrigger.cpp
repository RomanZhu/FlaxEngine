// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Events/AudioEventBackendNone.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/Actors/AudioTrigger.h"
#include "Engine/Scripting/ScriptingObject.h"
#if USE_EDITOR
#include "Editor/Editor.h"
#endif
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR
namespace
{
    struct AudioTriggerPlayModeScope
    {
        bool Previous;

        explicit AudioTriggerPlayModeScope(bool value)
            : Previous(Editor::IsPlayMode)
        {
            Editor::IsPlayMode = value;
        }

        ~AudioTriggerPlayModeScope()
        {
            Editor::IsPlayMode = Previous;
        }
    };
}
#endif

TEST_CASE("AudioTrigger")
{
#if USE_EDITOR
    AudioTriggerPlayModeScope playMode(true);
#endif
    AudioEventBackendNone backend;
    CHECK(!backend.Init());
    AudioEventSystem::SetBackend(&backend);

    AudioTrigger* trigger = New<AudioTrigger>(ScriptingObject::SpawnParams(Guid::New(), AudioTrigger::TypeInitializer));
    trigger->EventPath = TEXT("event:/Test/Trigger");
    trigger->SetBoxSize(Vector3(200.0f));
    trigger->SetStopOnExit(true);
    trigger->Flags |= ObjectFlags::IsDuringPlay;

    // A listener entering starts one persistent instance exactly once.
    trigger->UpdateListenerPosition(Vector3::Zero);
    CHECK(trigger->GetIsInside());
    CHECK(trigger->GetHandle().IsValid());
    const AudioEventHandle firstHandle = trigger->GetHandle();
    trigger->UpdateListenerPosition(Vector3::Zero);
    CHECK(trigger->GetHandle() == firstHandle);

    // A retained instance that finished naturally can restart on a later boundary entry.
    trigger->SetStopOnExit(false);
    CHECK(AudioEventSystem::Stop(firstHandle, AudioStopMode::Immediate));
    trigger->UpdateListenerPosition(Vector3(1000.0f, 0.0f, 0.0f));
    trigger->UpdateListenerPosition(Vector3::Zero);
    CHECK(trigger->GetHandle() == firstHandle);

    // Exiting releases the instance when StopOnExit is enabled.
    trigger->SetStopOnExit(true);
    trigger->UpdateListenerPosition(Vector3(1000.0f, 0.0f, 0.0f));
    CHECK(!trigger->GetIsInside());
    CHECK(!trigger->GetHandle().IsValid());

    Delete(trigger);
    AudioEventSystem::SetBackend(nullptr);
    backend.Dispose();

    SECTION("Play mode guard")
    {
#if USE_EDITOR
        Editor::IsPlayMode = false;
#endif
        AudioTrigger* editorTrigger = New<AudioTrigger>(ScriptingObject::SpawnParams(Guid::New(), AudioTrigger::TypeInitializer));
        editorTrigger->EventPath = TEXT("event:/Test/EditorOnly");
        editorTrigger->UpdateListenerPosition(Vector3::Zero);
        CHECK(!editorTrigger->GetHandle().IsValid());
        Delete(editorTrigger);
    }
}
