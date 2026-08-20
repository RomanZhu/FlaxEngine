// Copyright (c) Wojciech Figat. All rights reserved.

#include "FMODAudioManager.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Level/Scene/Scene.h"

FMODAudioManager::FMODAudioManager(const SpawnParams& params)
    : Actor(params)
{
}

void FMODAudioManager::CaptureDiagnostics()
{
    AudioEventSystem::CaptureDiagnostics(_snapshot);
}

void FMODAudioManager::StopAllEvents()
{
    AudioEventSystem::StopAll(AudioStopMode::AllowFadeOut);
}

void FMODAudioManager::Update()
{
    CaptureDiagnostics();
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
