// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioOcclusionScheduler.h"
#include "Engine/Audio/Events/Actors/AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Physics/Types.h"
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
    RayCastHit hitBuffer[16];

    while (processed < _budgetPerFrame && processed < count)
    {
        if (_cursor >= count)
            _cursor = 0;

        auto& item = _items[_cursor];
        if (item.Emitter && item.Emitter->IsActuallyPlaying())
        {
            Vector3 emitterPos = item.Emitter->GetPosition();
            Vector3 delta = emitterPos - listenerPosition;
            float distance = (float)delta.Length();

            if (distance > 10.0f)
            {
                Vector3 direction = delta / distance;
                int32 hits = scene->RayCastAllNonAlloc(listenerPosition, direction, Span<RayCastHit>(hitBuffer, 16), distance, _occlusionLayerMask, false);
                item.TargetOcclusion = hits > 0 ? 1.0f : 0.0f;
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
    const float filterSpeed = 10.0f;
    for (int32 i = 0; i < _items.Count(); i++)
    {
        auto& item = _items[i];
        if (item.Emitter && item.Emitter->IsActuallyPlaying())
        {
            item.CurrentOcclusion = Math::Lerp(item.CurrentOcclusion, item.TargetOcclusion, Math::Saturate(dt * filterSpeed));
            item.Emitter->SetParameter(TEXT("Occlusion"), item.CurrentOcclusion);
        }
    }
}

void AudioOcclusionScheduler::Clear()
{
    _items.Clear();
    _cursor = 0;
}
