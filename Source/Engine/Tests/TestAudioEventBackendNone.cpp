// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Audio/Events/AudioEventBackendNone.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Audio/Events/Actors/AudioEmitter.h"
#include "Engine/Audio/Events/Assets/AudioEvent.h"
#include "Engine/Scripting/ScriptingObject.h"
#if USE_EDITOR
#include "Editor/Editor.h"
#endif
#include <ThirdParty/catch2/catch.hpp>

#if USE_EDITOR
namespace
{
    struct AudioTestPlayModeScope
    {
        bool Previous;

        explicit AudioTestPlayModeScope(bool value)
            : Previous(Editor::IsPlayMode)
        {
            Editor::IsPlayMode = value;
        }

        ~AudioTestPlayModeScope()
        {
            Editor::IsPlayMode = Previous;
        }
    };
}
#endif

TEST_CASE("AudioEventBackendNone")
{
    SECTION("Lifecycle and Basic Operations")
    {
#if USE_EDITOR
        AudioTestPlayModeScope playMode(true);
#endif
        AudioEventBackendNone backend;
        CHECK(!backend.Init());
        CHECK(backend.GetType() == AudioEventBackendType::None);
        CHECK(String(backend.GetName()) == TEXT("Null"));

        AudioEventSystem::SetBackend(&backend);
        CHECK(AudioEventSystem::GetBackend() == &backend);
        CHECK(AudioEventSystem::GetBackendType() == AudioEventBackendType::None);
        CHECK(AudioEventSystem::GetBackendName() == TEXT("Null"));

        // Bank operations
        Guid bankId = Guid::New();
        CHECK(!AudioEventSystem::IsBankLoaded(bankId));
        CHECK(AudioEventSystem::LoadBank(bankId));
        CHECK(AudioEventSystem::IsBankLoaded(bankId));
        CHECK(AudioEventSystem::UnloadBank(bankId));
        CHECK(!AudioEventSystem::IsBankLoaded(bankId));

        const String bankPath = TEXT("Banks/Test.bank");
        CHECK(AudioEventSystem::LoadBank(Guid::Empty, bankPath));
        CHECK(AudioEventSystem::UnloadBank(Guid::Empty, bankPath));
        CHECK(AudioEventSystem::LoadBank(bankId, bankPath));
        CHECK(AudioEventSystem::UnloadBank(Guid::Empty, bankPath));
        CHECK(!AudioEventSystem::IsBankLoaded(bankId));
        CHECK(AudioEventSystem::LoadBank(Guid::Empty, TEXT("Content/Audio/Banks/Test.bank")));
        AudioDiagnosticsSnapshot bankDiag;
        AudioEventSystem::CaptureDiagnostics(bankDiag);
        CHECK(bankDiag.LoadedBanks == 1);
        CHECK(AudioEventSystem::UnloadBank(Guid::Empty, TEXT("Content/Audio/Banks/Test.bank")));

        // Instance creation & parameter setting
        Guid eventId = Guid::New();
        AudioEventCreateOptions options;
        options.AutoPlay = false;
        options.Attributes = Audio3DAttributes(Vector3(100.0f, 0.0f, 0.0f), Vector3::Zero, Vector3::Forward, Vector3::Up);

        AudioEventHandle handle = AudioEventSystem::CreateInstance(eventId, StringView::Empty, options);
        CHECK(handle.IsValid());

        AudioEventInstanceState state;
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Stopped);
        CHECK(state.Volume == 1.0f);
        CHECK(state.Pitch == 1.0f);

        // State transitions
        CHECK(AudioEventSystem::Play(handle));
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Playing);

        // Update simulation
        backend.Update(0.1f);
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.TimelinePosition == 100); // 100 ms after 0.1s update

        CHECK(AudioEventSystem::Pause(handle));
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Paused);
        CHECK(state.IsPaused);

        CHECK(AudioEventSystem::Stop(handle, AudioStopMode::Immediate));
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Stopped);

        // Volume & Pitch
        CHECK(AudioEventSystem::SetVolume(handle, 0.75f));
        CHECK(AudioEventSystem::SetPitch(handle, 1.25f));
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.Volume == 0.75f);
        CHECK(state.Pitch == 1.25f);

        // Parameters
        AudioParameterId param(Guid::New(), TEXT("Speed"));
        CHECK(AudioEventSystem::SetParameter(handle, param, 42.0f));
        CHECK(AudioEventSystem::SetGlobalParameter(param, 99.0f));

        // Diagnostics
        AudioDiagnosticsSnapshot diag;
        AudioEventSystem::CaptureDiagnostics(diag);
        CHECK(diag.ActiveInstances == 1);

        // Release
        CHECK(AudioEventSystem::ReleaseInstance(handle));
        AudioEventSystem::CaptureDiagnostics(diag);
        CHECK(diag.ActiveInstances == 0);

