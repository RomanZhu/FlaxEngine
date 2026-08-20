// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioPhysicsInteractionSystem.h"
#include "AudioSurfaceLibrary.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Physics/Colliders/Collider.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Physics/Actors/RigidBody.h"
#include "Engine/Physics/PhysicalMaterial.h"
#include "Engine/Profiler/ProfilerCPU.h"

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
    const int32 maxPending = Math::Max(32, _budgetPerFrame * 4);
    if (_pending.Count() >= maxPending)
    {
        int32 weakest = 0;
        for (int32 i = 1; i < _pending.Count(); i++)
            if (_pending[i].Context.Impulse < _pending[weakest].Context.Impulse)
                weakest = i;
        if (context.Impulse <= _pending[weakest].Context.Impulse)
            return;
        _pending[weakest].Context = context;
        _pending[weakest].PairKey = pairKey;
        return;
    }
    PendingImpact pending;
    pending.Context = context;
    pending.PairKey = pairKey;
    _pending.Add(pending);
}

void AudioPhysicsInteractionSystem::QueueCollision(const Collision& collision, uint64 pairKey)
{
    QueueImpact(MakeImpactContext(collision), pairKey);
}

uint64 AudioPhysicsInteractionSystem::GetPairKey(const Collision& collision)
{
    const uint64 a = (uint64)(uintptr)collision.ThisActor;
    const uint64 b = (uint64)(uintptr)collision.OtherActor;
    return a < b ? (a * 0x9E3779B185EBCA87ull) ^ b : (b * 0x9E3779B185EBCA87ull) ^ a;
}

AudioImpactContext AudioPhysicsInteractionSystem::MakeImpactContext(const Collision& collision)
{
    AudioImpactContext context;
    context.RelativeVelocity = collision.GetRelativeVelocity();
    context.RelativeSpeed = (float)context.RelativeVelocity.Length();
    context.Impulse = (float)collision.Impulse.Length();
    if (collision.ContactsCount > 0)
    {
        const ContactPoint* strongest = &collision.Contacts[0];
        for (int32 i = 1; i < collision.ContactsCount; i++)
            if (collision.Contacts[i].Separation < strongest->Separation)
                strongest = &collision.Contacts[i];
        context.Point = strongest->Point;
        context.Normal = strongest->Normal;
        context.NormalSpeed = Math::Abs((float)Vector3::Dot(context.RelativeVelocity, context.Normal));
    }
    if (auto* collider = dynamic_cast<Collider*>(collision.ThisActor))
        context.MaterialA = collider->Material.GetInstance();
    if (auto* collider = dynamic_cast<Collider*>(collision.OtherActor))
        context.MaterialB = collider->Material.GetInstance();
    return context;
}

void AudioPhysicsInteractionSystem::RegisterCollider(PhysicsColliderActor* collider)
{
    if (!collider || _colliders.Contains(collider))
        return;
    _colliders.Add(collider);
    collider->CollisionEnter.Bind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnCollision>(this);
    collider->CollisionExit.Bind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnCollisionExit>(this);
    collider->Deleted.Bind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnColliderDeleted>(this);
}

void AudioPhysicsInteractionSystem::UnregisterCollider(PhysicsColliderActor* collider)
{
    if (!collider)
        return;
    collider->CollisionEnter.Unbind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnCollision>(this);
    collider->CollisionExit.Unbind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnCollisionExit>(this);
    collider->Deleted.Unbind<AudioPhysicsInteractionSystem, &AudioPhysicsInteractionSystem::OnColliderDeleted>(this);
    _colliders.Remove(collider);
}

void AudioPhysicsInteractionSystem::SyncColliders(const Array<PhysicsColliderActor*>& colliders)
{
    for (int32 i = 0; i < colliders.Count(); i++)
    {
        if (colliders[i] && colliders[i]->IsActiveInHierarchy())
            RegisterCollider(colliders[i]);
        else
            UnregisterCollider(colliders[i]);
    }
    for (int32 i = _colliders.Count() - 1; i >= 0; i--)
        if (!colliders.Contains(_colliders[i]))
            UnregisterCollider(_colliders[i]);
}

