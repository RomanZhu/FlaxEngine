// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioWorld.h"
#include "AudioEventSystem.h"
#include "Actors/AudioVolumeBase.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Engine/Engine.h"

Array<AudioEmitter*> AudioWorld::Emitters;
Array<AudioVolumeBase*> AudioWorld::Volumes;

void AudioWorld::Register(AudioEmitter* emitter)
{
    if (emitter && !Emitters.Contains(emitter))
        Emitters.Add(emitter);
}

void AudioWorld::Unregister(AudioEmitter* emitter)
{
    if (emitter)
        Emitters.Remove(emitter);
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

#include "Actors/AudioEmitter.h"

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

    // Use the first active listener for volume evaluation until multi-listener blending is implemented.
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
}
