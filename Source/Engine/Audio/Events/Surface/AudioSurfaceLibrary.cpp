// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioSurfaceLibrary.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
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
    if (profile)
    {
        const auto& reference = profile->Interactions.Footstep ? profile->Interactions.Footstep : profile->FootstepEvent;
        const auto* eventData = reference ? reference->GetInstance<AudioEvent>() : nullptr;
        if (eventData)
        {
            if (!AudioEventCatalog::EnsureDependenciesLoaded(eventData))
                return;
            Audio3DAttributes attrs(hit.Point, Vector3::Zero, hit.Normal, Vector3::Up);
            AudioEventSystem::PlayOneShot(eventData->BackendId, eventData->Path, attrs, volume, 1.0f);
        }
    }
}

void AudioSurfaceLibrary::PlayLanding(const RayCastHit& hit, float volume)
{
    Tag tag;
    if (hit.Material)
        tag = hit.Material->Tag;
    const auto* profile = GetProfile(tag);
    if (!profile)
        return;
    const auto& reference = profile->Interactions.Landing ? profile->Interactions.Landing : profile->LandEvent;
    if (!reference)
        return;
    const auto* eventData = reference->GetInstance<AudioEvent>();
    if (!eventData || !AudioEventCatalog::EnsureDependenciesLoaded(eventData))
        return;
    const Audio3DAttributes attrs(hit.Point, Vector3::Zero, hit.Normal, Vector3::Up);
    AudioEventSystem::PlayOneShot(eventData->BackendId, eventData->Path, attrs, volume, 1.0f);
}

void AudioSurfaceLibrary::PlayImpact(Tag surfaceTag, const Vector3& position, float impulse, float volume)
{
    const auto* profile = GetProfile(surfaceTag);
    if (profile && profile->ImpactEvent)
    {
        const auto* eventData = profile->ImpactEvent->GetInstance<AudioEvent>();
        if (eventData)
        {
            if (!AudioEventCatalog::EnsureDependenciesLoaded(eventData))
                return;
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
            if (!AudioEventCatalog::EnsureDependenciesLoaded(eventData))
                return;
            Audio3DAttributes attrs(context.Point, context.RelativeVelocity, context.Normal, Vector3::Up);
            AudioEventCreateOptions options;
            options.Attributes = attrs;
            const AudioEventHandle handle = AudioEventSystem::CreateInstance(eventData->BackendId, eventData->Path, options);
            if (!handle.IsValid())
                return;
            const Vector3 direction = context.RelativeSpeed > ZeroTolerance ? context.RelativeVelocity / context.RelativeSpeed : context.Normal;
            const float angle = Math::Acos(Math::Saturate(Math::Abs((float)Vector3::Dot(direction, context.Normal)))) * RadiansToDegrees;
            AudioEventSystem::SetVolume(handle, volume);
            AudioEventSystem::SetParameter(handle, AudioParameterId(TEXT("ImpactSpeed")), context.RelativeSpeed);
            AudioEventSystem::SetParameter(handle, AudioParameterId(TEXT("Impulse")), context.Impulse);
            AudioEventSystem::SetParameter(handle, AudioParameterId(TEXT("Angle")), angle);
            AudioEventSystem::Play(handle);
            AudioEventSystem::ReleaseInstance(handle);
        }
    }
}

const AudioEvent* AudioSurfaceLibrary::ResolvePersistentEvent(const AudioImpactContext& context, bool rolling) const
{
    Tag tag;
    if (context.MaterialA)
        tag = context.MaterialA->Tag;
    if (!tag && context.MaterialB)
        tag = context.MaterialB->Tag;
    const auto* profile = GetProfile(tag);
    if (!profile)
        return nullptr;
    const auto& reference = rolling ? profile->Interactions.RollLoop : profile->Interactions.ScrapeLoop;
    if (!reference)
        return nullptr;
    reference->WaitForLoaded();
    return reference->GetInstance<AudioEvent>();
}
