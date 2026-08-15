// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Physics/CollisionCooking.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/Content/Content.h"
#include "Engine/Physics/Actors/IPhysicsActor.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Physics/Colliders/CharacterController.h"
#include "Engine/Physics/Joints/D6Joint.h"
#include "Engine/Physics/PhysicsBackend.h"
#include "Engine/Physics/Physics.h"
#include "Engine/Physics/PhysicsScene.h"
#include "Engine/Physics/PhysicsSettings.h"
#include "Engine/Physics/Types.h"
#include "Engine/Graphics/Models/ModelData.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include <ThirdParty/catch2/catch.hpp>

namespace
{
    class TestPhysicsOwner : public IPhysicsActor
    {
    public:
        void* Actor = nullptr;
        int32 TransformChanges = 0;

        void* GetPhysicsActor() const override
        {
            return Actor;
        }

        void OnActiveTransformChanged() override
        {
            TransformChanges++;
        }
    };

    class TestPhysicsCollider final : public PhysicsColliderActor
    {
    public:
        void* Shape = nullptr;

        explicit TestPhysicsCollider(const SpawnParams& params)
            : PhysicsColliderActor(params)
        {
        }

        RigidBody* GetAttachedRigidBody() const override
        {
            return nullptr;
        }

        int32 GetPhysicsShapesCount() const override
        {
            return Shape ? 1 : 0;
        }

        void* GetPhysicsShape(int32 index) const override
        {
            return index == 0 ? Shape : nullptr;
        }

        bool RayCast(const Vector3& origin, const Vector3& direction, float& resultHitDistance, float maxDistance) const override
        {
            return false;
        }

        bool RayCast(const Vector3& origin, const Vector3& direction, RayCastHit& hitInfo, float maxDistance) const override
        {
            return false;
        }

        void ClosestPoint(const Vector3& point, Vector3& result) const override
        {
            result = point;
        }

        bool ContainsPoint(const Vector3& point) const override
        {
            return false;
        }
    };

    TestPhysicsCollider* CreateTestCollider()
    {
        return New<TestPhysicsCollider>(ScriptingObject::SpawnParams(Guid::New(), PhysicsColliderActor::TypeInitializer));
    }
}

TEST_CASE("PhysicsBackend")
{
#if !COMPILE_WITH_EMPTY_PHYSICS
    PhysicsSettings settings;
    settings.DefaultGravity = Vector3(0.0, -981.0, 0.0);
    settings.DisableCCD = true;

    auto physicsScene = New<PhysicsScene>();
    REQUIRE(physicsScene);
    REQUIRE_FALSE(physicsScene->Init(TEXT("PhysicsBackendTest"), settings));
    void* scene = physicsScene->GetPhysicsScene();
    SCOPE_EXIT
    {
        Delete(physicsScene);
    };

    TestPhysicsOwner floorOwner;
    TestPhysicsOwner sphereOwner;
    void* floorActor = PhysicsBackend::CreateRigidStaticActor(&floorOwner, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, scene);
    void* sphereActor = PhysicsBackend::CreateRigidDynamicActor(&sphereOwner, Vector3(0.0, 200.0, 0.0), Quaternion::Identity, scene);
    REQUIRE(floorActor);
    REQUIRE(sphereActor);
    floorOwner.Actor = floorActor;
    sphereOwner.Actor = sphereActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(sphereActor);
        PhysicsBackend::DestroyActor(floorActor);
    };

    auto floorCollider = CreateTestCollider();
    auto sphereCollider = CreateTestCollider();
    REQUIRE(floorCollider);
    REQUIRE(sphereCollider);
    SCOPE_EXIT
    {
        sphereCollider->DeleteObjectNow();
        floorCollider->DeleteObjectNow();
    };

    CollisionShape floorGeometry;
    float floorHalfExtents[3] = { 500.0f, 10.0f, 500.0f };
    floorGeometry.SetBox(floorHalfExtents);
    void* floorShape = PhysicsBackend::CreateShape(floorCollider, floorGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(floorShape);
    floorCollider->Shape = floorShape;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(floorShape);
    };
    PhysicsBackend::AttachShape(floorShape, floorActor);

    CollisionShape sphereGeometry;
    sphereGeometry.SetSphere(25.0f);
    void* sphereShape = PhysicsBackend::CreateShape(sphereCollider, sphereGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(sphereShape);
    sphereCollider->Shape = sphereShape;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(sphereShape);
    };
    PhysicsBackend::AttachShape(sphereShape, sphereActor);

    PhysicsBackend::AddSceneActor(scene, floorActor);
    PhysicsBackend::AddSceneActor(scene, sphereActor);

#if COMPILE_WITH_BOX3D
    TestPhysicsOwner controllerOwner;
    auto controllerCollider = CreateTestCollider();
    REQUIRE(controllerCollider);
    SCOPE_EXIT
    {
        controllerCollider->DeleteObjectNow();
    };

    void* controllerShape = nullptr;
    void* controller = PhysicsBackend::CreateController(scene, &controllerOwner, controllerCollider, 0.1f, Vector3(0.0, 60.0, 0.0), 1.0f, 0, nullptr, 10.0f, 40.0f, 1.0f, controllerShape);
    REQUIRE(controller);
    REQUIRE(controllerShape);
    controllerOwner.Actor = PhysicsBackend::GetControllerRigidDynamicActor(controller);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyController(controller);
    };

    const int32 groundFlags = PhysicsBackend::MoveController(controller, controllerShape, Vector3(0.0, -100.0, 0.0), 0.0f, 1.0f / 60.0f);
    CHECK((groundFlags & (int32)CharacterController::CollisionFlags::Below) != 0);
    const Vector3 groundedPosition = PhysicsBackend::GetControllerPosition(controller);
    CHECK(groundedPosition.Y == Approx(30.0).margin(1.0));

    PhysicsBackend::MoveController(controller, controllerShape, Vector3(50.0, 0.0, 0.0), 0.0f, 1.0f / 60.0f);
    const Vector3 movedPosition = PhysicsBackend::GetControllerPosition(controller);
    CHECK(movedPosition.X > groundedPosition.X + 25.0);
    CHECK(movedPosition.Y == Approx(groundedPosition.Y).margin(1.0));

    const int32 jumpFlags = PhysicsBackend::MoveController(controller, controllerShape, Vector3(0.0, 20.0, 0.0), 0.0f, 1.0f / 60.0f);
    CHECK((jumpFlags & (int32)CharacterController::CollisionFlags::Below) == 0);
    const Vector3 jumpedPosition = PhysicsBackend::GetControllerPosition(controller);
    CHECK(jumpedPosition.Y > movedPosition.Y + 15.0f);
