// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioWorld.h"
#include "AudioEventSystem.h"
#include "AudioEventCallbacks.h"
#include "AudioZoneMixer.h"
#include "Occlusion/AudioOcclusionScheduler.h"
#include "Surface/AudioPhysicsInteractionSystem.h"
#include "Surface/AudioSurfaceLibrary.h"
#include "Actors/AudioVolumeBase.h"
#include "Actors/AudioEmitter.h"
#include "Actors/AudioAreaEmitter.h"
#include "Actors/AudioZoneVolume.h"
#include "Actors/AudioTrigger.h"
#include "Engine/Audio/Audio.h"
#include "Engine/Audio/AudioListener.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Level/Level.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Profiler/ProfilerCPU.h"

Array<AudioEmitter*> AudioWorld::Emitters;
Array<AudioVolumeBase*> AudioWorld::Volumes;
AudioOcclusionScheduler AudioWorld::Occlusion;
AudioPhysicsInteractionSystem AudioWorld::SurfaceInteractions;
AudioSurfaceLibrary* AudioWorld::SurfaceLibrary = nullptr;

AudioOcclusionScheduler& AudioWorld::GetOcclusionScheduler()
{
    return Occlusion;
}

AudioPhysicsInteractionSystem& AudioWorld::GetSurfaceInteractions()
{
    return SurfaceInteractions;
}

void AudioWorld::SetSurfaceLibrary(AudioSurfaceLibrary* library)
{
    SurfaceLibrary = library;
}

void AudioWorld::Register(AudioEmitter* emitter)
{
    if (emitter && !Emitters.Contains(emitter))
    {
        static bool callbackBound = false;
        if (!callbackBound)
        {
            AudioEventSystem::EventCallback.Bind(&AudioWorld::OnEventCallback);
            callbackBound = true;
        }
        Emitters.Add(emitter);
        Occlusion.Register(emitter);
    }
}

