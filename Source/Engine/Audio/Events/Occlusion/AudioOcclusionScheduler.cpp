// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioOcclusionScheduler.h"
#include "Engine/Audio/Events/Actors/AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Physics/Types.h"
#include "AudioOcclusionMaterial.h"
#include "Engine/Core/Math/Math.h"

void AudioOcclusionScheduler::Register(AudioEmitter* emitter)
{
    if (!emitter)
        return;

    for (int32 i = 0; i < _items.Count(); i++)
    {
        if (_items[i].Emitter == emitter)
            return;
    }

    OcclusionItem item;
    item.Emitter = emitter;
    _items.Add(item);
}

void AudioOcclusionScheduler::Unregister(AudioEmitter* emitter)
{
    for (int32 i = 0; i < _items.Count(); i++)
    {
        if (_items[i].Emitter == emitter)
        {
            _items.RemoveAt(i);
            break;
        }
    }
}

void AudioOcclusionScheduler::Update(PhysicsScene* scene, const Vector3& listenerPosition, float dt)
{
    if (!scene || _items.IsEmpty())
        return;

    int32 count = _items.Count();
    int32 processed = 0;
    RayCastHit hitBuffer[32];

    while (processed < _budgetPerFrame && processed < count)
    {
        if (_cursor >= count)
            _cursor = 0;

        auto& item = _items[_cursor];
        if (item.Emitter && item.Emitter->GetPhysicsScene() != scene)
        {
            _cursor++;
            processed++;
            continue;
        }
        if (item.Emitter && item.Emitter->IsActuallyPlaying())
        {
            const AudioOcclusionSettings& settings = item.Emitter->GetOcclusionSettings();
            if (!settings.Enabled)
            {
                item.TargetOcclusion = 0.0f;
                item.TimeSinceQuery = 0.0f;
                _cursor++;
                processed++;
                continue;
            }
            Vector3 emitterPos = item.Emitter->GetPosition();
            Vector3 delta = emitterPos - listenerPosition;
            float distance = (float)delta.Length();
            item.Distance = distance;
            item.TimeSinceQuery += dt;

            const float interval = distance < settings.MaxDistance * 0.5f ? settings.NearInterval : settings.FarInterval;
            if (distance > 0.01f && distance <= settings.MaxDistance && item.TimeSinceQuery >= interval)
            {
                Vector3 direction = delta / distance;
                const int32 rayCount = settings.Mode == AudioOcclusionMode::MultipleRays ? Math::Clamp((int32)settings.Rays, 1, 16) : 1;
                float totalTransmission = 0.0f;
                for (int32 ray = 0; ray < rayCount; ray++)
                {
                    // Deterministic offsets avoid shimmer while still sampling the source aperture.
                    Vector3 offset = Vector3::Zero;
                    if (ray > 0 && settings.Radius > 0.0f)
                    {
                        const float angle = (float)ray * 2.39996323f;
                        Vector3 side = Vector3::Cross(direction, Vector3::Up);
                        if (side.LengthSquared() < 0.0001f)
                            side = Vector3::Cross(direction, Vector3::Right);
                        side.Normalize();
                        Vector3 up = Vector3::Cross(side, direction);
                        up.Normalize();
                        offset = (side * Math::Cos(angle) + up * Math::Sin(angle)) * (settings.Radius * ((float)ray / (float)rayCount));
                    }
                    const Vector3 origin = listenerPosition + offset;
                    const Vector3 target = emitterPos + offset;
                    const Vector3 rayDelta = target - origin;
                    const float rayDistance = (float)rayDelta.Length();
                    if (rayDistance <= 0.01f)
                    {
                        totalTransmission += 1.0f;
                        continue;
                    }
                    const Vector3 rayDirection = rayDelta / rayDistance;
                    int32 hits = scene->RayCastAllNonAlloc(origin, rayDirection, Span<RayCastHit>(hitBuffer, 32), rayDistance, settings.CollisionMask & _occlusionLayerMask, false);
                    float transmission = 1.0f;
                    for (int32 hitIndex = 0; hitIndex < hits; hitIndex++)
                        transmission *= AudioOcclusionMaterial::ResolveTransmission(hitBuffer[hitIndex].Material);
                    totalTransmission += transmission;
                }
                item.TargetOcclusion = 1.0f - totalTransmission / (float)rayCount;
                item.TimeSinceQuery = 0.0f;
            }
            else
            {
                item.TargetOcclusion = 0.0f;
            }
        }

        _cursor++;
        processed++;
    }

    // Smooth filter occlusion towards target and update emitter parameter
    for (int32 i = 0; i < _items.Count(); i++)
    {
        auto& item = _items[i];
        if (item.Emitter && item.Emitter->IsActuallyPlaying())
        {
            const AudioOcclusionSettings& settings = item.Emitter->GetOcclusionSettings();
            const float filterSpeed = item.TargetOcclusion > item.CurrentOcclusion ? settings.Attack : settings.Release;
            item.CurrentOcclusion = Math::Lerp(item.CurrentOcclusion, item.TargetOcclusion, Math::Saturate(dt * filterSpeed));
            if (settings.Enabled && settings.Parameter.HasChars() && (item.LastSentOcclusion < 0.0f || Math::Abs(item.CurrentOcclusion - item.LastSentOcclusion) > 0.01f))
            {
                if (item.Emitter->SetParameter(settings.Parameter, item.CurrentOcclusion))
                    item.LastSentOcclusion = item.CurrentOcclusion;
            }
        }
    }
}

void AudioOcclusionScheduler::Update(const Vector3& listenerPosition, float dt)
{
    Array<PhysicsScene*> scenes;
    for (int32 i = 0; i < _items.Count(); i++)
    {
        auto* emitter = _items[i].Emitter;
        if (emitter && emitter->GetPhysicsScene() && !scenes.Contains(emitter->GetPhysicsScene()))
        {
            scenes.Add(emitter->GetPhysicsScene());
            Update(emitter->GetPhysicsScene(), listenerPosition, dt);
        }
    }
}

void AudioOcclusionScheduler::Clear()
{
    _items.Clear();
    _cursor = 0;
}
