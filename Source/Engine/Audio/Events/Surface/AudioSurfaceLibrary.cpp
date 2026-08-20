// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioSurfaceLibrary.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Physics/PhysicalMaterial.h"
#include "AudioSurfaceResolver.h"

const AudioSurfaceProfile* AudioSurfaceLibrary::GetProfile(Tag surfaceTag) const
{
    return AudioSurfaceResolver::Resolve(*this, surfaceTag);
}

bool AudioSurfaceLibrary::TryGetProfile(Tag surfaceTag, AudioSurfaceProfile& outProfile) const
{
    const auto* profile = GetProfile(surfaceTag);
    if (profile)
    {
        outProfile = *profile;
        return true;
    }
    return false;
}

void AudioSurfaceLibrary::PlayFootstep(const RayCastHit& hit, float volume)
{
    Tag tag;
    if (hit.Material)
    {
        tag = hit.Material->Tag;
    }

    const auto* profile = GetProfile(tag);
    if (profile && profile->FootstepEvent)
    {
        const auto* eventData = profile->FootstepEvent->GetInstance<AudioEvent>();
        if (eventData)
        {
            Audio3DAttributes attrs(hit.Point, Vector3::Zero, hit.Normal, Vector3::Up);
            AudioEventSystem::PlayOneShot(eventData->BackendId, eventData->Path, attrs, volume, 1.0f);
        }
    }
}

void AudioSurfaceLibrary::PlayImpact(Tag surfaceTag, const Vector3& position, float impulse, float volume)
{
    const auto* profile = GetProfile(surfaceTag);
    if (profile && profile->ImpactEvent)
    {
        const auto* eventData = profile->ImpactEvent->GetInstance<AudioEvent>();
        if (eventData)
        {
            Audio3DAttributes attrs(position, Vector3::Zero, Vector3::Forward, Vector3::Up);
            AudioEventSystem::PlayOneShot(eventData->BackendId, eventData->Path, attrs, volume, 1.0f);
        }
    }
}

void AudioSurfaceLibrary::PlayImpact(const AudioImpactContext& context, float volume) const
{
    Tag tag;
    if (context.MaterialA)
        tag = context.MaterialA->Tag;
    if (!tag && context.MaterialB)
        tag = context.MaterialB->Tag;
    const auto* profile = GetProfile(tag);
    const auto& event = profile && profile->Interactions.Impact ? profile->Interactions.Impact : (profile ? profile->ImpactEvent : AssetReference<JsonAsset>());
    if (event)
    {
        const auto* eventData = event->GetInstance<AudioEvent>();
        if (eventData)
        {
            Audio3DAttributes attrs(context.Point, context.RelativeVelocity, context.Normal, Vector3::Up);
            AudioEventSystem::PlayOneShot(eventData->BackendId, eventData->Path, attrs, volume, 1.0f);
        }
    }
}