void AudioPhysicsInteractionSystem::OnColliderDeleted(ScriptingObject* object)
{
    auto* collider = static_cast<PhysicsColliderActor*>(object);
    Array<uint64> pairKeys;
    pairKeys.EnsureCapacity(_contacts.Count());
    for (int32 i = _contacts.Count() - 1; i >= 0; i--)
    {
        if (_contacts[i].ThisCollider == collider || _contacts[i].OtherCollider == collider)
        {
            pairKeys.Add(_contacts[i].PairKey);
            _contacts.RemoveAt(i);
        }
    }
    for (int32 i = _pending.Count() - 1; i >= 0; i--)
    {
        bool remove = false;
        for (int32 pair = 0; pair < pairKeys.Count(); pair++)
            remove |= _pending[i].PairKey == pairKeys[pair];
        if (remove)
            _pending.RemoveAt(i);
    }
    for (int32 i = _loops.Count() - 1; i >= 0; i--)
    {
        bool remove = false;
        for (int32 pair = 0; pair < pairKeys.Count(); pair++)
            remove |= _loops[i].PairKey == pairKeys[pair];
        if (remove)
        {
            if (_loops[i].Handle.IsValid())
            {
                AudioEventSystem::Stop(_loops[i].Handle, AudioStopMode::Immediate);
                AudioEventSystem::ReleaseInstance(_loops[i].Handle);
            }
            _loops.RemoveAt(i);
        }
    }
    UnregisterCollider(collider);
}

void AudioPhysicsInteractionSystem::OnCollision(const Collision& collision)
{
    if (!_library)
        return;
    const uint64 pairKey = GetPairKey(collision);
    const AudioImpactContext context = MakeImpactContext(collision);
    QueueImpact(context, pairKey);
    ContactState* contact = nullptr;
    for (int32 i = 0; i < _contacts.Count(); i++)
    {
        if (_contacts[i].PairKey == pairKey)
        {
            contact = &_contacts[i];
            break;
        }
    }
    if (!contact)
    {
        ContactState state;
        state.PairKey = pairKey;
        state.ThisCollider = collision.ThisActor;
        state.OtherCollider = collision.OtherActor;
        _contacts.Add(state);
        contact = &_contacts.Last();
    }
    contact->Context = context;
    contact->ThisCollider = collision.ThisActor;
    contact->OtherCollider = collision.OtherActor;
    contact->Active = true;
}

void AudioPhysicsInteractionSystem::OnCollisionExit(const Collision& collision)
{
    const uint64 pairKey = GetPairKey(collision);
    for (int32 i = 0; i < _contacts.Count(); i++)
    {
        if (_contacts[i].PairKey == pairKey)
        {
            _contacts[i].Active = false;
            break;
        }
    }
}

void AudioPhysicsInteractionSystem::UpdateContacts(const AudioSurfaceLibrary& library, float dt)
{
    for (int32 i = _contacts.Count() - 1; i >= 0; i--)
    {
        auto& contact = _contacts[i];
        if (contact.Active)
        {
            // Physics emits enter/exit notifications, but not a per-frame stay event.
            // Refresh the relative velocity from the live rigid bodies so persistent
            // loops follow real runtime motion between those notifications.
            if (contact.ThisCollider && contact.OtherCollider)
            {
                auto* thisBody = contact.ThisCollider->GetAttachedRigidBody();
                auto* otherBody = contact.OtherCollider->GetAttachedRigidBody();
                if (thisBody || otherBody)
                {
                    contact.Context.RelativeVelocity = (thisBody ? thisBody->GetLinearVelocity() : Vector3::Zero)
                        - (otherBody ? otherBody->GetLinearVelocity() : Vector3::Zero);
                    contact.Context.RelativeSpeed = (float)contact.Context.RelativeVelocity.Length();
                    contact.Context.NormalSpeed = Math::Abs((float)Vector3::Dot(contact.Context.RelativeVelocity, contact.Context.Normal));
                }
            }
            const bool rolling = contact.Context.NormalSpeed < contact.Context.RelativeSpeed * 0.35f;
            UpdatePersistent(library, contact.PairKey, contact.Context, rolling, dt);
        }
        else
        {
            _contacts.RemoveAt(i);
        }
    }
}

int32 AudioPhysicsInteractionSystem::Flush(const AudioSurfaceLibrary& library)
{
    PROFILE_CPU_NAMED("Audio.SurfaceInteractions");
    const int32 count = Math::Min(_budgetPerFrame, _pending.Count());
    for (int32 i = 0; i < count; i++)
        library.PlayImpact(_pending[i].Context);
    for (int32 i = 0; i < count; i++)
        _pending.RemoveAt(0);
    return count;
}

