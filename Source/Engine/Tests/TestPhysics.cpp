// Copyright (c) Wojciech Figat. All rights reserved.

#include "Engine/Core/ScopeExit.h"
#include "Engine/Core/Math/Math.h"
#include "Engine/Physics/CollisionCooking.h"
#include "Engine/Physics/Actors/IPhysicsActor.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Physics/Colliders/CharacterController.h"
#include "Engine/Physics/Joints/D6Joint.h"
#include "Engine/Physics/PhysicsBackend.h"
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
        explicit TestPhysicsCollider(const SpawnParams& params)
            : PhysicsColliderActor(params)
        {
        }

        RigidBody* GetAttachedRigidBody() const override
        {
            return nullptr;
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

    void* scene = PhysicsBackend::CreateScene(settings);
    REQUIRE(scene);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyScene(scene);
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

    Vector3 penetrationDirection;
    float penetrationDistance;
    CHECK(PhysicsBackend::ComputeShapesPenetration(sphereShape, floorShape, Vector3(0.0, 10.0, 0.0), Quaternion::Identity, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, penetrationDirection, penetrationDistance));
    CHECK(penetrationDirection.Y > 0.9f);
    CHECK(penetrationDistance == Approx(15.0f).margin(0.75f));
    CHECK_FALSE(PhysicsBackend::ComputeShapesPenetration(sphereShape, floorShape, Vector3(0.0, 100.0, 0.0), Quaternion::Identity, Vector3(0.0, -10.0, 0.0), Quaternion::Identity, penetrationDirection, penetrationDistance));

#if COMPILE_WITH_PHYSICS_COOKING
    PhysicsBackend::HeightFieldSample heightSamples[4] = {};
    heightSamples[0].Height = 0;
    heightSamples[1].Height = 1;
    heightSamples[2].Height = 2;
    heightSamples[3].Height = 3;
    MemoryWriteStream heightFieldStream;
    REQUIRE_FALSE(CollisionCooking::CookHeightField(2, 2, heightSamples, heightFieldStream));
    void* heightField = PhysicsBackend::CreateHeightField((byte*)heightFieldStream.GetHandle(), heightFieldStream.GetPosition());
    REQUIRE(heightField);
    SCOPE_EXIT
    {
        PhysicsBackend::DestroyObject(heightField);
    };
    CHECK(PhysicsBackend::GetHeightFieldHeight(heightField, 1, 0) == Approx(1.0f));
    PhysicsBackend::HeightFieldSample modifiedSample = {};
    modifiedSample.Height = 7;
    CHECK_FALSE(PhysicsBackend::ModifyHeightField(heightField, 1, 0, 1, 1, &modifiedSample));
    CHECK(PhysicsBackend::GetHeightFieldHeight(heightField, 1, 0) == Approx(7.0f));
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
#endif
}
