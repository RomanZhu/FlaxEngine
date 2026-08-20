// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioWorld.h"
#include "AudioEventSystem.h"
#include "AudioZoneMixer.h"
#include "Occlusion/AudioOcclusionScheduler.h"
#include "Actors/AudioVolumeBase.h"
#include "Actors/AudioEmitter.h"
#include "Actors/AudioZoneVolume.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Engine/Engine.h"

Array<AudioEmitter*> AudioWorld::Emitters;
Array<AudioVolumeBase*> AudioWorld::Volumes;
AudioOcclusionScheduler AudioWorld::Occlusion;

void AudioWorld::Register(AudioEmitter* emitter)
{
    if (emitter && !Emitters.Contains(emitter))
    {
        Emitters.Add(emitter);
        Occlusion.Register(emitter);
    }
}

void AudioWorld::Unregister(AudioEmitter* emitter)
{
    if (emitter)
    {
        Emitters.Remove(emitter);
        Occlusion.Unregister(emitter);
    }
}

void AudioWorld::Register(AudioVolumeBase* volume)
{
    if (volume && !Volumes.Contains(volume))
        Volumes.Add(volume);
}

void AudioWorld::Unregister(AudioVolumeBase* volume)
{
    if (volume)
        Volumes.Remove(volume);
}

void AudioWorld::Update(float dt)
{
    if (!Engine::IsPlayMode())
        return;

    for (int32 i = 0; i < Emitters.Count(); i++)
    {
        auto* emitter = Emitters[i];
        if (emitter && emitter->IsActiveInHierarchy() && emitter->IsDuringPlay())
        {
            emitter->UpdateVelocity(dt);
            emitter->Push3DAttributes();
        }
    }

    // Use the first active listener for volume evaluation. The aggregation pass
    // remains deterministic and can be extended to weighted listeners later.
    AudioListener* activeListener = nullptr;
    for (int32 i = 0; i < Audio::Listeners.Count(); i++)
    {
        auto* listener = Audio::Listeners[i];
        if (listener && listener->IsActiveInHierarchy() && listener->IsDuringPlay())
        {
            activeListener = listener;
            break;
        }
    }
    if (!activeListener)
        return;

    const Vector3 listenerPosition = activeListener->GetPosition();
    for (int32 i = 0; i < Volumes.Count(); i++)
    {
        auto* volume = Volumes[i];
        if (volume && volume->IsActiveInHierarchy() && volume->IsDuringPlay())
            volume->UpdateListenerPosition(listenerPosition);
    }

    Array<AudioZoneVolume*> zones;
    for (int32 i = 0; i < Volumes.Count(); i++)
    {
        if (auto* zone = dynamic_cast<AudioZoneVolume*>(Volumes[i]))
            zones.Add(zone);
    }
    AudioZoneMixer::Apply(zones);
    Occlusion.Update(listenerPosition, dt);
}