#endif

    TestPhysicsOwner jointAnchorOwner;
    TestPhysicsOwner jointBodyOwner;
    void* jointAnchorActor = PhysicsBackend::CreateRigidStaticActor(&jointAnchorOwner, Vector3(1000.0, 0.0, 0.0), Quaternion::Identity, scene);
    void* jointBodyActor = PhysicsBackend::CreateRigidDynamicActor(&jointBodyOwner, Vector3(1000.0, 100.0, 0.0), Quaternion::Identity, scene);
    REQUIRE(jointAnchorActor);
    REQUIRE(jointBodyActor);
    jointAnchorOwner.Actor = jointAnchorActor;
    jointBodyOwner.Actor = jointBodyActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(jointBodyActor);
        PhysicsBackend::DestroyActor(jointAnchorActor);
    };

    auto jointBodyCollider = CreateTestCollider();
    REQUIRE(jointBodyCollider);
    SCOPE_EXIT
    {
        jointBodyCollider->DeleteObjectNow();
    };

    CollisionShape jointBodyGeometry;
    float jointBodyHalfExtents[3] = { 10.0f, 10.0f, 10.0f };
    jointBodyGeometry.SetBox(jointBodyHalfExtents);
    void* jointBodyShape = PhysicsBackend::CreateShape(jointBodyCollider, jointBodyGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(jointBodyShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(jointBodyShape);
    };
    PhysicsBackend::AttachShape(jointBodyShape, jointBodyActor);
    PhysicsBackend::AddSceneActor(scene, jointAnchorActor);
    PhysicsBackend::AddSceneActor(scene, jointBodyActor);

    PhysicsJointDesc d6Desc;
    Platform::MemoryClear(&d6Desc, sizeof(d6Desc));
    d6Desc.Actor0 = jointAnchorActor;
    d6Desc.Actor1 = jointBodyActor;
    d6Desc.Rot0 = Quaternion::Identity;
    d6Desc.Rot1 = Quaternion::Identity;
    d6Desc.Pos0 = Vector3::Zero;
    d6Desc.Pos1 = Vector3(0.0, -100.0, 0.0);
    void* d6Joint = PhysicsBackend::CreateD6Joint(d6Desc);
    REQUIRE(d6Joint);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyJoint(d6Joint);
    };

    RayCastHit hit;
    CHECK(PhysicsBackend::RayCast(scene, Vector3(200.0, 100.0, 0.0), Vector3::Down, hit, 300.0f, MAX_uint32, true));
    CHECK(hit.Collider == floorCollider);
    CHECK(hit.Distance == Approx(100.0f).margin(0.5f));

    // MAX_float is the public API default for an unbounded cast. Backends that use a
    // displacement vector internally must keep this query finite and numerically valid.
    CHECK(PhysicsBackend::RayCast(scene, Vector3(200.0, 100.0, 0.0), Vector3::Down, hit, MAX_float, MAX_uint32, true));
    CHECK(hit.Collider == floorCollider);
    CHECK(hit.Distance == Approx(100.0f).margin(0.5f));
    float shapeHitDistance;
    CHECK(PhysicsBackend::RayCastShape(floorShape, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, Vector3(200.0, 100.0, 0.0), Vector3::Down, shapeHitDistance, MAX_float));
    CHECK(shapeHitDistance == Approx(100.0f).margin(0.5f));

    RayCastHit castHits[4] = {};
    const int32 rayHitCount = PhysicsBackend::RayCastNonAlloc(scene, Vector3(0.0, 300.0, 0.0), Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), 400.0f, MAX_uint32, true);
    CHECK(rayHitCount == 2);
    CHECK(physicsScene->LineCastNonAlloc(Vector3(0.0, 300.0, 0.0), Vector3(0.0, -100.0, 0.0), ToSpan(castHits, ARRAY_COUNT(castHits)), MAX_uint32, true) == 2);
    CHECK(physicsScene->RayCastNonAlloc(Vector3(0.0, 300.0, 0.0), Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), 400.0f, MAX_uint32, true) == 2);
    castHits[1].Distance = 12345.0f;
    CHECK(PhysicsBackend::RayCastNonAlloc(scene, Vector3(0.0, 300.0, 0.0), Vector3::Down, ToSpan(castHits, 1), 400.0f, MAX_uint32, true) == 1);
    CHECK(castHits[1].Distance == 12345.0f);
    CHECK(PhysicsBackend::RayCastNonAlloc(scene, Vector3(0.0, 300.0, 0.0), Vector3::Down, Span<RayCastHit>(), 400.0f, MAX_uint32, true) == 0);

    CHECK(physicsScene->BoxCastNonAlloc(Vector3(0.0, 200.0, 0.0), Vector3(10.0f), Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), Quaternion::Identity, 100.0f, MAX_uint32, true) >= 1);
    CHECK(castHits[0].Distance == Approx(0.0f));
    CHECK(physicsScene->SphereCastNonAlloc(Vector3(0.0, 200.0, 0.0), 10.0f, Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), 100.0f, MAX_uint32, true) >= 1);
    CHECK(castHits[0].Distance == Approx(0.0f));
    CHECK(physicsScene->CapsuleCastNonAlloc(Vector3(0.0, 200.0, 0.0), 10.0f, 20.0f, Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), Quaternion::Identity, 100.0f, MAX_uint32, true) >= 1);
    CHECK(castHits[0].Distance == Approx(0.0f));

    PhysicsColliderActor* overlapHits[4] = {};
    CHECK(PhysicsBackend::OverlapSphereNonAlloc(scene, Vector3(0.0, 100.0, 0.0), 400.0f, ToSpan(overlapHits, 1), MAX_uint32, true) == 1);
    CHECK(physicsScene->OverlapBoxNonAlloc(Vector3(0.0, 200.0, 0.0), Vector3(30.0f), ToSpan(overlapHits, ARRAY_COUNT(overlapHits)), Quaternion::Identity, MAX_uint32, true) >= 1);
    CHECK(physicsScene->OverlapSphereNonAlloc(Vector3(0.0, 200.0, 0.0), 30.0f, ToSpan(overlapHits, ARRAY_COUNT(overlapHits)), MAX_uint32, true) >= 1);
    CHECK(physicsScene->OverlapCapsuleNonAlloc(Vector3(0.0, 200.0, 0.0), 10.0f, 20.0f, ToSpan(overlapHits, ARRAY_COUNT(overlapHits)), Quaternion::Identity, MAX_uint32, true) >= 1);

    PhysicsBackend::SetShapeState(sphereShape, true, true);
    CHECK(PhysicsBackend::RayCastNonAlloc(scene, Vector3(0.0, 300.0, 0.0), Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), 400.0f, MAX_uint32, false) == 1);
    CHECK(PhysicsBackend::RayCastNonAlloc(scene, Vector3(0.0, 300.0, 0.0), Vector3::Down, ToSpan(castHits, ARRAY_COUNT(castHits)), 400.0f, MAX_uint32, true) == 2);
    CHECK(PhysicsBackend::OverlapSphereNonAlloc(scene, Vector3(0.0, 200.0, 0.0), 30.0f, ToSpan(overlapHits, ARRAY_COUNT(overlapHits)), MAX_uint32, false) == 0);
    CHECK(PhysicsBackend::OverlapSphereNonAlloc(scene, Vector3(0.0, 200.0, 0.0), 30.0f, ToSpan(overlapHits, ARRAY_COUNT(overlapHits)), MAX_uint32, true) == 1);
    PhysicsBackend::SetShapeState(sphereShape, true, false);

    PhysicsBackend::SetRigidDynamicActorLinearVelocity(sphereActor, Vector3(10.0f, 0.0f, 0.0f), true);
    PhysicsBackend::SetRigidDynamicActorAngularVelocity(sphereActor, Vector3(0.0f, 0.0f, 1.0f), true);
    const Vector3 pointVelocity = PhysicsBackend::GetRigidDynamicActorPointVelocity(sphereActor, Vector3(0.0, 300.0, 0.0));
    CHECK(pointVelocity.X == Approx(-90.0f).margin(0.5f));
    CHECK(pointVelocity.Y == Approx(0.0f).margin(0.5f));
    PhysicsBackend::SetRigidDynamicActorLinearVelocity(sphereActor, Vector3::Zero, true);
    PhysicsBackend::SetRigidDynamicActorAngularVelocity(sphereActor, Vector3::Zero, true);

    const uint32 layerMask2 = Physics::LayerMasks[2];
    const uint32 layerMask3 = Physics::LayerMasks[3];
    Physics::LayerMasks[2] = ~(1u << 3);
    Physics::LayerMasks[3] = MAX_uint32;
    CHECK(Physics::GetLayerMask(2) == ~(1u << 3));
    CHECK(Physics::GetIgnoreLayerCollision(2, 3));
    Physics::LayerMasks[2] = layerMask2;
    Physics::LayerMasks[3] = layerMask3;

    Vector3 penetrationDirection;
    float penetrationDistance;
    CHECK(PhysicsBackend::ComputeShapesPenetration(sphereShape, floorShape, Vector3(0.0, 10.0, 0.0), Quaternion::Identity, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, penetrationDirection, penetrationDistance));
    CHECK(penetrationDirection.Y > 0.9f);
    CHECK(penetrationDistance == Approx(15.0f).margin(0.75f));
    CHECK_FALSE(PhysicsBackend::ComputeShapesPenetration(sphereShape, floorShape, Vector3(0.0, 100.0, 0.0), Quaternion::Identity, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, penetrationDirection, penetrationDistance));
    const Vector3 arbitraryFloorPosition(750.0, -10.0, -250.0);
    const Quaternion arbitraryRotation = Quaternion::Euler(0.0f, 0.0f, 30.0f);
    const Vector3 arbitraryUp = Vector3::Transform(Vector3::Up, arbitraryRotation);
    const Vector3 sphereLocalOffset(5.0f, 0.0f, 0.0f);
    PhysicsBackend::SetShapeLocalPose(sphereShape, sphereLocalOffset, Quaternion::Identity);
    const Vector3 arbitrarySpherePosition = arbitraryFloorPosition + arbitraryUp * 20.0f - Vector3::Transform(sphereLocalOffset, arbitraryRotation);
    CHECK(Physics::ComputePenetration(sphereCollider, arbitrarySpherePosition, arbitraryRotation, floorCollider, arbitraryFloorPosition, arbitraryRotation, penetrationDirection, penetrationDistance));
    CHECK(Vector3::Dot(penetrationDirection, arbitraryUp) > 0.9f);
    CHECK(penetrationDistance == Approx(15.0f).margin(0.75f));
    PhysicsBackend::SetShapeLocalPose(sphereShape, Vector3::Zero, Quaternion::Identity);

