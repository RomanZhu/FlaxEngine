// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Physics/PhysicsScene.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "AudioOcclusionTypes.h"

class AudioEmitter;

/// <summary>Debug telemetry for one scheduled acoustic query.</summary>
struct FLAXENGINE_API AudioOcclusionDebugRecord
{
    AudioEmitter* Emitter = nullptr;
    Vector3 ListenerPosition = Vector3::Zero;
    Vector3 EmitterPosition = Vector3::Zero;
    int32 ListenerIndex = -1;
    float Distance = 0.0f;
    float CurrentOcclusion = 0.0f;
    float TargetOcclusion = 0.0f;
    uint8 Rays = 0;
    int32 Hits = 0;
    bool Deferred = false;
};

/// <summary>
/// Budgeted raycast scheduler that evaluates environmental acoustic occlusion between audio listeners and emitters.
/// </summary>
class FLAXENGINE_API AudioOcclusionScheduler
{
public:
    struct OcclusionItem
    {
        AudioEmitter* Emitter = nullptr;
        float CurrentOcclusion = 0.0f;
        float TargetOcclusion = 0.0f;
        float LastSentOcclusion = -1.0f;
        float TimeSinceQuery = 0.0f;
        float Distance = MAX_float;
        int32 LastUpdateFrame = 0;
        Vector3 SelectedListenerPosition = Vector3::Zero;
        int32 SelectedListenerIndex = -1;
        bool HasSelectedListener = false;
    };

private:
    Array<OcclusionItem> _items;
    int32 _cursor = 0;
    int32 _budgetPerFrame = 16;
    int32 _queriesThisFrame = 0;
    int32 _deferredThisFrame = 0;
    int32 _frameCounter = 0;
    int32 _batchDepth = 0;
    float _timeBudgetMs = 0.5f;
    double _frameStartTime = 0.0;
    uint32 _occlusionLayerMask = MAX_uint32;
    Array<AudioOcclusionDebugRecord> _debugRecords;

public:
    AudioOcclusionScheduler() = default;

    void SetBudgetPerFrame(int32 budget) { _budgetPerFrame = Math::Max(1, budget); }
    void SetLayerMask(uint32 mask) { _occlusionLayerMask = mask; }
    void SetTimeBudgetMs(float value) { _timeBudgetMs = Math::Max(0.0f, value); }
    float GetTimeBudgetMs() const { return _timeBudgetMs; }
    int32 GetQueriesThisFrame() const { return _queriesThisFrame; }
    int32 GetDeferredThisFrame() const { return _deferredThisFrame; }
    const Array<AudioOcclusionDebugRecord>& GetDebugRecords() const { return _debugRecords; }

    void Register(AudioEmitter* emitter);
    void Unregister(AudioEmitter* emitter);

    /// <summary>
    /// Executes budgeted occlusion raycasts and smooth filtering.
    /// </summary>
    void Update(PhysicsScene* scene, const Vector3& listenerPosition, float dt);

    /// <summary>Updates all registered emitters using their owning physics scenes.</summary>
    void Update(const Vector3& listenerPosition, float dt);
    void Update(const Array<Vector3>& listenerPositions, float dt);

    void Clear();
};
