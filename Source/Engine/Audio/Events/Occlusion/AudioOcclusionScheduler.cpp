// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioOcclusionScheduler.h"
#include "Engine/Audio/Events/Actors/AudioEmitter.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Physics/Types.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "AudioOcclusionMaterial.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Platform/Platform.h"

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
    PROFILE_CPU_NAMED("Audio.Occlusion");
    if (!scene || _items.IsEmpty())
        return;

    if (_batchDepth == 0)
    {
        _queriesThisFrame = 0;
        _deferredThisFrame = 0;
        _debugRecords.Clear();
        _frameStartTime = Platform::GetTimeSeconds();
        for (auto& item : _items)
            if (item.Emitter && item.Emitter->GetPhysicsScene() == scene)
                item.TimeSinceQuery += dt;
    }
    const int32 frame = _batchDepth > 0 ? _frameCounter : ++_frameCounter;

    int32 count = _items.Count();
    int32 processed = 0;
    RayCastHit hitBuffer[32];

    while (processed < _budgetPerFrame && processed < count)
    {
        int32 selected = -1;
        float bestScore = -MAX_float;
        for (int32 candidateIndex = 0; candidateIndex < count; candidateIndex++)
        {
            auto& candidate = _items[candidateIndex];
            if (candidate.LastUpdateFrame == frame || !candidate.Emitter || candidate.Emitter->GetPhysicsScene() != scene)
                continue;
            const float score = (float)candidate.Emitter->GetOcclusionPriority() * 1000000.0f - candidate.Distance + candidate.TimeSinceQuery * 1000.0f;
            if (_batchDepth > 0 && !candidate.HasSelectedListener)
                continue;
            if (selected < 0 || score > bestScore)
            {
                selected = candidateIndex;
                bestScore = score;
            }
        }
        if (selected < 0)
            break;
        auto& item = _items[selected];
        item.LastUpdateFrame = frame;
        const Vector3 effectiveListenerPosition = _batchDepth > 0 && item.HasSelectedListener ? item.SelectedListenerPosition : listenerPosition;
        AudioOcclusionDebugRecord debug;
        debug.Emitter = item.Emitter;
        debug.ListenerPosition = effectiveListenerPosition;
        debug.ListenerIndex = item.SelectedListenerIndex;
        if (item.Emitter && item.Emitter->IsActuallyPlaying())
        {
            const AudioOcclusionSettings& settings = item.Emitter->GetOcclusionSettings();
            if (!settings.Enabled)
            {
                item.TargetOcclusion = 0.0f;
                item.TimeSinceQuery = 0.0f;
                processed++;
                continue;
            }
            Vector3 emitterPos = item.Emitter->GetPosition();
            debug.EmitterPosition = emitterPos;
            Vector3 delta = emitterPos - effectiveListenerPosition;
            float distance = (float)delta.Length();
            item.Distance = distance;
            debug.Distance = distance;
            debug.Rays = settings.Rays;
            const float interval = distance < settings.MaxDistance * 0.5f ? settings.NearInterval : settings.FarInterval;
            if (distance > 0.01f && distance <= settings.MaxDistance && item.TimeSinceQuery >= interval)
            {
                const int32 rayCount = settings.Mode == AudioOcclusionMode::MultipleRays ? Math::Clamp((int32)settings.Rays, 1, 16) : 1;
                // Reserve the entire multi-ray query against the shared frame
                // budget. A query must never partially consume the ray budget.
                if (_queriesThisFrame + rayCount > _budgetPerFrame || (_timeBudgetMs > 0.0f && (Platform::GetTimeSeconds() - _frameStartTime) * 1000.0 > _timeBudgetMs))
                {
                    debug.Deferred = true;
                    _deferredThisFrame++;
                    processed++;
                    _debugRecords.Add(debug);
                    continue;
                }
                Vector3 direction = delta / distance;
                float totalTransmission = 0.0f;
                bool deferred = false;
                for (int32 ray = 0; ray < rayCount; ray++)
                {
                    // Check the time budget immediately before every individual
                    // ray. The reservation above enforces the count budget;
                    // this check prevents a long scene query from overrunning
                    // the shared wall-clock budget.
                    if (_timeBudgetMs > 0.0f && (Platform::GetTimeSeconds() - _frameStartTime) * 1000.0 > _timeBudgetMs)
                    {
                        deferred = true;
                        break;
                    }
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
                    const Vector3 origin = effectiveListenerPosition + offset;
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
                    _queriesThisFrame++;
                    debug.Hits += hits;
                    float transmission = 1.0f;
                    for (int32 hitIndex = 0; hitIndex < hits; hitIndex++)
                    {
                        auto* collider = hitBuffer[hitIndex].Collider;
                        bool selfHit = false;
                        for (Actor* owner = collider; owner; owner = owner->GetParent())
                        {
                            if (owner == item.Emitter)
                            {
                                selfHit = true;
                                break;
                            }
                        }
                        if (!selfHit)
                            transmission *= AudioOcclusionMaterial::ResolveTransmission(hitBuffer[hitIndex].Material);
                    }
                    totalTransmission += transmission;
                }
                if (deferred)
                {
                    debug.Deferred = true;
                }
                else
                {
                    item.TargetOcclusion = 1.0f - totalTransmission / (float)rayCount;
                    item.TimeSinceQuery = 0.0f;
                }
            }
            else
            {
                item.TargetOcclusion = 0.0f;
                if (distance > settings.MaxDistance || item.TimeSinceQuery < interval)
                    debug.Deferred = true;
            }
        }

        processed++;
        debug.CurrentOcclusion = item.CurrentOcclusion;
        debug.TargetOcclusion = item.TargetOcclusion;
        _debugRecords.Add(debug);
        if (debug.Deferred)
            _deferredThisFrame++;
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
    Array<Vector3> listeners;
    listeners.Add(listenerPosition);
    Update(listeners, dt);
}

