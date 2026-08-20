// Copyright (c) Wojciech Figat. All rights reserved.

#include "FMODAudioManager.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Level/Scene/Scene.h"
#include "Engine/Audio/Events/AudioWorld.h"
#include "Engine/Audio/Events/Actors/AudioZoneVolume.h"
#include "Engine/Audio/Events/Occlusion/AudioOcclusionScheduler.h"
#include "Engine/Audio/Events/Surface/AudioPhysicsInteractionSystem.h"
#include "Engine/Engine/Time.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Debug/DebugDraw.h"

FMODAudioManager::FMODAudioManager(const SpawnParams& params)
    : Actor(params)
{
}

void FMODAudioManager::CaptureDiagnostics()
{
    _sceneDiagnostics.Emitters = AudioWorld::Emitters.Count();
    _sceneDiagnostics.Volumes = AudioWorld::Volumes.Count();
    _sceneDiagnostics.ActiveZones = 0;
    for (auto* volume : AudioWorld::Volumes)
        if (auto* zone = dynamic_cast<AudioZoneVolume*>(volume))
            if (zone->IsActiveInHierarchy() && zone->IsDuringPlay() && zone->GetMixerWeight() > 0.001f)
                _sceneDiagnostics.ActiveZones++;
    const auto& scheduler = AudioWorld::GetOcclusionScheduler();
    _sceneDiagnostics.OcclusionQueries = scheduler.GetQueriesThisFrame();
    _sceneDiagnostics.OcclusionDeferred = scheduler.GetDeferredThisFrame();
    _sceneDiagnostics.PersistentInteractions = AudioWorld::GetSurfaceInteractions().GetPersistentLoopCount();
}

AudioDiagnosticsSnapshot FMODAudioManager::GetSnapshot() const
{
    AudioDiagnosticsSnapshot snapshot;
    AudioEventSystem::CaptureDiagnostics(snapshot);
    snapshot.OcclusionQueriesThisFrame = _sceneDiagnostics.OcclusionQueries;
    snapshot.OcclusionDeferred = _sceneDiagnostics.OcclusionDeferred;
    return snapshot;
}

void FMODAudioManager::StopAllEvents()
{
    AudioEventSystem::StopAll(AudioStopMode::AllowFadeOut);
}

void FMODAudioManager::Update()
{
    const float dt = (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
    _captureElapsed += dt;
    if (_captureElapsed >= Math::Max(DiagnosticsRefreshInterval, 0.05f))
    {
        _captureElapsed = 0.0f;
        CaptureDiagnostics();
    }

    if (!EnableConsoleLogging)
        return;

    const AudioDiagnosticsSnapshot snapshot = GetSnapshot();

    _consoleLogElapsed += dt;
    const float interval = Math::Max(ConsoleLogInterval, 0.1f);
    const bool changed = _lastLoggedInitialized != snapshot.Initialized ||
                         _lastLoggedActiveInstances != snapshot.ActiveInstances ||
                         _lastLoggedLoadedBanks != snapshot.LoadedBanks ||
                         _lastLoggedDroppedCallbacks != snapshot.DroppedCallbacks;
    if (_consoleLogElapsed < interval || (LogOnlyOnChanges && !changed))
        return;

    _consoleLogElapsed = 0.0f;
    LOG(Info, "FMOD diagnostics: {0}, events {1}, voices real/virtual {2}/{3}, banks {4}, callbacks {5} (dropped {6})",
        snapshot.Initialized ? TEXT("ready") : TEXT("not ready"), snapshot.ActiveInstances,
        snapshot.RealVoices, snapshot.VirtualVoices, snapshot.LoadedBanks,
        snapshot.CallbackQueueDepth, snapshot.DroppedCallbacks);
    _lastLoggedInitialized = snapshot.Initialized;
    _lastLoggedActiveInstances = snapshot.ActiveInstances;
    _lastLoggedLoadedBanks = snapshot.LoadedBanks;
    _lastLoggedDroppedCallbacks = snapshot.DroppedCallbacks;
}

void FMODAudioManager::OnEnable()
{
    Actor::OnEnable();
    GetScene()->Ticking.Update.AddTick<FMODAudioManager, &FMODAudioManager::Update>(this);
    CaptureDiagnostics();
}

void FMODAudioManager::OnDisable()
{
    GetScene()->Ticking.Update.RemoveTick(this);
    Actor::OnDisable();
}

#if USE_EDITOR
void FMODAudioManager::OnDebugDraw()
{
    if (ShowOcclusionRays)
    {
        const auto& records = AudioWorld::GetOcclusionScheduler().GetDebugRecords();
        for (const auto& record : records)
        {
            const Color color = record.Deferred ? Color::Yellow : (record.Hits > 0 ? Color::Red : Color::Green);
            DEBUG_DRAW_LINE(record.ListenerPosition, record.EmitterPosition, color, 0.0f, true);
        }
    }
    Actor::OnDebugDraw();
}
#endif
