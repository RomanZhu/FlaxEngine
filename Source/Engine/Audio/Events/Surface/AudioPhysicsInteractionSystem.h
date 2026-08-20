// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AudioInteractionTypes.h"
#include "Engine/Core/Collections/Array.h"
#include "Engine/Audio/Events/AudioEventHandle.h"
#include "Engine/Physics/Collisions.h"

class AudioSurfaceLibrary;
class PhysicsColliderActor;

/// <summary>Budgeted collision-to-audio interaction dispatcher.</summary>
class FLAXENGINE_API AudioPhysicsInteractionSystem
{
    struct PendingImpact
    {
        AudioImpactContext Context;
        uint64 PairKey = 0;
    };

    Array<PendingImpact> _pending;
    struct PersistentLoop
    {
        uint64 PairKey = 0;
        AudioEventHandle Handle;
        float SmoothedSpeed = 0.0f;
        float SmoothedForce = 0.0f;
        float TimeSinceContact = 0.0f;
    };
    Array<PersistentLoop> _loops;
    int32 _budgetPerFrame = 32;
    float _releaseGracePeriod = 0.15f;
    Array<PhysicsColliderActor*> _colliders;
    struct ContactState
    {
        uint64 PairKey = 0;
        AudioImpactContext Context;
        PhysicsColliderActor* ThisCollider = nullptr;
        PhysicsColliderActor* OtherCollider = nullptr;
        bool Active = true;
    };
    Array<ContactState> _contacts;
    AudioSurfaceLibrary* _library = nullptr;

public:
    void SetBudgetPerFrame(int32 value) { _budgetPerFrame = Math::Max(1, value); }
    void QueueImpact(const AudioImpactContext& context, uint64 pairKey);
    /// <summary>Normalizes and coalesces a native collision callback by rigid-body pair.</summary>
    void QueueCollision(const Collision& collision, uint64 pairKey);
    void SetSurfaceLibrary(AudioSurfaceLibrary* library) { _library = library; }
    void RegisterCollider(PhysicsColliderActor* collider);
    void UnregisterCollider(PhysicsColliderActor* collider);
    void SyncColliders(const Array<PhysicsColliderActor*>& colliders);
    void UpdateContacts(const AudioSurfaceLibrary& library, float dt);
    int32 Flush(const AudioSurfaceLibrary& library);
    void UpdatePersistent(uint64 pairKey, float speed, float force, float dt);
    void UpdatePersistent(const AudioSurfaceLibrary& library, uint64 pairKey, const AudioImpactContext& context, bool rolling, float dt);
    void ReleaseExpired(float dt);
    int32 GetPersistentLoopCount() const { return _loops.Count(); }
    int32 GetPendingImpactCount() const { return _pending.Count(); }
    int32 GetRegisteredColliderCount() const { return _colliders.Count(); }
    void Clear();

private:
    void OnCollision(const Collision& collision);
    void OnCollisionExit(const Collision& collision);
    void OnColliderDeleted(ScriptingObject* object);
    static uint64 GetPairKey(const Collision& collision);
    static AudioImpactContext MakeImpactContext(const Collision& collision);
};
