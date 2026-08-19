// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "Engine/Core/Types/BaseTypes.h"
#include "Engine/Core/Math/Vector3.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Physics/PhysicsScene.h"
#include "Engine/Audio/Events/AudioEventHandle.h"

class AudioEmitter;

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
        int32 LastUpdateFrame = 0;
    };

private:
    Array<OcclusionItem> _items;
    int32 _cursor = 0;
    int32 _budgetPerFrame = 16;
    uint32 _occlusionLayerMask = MAX_uint32;

public:
    AudioOcclusionScheduler() = default;

    void SetBudgetPerFrame(int32 budget) { _budgetPerFrame = Math::Max(1, budget); }
    void SetLayerMask(uint32 mask) { _occlusionLayerMask = mask; }

    void Register(AudioEmitter* emitter);
    void Unregister(AudioEmitter* emitter);

    /// <summary>
    /// Executes budgeted occlusion raycasts and smooth filtering.
    /// </summary>
    void Update(PhysicsScene* scene, const Vector3& listenerPosition, float dt);

    void Clear();
};