#if COMPILE_WITH_PHYSICS_COOKING
    PhysicsBackend::HeightFieldSample heightSamples[6] = {};
    heightSamples[0].Height = 0;
    heightSamples[1].Height = 1;
    heightSamples[2].Height = 2;
    heightSamples[3].Height = 3;
    heightSamples[4].Height = 4;
    heightSamples[5].Height = 5;
    MemoryWriteStream heightFieldStream;
    REQUIRE_FALSE(CollisionCooking::CookHeightField(3, 2, heightSamples, heightFieldStream));
    void* heightField = PhysicsBackend::CreateHeightField((byte*)heightFieldStream.GetHandle(), heightFieldStream.GetPosition());
    REQUIRE(heightField);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyObject(heightField);
    };
    int32 heightFieldRows, heightFieldColumns;
    PhysicsBackend::GetHeightFieldSize(heightField, heightFieldRows, heightFieldColumns);
    CHECK(heightFieldRows == 2);
    CHECK(heightFieldColumns == 3);
    CHECK(PhysicsBackend::GetHeightFieldHeight(heightField, 1, 0) == Approx(3.0f));
    PhysicsBackend::HeightFieldSample modifiedSample = {};
    modifiedSample.Height = 7;
    CHECK_FALSE(PhysicsBackend::ModifyHeightField(heightField, 1, 0, 1, 1, &modifiedSample));
    CHECK(PhysicsBackend::GetHeightFieldHeight(heightField, 0, 1) == Approx(7.0f));

