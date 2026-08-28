// Copyright (c) Wojciech Figat. All rights reserved.

#include "AudioPhysics.h"
#include "Engine/Audio/Events/AudioEventCatalog.h"
#include "Engine/Audio/Events/AudioEventSystem.h"
#include "Engine/Engine/Time.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Physics/Actors/RigidBody.h"
#include "Engine/Physics/Colliders/Collider.h"
#include "Engine/Physics/PhysicalMaterial.h"
#include "Engine/Core/Log.h"
#include "Engine/Content/Content.h"

AudioPhysics::AudioPhysics(const SpawnParams& params)
    : Script(params)
{
    _tickUpdate = 1;
}

uint64 AudioPhysics::MakePairKey(const ScriptingObject* a, const ScriptingObject* b)
{
    const uint64 x = (uint64)(uintptr)a;
    const uint64 y = (uint64)(uintptr)b;
    return x < y ? (x * 0x9E3779B185EBCA87ull) ^ y : (y * 0x9E3779B185EBCA87ull) ^ x;
}

float AudioPhysics::ComputeIntensity(const AudioPhysicsRule& rule, const AudioImpactContext& context)
{
    switch (rule.IntensitySource)
    {
    case AudioPhysicsIntensitySource::NormalSpeed: return context.NormalSpeed;
    case AudioPhysicsIntensitySource::Impulse: return context.Impulse;
    case AudioPhysicsIntensitySource::KineticEnergyApproximation: return 0.5f * context.RelativeSpeed * context.RelativeSpeed;
    case AudioPhysicsIntensitySource::TangentialSpeed:
        return Math::Sqrt(Math::Max(0.0f, context.RelativeSpeed * context.RelativeSpeed - context.NormalSpeed * context.NormalSpeed));
    default: return context.RelativeSpeed;
    }
}

int32 AudioPhysics::FindRule(const Array<AudioPhysicsRule>& rules, const AudioImpactContext& context, PhysicsColliderActor* other, bool trigger, String& rejection) const
{
    for (int32 i = 0; i < rules.Count(); i++)
    {
        const auto& rule = rules[i];
        if (rule.Events.IsEmpty()) { rejection = TEXT("rule has no event"); continue; }
        if ((trigger && rule.PlayOn == AudioPhysicsContactType::Collision) || (!trigger && rule.PlayOn == AudioPhysicsContactType::Trigger)) { rejection = TEXT("contact type rejected"); continue; }
        Actor* actor = other;
        const auto& c = rule.Condition;
        if (c.IncludeActor && c.IncludeActor != actor) { rejection = TEXT("include Actor did not match"); continue; }
        if (c.ExcludeActor && c.ExcludeActor == actor) { rejection = TEXT("excluded Actor matched"); continue; }
        if (actor)
        {
            const uint32 layer = 1u << Math::Clamp(actor->GetLayer(), 0, 31);
            if ((c.IncludeLayers & layer) == 0 || (c.ExcludeLayers & layer) != 0) { rejection = TEXT("layer rejected"); continue; }
            if (c.IncludeTag && !actor->HasTag(c.IncludeTag)) { rejection = TEXT("include tag did not match"); continue; }
            if (c.ExcludeTag && actor->HasTag(c.ExcludeTag)) { rejection = TEXT("exclude tag matched"); continue; }
        }
        if (context.RelativeSpeed < c.MinimumRelativeSpeed || context.RelativeSpeed > c.MaximumRelativeSpeed) { rejection = TEXT("relative speed outside range"); continue; }
        if (context.Impulse < c.MinimumImpulse || context.Impulse > c.MaximumImpulse) { rejection = TEXT("impulse outside range"); continue; }
        const float intensity = ComputeIntensity(rule, context);
        if (intensity < rule.MinimumIntensity || intensity > rule.MaximumIntensity) { rejection = TEXT("intensity outside range"); continue; }
        return i;
    }
    return -1;
}