void AudioWorld::OnEventCallback(const AudioEventCallback& callback)
{
    for (int32 i = 0; i < Emitters.Count(); i++)
    {
        auto* emitter = Emitters[i];
        if (emitter && emitter->GetHandle() == callback.Handle)
            emitter->HandleEventCallback(callback);
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
    {
        if (auto* zone = dynamic_cast<AudioZoneVolume*>(volume))
            AudioZoneMixer::Remove(zone);
        Volumes.Remove(volume);
    }
}

void AudioWorld::Update(float dt)
{
    PROFILE_CPU_NAMED("AudioWorld.Update");
    if (!Engine::IsPlayMode())
        return;

    SurfaceInteractions.SetSurfaceLibrary(SurfaceLibrary);
    SurfaceInteractions.SyncColliders(Level::GetActors<PhysicsColliderActor>(true));
    if (SurfaceLibrary)
    {
        SurfaceInteractions.UpdateContacts(*SurfaceLibrary, dt);
        SurfaceInteractions.Flush(*SurfaceLibrary);
        SurfaceInteractions.ReleaseExpired(dt);
    }

    for (int32 i = 0; i < Emitters.Count(); i++)
    {
        auto* emitter = Emitters[i];
        if (emitter && emitter->IsActiveInHierarchy() && emitter->IsDuringPlay())
        {
            emitter->UpdateVelocity(dt);
            emitter->Push3DAttributes();
        }
    }

    Array<AudioListener*> activeListeners;
    Array<Vector3> listenerPositions;
    Array<uint32> listenerMasks;
    for (int32 i = 0; i < Audio::Listeners.Count(); i++)
    {
        auto* listener = Audio::Listeners[i];
        if (listener && listener->IsActiveInHierarchy() && listener->IsDuringPlay())
        {
            activeListeners.Add(listener);
            listenerPositions.Add(listener->GetPosition());
            const int32 compactIndex = activeListeners.Count() - 1;
            listenerMasks.Add(compactIndex < 32 ? (1u << compactIndex) : 0u);
        }
    }
    for (int32 i = 0; i < Volumes.Count(); i++)
    {
        auto* trigger = dynamic_cast<AudioTrigger*>(Volumes[i]);
        if (!trigger || !trigger->IsActiveInHierarchy() || !trigger->IsDuringPlay())
            continue;
        if (trigger->GetTargetMode() == AudioTriggerTargetMode::Actor && trigger->TargetActor)
            trigger->UpdateTarget(trigger->TargetActor);
        else
        {
            Array<Actor*> targets;
            targets.EnsureCapacity(activeListeners.Count());
            for (auto* listener : activeListeners)
                targets.Add(listener);
            trigger->UpdateTargets(targets);
        }
    }
    struct ResolvedVolumeSample
    {
        AudioVolumeBase* Volume = nullptr;
        Vector3 ListenerPosition = Vector3::Zero;
        AudioVolumeSample Raw;
        float ResolvedWeight = 0.0f;
    };
    Array<ResolvedVolumeSample> samples;
    samples.EnsureCapacity(Volumes.Count());
    for (int32 i = 0; i < Volumes.Count(); i++)
    {
        if (auto* volume = Volumes[i])
        {
            ResolvedVolumeSample sample;
            sample.Volume = volume;
            samples.Add(sample);
        }
    }

    for (auto* volume : Volumes)
        if (auto* area = dynamic_cast<AudioAreaEmitter*>(volume))
            area->BeginListenerUpdate();

    for (int32 listenerIndex = 0; listenerIndex < activeListeners.Count(); listenerIndex++)
    {
        const uint32 listenerBit = listenerMasks[listenerIndex];
        const Vector3& listenerPosition = listenerPositions[listenerIndex];
        Array<ResolvedVolumeSample> current;
        current.EnsureCapacity(Volumes.Count());
        for (int32 i = 0; i < Volumes.Count(); i++)
        {
            auto* volume = Volumes[i];
            if (!volume || !volume->IsActiveInHierarchy() || !volume->IsDuringPlay() || (volume->GetListenerMask() & listenerBit) == 0)
                continue;
            ResolvedVolumeSample sample;
            sample.Volume = volume;
            sample.ListenerPosition = listenerPosition;
            sample.Raw = volume->Evaluate(listenerPosition);
            sample.ResolvedWeight = sample.Raw.Weight;
            current.Add(sample);
        }
        for (int32 i = 0; i < current.Count(); i++)
        {
            auto& sample = current[i];
            const String& group = sample.Volume->GetBlendGroup();
            if (sample.Volume->GetBlendMode() != AudioVolumeBlendMode::ExclusiveByGroup || group.IsEmpty())
                continue;
            int32 bestPriority = MIN_int32;
            for (int32 j = 0; j < current.Count(); j++)
            {
                const auto& candidate = current[j];
                if (candidate.Volume->GetBlendMode() == AudioVolumeBlendMode::ExclusiveByGroup && candidate.Volume->GetBlendGroup() == group && candidate.Raw.Weight > 0.001f)
                    bestPriority = Math::Max(bestPriority, candidate.Volume->GetPriority());
            }
            if (sample.Raw.Weight <= 0.001f || sample.Volume->GetPriority() != bestPriority)
                sample.ResolvedWeight = 0.0f;
        }
        for (int32 i = 0; i < current.Count(); i++)
        {
            auto& sample = current[i];
            const String& group = sample.Volume->GetBlendGroup();
            if (sample.Volume->GetBlendMode() != AudioVolumeBlendMode::NormalizedByGroup || group.IsEmpty())
                continue;
            float sum = 0.0f;
            for (int32 j = 0; j < current.Count(); j++)
            {
                const auto& candidate = current[j];
                if (candidate.Volume->GetBlendMode() == AudioVolumeBlendMode::NormalizedByGroup && candidate.Volume->GetBlendGroup() == group)
                    sum += candidate.Raw.Weight;
            }
            sample.ResolvedWeight = sum > 0.001f ? sample.Raw.Weight / sum : 0.0f;
        }
        for (int32 i = 0; i < current.Count(); i++)
        {
            const auto& candidate = current[i];
            if (auto* area = dynamic_cast<AudioAreaEmitter*>(candidate.Volume))
                area->ApplyResolvedListenerSample(activeListeners[listenerIndex], listenerIndex, candidate.Raw, candidate.ResolvedWeight);
            for (int32 j = 0; j < samples.Count(); j++)
            {
                if (samples[j].Volume == candidate.Volume && candidate.ResolvedWeight > samples[j].ResolvedWeight)
                    samples[j] = candidate;
            }
        }
    }
    for (int32 i = 0; i < samples.Count(); i++)
        if (samples[i].Volume && !dynamic_cast<AudioAreaEmitter*>(samples[i].Volume) && samples[i].Volume->IsActiveInHierarchy() && samples[i].Volume->IsDuringPlay())
            samples[i].Volume->ApplyResolvedSample(samples[i].ListenerPosition, samples[i].Raw, samples[i].ResolvedWeight);
    for (auto* volume : Volumes)
        if (auto* area = dynamic_cast<AudioAreaEmitter*>(volume))
            area->EndListenerUpdate();

    Array<AudioZoneVolume*> zones;
    for (int32 i = 0; i < Volumes.Count(); i++)
    {
        if (auto* zone = dynamic_cast<AudioZoneVolume*>(Volumes[i]))
            zones.Add(zone);
    }
    AudioZoneMixer::Apply(zones);
    Occlusion.Update(listenerPositions, dt);
}