#if COMPILE_WITH_BOX3D
    CollisionShape heightFieldGeometry;
    heightFieldGeometry.SetHeightField(heightField, 2.0f, 3.0f, 4.0f);
    const Vector3 heightFieldPosition(400.0, 50.0, -300.0);
    const Quaternion heightFieldRotation = Quaternion::Euler(0.0f, 25.0f, 0.0f);
    TestPhysicsOwner heightFieldOwner;
    void* heightFieldActor = PhysicsBackend::CreateRigidStaticActor(&heightFieldOwner, heightFieldPosition, heightFieldRotation, scene);
    REQUIRE(heightFieldActor);
    heightFieldOwner.Actor = heightFieldActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(heightFieldActor);
    };
    auto heightFieldCollider = CreateTestCollider();
    REQUIRE(heightFieldCollider);
    SCOPE_EXIT
    {
        heightFieldCollider->DeleteObjectNow();
    };
    void* heightFieldShape = PhysicsBackend::CreateShape(heightFieldCollider, heightFieldGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(heightFieldShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(heightFieldShape);
    };
    PhysicsBackend::AttachShape(heightFieldShape, heightFieldActor);
    CHECK(PhysicsBackend::GetRigidActorShapesCount(heightFieldActor) == 1);
    PhysicsBackend::AddSceneActor(scene, heightFieldActor);

    RayCastHit heightFieldHit;
    const Vector3 heightFieldRayOrigin = heightFieldPosition + Vector3::Transform(Vector3(1.5, 100.0, 2.0), heightFieldRotation);
    const Vector3 heightFieldRayDirection = Vector3::Transform(Vector3::Down, heightFieldRotation);
    CHECK(PhysicsBackend::RayCast(scene, heightFieldRayOrigin, heightFieldRayDirection, heightFieldHit, 200.0f, MAX_uint32, true));
    CHECK(heightFieldHit.Collider == heightFieldCollider);

    const Vector3 heightFieldSpherePosition = heightFieldPosition + Vector3::Transform(Vector3(1.5, 20.0, 2.0), heightFieldRotation);
    CHECK(PhysicsBackend::ComputeShapesPenetration(sphereShape, heightFieldShape, heightFieldSpherePosition, Quaternion::Identity, heightFieldPosition, heightFieldRotation, penetrationDirection, penetrationDistance));
    CHECK(penetrationDistance > 0.0f);
#endif
#endif

    Vector3 startPosition;
    Quaternion startOrientation;
    PhysicsBackend::GetRigidActorPose(sphereActor, startPosition, startOrientation);
    Vector3 jointStartPosition;
    Quaternion jointStartOrientation;
    PhysicsBackend::GetRigidActorPose(jointBodyActor, jointStartPosition, jointStartOrientation);

    for (int32 i = 0; i < 20; i++)
    {
        PhysicsBackend::StartSimulateScene(scene, 1.0f / 60.0f);
        PhysicsBackend::EndSimulateScene(scene);
    }

    Vector3 endPosition;
    Quaternion endOrientation;
    PhysicsBackend::GetRigidActorPose(sphereActor, endPosition, endOrientation);
    CHECK(endPosition.Y < startPosition.Y - 1.0);
    CHECK(sphereOwner.TransformChanges > 0);
    Vector3 jointEndPosition;
    Quaternion jointEndOrientation;
    PhysicsBackend::GetRigidActorPose(jointBodyActor, jointEndPosition, jointEndOrientation);
    CHECK(jointEndPosition.Y == Approx(jointStartPosition.Y).margin(10.0f));
#endif
}