void AudioOcclusionScheduler::Update(const Array<Vector3>& listenerPositions, float dt)
{
    if (listenerPositions.IsEmpty())
        return;
    _batchDepth++;
    _queriesThisFrame = 0;
    _deferredThisFrame = 0;
    _debugRecords.Clear();
    _frameStartTime = Platform::GetTimeSeconds();
    ++_frameCounter;
    // A single middleware parameter cannot represent different occlusion values
    // per listener. Select the nearest eligible listener deterministically (the
    // compact listener index breaks equal-distance ties) and expose that choice
    // in the debug records.
    for (auto& item : _items)
    {
        item.TimeSinceQuery += dt;
        item.HasSelectedListener = false;
        item.SelectedListenerIndex = -1;
        if (!item.Emitter)
            continue;
        float bestDistance = MAX_float;
        for (int32 listenerIndex = 0; listenerIndex < listenerPositions.Count(); listenerIndex++)
        {
            if (listenerIndex >= 32 || (item.Emitter->GetListenerMask() & (1u << listenerIndex)) == 0)
                continue;
            const float distance = (float)(item.Emitter->GetPosition() - listenerPositions[listenerIndex]).LengthSquared();
            if (!item.HasSelectedListener || distance < bestDistance || (distance == bestDistance && listenerIndex < item.SelectedListenerIndex))
            {
                item.HasSelectedListener = true;
                item.SelectedListenerIndex = listenerIndex;
                item.SelectedListenerPosition = listenerPositions[listenerIndex];
                bestDistance = distance;
            }
        }
    }
    Array<PhysicsScene*> scenes;
    for (int32 i = 0; i < _items.Count(); i++)
    {
        auto* emitter = _items[i].Emitter;
        if (emitter && emitter->GetPhysicsScene() && !scenes.Contains(emitter->GetPhysicsScene()))
        {
            scenes.Add(emitter->GetPhysicsScene());
            Update(emitter->GetPhysicsScene(), Vector3::Zero, dt);
        }
    }
    _batchDepth--;
}

void AudioOcclusionScheduler::Clear()
{
    _items.Clear();
    _cursor = 0;
}