bool AudioPhysics::PlayRule(const AudioPhysicsRule& rule, const AudioImpactContext& context, uint64 pairKey, bool persistent, AudioEventHandle& handle, AudioParameterId* resolvedIntensityParameter)
{
    if (rule.Events.IsEmpty())
        return false;
    const int32 eventIndex = (int32)(pairKey % (uint64)rule.Events.Count());
    const auto reference = Content::LoadAsync<JsonAsset>(rule.Events[eventIndex]);
    if (!reference || reference->WaitForLoaded())
    {
        LastExplanation = TEXT("selected event asset is missing or failed to load");
        return false;
    }
    const auto* eventData = reference->GetInstance<AudioEvent>();
    if (!eventData || !AudioEventCatalog::EnsureDependenciesLoaded(eventData))
    {
        LastExplanation = TEXT("required bank or event metadata is unavailable");
        return false;
    }
    LastIntensity = ComputeIntensity(rule, context);
    LastSelectedEvent = eventData->Path;
    AudioEventCreateOptions options;
    options.AutoPlay = false;
    options.ListenerMask = rule.ListenerMask;
    options.OwnerId = GetID();
    options.Attributes = Audio3DAttributes(context.Point, context.RelativeVelocity, context.Normal, Vector3::Up);
    AudioParameterId parameter;
    const bool hasIntensityParameter = rule.IntensityParameter.Name.HasChars() &&
        AudioEventSystem::ResolveParameterId(eventData->BackendId, eventData->Path, rule.IntensityParameter.Name, parameter);
    if (hasIntensityParameter)
    {
        AudioParameterValue initial;
        initial.Id = parameter;
        initial.Value = LastIntensity;
        options.InitialParameters.Add(initial);
    }
    if (resolvedIntensityParameter)
        *resolvedIntensityParameter = hasIntensityParameter ? parameter : AudioParameterId();
    handle = AudioEventSystem::CreateInstance(eventData->BackendId, eventData->Path, options);
    if (!handle.IsValid() || !AudioEventSystem::Play(handle))
    {
        LastExplanation = TEXT("event instance was rejected by the audio backend");
        return false;
    }
    LastExplanation = persistent ? TEXT("friction instance started") : TEXT("one-shot started at strongest contact");
    if (!persistent)
    {
        AudioEventSystem::ReleaseInstance(handle);
        handle = AudioEventHandle();
    }
    return true;
}

AudioPhysics::ContactVoice* AudioPhysics::FindContact(uint64 pairKey)
{
    for (auto& contact : _contacts)
        if (contact.PairKey == pairKey)
            return &contact;
    return nullptr;
}

void AudioPhysics::ProcessEnter(const AudioImpactContext& context, PhysicsColliderActor* other, bool trigger)
{
    const uint64 pairKey = MakePairKey(this, other);
    ContactVoice* contact = FindContact(pairKey);
    if (!contact)
    {
        ContactVoice value;
        value.PairKey = pairKey;
        value.Other = other;
        _contacts.Add(value);
        contact = &_contacts.Last();
    }
    contact->Context = context;
    String rejection;
    if (EnableImpact)
    {
        const int32 ruleIndex = FindRule(ImpactRules, context, other, trigger, rejection);
        if (ruleIndex >= 0)
        {
            const float now = (float)Time::Update.UnscaledTime.GetTotalSeconds();
            if (now - contact->LastImpactTime >= Math::Max(0.0f, ImpactRules[ruleIndex].Cooldown))
            {
                AudioEventHandle ignored;
                if (PlayRule(ImpactRules[ruleIndex], context, pairKey, false, ignored))
                    contact->LastImpactTime = now;
            }
        }
    }
    if (EnableFriction && ActiveFrictionVoices < MaximumFrictionVoices)
    {
        const int32 ruleIndex = FindRule(FrictionRules, context, other, trigger, rejection);
        if (ruleIndex >= 0 && ComputeIntensity(FrictionRules[ruleIndex], context) >= FrictionRules[ruleIndex].StartThreshold)
        {
            contact->FrictionRule = ruleIndex;
            if (PlayRule(FrictionRules[ruleIndex], context, pairKey, true, contact->FrictionHandle, &contact->ResolvedIntensityParameter))
            {
                contact->HasIntensityParameter = contact->ResolvedIntensityParameter.IsValid();
                ActiveFrictionVoices++;
            }
        }
    }
    if (!LastExplanation.HasChars())
        LastExplanation = rejection.HasChars() ? rejection : TEXT("no matching rule");
}