TEST_CASE("PhysicsBackendCapsuleQueryConsistency")
{
#if COMPILE_WITH_BOX3D
    PhysicsSettings settings;
    void* scene = PhysicsBackend::CreateScene(settings);
    REQUIRE(scene);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyScene(scene);
    };

    TestPhysicsOwner wallOwner;
    TestPhysicsOwner ballOwner;
    void* wallActor = PhysicsBackend::CreateRigidStaticActor(&wallOwner, Vector3(0.0, 62.0, 600.0), Quaternion::Identity, scene);
    void* ballActor = PhysicsBackend::CreateRigidStaticActor(&ballOwner, Vector3(-71.0, 75.0, 523.0), Quaternion::Identity, scene);
    REQUIRE(wallActor);
    REQUIRE(ballActor);
    wallOwner.Actor = wallActor;
    ballOwner.Actor = ballActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(ballActor);
        PhysicsBackend::DestroyActor(wallActor);
    };

    auto wallCollider = CreateTestCollider();
    auto ballCollider = CreateTestCollider();
    auto capsuleCollider = CreateTestCollider();
    REQUIRE(wallCollider);
    REQUIRE(ballCollider);
    REQUIRE(capsuleCollider);
    SCOPE_EXIT
    {
        capsuleCollider->DeleteObjectNow();
        ballCollider->DeleteObjectNow();
        wallCollider->DeleteObjectNow();
    };

    CollisionShape wallGeometry;
    float wallHalfExtents[3] = { 600.0f, 50.0f, 17.5f };
    wallGeometry.SetBox(wallHalfExtents);
    CollisionShape ballGeometry;
    ballGeometry.SetSphere(50.0f);
    CollisionShape capsuleGeometry;
    capsuleGeometry.SetCapsule(31.0f, 60.0f);

    void* wallShape = PhysicsBackend::CreateShape(wallCollider, wallGeometry, (JsonAsset*)nullptr, true, false);
    void* ballShape = PhysicsBackend::CreateShape(ballCollider, ballGeometry, (JsonAsset*)nullptr, true, false);
    void* capsuleShape = PhysicsBackend::CreateShape(capsuleCollider, capsuleGeometry, (JsonAsset*)nullptr, true, true);
    REQUIRE(wallShape);
    REQUIRE(ballShape);
    REQUIRE(capsuleShape);
    wallCollider->Shape = wallShape;
    ballCollider->Shape = ballShape;
    capsuleCollider->Shape = capsuleShape;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(capsuleShape);
        PhysicsBackend::DestroyShape(ballShape);
        PhysicsBackend::DestroyShape(wallShape);
    };

    PhysicsBackend::AttachShape(wallShape, wallActor);
    PhysicsBackend::AttachShape(ballShape, ballActor);
    PhysicsBackend::AddSceneActor(scene, wallActor);
    PhysicsBackend::AddSceneActor(scene, ballActor);

    const Quaternion capsuleRotation = Quaternion::Euler(0.0f, 0.0f, 90.0f);
    PhysicsColliderActor* overlaps[4] = {};
    RayCastHit casts[4] = {};
    Vector3 penetrationDirection;
    float penetrationDistance;

    for (int32 i = 0; i < 256; i++)
    {
        const float z = 552.5f + (float)(i % 16) * 0.125f;
        const Vector3 capsuleCenter(-143.0, 117.0, z);
        const int32 overlapCount = PhysicsBackend::OverlapCapsuleNonAlloc(scene, capsuleCenter, 31.0f, 120.0f,
            ToSpan(overlaps, ARRAY_COUNT(overlaps)), capsuleRotation, MAX_uint32, true);

        bool foundWall = false;
        bool foundBall = false;
        for (int32 hitIndex = 0; hitIndex < overlapCount; hitIndex++)
        {
            foundWall |= overlaps[hitIndex] == wallCollider;
            foundBall |= overlaps[hitIndex] == ballCollider;
        }
        CHECK(foundWall);
        CHECK(foundBall);
        CHECK(PhysicsBackend::ComputeShapesPenetration(capsuleShape, wallShape, capsuleCenter, capsuleRotation,
            Vector3(0.0, 62.0, 600.0), Quaternion::Identity, penetrationDirection, penetrationDistance));
        CHECK(penetrationDirection.Z < -0.9f);
        CHECK(penetrationDistance > 0.0f);
        CHECK(PhysicsBackend::CapsuleCastNonAlloc(scene, capsuleCenter, 31.0f, 120.0f, Vector3::Forward,
            ToSpan(casts, ARRAY_COUNT(casts)), capsuleRotation, 2.0f, MAX_uint32, true) >= 2);
    }
#endif
}

