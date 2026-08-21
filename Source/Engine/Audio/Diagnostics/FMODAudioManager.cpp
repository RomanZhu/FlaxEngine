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
#include "Engine/Serialization/Serialization.h"

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

void FMODAudioManager::Serialize(SerializeStream& stream, const void* otherObj)
{
    Actor::Serialize(stream, otherObj);
    SERIALIZE_GET_OTHER_OBJ(FMODAudioManager);
    SERIALIZE(ShowSceneOverlay);
    SERIALIZE(ShowGameOverlay);
    SERIALIZE(ShowEventLabels);
    SERIALIZE(ShowOcclusionRays);
    SERIALIZE(ShowInactiveEmitters);
    SERIALIZE(MaxLabelDistance);
    SERIALIZE(MaxLabels);
    SERIALIZE(HideDistance);
    SERIALIZE(LabelFontSize);
    SERIALIZE(LabelVolumeOpacity);
    SERIALIZE(LabelLifetimeOpacity);
    SERIALIZE(LabelFadeLength);
    SERIALIZE(LabelStartColor);
    SERIALIZE(LabelEndColor);
    SERIALIZE(LabelOutlineColor);
    SERIALIZE(LabelStaleColor);
    SERIALIZE(LabelStartedPulseColor);
    SERIALIZE(LabelStoppedPulseColor);
    SERIALIZE(ShowStaleLabels);
    SERIALIZE(StaleLabelLifetime);
    SERIALIZE(EnableConsoleLogging);
    SERIALIZE(ConsoleLogInterval);
    SERIALIZE(LogOnlyOnChanges);
    SERIALIZE(DiagnosticsRefreshInterval);
}

void FMODAudioManager::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    Actor::Deserialize(stream, modifier);
    DESERIALIZE(ShowSceneOverlay);
    DESERIALIZE(ShowGameOverlay);
    DESERIALIZE(ShowEventLabels);
    DESERIALIZE(ShowOcclusionRays);
    DESERIALIZE(ShowInactiveEmitters);
    DESERIALIZE(MaxLabelDistance);
    DESERIALIZE(MaxLabels);
    DESERIALIZE(HideDistance);
    DESERIALIZE(LabelFontSize);
    DESERIALIZE(LabelVolumeOpacity);
    DESERIALIZE(LabelLifetimeOpacity);
    DESERIALIZE(LabelFadeLength);
    DESERIALIZE(LabelStartColor);
    DESERIALIZE(LabelEndColor);
    DESERIALIZE(LabelOutlineColor);
    DESERIALIZE(LabelStaleColor);
    DESERIALIZE(LabelStartedPulseColor);
    DESERIALIZE(LabelStoppedPulseColor);
    DESERIALIZE(ShowStaleLabels);
    DESERIALIZE(StaleLabelLifetime);
    DESERIALIZE(EnableConsoleLogging);
    DESERIALIZE(ConsoleLogInterval);
    DESERIALIZE(LogOnlyOnChanges);
    DESERIALIZE(DiagnosticsRefreshInterval);
    MaxLabelDistance = Math::Max(0.0f, MaxLabelDistance);
    MaxLabels = Math::Max(0, MaxLabels);
    LabelFontSize = Math::Max(8, LabelFontSize);
    LabelVolumeOpacity = Math::Saturate(LabelVolumeOpacity);
    LabelLifetimeOpacity = Math::Saturate(LabelLifetimeOpacity);
    LabelFadeLength = Math::Max(0.0f, LabelFadeLength);
    StaleLabelLifetime = Math::Max(0.0f, StaleLabelLifetime);
    ConsoleLogInterval = Math::Max(0.1f, ConsoleLogInterval);
    DiagnosticsRefreshInterval = Math::Max(0.05f, DiagnosticsRefreshInterval);
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