void AudioPhysics::OnCollisionEnter(const Collision& collision)
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
    ProcessEnter(context, collision.OtherActor, false);
}

void AudioPhysics::OnCollisionExit(const Collision& collision) { ProcessExit(collision.OtherActor, false); }

void AudioPhysics::OnTriggerEnter(PhysicsColliderActor* other)
{
    AudioImpactContext context;
    context.Point = other ? other->GetPosition() : GetActor()->GetPosition();
    if (other)
    {
        if (auto* body = other->GetAttachedRigidBody())
            context.RelativeVelocity = body->GetLinearVelocity();
        context.RelativeSpeed = (float)context.RelativeVelocity.Length();
    }
    ProcessEnter(context, other, true);
}

void AudioPhysics::OnTriggerExit(PhysicsColliderActor* other) { ProcessExit(other, true); }

void AudioPhysics::StopContact(ContactVoice& contact)
{
    if (contact.FrictionHandle.IsValid())
    {
        const auto mode = contact.FrictionRule >= 0 && contact.FrictionRule < FrictionRules.Count() ? FrictionRules[contact.FrictionRule].StopMode : AudioStopMode::AllowFadeOut;
        AudioEventInstanceState state;
        if (AudioEventSystem::QueryInstance(contact.FrictionHandle, state))
        {
            AudioEventSystem::Stop(contact.FrictionHandle, mode);
            AudioEventSystem::ReleaseInstance(contact.FrictionHandle);
        }
        contact.FrictionHandle = AudioEventHandle();
        contact.HasIntensityParameter = false;
        ActiveFrictionVoices = Math::Max(0, ActiveFrictionVoices - 1);
    }
}

void AudioPhysics::ProcessExit(PhysicsColliderActor* other, bool trigger)
{
    const uint64 pairKey = MakePairKey(this, other);
    for (int32 i = _contacts.Count() - 1; i >= 0; i--)
    {
        if (_contacts[i].PairKey != pairKey)
            continue;
        StopContact(_contacts[i]);
        if (EnableExit)
        {
            String rejection;
            const int32 ruleIndex = FindRule(ExitRules, _contacts[i].Context, other, trigger, rejection);
            if (ruleIndex >= 0)
            {
                AudioEventHandle ignored;
                PlayRule(ExitRules[ruleIndex], _contacts[i].Context, pairKey, false, ignored);
            }
        }
        _contacts.RemoveAt(i);
    }
}

void AudioPhysics::BindColliders()
{
    UnbindColliders();
    if (auto* own = dynamic_cast<PhysicsColliderActor*>(GetActor()))
        _colliders.Add(own);
    for (auto* collider : GetActor()->GetChildren<PhysicsColliderActor>())
        _colliders.Add(collider);
    for (auto* collider : _colliders)
    {
        collider->CollisionEnter.Bind<AudioPhysics, &AudioPhysics::OnCollisionEnter>(this);
        collider->CollisionExit.Bind<AudioPhysics, &AudioPhysics::OnCollisionExit>(this);
        collider->TriggerEnter.Bind<AudioPhysics, &AudioPhysics::OnTriggerEnter>(this);
        collider->TriggerExit.Bind<AudioPhysics, &AudioPhysics::OnTriggerExit>(this);
    }
}