TEST_CASE("PhysicsBackendConvexMesh")
{
#if !COMPILE_WITH_EMPTY_PHYSICS && COMPILE_WITH_PHYSICS_COOKING
    Array<Float3> convexVertices;
    const float radius = 50.0f;
    convexVertices.Add(Float3(0.0f, radius, 0.0f));
    convexVertices.Add(Float3(0.0f, -radius, 0.0f));
    for (int32 ring = 1; ring < 12; ring++)
    {
        float sinPhi, cosPhi;
        Math::SinCos(PI * (float)ring / 12.0f, sinPhi, cosPhi);
        for (int32 slice = 0; slice < 32; slice++)
        {
            float sinTheta, cosTheta;
            Math::SinCos(TWO_PI * (float)slice / 32.0f, sinTheta, cosTheta);
            convexVertices.Add(Float3(radius * sinPhi * cosTheta, radius * cosPhi, radius * sinPhi * sinTheta));
        }
    }

    CollisionCooking::CookingInput input;
    input.VertexCount = convexVertices.Count();
    input.VertexData = convexVertices.Get();
    input.ConvexVertexLimit = 255;
    BytesContainer cookedData;
    REQUIRE_FALSE(CollisionCooking::CookConvexMesh(input, cookedData));

    BoundingBox localBounds;
    void* convexMesh = PhysicsBackend::CreateConvexMesh(cookedData.Get(), cookedData.Length(), localBounds);
    REQUIRE(convexMesh);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyObject(convexMesh);
    };

    Array<Float3, HeapAllocation> debugVertices;
    Array<int32, HeapAllocation> debugIndices;
    PhysicsBackend::GetConvexMeshTriangles(convexMesh, debugVertices, debugIndices);
    CHECK(debugVertices.HasItems());
    CHECK(debugIndices.Count() >= 12);
    CHECK(debugIndices.Count() % 3 == 0);

    auto* convexData = Content::CreateVirtualAsset<CollisionData>();
    REQUIRE(convexData);
    SCOPE_EXIT
    {
        Content::DeleteAsset(convexData);
    };
    REQUIRE_FALSE(convexData->CookCollision(CollisionDataType::ConvexMesh, ToSpan(debugVertices), ToSpan(debugIndices), ConvexMeshGenerationFlags::None, 255));

    PhysicsSettings settings;
    settings.DefaultGravity = Vector3::Zero;
    settings.DisableCCD = true;
    void* scene = PhysicsBackend::CreateScene(settings);
    REQUIRE(scene);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyScene(scene);
    };

    TestPhysicsOwner convexOwner;
    void* convexActor = PhysicsBackend::CreateRigidStaticActor(&convexOwner, Vector3::Zero, Quaternion::Identity, scene);
    REQUIRE(convexActor);
    convexOwner.Actor = convexActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(convexActor);
    };

    auto convexCollider = CreateTestCollider();
    REQUIRE(convexCollider);
    SCOPE_EXIT
    {
        convexCollider->DeleteObjectNow();
    };

    CollisionShape convexGeometry;
    float convexScale[3] = { 1.0f, 1.0f, 1.0f };
    convexGeometry.SetConvexMesh(convexMesh, convexScale);
    void* convexShape = PhysicsBackend::CreateShape(convexCollider, convexGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(convexShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(convexShape);
    };
    PhysicsBackend::AttachShape(convexShape, convexActor);
    PhysicsBackend::AddSceneActor(scene, convexActor);

    RayCastHit hit;
    CHECK(PhysicsBackend::RayCast(scene, Vector3(0.0, 0.0, -150.0), Vector3(0.0, 0.0, 1.0), hit, 300.0f, MAX_uint32, true));
    CHECK(hit.Collider == convexCollider);

    RayCastHit convexCastHits[2] = {};
    CHECK(PhysicsBackend::ConvexCastNonAlloc(scene, Vector3(0.0, 0.0, -150.0), convexData, Vector3::One, Vector3::Forward, ToSpan(convexCastHits, ARRAY_COUNT(convexCastHits)), Quaternion::Identity, 300.0f, MAX_uint32, true) >= 1);
    CHECK(PhysicsBackend::ConvexCastNonAlloc(scene, Vector3::Zero, convexData, Vector3::One, Vector3::Forward, ToSpan(convexCastHits, ARRAY_COUNT(convexCastHits)), Quaternion::Identity, 100.0f, MAX_uint32, true) >= 1);
    CHECK(convexCastHits[0].Distance == Approx(0.0f));
    PhysicsColliderActor* convexOverlapHits[1] = {};
    CHECK(PhysicsBackend::OverlapConvexNonAlloc(scene, Vector3::Zero, convexData, Vector3::One, ToSpan(convexOverlapHits, ARRAY_COUNT(convexOverlapHits)), Quaternion::Identity, MAX_uint32, true) == 1);
    CHECK(convexOverlapHits[0] == convexCollider);

    TestPhysicsOwner projectileOwner;
    void* projectileActor = PhysicsBackend::CreateRigidDynamicActor(&projectileOwner, Vector3(0.0, 0.0, -150.0), Quaternion::Identity, scene);
    REQUIRE(projectileActor);
    projectileOwner.Actor = projectileActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(projectileActor);
    };

    auto projectileCollider = CreateTestCollider();
    REQUIRE(projectileCollider);
    SCOPE_EXIT
    {
        projectileCollider->DeleteObjectNow();
    };

    CollisionShape projectileGeometry;
    projectileGeometry.SetSphere(10.0f);
    void* projectileShape = PhysicsBackend::CreateShape(projectileCollider, projectileGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(projectileShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(projectileShape);
    };
    PhysicsBackend::AttachShape(projectileShape, projectileActor);
    PhysicsBackend::AddSceneActor(scene, projectileActor);
    PhysicsBackend::SetRigidDynamicActorLinearVelocity(projectileActor, Vector3(0.0, 0.0, 200.0), true);

    for (int32 i = 0; i < 120; i++)
    {
        PhysicsBackend::StartSimulateScene(scene, 1.0f / 60.0f);
        PhysicsBackend::EndSimulateScene(scene);
    }

    Vector3 endPosition;
    Quaternion endOrientation;
    PhysicsBackend::GetRigidActorPose(projectileActor, endPosition, endOrientation);
    CHECK(endPosition.Z < -25.0);
#endif
}

TEST_CASE("CollisionCookingMaterialSlotsMask")
{
#if !COMPILE_WITH_EMPTY_PHYSICS && COMPILE_WITH_PHYSICS_COOKING
    ModelData modelData;
    modelData.Materials.Resize(1);
    modelData.LODs.Resize(1);

    auto mesh = New<MeshData>();
    mesh->MaterialSlotIndex = 0;
    mesh->Positions.Add(Float3(0.0f, 0.0f, 0.0f));
    mesh->Positions.Add(Float3(100.0f, 0.0f, 0.0f));
    mesh->Positions.Add(Float3(0.0f, 100.0f, 0.0f));
    mesh->Positions.Add(Float3(0.0f, 0.0f, 100.0f));
    mesh->Indices.Add(0);
    mesh->Indices.Add(1);
    mesh->Indices.Add(2);
    mesh->Indices.Add(0);
    mesh->Indices.Add(3);
    mesh->Indices.Add(1);
    mesh->Indices.Add(0);
    mesh->Indices.Add(2);
    mesh->Indices.Add(3);
    mesh->Indices.Add(1);
    mesh->Indices.Add(3);
    mesh->Indices.Add(2);
    modelData.LODs[0].Meshes.Add(mesh);

    CollisionCooking::Argument arg;
    arg.Type = CollisionDataType::ConvexMesh;
    arg.OverrideModelData = &modelData;
    arg.MaterialSlotsMask = 0xd6faa230;
    arg.ConvexVertexLimit = 8;

    CollisionData::SerializedOptions options;
    BytesContainer outputData;
    REQUIRE_FALSE(CollisionCooking::CookCollision(arg, options, outputData));
    CHECK(options.MaterialSlotsMask == MAX_uint32);
    CHECK(outputData.HasItems());
#endif
}

