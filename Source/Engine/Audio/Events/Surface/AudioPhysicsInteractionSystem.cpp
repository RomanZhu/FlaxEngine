// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioPhysicsInteractionSystem.h"
#include "AudioSurfaceLibrary.h"

void AudioPhysicsInteractionSystem::QueueImpact(const AudioImpactContext& context, uint64 pairKey)
{
    for (int32 i = 0; i < _pending.Count(); i++)
    {
        if (_pending[i].PairKey == pairKey)
        {
            if (context.Impulse > _pending[i].Context.Impulse)
                _pending[i].Context = context;
            return;
        }
    }
    PendingImpact pending;
    pending.Context = context;
    pending.PairKey = pairKey;
    _pending.Add(pending);
}

int32 AudioPhysicsInteractionSystem::Flush(const AudioSurfaceLibrary& library)
{
    const int32 count = Math::Min(_budgetPerFrame, _pending.Count());
    for (int32 i = 0; i < count; i++)
        library.PlayImpact(_pending[i].Context);
    for (int32 i = 0; i < count; i++)
        _pending.RemoveAt(0);
    return count;
}