void AudioPhysics::UnbindColliders()
{
    for (auto* collider : _colliders)
    {
        if (!collider)
            continue;
        collider->CollisionEnter.Unbind<AudioPhysics, &AudioPhysics::OnCollisionEnter>(this);
        collider->CollisionExit.Unbind<AudioPhysics, &AudioPhysics::OnCollisionExit>(this);
        collider->TriggerEnter.Unbind<AudioPhysics, &AudioPhysics::OnTriggerEnter>(this);
        collider->TriggerExit.Unbind<AudioPhysics, &AudioPhysics::OnTriggerExit>(this);
    }
    _colliders.Clear();
}

void AudioPhysics::OnEnable() { BindColliders(); }
void AudioPhysics::OnDisable() { UnbindColliders(); for (auto& c : _contacts) StopContact(c); _contacts.Clear(); }
void AudioPhysics::OnDestroy() { OnDisable(); }

void AudioPhysics::OnUpdate()
{
    const float dt = (float)Time::Update.UnscaledDeltaTime.GetTotalSeconds();
    for (auto& contact : _contacts)
    {
        if (!contact.FrictionHandle.IsValid() || contact.FrictionRule < 0 || contact.FrictionRule >= FrictionRules.Count())
            continue;
        auto* body = contact.Other ? contact.Other->GetAttachedRigidBody() : nullptr;
        if (body)
        {
            contact.Context.RelativeVelocity = body->GetLinearVelocity();
            contact.Context.RelativeSpeed = (float)contact.Context.RelativeVelocity.Length();
            contact.Context.NormalSpeed = Math::Abs((float)Vector3::Dot(contact.Context.RelativeVelocity, contact.Context.Normal));
        }
        const auto& rule = FrictionRules[contact.FrictionRule];
        const float target = ComputeIntensity(rule, contact.Context);
        const float rate = target > contact.SmoothedIntensity ? rule.Attack : rule.Release;
        contact.SmoothedIntensity = Math::Lerp(contact.SmoothedIntensity, target, Math::Saturate(dt * Math::Max(0.01f, rate)));
        if (contact.SmoothedIntensity < rule.StopThreshold)
        {
            StopContact(contact);
            continue;
        }
        AudioEventSystem::Set3DAttributes(contact.FrictionHandle, Audio3DAttributes(contact.Context.Point, contact.Context.RelativeVelocity, contact.Context.Normal, Vector3::Up));
        if (contact.HasIntensityParameter)
            AudioEventSystem::SetParameter(contact.FrictionHandle, contact.ResolvedIntensityParameter, contact.SmoothedIntensity);
    }
}

bool AudioPhysics::Validate(String& result) const
{
    if (_colliders.IsEmpty() && !dynamic_cast<PhysicsColliderActor*>(GetActor()) && GetActor()->GetChildren<PhysicsColliderActor>().IsEmpty())
    {
        result = TEXT("AudioPhysics requires a collider on its Actor or a direct child.");
        return false;
    }
    if ((EnableImpact && ImpactRules.IsEmpty()) ||
        (EnableFriction && FrictionRules.IsEmpty()) ||
        (EnableExit && ExitRules.IsEmpty()))
    {
        result = TEXT("Enabled sections contain no rules.");
        return false;
    }
    result = TEXT("AudioPhysics configuration is valid.");
    return true;
}

bool AudioPhysics::SimulateImpact(float relativeSpeed, float normalSpeed, float impulse, bool trigger)
{
    AudioImpactContext context;
    context.Point = GetActor()->GetPosition();
    context.RelativeSpeed = Math::Max(0.0f, relativeSpeed);
    context.NormalSpeed = Math::Max(0.0f, normalSpeed);
    context.Impulse = Math::Max(0.0f, impulse);
    context.RelativeVelocity = Vector3::Forward * context.RelativeSpeed;
    ProcessEnter(context, nullptr, trigger);
    return LastSelectedEvent.HasChars();
}