TEST_CASE("PhysicsBackendActorRemovalQueue")
{
#if !COMPILE_WITH_EMPTY_PHYSICS
    PhysicsSettings settings;
    settings.DefaultGravity = Vector3::Zero;
    settings.DisableCCD = true;

    void* scene = PhysicsBackend::CreateScene(settings);
    REQUIRE(scene);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyScene(scene);
    };

    for (int32 i = 0; i < 16; i++)
    {
        TestPhysicsOwner removedOwner;
        void* removedActor = PhysicsBackend::CreateRigidDynamicActor(&removedOwner, Vector3::Zero, Quaternion::Identity, scene);
        REQUIRE(removedActor);
        removedOwner.Actor = removedActor;
        PhysicsBackend::AddSceneActor(scene, removedActor);
        PhysicsBackend::RemoveSceneActor(scene, removedActor);
        PhysicsBackend::DestroyActor(removedActor);

        TestPhysicsOwner movingOwner;
        void* movingActor = PhysicsBackend::CreateRigidDynamicActor(&movingOwner, Vector3::Zero, Quaternion::Identity, scene);
        REQUIRE(movingActor);
        movingOwner.Actor = movingActor;
        SCOPE_EXIT
        {
            PhysicsBackend::RemoveSceneActor(scene, movingActor);
            PhysicsBackend::DestroyActor(movingActor);
        };

        auto movingCollider = CreateTestCollider();
        REQUIRE(movingCollider);
        SCOPE_EXIT
        {
            movingCollider->DeleteObjectNow();
        };

        CollisionShape geometry;
        geometry.SetSphere(10.0f);
        void* shape = PhysicsBackend::CreateShape(movingCollider, geometry, (JsonAsset*)nullptr, true, false);
        REQUIRE(shape);
        SCOPE_EXIT
        {
            PhysicsBackend::DestroyShape(shape);
        };
        PhysicsBackend::AttachShape(shape, movingActor);
        PhysicsBackend::AddSceneActor(scene, movingActor);
        PhysicsBackend::SetRigidDynamicActorLinearVelocity(movingActor, Vector3(0.0f, 600.0f, 0.0f), true);

        PhysicsBackend::StartSimulateScene(scene, 1.0f / 60.0f);
        PhysicsBackend::EndSimulateScene(scene);

        Vector3 position;
        Quaternion orientation;
        PhysicsBackend::GetRigidActorPose(movingActor, position, orientation);
        CHECK(position.Y > 1.0f);
    }
#endif
}

TEST_CASE("PhysicsBackendBulletCCD")
{
#if !COMPILE_WITH_EMPTY_PHYSICS
    for (int32 targetUsesCCD = 0; targetUsesCCD < 2; targetUsesCCD++)
    {
        PhysicsSettings settings;
        settings.DefaultGravity = Vector3::Zero;
        settings.DisableCCD = false;

        void* scene = PhysicsBackend::CreateScene(settings);
        REQUIRE(scene);
        SCOPE_EXIT
        {
            PhysicsBackend::DestroyScene(scene);
        };

        TestPhysicsOwner targetOwner;
        TestPhysicsOwner projectileOwner;
        void* targetActor = PhysicsBackend::CreateRigidDynamicActor(&targetOwner, Vector3::Zero, Quaternion::Identity, scene);
        void* projectileActor = PhysicsBackend::CreateRigidDynamicActor(&projectileOwner, Vector3(-100.0, 0.0, 0.0), Quaternion::Identity, scene);
        REQUIRE(targetActor);
        REQUIRE(projectileActor);
        targetOwner.Actor = targetActor;
        projectileOwner.Actor = projectileActor;
        SCOPE_EXIT
        {
            PhysicsBackend::DestroyActor(projectileActor);
            PhysicsBackend::DestroyActor(targetActor);
        };

        auto targetCollider = CreateTestCollider();
        auto projectileCollider = CreateTestCollider();
        REQUIRE(targetCollider);
        REQUIRE(projectileCollider);
        SCOPE_EXIT
        {
            projectileCollider->DeleteObjectNow();
            targetCollider->DeleteObjectNow();
        };

        CollisionShape targetGeometry;
        float targetHalfExtents[3] = { 10.0f, 10.0f, 10.0f };
        targetGeometry.SetBox(targetHalfExtents);
        void* targetShape = PhysicsBackend::CreateShape(targetCollider, targetGeometry, (JsonAsset*)nullptr, true, false);
        REQUIRE(targetShape);
        SCOPE_EXIT
        {
            PhysicsBackend::DestroyShape(targetShape);
        };
        PhysicsBackend::AttachShape(targetShape, targetActor);

        CollisionShape projectileGeometry;
        projectileGeometry.SetSphere(5.0f);
        void* projectileShape = PhysicsBackend::CreateShape(projectileCollider, projectileGeometry, (JsonAsset*)nullptr, true, false);
        REQUIRE(projectileShape);
        SCOPE_EXIT
        {
            PhysicsBackend::DestroyShape(projectileShape);
        };
        PhysicsBackend::AttachShape(projectileShape, projectileActor);

        if (targetUsesCCD != 0)
            PhysicsBackend::SetRigidDynamicActorFlags(targetActor, PhysicsBackend::RigidDynamicFlags::CCD);
        PhysicsBackend::SetRigidDynamicActorFlags(projectileActor, PhysicsBackend::RigidDynamicFlags::CCD);
        PhysicsBackend::AddSceneActor(scene, targetActor);
        PhysicsBackend::AddSceneActor(scene, projectileActor);
        PhysicsBackend::RigidDynamicActorSleep(targetActor);
        PhysicsBackend::SetRigidDynamicActorLinearVelocity(projectileActor, Vector3(12000.0f, 0.0f, 0.0f), true);

        PhysicsBackend::StartSimulateScene(scene, 1.0f / 60.0f);
        PhysicsBackend::EndSimulateScene(scene);

        Vector3 projectilePosition;
        Quaternion projectileOrientation;
        PhysicsBackend::GetRigidActorPose(projectileActor, projectilePosition, projectileOrientation);
        CHECK(projectilePosition.X < -10.0f);
    }
#endif
}