void AudioPhysicsInteractionSystem::UpdatePersistent(uint64 pairKey, float speed, float force, float dt)
{
    PersistentLoop* loop = nullptr;
    for (int32 i = 0; i < _loops.Count(); i++)
    {
        if (_loops[i].PairKey == pairKey)
        {
            loop = &_loops[i];
            break;
        }
    }
    if (!loop)
    {
        PersistentLoop value;
        value.PairKey = pairKey;
        _loops.Add(value);
        loop = &_loops.Last();
    }
    const float alpha = Math::Saturate(dt * 12.0f);
    loop->SmoothedSpeed = Math::Lerp(loop->SmoothedSpeed, Math::Max(0.0f, speed), alpha);
    loop->SmoothedForce = Math::Lerp(loop->SmoothedForce, Math::Max(0.0f, force), alpha);
    loop->TimeSinceContact = 0.0f;
}

void AudioPhysicsInteractionSystem::UpdatePersistent(const AudioSurfaceLibrary& library, uint64 pairKey, const AudioImpactContext& context, bool rolling, float dt)
{
    if (context.RelativeSpeed <= 1.0f)
        return;
    PersistentLoop* loop = nullptr;
    for (int32 i = 0; i < _loops.Count(); i++)
    {
        if (_loops[i].PairKey == pairKey)
        {
            loop = &_loops[i];
            break;
        }
    }
    if (!loop)
    {
        // Keep persistent voice creation under the same budget as transient contacts.
        if (_loops.Count() >= _budgetPerFrame)
            return;
        PersistentLoop value;
        value.PairKey = pairKey;
        _loops.Add(value);
        loop = &_loops.Last();
        if (const auto* eventData = library.ResolvePersistentEvent(context, rolling))
        {
            if (!AudioEventCatalog::EnsureDependenciesLoaded(eventData))
            {
                _loops.RemoveAt(_loops.Count() - 1);
                return;
            }
            AudioEventCreateOptions options;
            options.AutoPlay = true;
            options.Attributes = Audio3DAttributes(context.Point, context.RelativeVelocity, context.Normal, Vector3::Up);
            _loops.Last().Handle = AudioEventSystem::CreateInstance(eventData->BackendId, eventData->Path, options);
        }
    }
    const float alpha = Math::Saturate(dt * 12.0f);
    loop->SmoothedSpeed = Math::Lerp(loop->SmoothedSpeed, Math::Max(0.0f, context.RelativeSpeed), alpha);
    loop->SmoothedForce = Math::Lerp(loop->SmoothedForce, Math::Max(0.0f, context.Impulse), alpha);
    loop->TimeSinceContact = 0.0f;
    if (loop->Handle.IsValid())
    {
        AudioEventSystem::Set3DAttributes(loop->Handle, Audio3DAttributes(context.Point, context.RelativeVelocity, context.Normal, Vector3::Up));
        AudioEventSystem::SetParameter(loop->Handle, AudioParameterId(TEXT("Speed")), loop->SmoothedSpeed);
        AudioEventSystem::SetParameter(loop->Handle, AudioParameterId(TEXT("Force")), loop->SmoothedForce);
        AudioEventSystem::SetParameter(loop->Handle, AudioParameterId(TEXT("ImpactSpeed")), loop->SmoothedSpeed);
        AudioEventSystem::SetParameter(loop->Handle, AudioParameterId(TEXT("Impulse")), loop->SmoothedForce);
        AudioEventSystem::SetParameter(loop->Handle, AudioParameterId(TEXT("Angle")), ComputeImpactAngle(context));
    }
}

void AudioPhysicsInteractionSystem::ReleaseExpired(float dt)
{
    for (int32 i = _loops.Count() - 1; i >= 0; i--)
    {
        auto& loop = _loops[i];
        loop.TimeSinceContact += dt;
        if (loop.TimeSinceContact >= _releaseGracePeriod)
        {
            if (loop.Handle.IsValid())
            {
                AudioEventSystem::Stop(loop.Handle, AudioStopMode::AllowFadeOut);
                AudioEventSystem::ReleaseInstance(loop.Handle);
            }
            _loops.RemoveAt(i);
        }
    }
}

void AudioPhysicsInteractionSystem::Clear()
{
    for (int32 i = _colliders.Count() - 1; i >= 0; i--)
        UnregisterCollider(_colliders[i]);
    for (auto& loop : _loops)
    {
        if (loop.Handle.IsValid())
        {
            AudioEventSystem::Stop(loop.Handle, AudioStopMode::Immediate);
            AudioEventSystem::ReleaseInstance(loop.Handle);
        }
    }
    _loops.Clear();
    _pending.Clear();
    _contacts.Clear();
}
