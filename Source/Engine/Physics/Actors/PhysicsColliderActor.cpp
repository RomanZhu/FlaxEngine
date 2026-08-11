// Copyright (c) Wojciech Figat. All rights reserved.

#include "PhysicsColliderActor.h"
#include "RigidBody.h"

PhysicsColliderActor::PhysicsColliderActor(const SpawnParams& params)
    : Actor(params)
{
}

int32 PhysicsColliderActor::GetPhysicsShapesCount() const
{
    return 0;
}

void* PhysicsColliderActor::GetPhysicsShape(int32 index) const
{
    return nullptr;
}

void PhysicsColliderActor::GetPhysicsShapeActorPose(int32 index, const Vector3& position, const Quaternion& rotation, Vector3& shapePosition, Quaternion& shapeRotation) const
{
    shapePosition = position;
    shapeRotation = rotation;
}

void PhysicsColliderActor::OnCollisionEnter(const Collision& c)
{
    CollisionEnter(c);

    auto rigidBody = GetAttachedRigidBody();
    if (rigidBody)
        rigidBody->OnCollisionEnter(c);
}

void PhysicsColliderActor::OnCollisionExit(const Collision& c)
{
    CollisionExit(c);

    auto rigidBody = GetAttachedRigidBody();
    if (rigidBody)
        rigidBody->OnCollisionExit(c);
}

void PhysicsColliderActor::OnTriggerEnter(PhysicsColliderActor* c)
{
    TriggerEnter(c);

    auto rigidBody = GetAttachedRigidBody();
    if (rigidBody)
        rigidBody->OnTriggerEnter(c);
}

void PhysicsColliderActor::OnTriggerExit(PhysicsColliderActor* c)
{
    TriggerExit(c);

    auto rigidBody = GetAttachedRigidBody();
    if (rigidBody)
        rigidBody->OnTriggerExit(c);
}