TEST_CASE("PhysicsBackendTriangleMesh")
{
#if !COMPILE_WITH_EMPTY_PHYSICS && COMPILE_WITH_PHYSICS_COOKING
    Float3 triangleVertices[4] =
    {
        Float3(-300.0f, 0.0f, -300.0f),
        Float3(300.0f, 0.0f, -300.0f),
        Float3(-300.0f, 0.0f, 300.0f),
        Float3(300.0f, 0.0f, 300.0f),
    };

    int32 invalidIndices[3] = { 0, 1, 7 };
    CollisionCooking::CookingInput invalidInput;
    invalidInput.VertexCount = 4;
    invalidInput.VertexData = triangleVertices;
    invalidInput.IndexCount = 3;
    invalidInput.IndexData = invalidIndices;
    BytesContainer invalidCookedData;
    REQUIRE_FALSE(CollisionCooking::CookTriangleMesh(invalidInput, invalidCookedData));
    BoundingBox invalidBounds;
    CHECK(PhysicsBackend::CreateTriangleMesh(invalidCookedData.Get(), invalidCookedData.Length(), invalidBounds) == nullptr);

    int32 triangleIndices[6] = { 0, 2, 1, 1, 2, 3 };
    CollisionCooking::CookingInput input;
    input.VertexCount = 4;
    input.VertexData = triangleVertices;
    input.IndexCount = 6;
    input.IndexData = triangleIndices;
    BytesContainer cookedData;
    REQUIRE_FALSE(CollisionCooking::CookTriangleMesh(input, cookedData));
#if COMPILE_WITH_BOX3D && USE_LARGE_WORLDS
    REQUIRE(cookedData.Length() > 24);
    CHECK(cookedData.Get()[20] == 0);
    CHECK(cookedData.Get()[21] == 0);
    CHECK(cookedData.Get()[22] == 0);
    CHECK(cookedData.Get()[23] == 0);
#endif
    BoundingBox localBounds;
    void* triangleMesh = PhysicsBackend::CreateTriangleMesh(cookedData.Get(), cookedData.Length(), localBounds);
    REQUIRE(triangleMesh);
    SCOPE_EXIT
    {
        if (triangleMesh)
            PhysicsBackend::DestroyObject(triangleMesh);
    };

    PhysicsSettings settings;
    settings.DefaultGravity = Vector3(0.0, -981.0, 0.0);
    settings.DisableCCD = true;
    void* scene = PhysicsBackend::CreateScene(settings);
    REQUIRE(scene);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyScene(scene);
    };

    TestPhysicsOwner floorOwner;
    TestPhysicsOwner sphereOwner;
    void* floorActor = PhysicsBackend::CreateRigidStaticActor(&floorOwner, Vector3::Zero, Quaternion::Identity, scene);
    void* sphereActor = PhysicsBackend::CreateRigidDynamicActor(&sphereOwner, Vector3(0.0, 150.0, 0.0), Quaternion::Identity, scene);
    REQUIRE(floorActor);
    REQUIRE(sphereActor);
    floorOwner.Actor = floorActor;
    sphereOwner.Actor = sphereActor;
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyActor(sphereActor);
        PhysicsBackend::DestroyActor(floorActor);
    };

    auto floorCollider = CreateTestCollider();
    auto sphereCollider = CreateTestCollider();
    REQUIRE(floorCollider);
    REQUIRE(sphereCollider);
    SCOPE_EXIT
    {
        sphereCollider->DeleteObjectNow();
        floorCollider->DeleteObjectNow();
    };

    CollisionShape floorGeometry;
    float meshScale[3] = { 1.0f, 1.0f, 1.0f };
    floorGeometry.SetTriangleMesh(triangleMesh, meshScale);
    void* floorShape = PhysicsBackend::CreateShape(floorCollider, floorGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(floorShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(floorShape);
    };
    PhysicsBackend::AttachShape(floorShape, floorActor);

    CollisionShape sphereGeometry;
    sphereGeometry.SetSphere(25.0f);
    void* sphereShape = PhysicsBackend::CreateShape(sphereCollider, sphereGeometry, (JsonAsset*)nullptr, true, false);
    REQUIRE(sphereShape);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyShape(sphereShape);
    };
    PhysicsBackend::AttachShape(sphereShape, sphereActor);

    PhysicsBackend::AddSceneActor(scene, floorActor);
    PhysicsBackend::AddSceneActor(scene, sphereActor);
    for (int32 i = 0; i < 180; i++)
    {
        PhysicsBackend::StartSimulateScene(scene, 1.0f / 60.0f);
        PhysicsBackend::EndSimulateScene(scene);
    }

    Vector3 endPosition;
    Quaternion endOrientation;
    PhysicsBackend::GetRigidActorPose(sphereActor, endPosition, endOrientation);
    CHECK(endPosition.Y > 10.0);
#if COMPILE_WITH_BOX3D
    // Box3D runtime shapes reference the cooked triangle mesh directly. Releasing
    // collision data while a collider is still attached must remove the runtime
    // shape before another scene query can reach the freed mesh.
    const Vector3 rayOrigin(250.0, 100.0, 250.0);
    CHECK(PhysicsBackend::RayCast(scene, rayOrigin, Vector3::Down, 200.0f, MAX_uint32, true));
    PhysicsBackend::DestroyObject(triangleMesh);
    triangleMesh = nullptr;
    CHECK_FALSE(PhysicsBackend::RayCast(scene, rayOrigin, Vector3::Down, 200.0f, MAX_uint32, true));
#endif
#endif
}
