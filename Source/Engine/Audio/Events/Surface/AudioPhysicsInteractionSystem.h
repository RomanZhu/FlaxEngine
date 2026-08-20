// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioInteractionTypes.h"
#include "Engine/Core/Collections/Array.h"

class AudioSurfaceLibrary;

/// <summary>Budgeted collision-to-audio interaction dispatcher.</summary>
class FLAXENGINE_API AudioPhysicsInteractionSystem
{
    struct PendingImpact
    {
        AudioImpactContext Context;
        uint64 PairKey = 0;
    };

    Array<PendingImpact> _pending;
    int32 _budgetPerFrame = 32;

public:
    void SetBudgetPerFrame(int32 value) { _budgetPerFrame = Math::Max(1, value); }
    void QueueImpact(const AudioImpactContext& context, uint64 pairKey);
    int32 Flush(const AudioSurfaceLibrary& library);
    void Clear() { _pending.Clear(); }
};