#if USE_EDITOR
        // System playback APIs reject creation while the editor is not in play mode.
        Editor::IsPlayMode = false;
        CHECK(!AudioEventSystem::CreateInstance(eventId, StringView::Empty, options).IsValid());
#endif

        AudioEventSystem::SetBackend(nullptr);
        backend.Dispose();
    }

    SECTION("Persistent Asset and Owner Operations")
    {
#if USE_EDITOR
        AudioTestPlayModeScope playMode(true);
#endif
        AudioEventBackendNone backend;
        CHECK(!backend.Init());
        AudioEventSystem::SetBackend(&backend);

        AudioEvent* audioEvent = New<AudioEvent>(ScriptingObject::SpawnParams(Guid::New(), AudioEvent::TypeInitializer));
        audioEvent->BackendId = Guid::New();
        audioEvent->Path = TEXT("event:/Test/Motor");
        AudioEmitter* owner = New<AudioEmitter>(ScriptingObject::SpawnParams(Guid::New(), AudioEmitter::TypeInitializer));
        owner->Flags |= ObjectFlags::IsDuringPlay;
        owner->SetPosition(Vector3(100.0f, 200.0f, 300.0f));

        AudioParameterValue initialSpeed;
        initialSpeed.Id = AudioParameterId(TEXT("Speed"));
        initialSpeed.Value = 0.25f;
        const AudioEventHandle handle = AudioEventSystem::Play(audioEvent, owner, Span<AudioParameterValue>(&initialSpeed, 1));
        CHECK(handle.IsValid());

        AudioEventInstanceState state;
        CHECK(AudioEventSystem::QueryInstance(handle, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Playing);
        AudioParameterState parameterState;
        CHECK(AudioEventSystem::GetParameter(handle, initialSpeed.Id, parameterState));
        CHECK(parameterState.Value == 0.25f);

        // Repeated owner-addressed play retriggers the same logical instance.
        CHECK(AudioEventSystem::Play(audioEvent, owner) == handle);
        CHECK(AudioEventSystem::SetParameter(audioEvent, owner, initialSpeed.Id, 0.75f));
        CHECK(AudioEventSystem::GetParameter(handle, initialSpeed.Id, parameterState));
        CHECK(parameterState.Value == 0.75f);

        CHECK(AudioEventSystem::Stop(audioEvent, owner, AudioStopMode::Immediate));
        CHECK(!AudioEventSystem::QueryInstance(handle, state));
        CHECK(!AudioEventSystem::SetParameter(audioEvent, owner, initialSpeed.Id, 1.0f));

        const AudioEventHandle positioned = AudioEventSystem::PlayAt(audioEvent, Vector3(400.0f, 500.0f, 600.0f));
        CHECK(positioned.IsValid());
        CHECK(AudioEventSystem::QueryInstance(positioned, state));
        CHECK(state.PlaybackState == AudioEventPlaybackState::Playing);
        CHECK(AudioEventSystem::StopAndRelease(positioned, AudioStopMode::Immediate));

        AudioEventSystem::SetBackend(nullptr);
        Delete(owner);
        Delete(audioEvent);
        backend.Dispose();
    }
}
