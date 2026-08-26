// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioZoneMixer.h"
#include "Actors/AudioZoneVolume.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Core/Math/Math.h"

namespace
{
    struct ZoneTargetState
    {
        String Key;
        AudioZoneTargetType Type = AudioZoneTargetType::Snapshot;
        Guid TargetId = Guid::Empty;
        String TargetPath;
        AudioParameterId TargetParameter;
        Guid WinnerId = Guid::Empty;
        bool Active = false;
        bool BaselineCaptured = false;
        float BaselineVolume = 1.0f;
        float BaselineVca = 1.0f;
        bool BaselineMute = false;
        float BaselineGlobal = 0.0f;
    };

    Array<ZoneTargetState> TargetStates;

    ZoneTargetState* FindTargetState(const String& key)
    {
        for (auto& state : TargetStates)
            if (state.Key == key)
                return &state;
        return nullptr;
    }

    ZoneTargetState& GetTargetState(AudioZoneVolume* zone)
    {
        const String key = zone->GetMixerTargetKey();
        if (auto* state = FindTargetState(key))
            return *state;
        ZoneTargetState state;
        state.Key = key;
        state.Type = zone->GetTargetType();
        state.TargetId = zone->GetTargetId();
        state.TargetPath = zone->GetTargetPath();
        state.TargetParameter = zone->GetTargetParameter();
        TargetStates.Add(state);
        return TargetStates.Last();
    }

    bool CaptureBaseline(ZoneTargetState& state)
    {
        if (state.BaselineCaptured)
            return true;
        bool captured = true;
        switch (state.Type)
        {
        case AudioZoneTargetType::BusVolume:
        {
            float finalVolume = 1.0f;
            state.BaselineVolume = 1.0f;
            captured = AudioEventSystem::GetBusVolume(state.TargetId, state.TargetPath, state.BaselineVolume, finalVolume);
            break;
        }
        case AudioZoneTargetType::BusMute:
            state.BaselineMute = false;
            captured = AudioEventSystem::GetBusMute(state.TargetId, state.TargetPath, state.BaselineMute);
            break;
        case AudioZoneTargetType::VCAVolume:
        {
            float finalVolume = 1.0f;
            state.BaselineVca = 1.0f;
            captured = AudioEventSystem::GetVCAVolume(state.TargetId, state.TargetPath, state.BaselineVca, finalVolume);
            break;
        }
        case AudioZoneTargetType::GlobalParameter:
        {
            AudioParameterState parameter;
            if (AudioEventSystem::GetGlobalParameter(state.TargetParameter, parameter))
                state.BaselineGlobal = parameter.Value;
            else
                captured = false;
            break;
        }
        default:
            break;
        }
        state.BaselineCaptured = captured;
        return captured;
    }

    void RestoreBaseline(ZoneTargetState& state)
    {
        switch (state.Type)
        {
        case AudioZoneTargetType::BusVolume:
            AudioEventSystem::SetBusVolume(state.TargetId, state.TargetPath, state.BaselineVolume);
            break;
        case AudioZoneTargetType::BusMute:
            AudioEventSystem::SetBusMute(state.TargetId, state.TargetPath, state.BaselineMute);
            break;
        case AudioZoneTargetType::VCAVolume:
            AudioEventSystem::SetVCAVolume(state.TargetId, state.TargetPath, state.BaselineVca);
            break;
        case AudioZoneTargetType::GlobalParameter:
            if (state.TargetParameter.IsValid())
                AudioEventSystem::SetGlobalParameter(state.TargetParameter, state.BaselineGlobal);
            break;
        default:
            break;
        }
        // A future activation must capture the value authored after this zone
        // relinquished the target, rather than reusing stale process-wide state.
        state.BaselineCaptured = false;
    }


