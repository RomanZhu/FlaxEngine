// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioZoneMixer.h"
#include "Actors/AudioZoneVolume.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Core/Math/Math.h"

void AudioZoneMixer::Apply(const Array<AudioZoneVolume*>& zones)
{
    Array<AudioZoneContribution> contributions;
    contributions.EnsureCapacity(zones.Count());
    for (int32 i = 0; i < zones.Count(); i++)
    {
        auto* zone = zones[i];
        if (zone && zone->IsActiveInHierarchy() && zone->IsDuringPlay())
        {
            AudioZoneContribution contribution;
            contribution.Zone = zone;
            contribution.Weight = Math::Saturate(zone->GetMixerWeight());
            contribution.Priority = zone->GetPriority();
            contributions.Add(contribution);
        }
    }

    // Each authored snapshot is a target. Highest priority wins; equal-priority
    // zones blend by weight, with actor registration order as a stable tie-breaker.
    for (int32 i = 0; i < contributions.Count(); i++)
    {
        auto& contribution = contributions[i];
        if (!contribution.Zone)
            continue;
        const String key = contribution.Zone->GetMixerTargetKey();
        float finalWeight = contribution.Weight;
        for (int32 j = i + 1; j < contributions.Count(); j++)
        {
            auto& other = contributions[j];
            if (!other.Zone || other.Zone->GetMixerTargetKey() != key)
                continue;
            if (other.Priority > contribution.Priority)
                finalWeight = other.Weight;
            else if (other.Priority == contribution.Priority)
                finalWeight = Math::Max(finalWeight, other.Weight);
        }
        contribution.Zone->ApplyMixerWeight(finalWeight);
    }
}