    AudioZoneVolume* FindZone(const Array<AudioZoneContribution>& contributions, const Guid& id)
    {
        if (!id.IsValid())
            return nullptr;
        for (const auto& contribution : contributions)
            if (contribution.Zone && contribution.Zone->GetID() == id)
                return contribution.Zone;
        return nullptr;
    }
}

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

    Array<String> seenKeys;
    seenKeys.EnsureCapacity(contributions.Count());

    // Each authored target is written once. Highest priority wins; equal-priority
    // zones use the strongest weight and then a stable actor name tie-break.
    for (int32 i = 0; i < contributions.Count(); i++)
    {
        auto& contribution = contributions[i];
        if (!contribution.Zone)
            continue;
        const String key = contribution.Zone->GetMixerTargetKey();
        int32 bestPriority = contribution.Priority;
        int32 winner = i;
        for (int32 j = 0; j < contributions.Count(); j++)
        {
            auto& other = contributions[j];
            if (!other.Zone || other.Zone->GetMixerTargetKey() != key)
                continue;
            if (other.Priority > bestPriority)
            {
                bestPriority = other.Priority;
                winner = j;
            }
        }
        float finalWeight = 0.0f;
        for (int32 j = 0; j < contributions.Count(); j++)
        {
            auto& other = contributions[j];
            if (!other.Zone || other.Zone->GetMixerTargetKey() != key || other.Priority != bestPriority)
                continue;
            if (other.Weight > finalWeight || (other.Weight == finalWeight && other.Zone->GetName() < contributions[winner].Zone->GetName()))
            {
                finalWeight = other.Weight;
                winner = j;
            }
        }
        if (winner == i)
        {
            if (seenKeys.Contains(key))
                continue;
            seenKeys.Add(key);
            auto& state = GetTargetState(contribution.Zone);
            if (finalWeight > 0.001f)
            {
                if (!CaptureBaseline(state))
                    continue;
                if (state.WinnerId != contribution.Zone->GetID())
                {
                    if (auto* previous = FindZone(contributions, state.WinnerId))
                        previous->ReleaseMixerInstance();
                    for (int32 otherIndex = 0; otherIndex < contributions.Count(); otherIndex++)
                    {
                        auto* otherZone = contributions[otherIndex].Zone;
                        if (otherZone && otherZone != contribution.Zone && otherZone->GetMixerTargetKey() == key)
                            otherZone->ReleaseMixerInstance();
                    }
                    state.WinnerId = contribution.Zone->GetID();
                }
                if (contribution.Zone->EnsureMixerInstance(finalWeight))
                {
                    contribution.Zone->ApplyMixerWeight(finalWeight);
                    state.Active = true;
                }
            }
            else if (state.Active)
            {
                if (auto* previous = FindZone(contributions, state.WinnerId))
                    previous->ReleaseMixerInstance();
                state.WinnerId = Guid::Empty;
                state.Active = false;
                RestoreBaseline(state);
            }
        }
    }

    // Disabled or out-of-range zones are not present in contributions. Restore
    // targets whose final contribution disappeared this frame.
    for (int32 i = TargetStates.Count() - 1; i >= 0; i--)
    {
        auto& state = TargetStates[i];
        if (!state.Active || seenKeys.Contains(state.Key))
            continue;
        if (auto* previous = FindZone(contributions, state.WinnerId))
            previous->ReleaseMixerInstance();
        state.WinnerId = Guid::Empty;
        state.Active = false;
        RestoreBaseline(state);
    }
}

void AudioZoneMixer::Remove(AudioZoneVolume* zone)
{
    if (!zone)
        return;
    const Guid id = zone->GetID();
    for (auto& state : TargetStates)
    {
        if (!state.Active || state.WinnerId != id)
            continue;
        // AudioWorld calls this while the actor is still alive, so the retained
        // snapshot handle can be released without dereferencing a stale pointer.
        zone->ReleaseMixerInstance();
        state.WinnerId = Guid::Empty;
        state.Active = false;
        RestoreBaseline(state);
    }
}
