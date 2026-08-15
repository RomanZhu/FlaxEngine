// Copyright (c) Wojciech Figat. All rights reserved.

#if COMPILE_WITH_BOX3D

#include "Engine/Core/Log.h"
#include "Engine/Core/Utilities.h"
#include "Engine/Content/JsonAsset.h"
#include "Engine/Physics/PhysicsBackend.h"
#include "Engine/Physics/CollisionData.h"
#include "Engine/Physics/PhysicalMaterial.h"
#include "Engine/Physics/PhysicsScene.h"
#include "Engine/Physics/PhysicsStatistics.h"
#include "Engine/Physics/CollisionCooking.h"
#include "Engine/Physics/Actors/IPhysicsActor.h"
#include "Engine/Physics/Actors/PhysicsColliderActor.h"
#include "Engine/Physics/Joints/Limits.h"
#include "Engine/Physics/Joints/DistanceJoint.h"
#include "Engine/Physics/Joints/HingeJoint.h"
#include "Engine/Physics/Joints/SliderJoint.h"
#include "Engine/Physics/Joints/SphericalJoint.h"
#include "Engine/Physics/Joints/D6Joint.h"
#include "Engine/Physics/Colliders/Collider.h"
#include "Engine/Physics/Colliders/CharacterController.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Profiler/ProfilerMemory.h"
#include "Engine/Serialization/MemoryWriteStream.h"
#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/constants.h>

#define BOX3D_LENGTH_UNITS_PER_METER 100.0f
#define BOX3D_DEFAULT_SUBSTEPS 4
#define BOX3D_COOKED_MAGIC 0x33425846u
#define BOX3D_COOKED_VERSION 1u
#define BOX3D_CONVEX_VERTEX_MIN 4
#define BOX3D_CONVEX_VERTEX_MAX 40

namespace
{
    FORCE_INLINE float GetCastDistance(float maxDistance)
    {
        // Flax uses MAX_float as the default for an unbounded query. Box3D receives a
        // displacement vector instead of a scalar distance, and multiplying a direction
        // by MAX_float makes its broad-phase and height-field math numerically unusable.
        // Clamp only to Box3D's documented spatial limit; ordinary finite casts retain
        // their exact requested distance.
        return Math::Min(maxDistance, B3_HUGE);
    }

    struct SceneBox3D;
    struct ActorBox3D;
    struct ShapeBox3D;

    struct MaterialBox3D
    {
        PhysicalMaterial* Owner = nullptr;
        b3SurfaceMaterial Surface;
    };

    struct MeshBox3D
    {
        CollisionShape::Types Type = CollisionShape::Types::ConvexMesh;
        b3HullData* Hull = nullptr;
        b3MeshData* Mesh = nullptr;
        b3HeightFieldData* HeightField = nullptr;
        Array<Float3> Vertices;
        Array<int32> Indices;
        Array<uint32> Remap;
        Array<float> Heights;
        Array<uint8> HeightMaterials;
        Array<PhysicsBackend::HeightFieldSample> HeightSamples;
        Array<ShapeBox3D*> Shapes;
        Array<ShapeBox3D*> HeightFieldShapes;
        int32 Rows = 0;
        int32 Columns = 0;
        BoundingBox LocalBounds = BoundingBox::Empty;

        ~MeshBox3D();
    };

    struct ActorBox3D
    {
        b3BodyId Body = b3_nullBodyId;
        IPhysicsActor* Actor = nullptr;
        SceneBox3D* Scene = nullptr;
        Array<ShapeBox3D*> Shapes;
        PhysicsBackend::ActorFlags Flags = PhysicsBackend::ActorFlags::None;
        PhysicsBackend::RigidDynamicFlags DynamicFlags = PhysicsBackend::RigidDynamicFlags::None;
        RigidbodyConstraints Constraints = RigidbodyConstraints::None;
        bool AddedToScene = false;
        bool Static = false;
    };

    struct ShapeBox3D
    {
        b3ShapeId Shape = b3_nullShapeId;
        b3CompoundData* Compound = nullptr;
        PhysicsColliderActor* Collider = nullptr;
        ActorBox3D* Actor = nullptr;
        CollisionShape Geometry;
        Vector3 LocalPosition = Vector3::Zero;
        Quaternion LocalRotation = Quaternion::Identity;
        Array<JsonAsset*, InlinedAllocation<1>> Materials;
        Array<b3SurfaceMaterial, InlinedAllocation<1>> Surfaces;
        MeshBox3D* HeightFieldOwner = nullptr;
        b3HeightFieldData* HeightField = nullptr;
        uint32 Mask0 = MAX_uint32;
        uint32 Mask1 = MAX_uint32;
        float ContactOffset = 0.0f;
        bool Enabled = true;
        bool Trigger = false;
    };

    struct ControllerBox3D
    {
        SceneBox3D* Scene = nullptr;
        ActorBox3D* Actor = nullptr;
        ShapeBox3D* Shape = nullptr;
        IPhysicsActor* Owner = nullptr;
        float Radius = 0.0f;
        float Height = 0.0f;
        float StepOffset = 0.0f;
        float SlopeLimit = 0.0f;
        Vector3 Position = Vector3::Zero;
        Vector3 Up = Vector3::Up;
    };

    enum class JointTypeBox3D
    {
        Fixed,
        Distance,
        Hinge,
        Slider,
        Spherical,
        D6,
    };

    enum class D6JointKindBox3D
    {
        None,
        Weld,
        Revolute,
        Prismatic,
        Spherical,
    };

    struct JointBox3D
    {
        b3JointId Handle = b3_nullJointId;
        SceneBox3D* Scene = nullptr;
        Joint* Owner = nullptr;
        ActorBox3D* Actor0 = nullptr;
        ActorBox3D* Actor1 = nullptr;
        b3Transform LocalFrame0 = b3Transform_identity;
        b3Transform LocalFrame1 = b3Transform_identity;
        JointTypeBox3D Type = JointTypeBox3D::Fixed;
        PhysicsBackend::JointFlags Flags = PhysicsBackend::JointFlags::None;
        float BreakForce = MAX_float;
        float BreakTorque = MAX_float;

        float DistanceLength = 0.0f;
        DistanceJointFlag DistanceFlags = DistanceJointFlag::MinDistance | DistanceJointFlag::MaxDistance;
        float DistanceMin = 0.0f;
        float DistanceMax = 10.0f;
        float DistanceTolerance = 25.0f;
        SpringParameters DistanceSpring;

        HingeJointFlag HingeFlags = HingeJointFlag::Limit | HingeJointFlag::Drive;
        LimitAngularRange HingeLimit;
        HingeJointDrive HingeDrive;
        bool HingeDriveFreeSpin = false;

        SliderJointFlag SliderFlags = SliderJointFlag::Limit;
        LimitLinearRange SliderLimit;

        SphericalJointFlag SphericalFlags = SphericalJointFlag::Limit;
        LimitConeRange SphericalLimit;

        D6JointKindBox3D D6Kind = D6JointKindBox3D::None;
        D6JointAxis D6Axis = D6JointAxis::X;
        D6JointMotion D6Motion[static_cast<int32>(D6JointAxis::MAX)];
        D6JointDrive D6Drive[static_cast<int32>(D6JointDriveType::MAX)];
        LimitLinear D6LimitLinear;
        LimitAngularRange D6LimitTwist;
        LimitConeRange D6LimitSwing;
        Vector3 D6DrivePosition = Vector3::Zero;
        Quaternion D6DriveRotation = Quaternion::Identity;
        Vector3 D6DriveLinearVelocity = Vector3::Zero;
        Vector3 D6DriveAngularVelocity = Vector3::Zero;

        JointBox3D()
        {
            for (int32 i = 0; i < static_cast<int32>(D6JointAxis::MAX); i++)
                D6Motion[i] = D6JointMotion::Locked;
            D6LimitLinear.Extent = 100.0f;
        }
    };

    struct ActionDataBox3D
    {
        PhysicsBackend::ActionType Type;
        ActorBox3D* Actor;
    };

    struct SceneBox3D
    {
        b3WorldId World = b3_nullWorldId;
        PhysicsSettings Settings;
        float LastDeltaTime = 0.0f;
        bool EnableCCD = true;
        Array<ActorBox3D*> RemoveActors;
        Array<ActionDataBox3D> Actions;
    };

    struct Box3DCookedHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 Type;
        uint32 VertexCount;
        uint32 IndexCount;
        BoundingBox Bounds;
    };

    struct Box3DHeightFieldHeader
    {
        uint32 Magic;
        uint32 Version;
        uint32 Type;
        uint32 Columns;
        uint32 Rows;
    };

    void RecreateJointHandle(JointBox3D* joint);
    b3HeightFieldData* BuildHeightFieldData(MeshBox3D* heightField);
    b3HeightFieldData* BuildHeightFieldData(MeshBox3D* heightField, const b3Vec3& scale);

    PhysicsCombineMode FrictionCombineMode = PhysicsCombineMode::Average;
    PhysicsCombineMode RestitutionCombineMode = PhysicsCombineMode::Average;
    MaterialBox3D DefaultMaterial;

    FORCE_INLINE b3Vec3 C2BVec(const Vector3& v)
    {
        return { (float)v.X, (float)v.Y, (float)v.Z };
    }

    FORCE_INLINE b3Vec3 C2BVec(const Float3& v)
    {
        return { v.X, v.Y, v.Z };
    }

    FORCE_INLINE b3Pos C2BPos(const Vector3& v)
    {
        return { v.X, v.Y, v.Z };
    }

    FORCE_INLINE Vector3 B2C(const b3Vec3& v)
    {
        return Vector3(v.x, v.y, v.z);
    }

    FORCE_INLINE Vector3 B2C(const b3Pos& v)
    {
        return Vector3(v.x, v.y, v.z);
    }

    FORCE_INLINE b3Quat C2B(const Quaternion& q)
    {
        return { { q.X, q.Y, q.Z }, q.W };
    }

    FORCE_INLINE Quaternion B2C(const b3Quat& q)
    {
        return Quaternion(q.v.x, q.v.y, q.v.z, q.s);
    }

    FORCE_INLINE b3Transform C2BTransform(const Vector3& position, const Quaternion& orientation)
    {
        return { C2BVec(position), C2B(orientation) };
    }

    FORCE_INLINE b3WorldTransform C2BWorldTransform(const Vector3& position, const Quaternion& orientation)
    {
        return { C2BPos(position), C2B(orientation) };
    }

    int32 GetBox3DConvexVertexLimit(int32 requestedLimit, int32 pointCount)
    {
        const int32 maxLimit = Math::Min(pointCount, BOX3D_CONVEX_VERTEX_MAX);
        if (maxLimit < BOX3D_CONVEX_VERTEX_MIN)
            return 0;
        if (requestedLimit == 0)
            return maxLimit;
        return Math::Clamp(requestedLimit, BOX3D_CONVEX_VERTEX_MIN, maxLimit);
    }

    b3HullData* CreateBox3DConvexHull(const b3Vec3* points, int32 pointCount, int32 requestedLimit)
    {
        int32 vertexLimit = GetBox3DConvexVertexLimit(requestedLimit, pointCount);
        while (vertexLimit >= BOX3D_CONVEX_VERTEX_MIN)
        {
            if (b3HullData* hull = b3CreateHull(points, pointCount, vertexLimit))
                return hull;
            if (vertexLimit == BOX3D_CONVEX_VERTEX_MIN)
                break;
            vertexLimit = Math::Max(BOX3D_CONVEX_VERTEX_MIN, vertexLimit - 8);
        }
        return nullptr;
    }

    FORCE_INLINE b3Vec3 Rotate(const Quaternion& q, const Vector3& v)
    {
        return b3RotateVector(C2B(q), C2BVec(v));
    }

    FORCE_INLINE b3Filter MakeFilter(uint32 mask0, uint32 mask1)
    {
        b3Filter filter = b3DefaultFilter();
        filter.categoryBits = mask0;
        filter.maskBits = mask1;
        return filter;
    }

    FORCE_INLINE b3QueryFilter MakeQueryFilter(uint32 layerMask)
    {
        b3QueryFilter filter = b3DefaultQueryFilter();
        filter.categoryBits = MAX_uint32;
        filter.maskBits = layerMask;
        return filter;
    }

    FORCE_INLINE float DensityToBox3D(float kgPerM3)
    {
        const float lengthUnits = BOX3D_LENGTH_UNITS_PER_METER;
        return Math::Max(kgPerM3, 0.08375f) / (lengthUnits * lengthUnits * lengthUnits);
    }

    void UpdateMaterial(MaterialBox3D* material)
    {
        material->Surface = b3DefaultSurfaceMaterial();
        if (material->Owner)
        {
            material->Surface.friction = material->Owner->Friction;
            material->Surface.restitution = material->Owner->Restitution;
            material->Surface.userMaterialId = (uint64)material->Owner;
        }
    }

    MaterialBox3D* GetMaterial(JsonAsset* materialAsset)
    {
        if (materialAsset && !materialAsset->WaitForLoaded() && materialAsset->Instance)
            return (MaterialBox3D*)((PhysicalMaterial*)materialAsset->Instance)->GetPhysicsMaterial();
        return &DefaultMaterial;
    }

    void UpdateShapeSurfaces(ShapeBox3D* shape)
    {
        shape->Surfaces.Resize(Math::Max(shape->Materials.Count(), 1));
        if (shape->Materials.IsEmpty())
        {
            shape->Surfaces[0] = DefaultMaterial.Surface;
            return;
        }
        for (int32 i = 0; i < shape->Materials.Count(); i++)
            shape->Surfaces[i] = GetMaterial(shape->Materials[i])->Surface;
    }

    bool IsShapeValid(const ShapeBox3D* shape)
    {
        return B3_IS_NON_NULL(shape->Shape) && b3Shape_IsValid(shape->Shape);
    }

    bool ComputeActorMassData(ActorBox3D* actor, b3MassData& result)
    {
        result = {};
        Array<b3MassData, InlinedAllocation<8>> shapeMasses;
        b3Vec3 center = b3Vec3_zero;
        for (auto shape : actor->Shapes)
        {
            if (!IsShapeValid(shape))
                continue;
            b3MassData massData = b3Shape_ComputeMassData(shape->Shape);
            if (massData.mass <= 0.0f)
                continue;
            result.mass += massData.mass;
            center = b3MulAdd(center, massData.mass, massData.center);
            shapeMasses.Add(massData);
        }
        if (result.mass <= 0.0f)
            return false;

        center = b3MulSV(1.0f / result.mass, center);
        for (const b3MassData& massData : shapeMasses)
        {
            const b3Vec3 offset = b3Sub(center, massData.center);
            result.inertia = b3AddMM(result.inertia, b3AddMM(massData.inertia, b3Steiner(massData.mass, offset)));
        }
        result.center = center;
        return true;
    }

    PhysicsColliderActor* GetCollider(b3ShapeId shapeId)
    {
        if (B3_IS_NULL(shapeId) || !b3Shape_IsValid(shapeId))
            return nullptr;
        auto shape = (ShapeBox3D*)b3Shape_GetUserData(shapeId);
        return shape ? shape->Collider : nullptr;
    }

    void DestroyRuntimeShape(ShapeBox3D* shape)
    {
        if (!shape)
            return;
        if (shape->Geometry.Type == CollisionShape::Types::ConvexMesh)
        {
            auto mesh = (MeshBox3D*)shape->Geometry.ConvexMesh.ConvexMesh;
            if (mesh)
                mesh->Shapes.Remove(shape);
        }
        else if (shape->Geometry.Type == CollisionShape::Types::TriangleMesh)
        {
            auto mesh = (MeshBox3D*)shape->Geometry.TriangleMesh.TriangleMesh;
            if (mesh)
                mesh->Shapes.Remove(shape);
        }
        if (shape->HeightFieldOwner)
        {
            shape->HeightFieldOwner->HeightFieldShapes.Remove(shape);
            shape->HeightFieldOwner = nullptr;
        }
        if (IsShapeValid(shape))
            b3DestroyShape(shape->Shape, true);
        shape->Shape = b3_nullShapeId;
        if (shape->HeightField)
        {
            b3DestroyHeightField(shape->HeightField);
            shape->HeightField = nullptr;
        }
        if (shape->Compound)
        {
            b3DestroyCompound(shape->Compound);
            shape->Compound = nullptr;
        }
    }

    MeshBox3D::~MeshBox3D()
    {
        // Runtime Box3D shapes reference cooked mesh data directly. Invalidate every
        // dependent shape before releasing that data (for example during an async
        // CollisionData reload) so scene queries cannot observe a dangling mesh.
        while (Shapes.HasItems())
        {
            auto shape = Shapes.Last();
            Shapes.RemoveLast();
            if (!shape)
                continue;
            DestroyRuntimeShape(shape);
            if (shape->Geometry.Type == CollisionShape::Types::ConvexMesh && shape->Geometry.ConvexMesh.ConvexMesh == this)
                shape->Geometry.ConvexMesh.ConvexMesh = nullptr;
            else if (shape->Geometry.Type == CollisionShape::Types::TriangleMesh && shape->Geometry.TriangleMesh.TriangleMesh == this)
                shape->Geometry.TriangleMesh.TriangleMesh = nullptr;
        }
        while (HeightFieldShapes.HasItems())
        {
            auto shape = HeightFieldShapes.Last();
            HeightFieldShapes.RemoveLast();
            if (!shape)
                continue;
            DestroyRuntimeShape(shape);
            if (shape->Geometry.Type == CollisionShape::Types::HeightField && shape->Geometry.HeightField.HeightField == this)
                shape->Geometry.HeightField.HeightField = nullptr;
        }
        if (Hull)
            b3DestroyHull(Hull);
        if (Mesh)
            b3DestroyMesh(Mesh);
        if (HeightField)
            b3DestroyHeightField(HeightField);
    }

    b3ShapeDef MakeShapeDef(ShapeBox3D* shape)
    {
        UpdateShapeSurfaces(shape);
        b3ShapeDef def = b3DefaultShapeDef();
        def.userData = shape;
        def.baseMaterial = shape->Surfaces[0];
        def.materials = shape->Surfaces.Get();
        def.materialCount = shape->Surfaces.Count();
        def.density = DensityToBox3D(DefaultMaterial.Owner ? DefaultMaterial.Owner->Density : 1000.0f);
        if (shape->Materials.HasItems())
        {
            if (auto materialAsset = shape->Materials[0])
            {
                if (!materialAsset->WaitForLoaded() && materialAsset->Instance)
                    def.density = DensityToBox3D(((PhysicalMaterial*)materialAsset->Instance)->Density);
            }
        }
        def.filter = MakeFilter(shape->Mask0, shape->Mask1);
        def.isSensor = shape->Trigger;
        def.enableSensorEvents = true;
        def.enableContactEvents = true;
        def.enableHitEvents = true;
        return def;
    }

    void RecreateRuntimeShape(ShapeBox3D* shape)
    {
        DestroyRuntimeShape(shape);
        if (!shape->Enabled || !shape->Actor || B3_IS_NULL(shape->Actor->Body) || !b3Body_IsValid(shape->Actor->Body))
            return;

        b3ShapeDef def = MakeShapeDef(shape);
        switch (shape->Geometry.Type)
        {
        case CollisionShape::Types::Sphere:
        {
            b3Sphere sphere;
            sphere.center = C2BVec(shape->LocalPosition);
            sphere.radius = shape->Geometry.Sphere.Radius;
            shape->Shape = b3CreateSphereShape(shape->Actor->Body, &def, &sphere);
            break;
        }
        case CollisionShape::Types::Capsule:
        {
            const Vector3 axis(shape->Geometry.Capsule.HalfHeight, 0.0f, 0.0f);
            b3Capsule capsule;
            capsule.center1 = b3Sub(C2BVec(shape->LocalPosition), Rotate(shape->LocalRotation, axis));
            capsule.center2 = b3Add(C2BVec(shape->LocalPosition), Rotate(shape->LocalRotation, axis));
            capsule.radius = shape->Geometry.Capsule.Radius;
            shape->Shape = b3CreateCapsuleShape(shape->Actor->Body, &def, &capsule);
            break;
        }
        case CollisionShape::Types::Box:
        {
            b3BoxHull box = b3MakeTransformedBoxHull(
                Math::Max(shape->Geometry.Box.HalfExtents[0], B3_MIN_SCALE),
                Math::Max(shape->Geometry.Box.HalfExtents[1], B3_MIN_SCALE),
                Math::Max(shape->Geometry.Box.HalfExtents[2], B3_MIN_SCALE),
                C2BTransform(shape->LocalPosition, shape->LocalRotation));
            shape->Shape = b3CreateHullShape(shape->Actor->Body, &def, &box.base);
            break;
        }
        case CollisionShape::Types::ConvexMesh:
        {
            auto mesh = (MeshBox3D*)shape->Geometry.ConvexMesh.ConvexMesh;
            if (mesh && mesh->Hull)
            {
                const b3Vec3 scale = { shape->Geometry.ConvexMesh.Scale[0], shape->Geometry.ConvexMesh.Scale[1], shape->Geometry.ConvexMesh.Scale[2] };
                shape->Shape = b3CreateTransformedHullShape(shape->Actor->Body, &def, mesh->Hull, C2BTransform(shape->LocalPosition, shape->LocalRotation), scale);
                if (IsShapeValid(shape) && !mesh->Shapes.Contains(shape))
                    mesh->Shapes.Add(shape);
            }
            break;
        }
        case CollisionShape::Types::TriangleMesh:
        {
            auto mesh = (MeshBox3D*)shape->Geometry.TriangleMesh.TriangleMesh;
            if (mesh && mesh->Mesh)
            {
                const b3Vec3 scale = { shape->Geometry.TriangleMesh.Scale[0], shape->Geometry.TriangleMesh.Scale[1], shape->Geometry.TriangleMesh.Scale[2] };
                if (shape->LocalPosition.IsZero() && shape->LocalRotation.IsIdentity())
                {
                    shape->Shape = b3CreateMeshShape(shape->Actor->Body, &def, mesh->Mesh, scale);
                }
                else if (shape->Actor->Static)
                {
                    b3CompoundMeshDef meshDef;
                    Platform::MemoryClear(&meshDef, sizeof(meshDef));
                    meshDef.meshData = mesh->Mesh;
                    meshDef.transform = C2BTransform(shape->LocalPosition, shape->LocalRotation);
                    meshDef.scale = scale;
                    meshDef.materials = shape->Surfaces.Get();
                    meshDef.materialCount = Math::Min(shape->Surfaces.Count(), B3_MAX_COMPOUND_MESH_MATERIALS);

                    b3CompoundDef compoundDef;
                    Platform::MemoryClear(&compoundDef, sizeof(compoundDef));
                    compoundDef.meshes = &meshDef;
                    compoundDef.meshCount = 1;
                    shape->Compound = b3CreateCompound(&compoundDef);
                    shape->Shape = b3CreateCompoundShape(shape->Actor->Body, &def, shape->Compound);
                }
                if (IsShapeValid(shape) && !mesh->Shapes.Contains(shape))
                    mesh->Shapes.Add(shape);
            }
            break;
        }
        case CollisionShape::Types::HeightField:
        {
            auto mesh = (MeshBox3D*)shape->Geometry.HeightField.HeightField;
            if (mesh && mesh->HeightField)
            {
                const b3Vec3 scale = {
                    Math::Max(shape->Geometry.HeightField.RowScale, B3_MIN_SCALE),
                    Math::Max(shape->Geometry.HeightField.HeightScale, B3_MIN_SCALE),
                    Math::Max(shape->Geometry.HeightField.ColumnScale, B3_MIN_SCALE)
                };
                shape->HeightField = BuildHeightFieldData(mesh, scale);
                if (shape->HeightField)
                    shape->Shape = b3CreateHeightFieldShape(shape->Actor->Body, &def, shape->HeightField);
                shape->HeightFieldOwner = mesh;
                if (!mesh->HeightFieldShapes.Contains(shape))
                    mesh->HeightFieldShapes.Add(shape);
            }
            break;
        }
        }
    }

    void AttachShapeInternal(ShapeBox3D* shape, ActorBox3D* actor)
    {
        if (shape->Actor == actor)
            return;
        if (shape->Actor)
            shape->Actor->Shapes.Remove(shape);
        shape->Actor = actor;
        if (actor && !actor->Shapes.Contains(shape))
            actor->Shapes.Add(shape);
        RecreateRuntimeShape(shape);
    }

    struct PenetrationShapeBox3D
    {
        CollisionShape::Types Type = CollisionShape::Types::Sphere;
        b3Sphere Sphere = {};
        b3Capsule Capsule = {};
        b3BoxHull Box = {};
        b3HullData* OwnedHull = nullptr;
        const b3HullData* Hull = nullptr;

        ~PenetrationShapeBox3D()
        {
            if (OwnedHull)
                b3DestroyHull(OwnedHull);
        }

        bool IsHull() const
        {
            return Type == CollisionShape::Types::Box || Type == CollisionShape::Types::ConvexMesh;
        }
    };

    bool BuildPenetrationShape(const ShapeBox3D* shape, PenetrationShapeBox3D& result)
    {
        if (!shape || !shape->Enabled)
            return false;

        result.Type = shape->Geometry.Type;
        switch (shape->Geometry.Type)
        {
        case CollisionShape::Types::Sphere:
            result.Sphere.center = C2BVec(shape->LocalPosition);
            result.Sphere.radius = Math::Max(shape->Geometry.Sphere.Radius, B3_MIN_SCALE);
            return true;
        case CollisionShape::Types::Capsule:
        {
            const Vector3 axis(shape->Geometry.Capsule.HalfHeight, 0.0f, 0.0f);
            result.Capsule.center1 = b3Sub(C2BVec(shape->LocalPosition), Rotate(shape->LocalRotation, axis));
            result.Capsule.center2 = b3Add(C2BVec(shape->LocalPosition), Rotate(shape->LocalRotation, axis));
            result.Capsule.radius = Math::Max(shape->Geometry.Capsule.Radius, B3_MIN_SCALE);
            return true;
        }
        case CollisionShape::Types::Box:
            result.Box = b3MakeTransformedBoxHull(
                Math::Max(shape->Geometry.Box.HalfExtents[0], B3_MIN_SCALE),
                Math::Max(shape->Geometry.Box.HalfExtents[1], B3_MIN_SCALE),
                Math::Max(shape->Geometry.Box.HalfExtents[2], B3_MIN_SCALE),
                C2BTransform(shape->LocalPosition, shape->LocalRotation));
            result.Hull = &result.Box.base;
            return true;
        case CollisionShape::Types::ConvexMesh:
        {
            auto mesh = (MeshBox3D*)shape->Geometry.ConvexMesh.ConvexMesh;
            if (!mesh || !mesh->Hull)
                return false;
            const b3Vec3 scale = { shape->Geometry.ConvexMesh.Scale[0], shape->Geometry.ConvexMesh.Scale[1], shape->Geometry.ConvexMesh.Scale[2] };
            result.OwnedHull = b3CloneAndTransformHull(mesh->Hull, C2BTransform(shape->LocalPosition, shape->LocalRotation), scale);
            result.Hull = result.OwnedHull;
            return result.Hull != nullptr;
        }
        default:
            return false;
        }
    }

    bool ReadPenetration(const b3LocalManifold& manifold, const Quaternion& frameOrientation, const Vector3& fallbackNormal, bool swapped, Vector3& direction, float& distance)
    {
        if (manifold.pointCount <= 0)
            return false;

        float deepestSeparation = 0.0f;
        bool hasPenetration = false;
        for (int32 i = 0; i < manifold.pointCount; i++)
        {
            if (manifold.points[i].separation < deepestSeparation)
            {
                deepestSeparation = manifold.points[i].separation;
                hasPenetration = true;
            }
        }
        if (!hasPenetration)
            return false;

        Vector3 normal = Vector3::Transform(B2C(manifold.normal), frameOrientation);
        if (normal.LengthSquared() < ZeroTolerance)
            normal = fallbackNormal;
        if (normal.LengthSquared() < ZeroTolerance)
            normal = Vector3::Up;
        normal.Normalize();

        direction = swapped ? normal : normal.GetNegative();
        distance = -deepestSeparation;
        return true;
    }

    bool ComputeConvexPenetration(
        const PenetrationShapeBox3D& shapeA,
        const PenetrationShapeBox3D& shapeB,
        b3Transform transformBtoA,
        b3Transform transformAtoB,
        const Quaternion& orientationA,
        const Quaternion& orientationB,
        const Vector3& positionA,
        const Vector3& positionB,
        Vector3& direction,
        float& distance)
    {
        b3LocalManifoldPoint points[32];
        b3LocalManifold manifold = {};
        manifold.points = points;
        b3SimplexCache simplexCache = {};
        b3SATCache satCache = {};
        bool swapped = false;

        if (shapeA.Type == CollisionShape::Types::Sphere)
        {
            if (shapeB.Type == CollisionShape::Types::Sphere)
            {
                b3CollideSpheres(&manifold, ARRAY_COUNT(points), &shapeA.Sphere, &shapeB.Sphere, transformBtoA);
            }
            else if (shapeB.Type == CollisionShape::Types::Capsule)
            {
                b3CollideCapsuleAndSphere(&manifold, ARRAY_COUNT(points), &shapeB.Capsule, &shapeA.Sphere, transformAtoB);
                swapped = true;
            }
            else if (shapeB.IsHull())
            {
                b3CollideHullAndSphere(&manifold, ARRAY_COUNT(points), shapeB.Hull, &shapeA.Sphere, transformAtoB, &simplexCache);
                swapped = true;
            }
        }
        else if (shapeA.Type == CollisionShape::Types::Capsule)
        {
            if (shapeB.Type == CollisionShape::Types::Sphere)
            {
                b3CollideCapsuleAndSphere(&manifold, ARRAY_COUNT(points), &shapeA.Capsule, &shapeB.Sphere, transformBtoA);
            }
            else if (shapeB.Type == CollisionShape::Types::Capsule)
            {
                b3CollideCapsules(&manifold, ARRAY_COUNT(points), &shapeA.Capsule, &shapeB.Capsule, transformBtoA);
            }
            else if (shapeB.IsHull())
            {
                b3CollideHullAndCapsule(&manifold, ARRAY_COUNT(points), shapeB.Hull, &shapeA.Capsule, transformAtoB, &simplexCache);
                swapped = true;
            }
        }
        else if (shapeA.IsHull())
        {
            if (shapeB.Type == CollisionShape::Types::Sphere)
            {
                b3CollideHullAndSphere(&manifold, ARRAY_COUNT(points), shapeA.Hull, &shapeB.Sphere, transformBtoA, &simplexCache);
            }
            else if (shapeB.Type == CollisionShape::Types::Capsule)
            {
                b3CollideHullAndCapsule(&manifold, ARRAY_COUNT(points), shapeA.Hull, &shapeB.Capsule, transformBtoA, &simplexCache);
            }
            else if (shapeB.IsHull())
            {
                b3CollideHulls(&manifold, ARRAY_COUNT(points), shapeA.Hull, shapeB.Hull, transformBtoA, &satCache);
            }
        }

        const Quaternion& frameOrientation = swapped ? orientationB : orientationA;
        const Vector3 fallbackNormal = swapped ? positionA - positionB : positionB - positionA;
        return ReadPenetration(manifold, frameOrientation, fallbackNormal, swapped, direction, distance);
    }

    bool TransformPenetrationShape(PenetrationShapeBox3D& shape, const b3Transform& transform)
    {
        if (shape.Type == CollisionShape::Types::Sphere)
        {
            shape.Sphere.center = b3TransformPoint(transform, shape.Sphere.center);
            return true;
        }
        if (shape.Type == CollisionShape::Types::Capsule)
        {
            shape.Capsule.center1 = b3TransformPoint(transform, shape.Capsule.center1);
            shape.Capsule.center2 = b3TransformPoint(transform, shape.Capsule.center2);
            return true;
        }
        if (shape.IsHull())
        {
            b3HullData* transformed = b3CloneAndTransformHull(shape.Hull, transform, { 1.0f, 1.0f, 1.0f });
            if (!transformed)
                return false;
            if (shape.OwnedHull)
                b3DestroyHull(shape.OwnedHull);
            shape.OwnedHull = transformed;
            shape.Hull = transformed;
            return true;
        }
        return false;
    }

    struct HeightFieldPenetrationContext
    {
        const PenetrationShapeBox3D* Shape = nullptr;
        Quaternion Orientation = Quaternion::Identity;
        Vector3 FallbackNormal = Vector3::Up;
        Vector3 Direction = Vector3::Zero;
        float Distance = 0.0f;
    };

    bool HeightFieldPenetrationCallback(b3Vec3 a, b3Vec3 b, b3Vec3 c, int triangleIndex, void* contextPtr)
    {
        auto context = (HeightFieldPenetrationContext*)contextPtr;
        b3LocalManifoldPoint points[32];
        b3LocalManifold manifold = {};
        manifold.points = points;
        const b3Vec3 triangle[3] = { a, b, c };
        b3SimplexCache simplexCache = {};
        b3SATCache satCache = {};

        if (context->Shape->Type == CollisionShape::Types::Sphere)
            b3CollideSphereAndTriangle(&manifold, ARRAY_COUNT(points), &context->Shape->Sphere, triangle);
        else if (context->Shape->Type == CollisionShape::Types::Capsule)
            b3CollideCapsuleAndTriangle(&manifold, ARRAY_COUNT(points), &context->Shape->Capsule, triangle, &simplexCache);
        else if (context->Shape->IsHull())
            b3CollideHullAndTriangle(&manifold, ARRAY_COUNT(points), context->Shape->Hull, a, b, c, 0, &satCache);

        Vector3 direction;
        float distance;
        if (ReadPenetration(manifold, context->Orientation, context->FallbackNormal, true, direction, distance) && distance > context->Distance)
        {
            context->Direction = direction;
            context->Distance = distance;
        }
        return true;
    }

    bool ComputeHeightFieldPenetration(const ShapeBox3D* convexShape, const ShapeBox3D* heightShape,
                                       const Vector3& convexPosition, const Quaternion& convexOrientation,
                                       const Vector3& heightPosition, const Quaternion& heightOrientation,
                                       Vector3& direction, float& distance)
    {
        if (!heightShape || !heightShape->HeightField)
            return false;

        PenetrationShapeBox3D convex;
        if (!BuildPenetrationShape(convexShape, convex))
            return false;

        const b3WorldTransform heightWorld = b3MulWorldTransforms(C2BWorldTransform(heightPosition, heightOrientation), C2BTransform(heightShape->LocalPosition, heightShape->LocalRotation));
        const b3Transform convexToHeight = b3InvMulWorldTransforms(heightWorld, C2BWorldTransform(convexPosition, convexOrientation));
        if (!TransformPenetrationShape(convex, convexToHeight))
            return false;

        b3AABB bounds;
        if (convex.Type == CollisionShape::Types::Sphere)
            bounds = b3ComputeSphereAABB(&convex.Sphere, b3Transform_identity);
        else if (convex.Type == CollisionShape::Types::Capsule)
            bounds = b3ComputeCapsuleAABB(&convex.Capsule, b3Transform_identity);
        else
            bounds = b3ComputeHullAABB(convex.Hull, b3Transform_identity);

        HeightFieldPenetrationContext context;
        context.Shape = &convex;
        context.Orientation = B2C(heightWorld.q);
        context.FallbackNormal = convexPosition - heightPosition;
        b3QueryHeightField(heightShape->HeightField, bounds, HeightFieldPenetrationCallback, &context);
        direction = context.Direction;
        distance = context.Distance;
        return distance > 0.0f;
    }

    void ApplyActorFlags(ActorBox3D* actor)
    {
        if (!actor || B3_IS_NULL(actor->Body) || !b3Body_IsValid(actor->Body))
            return;

        b3Body_SetGravityScale(actor->Body, EnumHasAnyFlags(actor->Flags, PhysicsBackend::ActorFlags::NoGravity) ? 0.0f : 1.0f);
        if (EnumHasAnyFlags(actor->Flags, PhysicsBackend::ActorFlags::NoSimulation))
            b3Body_Disable(actor->Body);
        else if (actor->AddedToScene && !b3Body_IsEnabled(actor->Body))
            b3Body_Enable(actor->Body);
    }

    void ApplyDynamicFlags(ActorBox3D* actor)
    {
        if (!actor || actor->Static || B3_IS_NULL(actor->Body) || !b3Body_IsValid(actor->Body))
            return;
        b3Body_SetType(actor->Body, EnumHasAnyFlags(actor->DynamicFlags, PhysicsBackend::RigidDynamicFlags::Kinematic) ? b3_kinematicBody : b3_dynamicBody);
        b3Body_SetBullet(actor->Body, EnumHasAnyFlags(actor->DynamicFlags, PhysicsBackend::RigidDynamicFlags::CCD));
    }

    void ApplyConstraints(ActorBox3D* actor)
    {
        if (!actor || B3_IS_NULL(actor->Body) || !b3Body_IsValid(actor->Body))
            return;
        b3MotionLocks locks = {};
        locks.linearX = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockPositionX);
        locks.linearY = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockPositionY);
        locks.linearZ = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockPositionZ);
        locks.angularX = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockRotationX);
        locks.angularY = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockRotationY);
        locks.angularZ = EnumHasAnyFlags(actor->Constraints, RigidbodyConstraints::LockRotationZ);
        b3Body_SetMotionLocks(actor->Body, locks);
    }

    ActorBox3D* CreateActor(IPhysicsActor* actor, const Vector3& position, const Quaternion& orientation, SceneBox3D* scene, b3BodyType type)
    {
        auto result = New<ActorBox3D>();
        result->Actor = actor;
        result->Scene = scene;
        result->Static = type == b3_staticBody;

        b3BodyDef def = b3DefaultBodyDef();
        def.type = type;
        def.position = C2BPos(position);
        def.rotation = C2B(orientation);
        def.userData = actor;
        def.isEnabled = false;
        result->Body = b3CreateBody(scene->World, &def);
        return result;
    }

    void ClearPendingActorRequests(ActorBox3D* actor)
    {
        if (!actor || !actor->Scene)
            return;

        actor->Scene->RemoveActors.RemoveAll(actor);
        for (int32 i = actor->Scene->Actions.Count() - 1; i >= 0; i--)
        {
            if (actor->Scene->Actions[i].Actor == actor)
                actor->Scene->Actions.RemoveAt(i);
        }
    }

    void SendTriggerEvent(b3ShapeId sensorId, b3ShapeId visitorId, bool enter)
    {
        PhysicsColliderActor* sensor = GetCollider(sensorId);
        PhysicsColliderActor* visitor = GetCollider(visitorId);
        if (!sensor || !visitor)
            return;
        if (enter)
        {
            sensor->OnTriggerEnter(visitor);
            visitor->OnTriggerEnter(sensor);
        }
        else
        {
            sensor->OnTriggerExit(visitor);
            visitor->OnTriggerExit(sensor);
        }
    }

    void FillCollision(Collision& collision, b3ShapeId shapeA, b3ShapeId shapeB, b3ContactId contactId)
    {
        collision.ThisActor = GetCollider(shapeA);
        collision.OtherActor = GetCollider(shapeB);
        collision.Impulse = Vector3::Zero;
        collision.ThisVelocity = Vector3::Zero;
        collision.OtherVelocity = Vector3::Zero;
        collision.ContactsCount = 0;

        if (B3_IS_NON_NULL(shapeA) && b3Shape_IsValid(shapeA))
            collision.ThisVelocity = B2C(b3Body_GetLinearVelocity(b3Shape_GetBody(shapeA)));
        if (B3_IS_NON_NULL(shapeB) && b3Shape_IsValid(shapeB))
            collision.OtherVelocity = B2C(b3Body_GetLinearVelocity(b3Shape_GetBody(shapeB)));

        if (B3_IS_NON_NULL(contactId) && b3Contact_IsValid(contactId))
        {
            b3ContactData contact = b3Contact_GetData(contactId);
            for (int32 manifoldIndex = 0; manifoldIndex < contact.manifoldCount && collision.ContactsCount < COLLISION_NAX_CONTACT_POINTS; manifoldIndex++)
            {
                const b3Manifold& manifold = contact.manifolds[manifoldIndex];
                for (int32 pointIndex = 0; pointIndex < manifold.pointCount && collision.ContactsCount < COLLISION_NAX_CONTACT_POINTS; pointIndex++)
                {
                    const b3ManifoldPoint& point = manifold.points[pointIndex];
                    ContactPoint& dst = collision.Contacts[collision.ContactsCount++];
                    dst.Normal = B2C(manifold.normal);
                    dst.Separation = point.separation;
                    if (B3_IS_NON_NULL(shapeA) && b3Shape_IsValid(shapeA))
                        dst.Point = B2C(b3Body_GetWorldCenterOfMass(b3Shape_GetBody(shapeA))) + B2C(point.anchorA);
                    else
                        dst.Point = Vector3::Zero;
                    collision.Impulse += dst.Normal * point.totalNormalImpulse;
                }
            }
        }
    }

    void SendCollisionEvent(b3ShapeId shapeA, b3ShapeId shapeB, b3ContactId contactId, bool enter)
    {
        Collision collision;
        FillCollision(collision, shapeA, shapeB, contactId);
        if (!collision.ThisActor || !collision.OtherActor)
            return;

        if (enter)
        {
            collision.ThisActor->OnCollisionEnter(collision);
            collision.SwapObjects();
            collision.ThisActor->OnCollisionEnter(collision);
        }
        else
        {
            collision.ThisActor->OnCollisionExit(collision);
            collision.SwapObjects();
            collision.ThisActor->OnCollisionExit(collision);
        }
    }

    struct QueryContext
    {
        uint32 LayerMask = MAX_uint32;
        bool HitTriggers = true;
        bool Any = false;
        bool All = false;
        float MaxDistance = 0.0f;
        Vector3 Center = Vector3::Zero;
        Vector3 Direction = Vector3::Zero;
        RayCastHit Hit;
        Array<RayCastHit, HeapAllocation>* Results = nullptr;
        RayCastHit* ResultsBuffer = nullptr;
        int32 ResultsCapacity = 0;
        int32 ResultsCount = 0;
        Array<b3ShapeId, InlinedAllocation<8>> InitialOverlaps;
    };

    bool AcceptQueryShape(const QueryContext& context, b3ShapeId shapeId)
    {
        if (B3_IS_NULL(shapeId) || !b3Shape_IsValid(shapeId))
            return false;
        if (!context.HitTriggers && b3Shape_IsSensor(shapeId))
            return false;
        return GetCollider(shapeId) != nullptr;
    }

    void FillRayHit(RayCastHit& hit, b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, float maxDistance, uint64 userMaterialId, int triangleIndex)
    {
        hit.Collider = GetCollider(shapeId);
        hit.Material = (PhysicalMaterial*)userMaterialId;
        hit.Normal = B2C(normal);
        hit.Distance = fraction * maxDistance;
        hit.Point = B2C(point);
        hit.FaceIndex = triangleIndex >= 0 ? (uint32)triangleIndex : MAX_uint32;
        hit.UV = Float2::Zero;
    }

    bool StoreQueryHit(QueryContext& context, const RayCastHit& hit)
    {
        if (context.Results)
        {
            context.Results->Add(hit);
            return true;
        }
        if (context.ResultsBuffer)
        {
            if (context.ResultsCount >= context.ResultsCapacity)
                return false;
            context.ResultsBuffer[context.ResultsCount++] = hit;
            return context.ResultsCount < context.ResultsCapacity;
        }
        context.Hit = hit;
        return false;
    }

    float QueryCastCallback(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId, int triangleIndex, int childIndex, void* userContext)
    {
        auto context = (QueryContext*)userContext;
        if (!AcceptQueryShape(*context, shapeId))
            return -1.0f;
        for (const b3ShapeId overlap : context->InitialOverlaps)
        {
            if (B3_ID_EQUALS(shapeId, overlap))
                return -1.0f;
        }

        RayCastHit hit;
        FillRayHit(hit, shapeId, point, normal, fraction, context->MaxDistance, userMaterialId, triangleIndex);
        if (context->All)
        {
            return StoreQueryHit(*context, hit) ? 1.0f : 0.0f;
        }
        context->Hit = hit;
        if (context->Any)
            return 0.0f;
        return fraction;
    }

    bool InitialOverlapCastCallback(b3ShapeId shapeId, void* userContext)
    {
        auto context = (QueryContext*)userContext;
        if (!AcceptQueryShape(*context, shapeId))
            return true;
        auto shape = (ShapeBox3D*)b3Shape_GetUserData(shapeId);
        const uint64 material = shape && shape->Surfaces.HasItems() ? shape->Surfaces[0].userMaterialId : 0;
        context->InitialOverlaps.Add(shapeId);
        RayCastHit hit;
        FillRayHit(hit, shapeId, C2BPos(context->Center), C2BVec(-context->Direction), 0.0f, context->MaxDistance, material, -1);
        return StoreQueryHit(*context, hit);
    }

    float CharacterCastCallback(b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId, int triangleIndex, int childIndex, void* userContext)
    {
        auto context = (QueryContext*)userContext;
        if (!AcceptQueryShape(*context, shapeId))
            return -1.0f;
        return fraction;
    }

    struct OverlapContext
    {
        bool HitTriggers = true;
        Array<PhysicsColliderActor*, HeapAllocation>* Results = nullptr;
        PhysicsOverlapResultBuffer* ResultsBuffer = nullptr;
        bool Any = false;
        bool Hit = false;
    };

    bool OverlapCallback(b3ShapeId shapeId, void* userContext)
    {
        auto context = (OverlapContext*)userContext;
        if (B3_IS_NULL(shapeId) || !b3Shape_IsValid(shapeId))
            return true;
        if (!context->HitTriggers && b3Shape_IsSensor(shapeId))
            return true;
        PhysicsColliderActor* collider = GetCollider(shapeId);
        if (!collider)
            return true;
        context->Hit = true;
        if (context->Results)
            context->Results->Add(collider);
        else if (context->ResultsBuffer && !context->ResultsBuffer->Add(collider))
            return false;
        return !context->Any;
    }

    void MakeBoxProxy(const Vector3& halfExtents, const Quaternion& rotation, Array<b3Vec3, InlinedAllocation<8>>& points, b3ShapeProxy& proxy)
    {
        points.Resize(8, false);
        int32 i = 0;
        for (int32 x = -1; x <= 1; x += 2)
        for (int32 y = -1; y <= 1; y += 2)
        for (int32 z = -1; z <= 1; z += 2)
            points[i++] = Rotate(rotation, Vector3(halfExtents.X * x, halfExtents.Y * y, halfExtents.Z * z));
        proxy.points = points.Get();
        proxy.count = points.Count();
        proxy.radius = 0.0f;
    }

    void MakeSphereProxy(float radius, Array<b3Vec3, InlinedAllocation<8>>& points, b3ShapeProxy& proxy)
    {
        points.Resize(1, false);
        points[0] = b3Vec3_zero;
        proxy.points = points.Get();
        proxy.count = 1;
        proxy.radius = radius;
    }

    void MakeCapsuleProxy(float radius, float height, const Quaternion& rotation, Array<b3Vec3, InlinedAllocation<8>>& points, b3ShapeProxy& proxy)
    {
        points.Resize(2, false);
        const Vector3 axis(height * 0.5f, 0.0f, 0.0f);
        points[0] = Rotate(rotation, -axis);
        points[1] = Rotate(rotation, axis);
        proxy.points = points.Get();
        proxy.count = 2;
        proxy.radius = radius;
    }

    bool CastShape(SceneBox3D* scene, const Vector3& center, const b3ShapeProxy& proxy, const Vector3& direction, RayCastHit* hitInfo, Array<RayCastHit, HeapAllocation>* results, float maxDistance, uint32 layerMask, bool hitTriggers, RayCastHit* resultsBuffer = nullptr, int32 resultsCapacity = 0, int32* resultsCount = nullptr)
    {
        maxDistance = GetCastDistance(maxDistance);
        QueryContext context;
        context.HitTriggers = hitTriggers;
        context.MaxDistance = maxDistance;
        context.Center = center;
        context.Direction = direction;
        context.Results = results;
        context.ResultsBuffer = resultsBuffer;
        context.ResultsCapacity = resultsCapacity;
        context.All = results != nullptr || resultsBuffer != nullptr;
        context.Any = hitInfo == nullptr && !context.All;

        const b3QueryFilter filter = MakeQueryFilter(layerMask);
        b3World_OverlapShape(scene->World, C2BPos(center), &proxy, filter, InitialOverlapCastCallback, &context);
        if ((context.All || !context.Hit.Collider) && (!resultsBuffer || context.ResultsCount < resultsCapacity))
        {
            // Box3D sweeps rounded shapes to radius - linear slop. Compensate that slop,
            // but retain its cast tolerance so a separated shape never becomes a zero-fraction hit.
            b3ShapeProxy castProxy = proxy;
            if (castProxy.radius > B3_LINEAR_SLOP)
                castProxy.radius += B3_LINEAR_SLOP * 0.75f;
            b3World_CastShape(scene->World, C2BPos(center), &castProxy, C2BVec(direction * maxDistance), filter, QueryCastCallback, &context);
        }
        if (resultsCount)
            *resultsCount = context.ResultsCount;
        if (resultsBuffer)
            return context.ResultsCount != 0;
        if (results)
            return results->HasItems();
        if (context.Any)
            return context.Hit.Collider != nullptr;
        if (context.Hit.Collider)
        {
            *hitInfo = context.Hit;
            return true;
        }
        return false;
    }

    bool OverlapShape(SceneBox3D* scene, const Vector3& center, const b3ShapeProxy& proxy, Array<PhysicsColliderActor*, HeapAllocation>* results, uint32 layerMask, bool hitTriggers, PhysicsOverlapResultBuffer* resultsBuffer = nullptr)
    {
        OverlapContext context;
        context.HitTriggers = hitTriggers;
        context.Results = results;
        context.ResultsBuffer = resultsBuffer;
        context.Any = results == nullptr && resultsBuffer == nullptr;
        b3World_OverlapShape(scene->World, C2BPos(center), &proxy, MakeQueryFilter(layerMask), OverlapCallback, &context);
        return context.Hit;
    }

    void FillMeshHeader(Box3DCookedHeader& header, CollisionDataType type, int32 vertexCount, int32 indexCount, const BoundingBox& bounds)
    {
        Platform::MemoryClear(&header, sizeof(header));
        header.Magic = BOX3D_COOKED_MAGIC;
        header.Version = BOX3D_COOKED_VERSION;
        header.Type = (uint32)type;
        header.VertexCount = vertexCount;
        header.IndexCount = indexCount;
        header.Bounds = bounds;
    }
}

void* PhysicalMaterial::GetPhysicsMaterial()
{
    if (_material == nullptr)
    {
        auto material = New<MaterialBox3D>();
        material->Owner = this;
        UpdateMaterial(material);
        _material = material;
    }
    return _material;
}

void PhysicalMaterial::UpdatePhysicsMaterial()
{
    if (_material)
        UpdateMaterial((MaterialBox3D*)_material);
}

#if COMPILE_WITH_PHYSICS_COOKING

bool CollisionCooking::CookConvexMesh(CookingInput& input, BytesContainer& output)
{
    if (input.VertexCount <= 0 || input.VertexData == nullptr)
        return true;

    Array<b3Vec3> points;
    points.Resize(input.VertexCount, false);
    for (int32 i = 0; i < input.VertexCount; i++)
        points[i] = C2BVec(input.VertexData[i]);

    b3HullData* hull = CreateBox3DConvexHull(points.Get(), points.Count(), input.ConvexVertexLimit);
    if (!hull)
        return true;

    const b3Vec3* hullPoints = b3GetHullPoints(hull);
    if (!hullPoints || hull->vertexCount < BOX3D_CONVEX_VERTEX_MIN)
    {
        b3DestroyHull(hull);
        return true;
    }

    Array<Float3> cookedVertices;
    cookedVertices.Resize(hull->vertexCount, false);
    for (int32 i = 0; i < hull->vertexCount; i++)
        cookedVertices[i] = Float3(hullPoints[i].x, hullPoints[i].y, hullPoints[i].z);
    b3DestroyHull(hull);

    BoundingBox bounds;
    BoundingBox::FromPoints(cookedVertices.Get(), cookedVertices.Count(), bounds);

    MemoryWriteStream stream(sizeof(Box3DCookedHeader) + cookedVertices.Count() * sizeof(Float3));
    Box3DCookedHeader header;
    FillMeshHeader(header, CollisionDataType::ConvexMesh, cookedVertices.Count(), 0, bounds);
    stream.Write(header);
    stream.WriteBytes(cookedVertices.Get(), cookedVertices.Count() * sizeof(Float3));
    output.Copy(stream.GetHandle(), stream.GetPosition());
    return false;
}

bool CollisionCooking::CookTriangleMesh(CookingInput& input, BytesContainer& output)
{
    if (input.VertexCount <= 0 || input.VertexData == nullptr || input.IndexCount <= 0 || input.IndexData == nullptr)
        return true;

    BoundingBox bounds;
    BoundingBox::FromPoints(input.VertexData, input.VertexCount, bounds);

    MemoryWriteStream stream(sizeof(Box3DCookedHeader) + input.VertexCount * sizeof(Float3) + input.IndexCount * sizeof(int32));
    Box3DCookedHeader header;
    FillMeshHeader(header, CollisionDataType::TriangleMesh, input.VertexCount, input.IndexCount, bounds);
    stream.Write(header);
    stream.WriteBytes(input.VertexData, input.VertexCount * sizeof(Float3));
    if (input.Is16bitIndexData)
    {
        const uint16* indices = (const uint16*)input.IndexData;
        for (int32 i = 0; i < input.IndexCount; i++)
        {
            const int32 index = indices[i];
            stream.Write(index);
        }
    }
    else
    {
        stream.WriteBytes(input.IndexData, input.IndexCount * sizeof(int32));
    }
    output.Copy(stream.GetHandle(), stream.GetPosition());
    return false;
}

bool CollisionCooking::CookHeightField(int32 cols, int32 rows, const PhysicsBackend::HeightFieldSample* data, WriteStream& stream)
{
    if (cols <= 1 || rows <= 1 || data == nullptr)
        return true;
    Box3DHeightFieldHeader header;
    header.Magic = BOX3D_COOKED_MAGIC;
    header.Version = BOX3D_COOKED_VERSION;
    header.Type = (uint32)CollisionShape::Types::HeightField;
    header.Columns = cols;
    header.Rows = rows;
    stream.Write(header);
    stream.WriteBytes(data, cols * rows * sizeof(PhysicsBackend::HeightFieldSample));
    return false;
}

#endif

bool PhysicsBackend::Init()
{
    auto version = b3GetVersion();
    LOG(Info, "Setup Box3D {0}.{1}.{2}", version.major, version.minor, version.revision);
    b3SetLengthUnitsPerMeter(BOX3D_LENGTH_UNITS_PER_METER);
    DefaultMaterial.Owner = nullptr;
    DefaultMaterial.Surface = b3DefaultSurfaceMaterial();
    DefaultMaterial.Surface.friction = 0.7f;
    DefaultMaterial.Surface.restitution = 0.3f;
    return false;
}

void PhysicsBackend::Shutdown()
{
}

void PhysicsBackend::ApplySettings(const PhysicsSettings& settings)
{
    FrictionCombineMode = settings.FrictionCombineMode;
    RestitutionCombineMode = settings.RestitutionCombineMode;
}

void* PhysicsBackend::CreateScene(const PhysicsSettings& settings)
{
    auto scene = New<SceneBox3D>();
    scene->Settings = settings;
    scene->EnableCCD = !settings.DisableCCD;

    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = C2BVec(settings.DefaultGravity);
    def.restitutionThreshold = settings.BounceThresholdVelocity;
    def.enableContinuous = scene->EnableCCD;
    def.userData = scene;
    scene->World = b3CreateWorld(&def);
    return scene;
}

void PhysicsBackend::DestroyScene(void* scene)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    if (sceneBox3D)
    {
        if (B3_IS_NON_NULL(sceneBox3D->World) && b3World_IsValid(sceneBox3D->World))
            b3DestroyWorld(sceneBox3D->World);
        Delete(sceneBox3D);
    }
}

void PhysicsBackend::StartSimulateScene(void* scene, float dt)
{
    PROFILE_CPU();
    auto sceneBox3D = (SceneBox3D*)scene;

    sceneBox3D->LastDeltaTime = dt;

    for (auto& action : sceneBox3D->Actions)
    {
        if (action.Type == ActionType::Sleep && action.Actor && b3Body_IsValid(action.Actor->Body))
            b3Body_SetAwake(action.Actor->Body, false);
    }
    sceneBox3D->Actions.Clear();

    for (auto actor : sceneBox3D->RemoveActors)
    {
        if (actor && b3Body_IsValid(actor->Body))
            b3Body_Disable(actor->Body);
    }
    sceneBox3D->RemoveActors.Clear();

    int32 subSteps = BOX3D_DEFAULT_SUBSTEPS;
    if (sceneBox3D->Settings.EnableSubstepping && sceneBox3D->Settings.SubstepDeltaTime > ZeroTolerance)
        subSteps = Math::Clamp((int32)Math::Ceil(dt / sceneBox3D->Settings.SubstepDeltaTime), 1, Math::Max(sceneBox3D->Settings.MaxSubsteps, 1));
    if (dt > ZeroTolerance)
        b3World_Step(sceneBox3D->World, dt, subSteps);
}

void PhysicsBackend::EndSimulateScene(void* scene)
{
    PROFILE_CPU();
    auto sceneBox3D = (SceneBox3D*)scene;

    b3BodyEvents bodyEvents = b3World_GetBodyEvents(sceneBox3D->World);
    for (int32 i = 0; i < bodyEvents.moveCount; i++)
    {
        auto actor = (IPhysicsActor*)bodyEvents.moveEvents[i].userData;
        if (actor)
            actor->OnActiveTransformChanged();
    }

    b3SensorEvents sensorEvents = b3World_GetSensorEvents(sceneBox3D->World);
    for (int32 i = 0; i < sensorEvents.beginCount; i++)
        SendTriggerEvent(sensorEvents.beginEvents[i].sensorShapeId, sensorEvents.beginEvents[i].visitorShapeId, true);
    for (int32 i = 0; i < sensorEvents.endCount; i++)
        SendTriggerEvent(sensorEvents.endEvents[i].sensorShapeId, sensorEvents.endEvents[i].visitorShapeId, false);

    b3ContactEvents contactEvents = b3World_GetContactEvents(sceneBox3D->World);
    for (int32 i = 0; i < contactEvents.beginCount; i++)
        SendCollisionEvent(contactEvents.beginEvents[i].shapeIdA, contactEvents.beginEvents[i].shapeIdB, contactEvents.beginEvents[i].contactId, true);
    for (int32 i = 0; i < contactEvents.endCount; i++)
        SendCollisionEvent(contactEvents.endEvents[i].shapeIdA, contactEvents.endEvents[i].shapeIdB, contactEvents.endEvents[i].contactId, false);

    sceneBox3D->LastDeltaTime = 0.0f;
}

Vector3 PhysicsBackend::GetSceneGravity(void* scene)
{
    return B2C(b3World_GetGravity(((SceneBox3D*)scene)->World));
}

void PhysicsBackend::SetSceneGravity(void* scene, const Vector3& value)
{
    b3World_SetGravity(((SceneBox3D*)scene)->World, C2BVec(value));
}

bool PhysicsBackend::GetSceneEnableCCD(void* scene)
{
    return ((SceneBox3D*)scene)->EnableCCD;
}

void PhysicsBackend::SetSceneEnableCCD(void* scene, bool value)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    sceneBox3D->EnableCCD = value;
    b3World_EnableContinuous(sceneBox3D->World, value);
}

float PhysicsBackend::GetSceneBounceThresholdVelocity(void* scene)
{
    return b3World_GetRestitutionThreshold(((SceneBox3D*)scene)->World);
}

void PhysicsBackend::SetSceneBounceThresholdVelocity(void* scene, float value)
{
    b3World_SetRestitutionThreshold(((SceneBox3D*)scene)->World, value);
}

void PhysicsBackend::SetSceneOrigin(void* scene, const Vector3& oldOrigin, const Vector3& newOrigin)
{
}

void PhysicsBackend::AddSceneActor(void* scene, void* actor)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    auto actorBox3D = (ActorBox3D*)actor;
    sceneBox3D->RemoveActors.RemoveAll(actorBox3D);
    actorBox3D->AddedToScene = true;
    if (!EnumHasAnyFlags(actorBox3D->Flags, ActorFlags::NoSimulation) && b3Body_IsValid(actorBox3D->Body) && !b3Body_IsEnabled(actorBox3D->Body))
        b3Body_Enable(actorBox3D->Body);
}

void PhysicsBackend::RemoveSceneActor(void* scene, void* actor, bool immediately)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    auto actorBox3D = (ActorBox3D*)actor;
    actorBox3D->AddedToScene = false;
    if (immediately)
    {
        if (b3Body_IsValid(actorBox3D->Body))
            b3Body_Disable(actorBox3D->Body);
    }
    else
    {
        if (!sceneBox3D->RemoveActors.Contains(actorBox3D))
            sceneBox3D->RemoveActors.Add(actorBox3D);
    }
}

void PhysicsBackend::AddSceneActorAction(void* scene, void* actor, ActionType action)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    auto& item = sceneBox3D->Actions.AddOne();
    item.Actor = (ActorBox3D*)actor;
    item.Type = action;
}

#if COMPILE_WITH_PROFILER
void PhysicsBackend::GetSceneStatistics(void* scene, PhysicsStatistics& result)
{
    auto counters = b3World_GetCounters(((SceneBox3D*)scene)->World);
    result.ActiveDynamicBodies = b3World_GetAwakeBodyCount(((SceneBox3D*)scene)->World);
    result.ActiveKinematicBodies = 0;
    result.ActiveJoints = counters.jointCount;
    result.StaticBodies = 0;
    result.DynamicBodies = counters.bodyCount;
    result.KinematicBodies = 0;
    result.NewPairs = 0;
    result.LostPairs = 0;
    result.NewTouches = 0;
    result.LostTouches = 0;
}
#endif

bool PhysicsBackend::RayCast(void* scene, const Vector3& origin, const Vector3& direction, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    maxDistance = GetCastDistance(maxDistance);
    QueryContext context;
    context.Any = true;
    context.HitTriggers = hitTriggers;
    context.MaxDistance = maxDistance;
    b3World_CastRay(((SceneBox3D*)scene)->World, C2BPos(origin), C2BVec(direction * maxDistance), MakeQueryFilter(layerMask), QueryCastCallback, &context);
    return context.Hit.Collider != nullptr;
}

bool PhysicsBackend::RayCast(void* scene, const Vector3& origin, const Vector3& direction, RayCastHit& hitInfo, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    maxDistance = GetCastDistance(maxDistance);
    QueryContext context;
    context.HitTriggers = hitTriggers;
    context.MaxDistance = maxDistance;
    b3World_CastRay(((SceneBox3D*)scene)->World, C2BPos(origin), C2BVec(direction * maxDistance), MakeQueryFilter(layerMask), QueryCastCallback, &context);
    if (!context.Hit.Collider)
        return false;
    hitInfo = context.Hit;
    return true;
}

bool PhysicsBackend::RayCastAll(void* scene, const Vector3& origin, const Vector3& direction, Array<RayCastHit, HeapAllocation>& results, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    maxDistance = GetCastDistance(maxDistance);
    QueryContext context;
    context.All = true;
    context.HitTriggers = hitTriggers;
    context.MaxDistance = maxDistance;
    context.Results = &results;
    b3World_CastRay(((SceneBox3D*)scene)->World, C2BPos(origin), C2BVec(direction * maxDistance), MakeQueryFilter(layerMask), QueryCastCallback, &context);
    return results.HasItems();
}

int32 PhysicsBackend::RayCastNonAlloc(void* scene, const Vector3& origin, const Vector3& direction, Span<RayCastHit> results, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (results.Length() == 0)
        return 0;
    maxDistance = GetCastDistance(maxDistance);
    QueryContext context;
    context.HitTriggers = hitTriggers;
    context.MaxDistance = maxDistance;
    context.All = true;
    context.ResultsBuffer = results.Get();
    context.ResultsCapacity = results.Length();
    b3World_CastRay(((SceneBox3D*)scene)->World, C2BPos(origin), C2BVec(direction * maxDistance), MakeQueryFilter(layerMask), QueryCastCallback, &context);
    return context.ResultsCount;
}

bool PhysicsBackend::BoxCast(void* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::BoxCast(void* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, RayCastHit& hitInfo, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, &hitInfo, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::BoxCastAll(void* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, Array<RayCastHit, HeapAllocation>& results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, &results, maxDistance, layerMask, hitTriggers);
}

int32 PhysicsBackend::BoxCastNonAlloc(void* scene, const Vector3& center, const Vector3& halfExtents, const Vector3& direction, Span<RayCastHit> results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (results.Length() == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    int32 count = 0;
    CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers, results.Get(), results.Length(), &count);
    return count;
}

bool PhysicsBackend::SphereCast(void* scene, const Vector3& center, float radius, const Vector3& direction, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::SphereCast(void* scene, const Vector3& center, float radius, const Vector3& direction, RayCastHit& hitInfo, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, &hitInfo, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::SphereCastAll(void* scene, const Vector3& center, float radius, const Vector3& direction, Array<RayCastHit, HeapAllocation>& results, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, &results, maxDistance, layerMask, hitTriggers);
}

int32 PhysicsBackend::SphereCastNonAlloc(void* scene, const Vector3& center, float radius, const Vector3& direction, Span<RayCastHit> results, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (results.Length() == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    int32 count = 0;
    CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers, results.Get(), results.Length(), &count);
    return count;
}

bool PhysicsBackend::CapsuleCast(void* scene, const Vector3& center, float radius, float height, const Vector3& direction, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::CapsuleCast(void* scene, const Vector3& center, float radius, float height, const Vector3& direction, RayCastHit& hitInfo, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, &hitInfo, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::CapsuleCastAll(void* scene, const Vector3& center, float radius, float height, const Vector3& direction, Array<RayCastHit, HeapAllocation>& results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, &results, maxDistance, layerMask, hitTriggers);
}

int32 PhysicsBackend::CapsuleCastNonAlloc(void* scene, const Vector3& center, float radius, float height, const Vector3& direction, Span<RayCastHit> results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (results.Length() == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    int32 count = 0;
    CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers, results.Get(), results.Length(), &count);
    return count;
}

bool PhysicsBackend::ConvexCast(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, const Vector3& direction, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    RayCastHit hit;
    return ConvexCast(scene, center, convexMesh, scale, direction, hit, rotation, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::ConvexCast(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, const Vector3& direction, RayCastHit& hitInfo, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (!convexMesh || convexMesh->GetOptions().Type != CollisionDataType::ConvexMesh)
        return false;
    auto mesh = (MeshBox3D*)convexMesh->GetConvex();
    if (!mesh || mesh->Vertices.IsEmpty())
        return false;

    Array<b3Vec3, InlinedAllocation<64>> points;
    const int32 count = Math::Min(mesh->Vertices.Count(), B3_MAX_SHAPE_CAST_POINTS);
    points.Resize(count, false);
    for (int32 i = 0; i < count; i++)
        points[i] = Rotate(rotation, Vector3(mesh->Vertices[i] * Float3((float)scale.X, (float)scale.Y, (float)scale.Z)));
    b3ShapeProxy proxy;
    proxy.points = points.Get();
    proxy.count = count;
    proxy.radius = 0.0f;
    return CastShape((SceneBox3D*)scene, center, proxy, direction, &hitInfo, nullptr, maxDistance, layerMask, hitTriggers);
}

bool PhysicsBackend::ConvexCastAll(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, const Vector3& direction, Array<RayCastHit, HeapAllocation>& results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (!convexMesh || convexMesh->GetOptions().Type != CollisionDataType::ConvexMesh)
        return false;
    auto mesh = (MeshBox3D*)convexMesh->GetConvex();
    if (!mesh || mesh->Vertices.IsEmpty())
        return false;

    Array<b3Vec3, InlinedAllocation<64>> points;
    const int32 count = Math::Min(mesh->Vertices.Count(), B3_MAX_SHAPE_CAST_POINTS);
    points.Resize(count, false);
    for (int32 i = 0; i < count; i++)
        points[i] = Rotate(rotation, Vector3(mesh->Vertices[i] * Float3((float)scale.X, (float)scale.Y, (float)scale.Z)));
    b3ShapeProxy proxy;
    proxy.points = points.Get();
    proxy.count = count;
    proxy.radius = 0.0f;
    return CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, &results, maxDistance, layerMask, hitTriggers);
}

int32 PhysicsBackend::ConvexCastNonAlloc(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, const Vector3& direction, Span<RayCastHit> results, const Quaternion& rotation, float maxDistance, uint32 layerMask, bool hitTriggers)
{
    if (results.Length() == 0 || !convexMesh || convexMesh->GetOptions().Type != CollisionDataType::ConvexMesh)
        return 0;
    auto mesh = (MeshBox3D*)convexMesh->GetConvex();
    if (!mesh || mesh->Vertices.IsEmpty())
        return 0;

    Array<b3Vec3, InlinedAllocation<64>> points;
    const int32 pointCount = Math::Min(mesh->Vertices.Count(), B3_MAX_SHAPE_CAST_POINTS);
    points.Resize(pointCount, false);
    for (int32 i = 0; i < pointCount; i++)
        points[i] = Rotate(rotation, Vector3(mesh->Vertices[i] * Float3((float)scale.X, (float)scale.Y, (float)scale.Z)));
    b3ShapeProxy proxy;
    proxy.points = points.Get();
    proxy.count = pointCount;
    proxy.radius = 0.0f;
    int32 count = 0;
    CastShape((SceneBox3D*)scene, center, proxy, direction, nullptr, nullptr, maxDistance, layerMask, hitTriggers, results.Get(), results.Length(), &count);
    return count;
}

bool PhysicsBackend::CheckBox(void* scene, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers);
}

bool PhysicsBackend::CheckSphere(void* scene, const Vector3& center, float radius, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers);
}

bool PhysicsBackend::CheckCapsule(void* scene, const Vector3& center, float radius, float height, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers);
}

bool PhysicsBackend::CheckConvex(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    Array<PhysicsColliderActor*, HeapAllocation> results;
    return OverlapConvex(scene, center, convexMesh, scale, results, rotation, layerMask, hitTriggers);
}

bool PhysicsBackend::OverlapBox(void* scene, const Vector3& center, const Vector3& halfExtents, Array<PhysicsColliderActor*, HeapAllocation>& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, &results, layerMask, hitTriggers);
}

bool PhysicsBackend::OverlapSphere(void* scene, const Vector3& center, float radius, Array<PhysicsColliderActor*, HeapAllocation>& results, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, &results, layerMask, hitTriggers);
}

bool PhysicsBackend::OverlapCapsule(void* scene, const Vector3& center, float radius, float height, Array<PhysicsColliderActor*, HeapAllocation>& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    return OverlapShape((SceneBox3D*)scene, center, proxy, &results, layerMask, hitTriggers);
}

bool PhysicsBackend::OverlapConvex(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, Array<PhysicsColliderActor*, HeapAllocation>& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    if (!convexMesh || convexMesh->GetOptions().Type != CollisionDataType::ConvexMesh)
        return false;
    auto mesh = (MeshBox3D*)convexMesh->GetConvex();
    if (!mesh || mesh->Vertices.IsEmpty())
        return false;

    Array<b3Vec3, InlinedAllocation<64>> points;
    const int32 count = Math::Min(mesh->Vertices.Count(), B3_MAX_SHAPE_CAST_POINTS);
    points.Resize(count, false);
    for (int32 i = 0; i < count; i++)
        points[i] = Rotate(rotation, Vector3(mesh->Vertices[i] * Float3((float)scale.X, (float)scale.Y, (float)scale.Z)));
    b3ShapeProxy proxy;
    proxy.points = points.Get();
    proxy.count = count;
    proxy.radius = 0.0f;
    return OverlapShape((SceneBox3D*)scene, center, proxy, &results, layerMask, hitTriggers);
}

int32 PhysicsBackend::OverlapBoxNonAlloc(void* scene, const Vector3& center, const Vector3& halfExtents, PhysicsOverlapResultBuffer& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    if (results.Capacity == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeBoxProxy(halfExtents, rotation, points, proxy);
    OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers, &results);
    return results.Count;
}

int32 PhysicsBackend::OverlapSphereNonAlloc(void* scene, const Vector3& center, float radius, PhysicsOverlapResultBuffer& results, uint32 layerMask, bool hitTriggers)
{
    if (results.Capacity == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeSphereProxy(radius, points, proxy);
    OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers, &results);
    return results.Count;
}

int32 PhysicsBackend::OverlapCapsuleNonAlloc(void* scene, const Vector3& center, float radius, float height, PhysicsOverlapResultBuffer& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    if (results.Capacity == 0)
        return 0;
    Array<b3Vec3, InlinedAllocation<8>> points;
    b3ShapeProxy proxy;
    MakeCapsuleProxy(radius, height, rotation, points, proxy);
    OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers, &results);
    return results.Count;
}

int32 PhysicsBackend::OverlapConvexNonAlloc(void* scene, const Vector3& center, const CollisionData* convexMesh, const Vector3& scale, PhysicsOverlapResultBuffer& results, const Quaternion& rotation, uint32 layerMask, bool hitTriggers)
{
    if (results.Capacity == 0 || !convexMesh || convexMesh->GetOptions().Type != CollisionDataType::ConvexMesh)
        return 0;
    auto mesh = (MeshBox3D*)convexMesh->GetConvex();
    if (!mesh || mesh->Vertices.IsEmpty())
        return 0;

    Array<b3Vec3, InlinedAllocation<64>> points;
    const int32 pointCount = Math::Min(mesh->Vertices.Count(), B3_MAX_SHAPE_CAST_POINTS);
    points.Resize(pointCount, false);
    for (int32 i = 0; i < pointCount; i++)
        points[i] = Rotate(rotation, Vector3(mesh->Vertices[i] * Float3((float)scale.X, (float)scale.Y, (float)scale.Z)));
    b3ShapeProxy proxy;
    proxy.points = points.Get();
    proxy.count = pointCount;
    proxy.radius = 0.0f;
    OverlapShape((SceneBox3D*)scene, center, proxy, nullptr, layerMask, hitTriggers, &results);
    return results.Count;
}

PhysicsBackend::ActorFlags PhysicsBackend::GetActorFlags(void* actor)
{
    return ((ActorBox3D*)actor)->Flags;
}

void PhysicsBackend::SetActorFlags(void* actor, ActorFlags value)
{
    auto actorBox3D = (ActorBox3D*)actor;
    actorBox3D->Flags = value;
    ApplyActorFlags(actorBox3D);
}

void PhysicsBackend::GetActorBounds(void* actor, BoundingBox& bounds)
{
    auto actorBox3D = (ActorBox3D*)actor;
    if (b3Body_IsValid(actorBox3D->Body))
    {
        const b3AABB aabb = b3Body_ComputeAABB(actorBox3D->Body);
        bounds = BoundingBox(B2C(aabb.lowerBound), B2C(aabb.upperBound));
    }
    else
    {
        bounds = BoundingBox::Empty;
    }
}

int32 PhysicsBackend::GetRigidActorShapesCount(void* actor)
{
    auto actorBox3D = (ActorBox3D*)actor;
    return b3Body_IsValid(actorBox3D->Body) ? b3Body_GetShapeCount(actorBox3D->Body) : 0;
}

void* PhysicsBackend::CreateRigidDynamicActor(IPhysicsActor* actor, const Vector3& position, const Quaternion& orientation, void* scene)
{
    return CreateActor(actor, position, orientation, (SceneBox3D*)scene, b3_dynamicBody);
}

void* PhysicsBackend::CreateRigidStaticActor(IPhysicsActor* actor, const Vector3& position, const Quaternion& orientation, void* scene)
{
    return CreateActor(actor, position, orientation, (SceneBox3D*)scene, b3_staticBody);
}

PhysicsBackend::RigidDynamicFlags PhysicsBackend::GetRigidDynamicActorFlags(void* actor)
{
    return ((ActorBox3D*)actor)->DynamicFlags;
}

void PhysicsBackend::SetRigidDynamicActorFlags(void* actor, RigidDynamicFlags value)
{
    auto actorBox3D = (ActorBox3D*)actor;
    actorBox3D->DynamicFlags = value;
    ApplyDynamicFlags(actorBox3D);
}

void PhysicsBackend::GetRigidActorPose(void* actor, Vector3& position, Quaternion& orientation)
{
    auto actorBox3D = (ActorBox3D*)actor;
    if (b3Body_IsValid(actorBox3D->Body))
    {
        const b3WorldTransform transform = b3Body_GetTransform(actorBox3D->Body);
        position = B2C(transform.p);
        orientation = B2C(transform.q);
    }
    else
    {
        position = Vector3::Zero;
        orientation = Quaternion::Identity;
    }
}

void PhysicsBackend::SetRigidActorPose(void* actor, const Vector3& position, const Quaternion& orientation, bool kinematic, bool wakeUp)
{
    auto actorBox3D = (ActorBox3D*)actor;
    if (!b3Body_IsValid(actorBox3D->Body))
        return;
    if (kinematic)
        b3Body_SetTargetTransform(actorBox3D->Body, C2BWorldTransform(position, orientation), Math::Max(actorBox3D->Scene ? actorBox3D->Scene->LastDeltaTime : 0.0f, 1.0f / 60.0f), wakeUp);
    else
        b3Body_SetTransform(actorBox3D->Body, C2BPos(position), C2B(orientation));
    if (wakeUp)
        b3Body_SetAwake(actorBox3D->Body, true);
}

void PhysicsBackend::SetRigidDynamicActorLinearDamping(void* actor, float value)
{
    b3Body_SetLinearDamping(((ActorBox3D*)actor)->Body, value);
}

void PhysicsBackend::SetRigidDynamicActorAngularDamping(void* actor, float value)
{
    b3Body_SetAngularDamping(((ActorBox3D*)actor)->Body, value);
}

void PhysicsBackend::SetRigidDynamicActorMaxAngularVelocity(void* actor, float value)
{
}

void PhysicsBackend::SetRigidDynamicActorConstraints(void* actor, RigidbodyConstraints value)
{
    auto actorBox3D = (ActorBox3D*)actor;
    actorBox3D->Constraints = value;
    ApplyConstraints(actorBox3D);
}

Vector3 PhysicsBackend::GetRigidDynamicActorLinearVelocity(void* actor)
{
    return B2C(b3Body_GetLinearVelocity(((ActorBox3D*)actor)->Body));
}

void PhysicsBackend::SetRigidDynamicActorLinearVelocity(void* actor, const Vector3& value, bool wakeUp)
{
    auto actorBox3D = (ActorBox3D*)actor;
    b3Body_SetLinearVelocity(actorBox3D->Body, C2BVec(value));
    if (wakeUp)
        b3Body_SetAwake(actorBox3D->Body, true);
}

Vector3 PhysicsBackend::GetRigidDynamicActorAngularVelocity(void* actor)
{
    return B2C(b3Body_GetAngularVelocity(((ActorBox3D*)actor)->Body));
}

void PhysicsBackend::SetRigidDynamicActorAngularVelocity(void* actor, const Vector3& value, bool wakeUp)
{
    auto actorBox3D = (ActorBox3D*)actor;
    b3Body_SetAngularVelocity(actorBox3D->Body, C2BVec(value));
    if (wakeUp)
        b3Body_SetAwake(actorBox3D->Body, true);
}

Vector3 PhysicsBackend::GetRigidDynamicActorPointVelocity(void* actor, const Vector3& point)
{
    auto actorBox3D = (ActorBox3D*)actor;
    const Vector3 linear = B2C(b3Body_GetLinearVelocity(actorBox3D->Body));
    const Vector3 angular = B2C(b3Body_GetAngularVelocity(actorBox3D->Body));
    const Vector3 centerOfMass = B2C(b3Body_GetWorldCenterOfMass(actorBox3D->Body));
    return linear + Vector3::Cross(angular, point - centerOfMass);
}

Vector3 PhysicsBackend::GetRigidDynamicActorCenterOfMass(void* actor)
{
    return B2C(b3Body_GetLocalCenterOfMass(((ActorBox3D*)actor)->Body));
}

void PhysicsBackend::AddRigidDynamicActorCenterOfMassOffset(void* actor, const Float3& value)
{
    auto actorBox3D = (ActorBox3D*)actor;
    b3MassData massData = b3Body_GetMassData(actorBox3D->Body);
    massData.center = b3Add(massData.center, C2BVec(value));
    b3Body_SetMassData(actorBox3D->Body, massData);
}

bool PhysicsBackend::GetRigidDynamicActorIsSleeping(void* actor)
{
    return !b3Body_IsAwake(((ActorBox3D*)actor)->Body);
}

void PhysicsBackend::RigidDynamicActorSleep(void* actor)
{
    b3Body_SetAwake(((ActorBox3D*)actor)->Body, false);
}

void PhysicsBackend::RigidDynamicActorWakeUp(void* actor)
{
    b3Body_SetAwake(((ActorBox3D*)actor)->Body, true);
}

float PhysicsBackend::GetRigidDynamicActorSleepThreshold(void* actor)
{
    return b3Body_GetSleepThreshold(((ActorBox3D*)actor)->Body);
}

void PhysicsBackend::SetRigidDynamicActorSleepThreshold(void* actor, float value)
{
    b3Body_SetSleepThreshold(((ActorBox3D*)actor)->Body, value);
}

float PhysicsBackend::GetRigidDynamicActorMaxDepenetrationVelocity(void* actor)
{
    return 0.0f;
}

void PhysicsBackend::SetRigidDynamicActorMaxDepenetrationVelocity(void* actor, float value)
{
}

void PhysicsBackend::SetRigidDynamicActorSolverIterationCounts(void* actor, int32 minPositionIters, int32 minVelocityIters)
{
}

void PhysicsBackend::UpdateRigidDynamicActorMass(void* actor, float& mass, float massScale, bool autoCalculate)
{
    auto actorBox3D = (ActorBox3D*)actor;
    if (autoCalculate)
    {
        b3Body_ApplyMassFromShapes(actorBox3D->Body);
        mass = b3Body_GetMass(actorBox3D->Body);
        if (!Math::IsOne(massScale))
        {
            b3MassData massData = b3Body_GetMassData(actorBox3D->Body);
            massData.mass = Math::Max(mass * massScale, 0.001f);
            b3Body_SetMassData(actorBox3D->Body, massData);
            mass = massData.mass;
        }
    }
    else
    {
        b3MassData massData;
        const b3MassData currentMassData = b3Body_GetMassData(actorBox3D->Body);
        const float targetMass = Math::Max(mass * massScale, 0.001f);
        if (ComputeActorMassData(actorBox3D, massData))
        {
            massData.inertia = b3MulSM(targetMass / massData.mass, massData.inertia);
            massData.center = currentMassData.center;
        }
        else
        {
            massData = currentMassData;
            if (massData.mass > ZeroTolerance)
                massData.inertia = b3MulSM(targetMass / massData.mass, massData.inertia);
        }
        massData.mass = targetMass;
        b3Body_SetMassData(actorBox3D->Body, massData);
    }
}

void PhysicsBackend::AddRigidDynamicActorForce(void* actor, const Vector3& force, ForceMode mode)
{
    auto actorBox3D = (ActorBox3D*)actor;
    const float mass = Math::Max(b3Body_GetMass(actorBox3D->Body), 0.001f);
    switch (mode)
    {
    case ForceMode::Force:
        b3Body_ApplyForceToCenter(actorBox3D->Body, C2BVec(force), true);
        break;
    case ForceMode::Impulse:
        b3Body_ApplyLinearImpulseToCenter(actorBox3D->Body, C2BVec(force), true);
        break;
    case ForceMode::VelocityChange:
        b3Body_ApplyLinearImpulseToCenter(actorBox3D->Body, C2BVec(force * mass), true);
        break;
    case ForceMode::Acceleration:
        b3Body_ApplyForceToCenter(actorBox3D->Body, C2BVec(force * mass), true);
        break;
    }
}

void PhysicsBackend::AddRigidDynamicActorForceAtPosition(void* actor, const Vector3& force, const Vector3& position, ForceMode mode)
{
    auto actorBox3D = (ActorBox3D*)actor;
    const float mass = Math::Max(b3Body_GetMass(actorBox3D->Body), 0.001f);
    switch (mode)
    {
    case ForceMode::Force:
        b3Body_ApplyForce(actorBox3D->Body, C2BVec(force), C2BPos(position), true);
        break;
    case ForceMode::Impulse:
        b3Body_ApplyLinearImpulse(actorBox3D->Body, C2BVec(force), C2BPos(position), true);
        break;
    case ForceMode::VelocityChange:
        b3Body_ApplyLinearImpulse(actorBox3D->Body, C2BVec(force * mass), C2BPos(position), true);
        break;
    case ForceMode::Acceleration:
        b3Body_ApplyForce(actorBox3D->Body, C2BVec(force * mass), C2BPos(position), true);
        break;
    }
}

void PhysicsBackend::AddRigidDynamicActorTorque(void* actor, const Vector3& torque, ForceMode mode)
{
    auto actorBox3D = (ActorBox3D*)actor;
    const float mass = Math::Max(b3Body_GetMass(actorBox3D->Body), 0.001f);
    switch (mode)
    {
    case ForceMode::Force:
    case ForceMode::Acceleration:
        b3Body_ApplyTorque(actorBox3D->Body, C2BVec(mode == ForceMode::Acceleration ? torque * mass : torque), true);
        break;
    case ForceMode::Impulse:
    case ForceMode::VelocityChange:
        b3Body_ApplyAngularImpulse(actorBox3D->Body, C2BVec(mode == ForceMode::VelocityChange ? torque * mass : torque), true);
        break;
    }
}

void* PhysicsBackend::CreateShape(PhysicsColliderActor* collider, const CollisionShape& geometry, Span<JsonAsset*> materials, bool enabled, bool trigger)
{
    auto shape = New<ShapeBox3D>();
    shape->Collider = collider;
    shape->Geometry = geometry;
    shape->Enabled = enabled;
    shape->Trigger = trigger;
    shape->Materials.Resize(materials.Length());
    for (int32 i = 0; i < materials.Length(); i++)
        shape->Materials[i] = materials[i];
    UpdateShapeSurfaces(shape);
    return shape;
}

void PhysicsBackend::SetShapeState(void* shape, bool enabled, bool trigger)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    shapeBox3D->Enabled = enabled;
    shapeBox3D->Trigger = trigger;
    RecreateRuntimeShape(shapeBox3D);
}

void PhysicsBackend::SetShapeFilterMask(void* shape, uint32 mask0, uint32 mask1)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    shapeBox3D->Mask0 = mask0;
    shapeBox3D->Mask1 = mask1;
    if (IsShapeValid(shapeBox3D))
        b3Shape_SetFilter(shapeBox3D->Shape, MakeFilter(mask0, mask1), true);
}

void* PhysicsBackend::GetShapeActor(void* shape)
{
    return ((ShapeBox3D*)shape)->Actor;
}

void PhysicsBackend::GetShapePose(void* shape, Vector3& position, Quaternion& orientation)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    if (shapeBox3D->Actor && b3Body_IsValid(shapeBox3D->Actor->Body))
    {
        const b3WorldTransform world = b3Body_GetTransform(shapeBox3D->Actor->Body);
        const b3WorldTransform pose = b3MulWorldTransforms(world, C2BTransform(shapeBox3D->LocalPosition, shapeBox3D->LocalRotation));
        position = B2C(pose.p);
        orientation = B2C(pose.q);
    }
    else
    {
        position = shapeBox3D->LocalPosition;
        orientation = shapeBox3D->LocalRotation;
    }
}

CollisionShape::Types PhysicsBackend::GetShapeType(void* shape)
{
    return ((ShapeBox3D*)shape)->Geometry.Type;
}

void PhysicsBackend::GetShapeLocalPose(void* shape, Vector3& position, Quaternion& orientation)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    position = shapeBox3D->LocalPosition;
    orientation = shapeBox3D->LocalRotation;
}

void PhysicsBackend::SetShapeLocalPose(void* shape, const Vector3& position, const Quaternion& orientation)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    if (shapeBox3D->LocalPosition == position && shapeBox3D->LocalRotation == orientation)
        return;
    shapeBox3D->LocalPosition = position;
    shapeBox3D->LocalRotation = orientation;
    RecreateRuntimeShape(shapeBox3D);
}

void PhysicsBackend::SetShapeContactOffset(void* shape, float value)
{
    ((ShapeBox3D*)shape)->ContactOffset = value;
}

void PhysicsBackend::SetShapeMaterials(void* shape, Span<JsonAsset*> materials)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    shapeBox3D->Materials.Resize(materials.Length());
    for (int32 i = 0; i < materials.Length(); i++)
        shapeBox3D->Materials[i] = materials[i];
    RecreateRuntimeShape(shapeBox3D);
}

void PhysicsBackend::SetShapeGeometry(void* shape, const CollisionShape& geometry)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    // Unlink using the old geometry before replacing its mesh pointer.
    DestroyRuntimeShape(shapeBox3D);
    shapeBox3D->Geometry = geometry;
    RecreateRuntimeShape(shapeBox3D);
}

void PhysicsBackend::AttachShape(void* shape, void* actor)
{
    AttachShapeInternal((ShapeBox3D*)shape, (ActorBox3D*)actor);
}

void PhysicsBackend::DetachShape(void* shape, void* actor)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    DestroyRuntimeShape(shapeBox3D);
    if (shapeBox3D->Actor)
        shapeBox3D->Actor->Shapes.Remove(shapeBox3D);
    shapeBox3D->Actor = nullptr;
}

bool PhysicsBackend::ComputeShapesPenetration(void* shapeA, void* shapeB, const Vector3& positionA, const Quaternion& orientationA, const Vector3& positionB, const Quaternion& orientationB, Vector3& direction, float& distance)
{
    direction = Vector3::Forward;
    distance = 0.0f;

    auto shapeBox3DA = (ShapeBox3D*)shapeA;
    auto shapeBox3DB = (ShapeBox3D*)shapeB;
    if (shapeBox3DB && shapeBox3DB->Geometry.Type == CollisionShape::Types::HeightField)
        return ComputeHeightFieldPenetration(shapeBox3DA, shapeBox3DB, positionA, orientationA, positionB, orientationB, direction, distance);
    if (shapeBox3DA && shapeBox3DA->Geometry.Type == CollisionShape::Types::HeightField)
    {
        if (!ComputeHeightFieldPenetration(shapeBox3DB, shapeBox3DA, positionB, orientationB, positionA, orientationA, direction, distance))
            return false;
        direction = -direction;
        return true;
    }

    PenetrationShapeBox3D localShapeA;
    PenetrationShapeBox3D localShapeB;
    if (!BuildPenetrationShape(shapeBox3DA, localShapeA) || !BuildPenetrationShape(shapeBox3DB, localShapeB))
        return false;

    const b3Transform transformBtoA = b3InvMulWorldTransforms(C2BWorldTransform(positionA, orientationA), C2BWorldTransform(positionB, orientationB));
    const b3Transform transformAtoB = b3InvertTransform(transformBtoA);
    return ComputeConvexPenetration(localShapeA, localShapeB, transformBtoA, transformAtoB, orientationA, orientationB, positionA, positionB, direction, distance);
}

float PhysicsBackend::ComputeShapeSqrDistanceToPoint(void* shape, const Vector3& position, const Quaternion& orientation, const Vector3& point, Vector3* closestPoint)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    if (IsShapeValid(shapeBox3D))
    {
        const Vector3 closest = B2C(b3Shape_GetClosestPoint(shapeBox3D->Shape, C2BVec(point)));
        if (closestPoint)
            *closestPoint = closest;
        return (float)Vector3::DistanceSquared(closest, point);
    }
    if (closestPoint)
        *closestPoint = position;
    return (float)Vector3::DistanceSquared(position, point);
}

bool PhysicsBackend::RayCastShape(void* shape, const Vector3& position, const Quaternion& orientation, const Vector3& origin, const Vector3& direction, float& resultHitDistance, float maxDistance)
{
    RayCastHit hit;
    if (!RayCastShape(shape, position, orientation, origin, direction, hit, maxDistance))
        return false;
    resultHitDistance = hit.Distance;
    return true;
}

bool PhysicsBackend::RayCastShape(void* shape, const Vector3& position, const Quaternion& orientation, const Vector3& origin, const Vector3& direction, RayCastHit& hitInfo, float maxDistance)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    if (!IsShapeValid(shapeBox3D))
        return false;
    maxDistance = GetCastDistance(maxDistance);
    const b3WorldCastOutput hit = b3Shape_RayCast(shapeBox3D->Shape, C2BPos(origin), C2BVec(direction * maxDistance));
    if (!hit.hit)
        return false;
    FillRayHit(hitInfo, shapeBox3D->Shape, hit.point, hit.normal, hit.fraction, maxDistance, shapeBox3D->Surfaces.HasItems() ? shapeBox3D->Surfaces[0].userMaterialId : 0, hit.triangleIndex);
    return true;
}

void PhysicsBackend::SetJointFlags(void* joint, JointFlags value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->Flags = value;
    if (jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle))
        b3Joint_SetCollideConnected(jointBox3D->Handle, EnumHasAnyFlags(value, JointFlags::Collision));
}

void PhysicsBackend::SetJointActors(void* joint, void* actors0, void* actor1)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->Actor0 = (ActorBox3D*)actors0;
    jointBox3D->Actor1 = (ActorBox3D*)actor1;
    jointBox3D->Scene = jointBox3D->Actor0 ? jointBox3D->Actor0->Scene : (jointBox3D->Actor1 ? jointBox3D->Actor1->Scene : nullptr);
    RecreateJointHandle(jointBox3D);
}

void PhysicsBackend::SetJointActorPose(void* joint, const Vector3& position, const Quaternion& orientation, uint8 index)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    if (index == 0)
        jointBox3D->LocalFrame0 = C2BTransform(position, orientation);
    else
        jointBox3D->LocalFrame1 = C2BTransform(position, orientation);
    RecreateJointHandle(jointBox3D);
}

void PhysicsBackend::SetJointBreakForce(void* joint, float force, float torque)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->BreakForce = force;
    jointBox3D->BreakTorque = torque;
    if (B3_IS_NULL(jointBox3D->Handle) || !b3Joint_IsValid(jointBox3D->Handle))
        return;
    b3Joint_SetForceThreshold(jointBox3D->Handle, force);
    b3Joint_SetTorqueThreshold(jointBox3D->Handle, torque);
}

void PhysicsBackend::GetJointForce(void* joint, Vector3& linear, Vector3& angular)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle))
    {
        linear = B2C(b3Joint_GetConstraintForce(jointBox3D->Handle));
        angular = B2C(b3Joint_GetConstraintTorque(jointBox3D->Handle));
    }
    else
    {
        linear = Vector3::Zero;
        angular = Vector3::Zero;
    }
}

namespace
{
    bool IsActorValid(const ActorBox3D* actor)
    {
        return actor && B3_IS_NON_NULL(actor->Body) && b3Body_IsValid(actor->Body);
    }

    bool IsJointValid(const JointBox3D* joint)
    {
        return joint && B3_IS_NON_NULL(joint->Handle) && b3Joint_IsValid(joint->Handle);
    }

    bool IsJointValid(const JointBox3D* joint, b3JointType type)
    {
        return IsJointValid(joint) && b3Joint_GetType(joint->Handle) == type;
    }

    void DestroyJointHandle(JointBox3D* joint)
    {
        if (IsJointValid(joint))
            b3DestroyJoint(joint->Handle, true);
        joint->Handle = b3_nullJointId;
        joint->D6Kind = D6JointKindBox3D::None;
    }

    b3Transform AdjustFrameAxis(b3Transform frame, const b3Vec3 from, const b3Vec3 to)
    {
        frame.q = b3NormalizeQuat(b3MulQuat(frame.q, b3ComputeQuatBetweenUnitVectors(from, to)));
        return frame;
    }

    b3Transform GetPrismaticFrame(const b3Transform& frame, D6JointAxis axis)
    {
        switch (axis)
        {
        case D6JointAxis::Y:
            return AdjustFrameAxis(frame, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        case D6JointAxis::Z:
            return AdjustFrameAxis(frame, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
        default:
            return frame;
        }
    }

    b3Transform GetRevoluteFrame(const b3Transform& frame, D6JointAxis axis)
    {
        switch (axis)
        {
        case D6JointAxis::Twist:
            return AdjustFrameAxis(frame, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
        case D6JointAxis::SwingY:
            return AdjustFrameAxis(frame, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
        default:
            return frame;
        }
    }

    void SetupJointBase(b3JointDef& base, const JointBox3D* joint, const b3Transform& localFrame0, const b3Transform& localFrame1)
    {
        base.bodyIdA = joint->Actor0 ? joint->Actor0->Body : b3_nullBodyId;
        base.bodyIdB = joint->Actor1 ? joint->Actor1->Body : b3_nullBodyId;
        base.localFrameA = localFrame0;
        base.localFrameB = localFrame1;
        base.userData = joint->Owner;
        base.forceThreshold = joint->BreakForce;
        base.torqueThreshold = joint->BreakTorque;
        base.collideConnected = EnumHasAnyFlags(joint->Flags, PhysicsBackend::JointFlags::Collision);
    }

    float ClampAngularLimit(float value)
    {
        return Math::Clamp(value * DegreesToRadians, -0.99f * PI, 0.99f * PI);
    }

    float ClampConeLimit(float value)
    {
        return Math::Clamp(value * DegreesToRadians, ZeroTolerance, PI - ZeroTolerance);
    }

    float GetAxisValue(const Vector3& value, D6JointAxis axis)
    {
        switch (axis)
        {
        case D6JointAxis::Y:
        case D6JointAxis::SwingY:
            return (float)value.Y;
        case D6JointAxis::Z:
        case D6JointAxis::SwingZ:
            return (float)value.Z;
        default:
            return (float)value.X;
        }
    }

    void ApplyDistanceJointState(JointBox3D* joint)
    {
        if (!IsJointValid(joint, b3_distanceJoint))
            return;
        const bool hasMin = EnumHasAnyFlags(joint->DistanceFlags, DistanceJointFlag::MinDistance);
        const bool hasMax = EnumHasAnyFlags(joint->DistanceFlags, DistanceJointFlag::MaxDistance);
        const bool hasSpring = EnumHasAnyFlags(joint->DistanceFlags, DistanceJointFlag::Spring) || hasMin || hasMax;
        float minLength = hasMin ? joint->DistanceMin : (float)B3_LINEAR_SLOP;
        float maxLength = hasMax ? joint->DistanceMax : Math::Max(joint->DistanceLength, minLength);
        minLength = Math::Max(minLength, (float)B3_LINEAR_SLOP);
        maxLength = Math::Max(maxLength, minLength);
        b3DistanceJoint_EnableSpring(joint->Handle, hasSpring);
        b3DistanceJoint_SetSpringHertz(joint->Handle, Math::Max(joint->DistanceSpring.Stiffness, 0.0f));
        b3DistanceJoint_SetSpringDampingRatio(joint->Handle, Math::Max(joint->DistanceSpring.Damping, 0.0f));
        b3DistanceJoint_EnableLimit(joint->Handle, hasMin || hasMax);
        b3DistanceJoint_SetLengthRange(joint->Handle, minLength, maxLength);
    }

    void ApplyHingeJointState(JointBox3D* joint)
    {
        if (!IsJointValid(joint, b3_revoluteJoint))
            return;
        b3RevoluteJoint_EnableLimit(joint->Handle, EnumHasAnyFlags(joint->HingeFlags, HingeJointFlag::Limit));
        b3RevoluteJoint_SetLimits(joint->Handle, ClampAngularLimit(joint->HingeLimit.Lower), ClampAngularLimit(Math::Max(joint->HingeLimit.Upper, joint->HingeLimit.Lower)));
        b3RevoluteJoint_EnableMotor(joint->Handle, EnumHasAnyFlags(joint->HingeFlags, HingeJointFlag::Drive));
        b3RevoluteJoint_SetMotorSpeed(joint->Handle, joint->HingeDrive.Velocity);
        b3RevoluteJoint_SetMaxMotorTorque(joint->Handle, Math::Max(joint->HingeDrive.ForceLimit, 0.0f));
    }

    void ApplySliderJointState(JointBox3D* joint)
    {
        if (!IsJointValid(joint, b3_prismaticJoint))
            return;
        b3PrismaticJoint_EnableLimit(joint->Handle, EnumHasAnyFlags(joint->SliderFlags, SliderJointFlag::Limit));
        b3PrismaticJoint_SetLimits(joint->Handle, joint->SliderLimit.Lower, Math::Max(joint->SliderLimit.Upper, joint->SliderLimit.Lower));
        b3PrismaticJoint_EnableSpring(joint->Handle, joint->SliderLimit.Spring.Stiffness > 0.0f || joint->SliderLimit.Spring.Damping > 0.0f);
        b3PrismaticJoint_SetSpringHertz(joint->Handle, Math::Max(joint->SliderLimit.Spring.Stiffness, 0.0f));
        b3PrismaticJoint_SetSpringDampingRatio(joint->Handle, Math::Max(joint->SliderLimit.Spring.Damping, 0.0f));
    }

    void ApplySphericalJointState(JointBox3D* joint)
    {
        if (!IsJointValid(joint, b3_sphericalJoint))
            return;
        b3SphericalJoint_EnableConeLimit(joint->Handle, EnumHasAnyFlags(joint->SphericalFlags, SphericalJointFlag::Limit));
        b3SphericalJoint_SetConeLimit(joint->Handle, Math::Min(ClampConeLimit(joint->SphericalLimit.YLimitAngle), ClampConeLimit(joint->SphericalLimit.ZLimitAngle)));
    }

    int32 CountD6Unlocked(const JointBox3D* joint, int32 first, int32 count, D6JointAxis& axis)
    {
        int32 result = 0;
        for (int32 i = 0; i < count; i++)
        {
            const int32 index = first + i;
            if (joint->D6Motion[index] != D6JointMotion::Locked)
            {
                axis = (D6JointAxis)index;
                result++;
            }
        }
        return result;
    }

    D6JointDrive GetD6AngularDrive(const JointBox3D* joint, D6JointAxis axis)
    {
        if (axis == D6JointAxis::Twist)
            return joint->D6Drive[(int32)D6JointDriveType::Twist];
        if (axis == D6JointAxis::SwingY || axis == D6JointAxis::SwingZ)
            return joint->D6Drive[(int32)D6JointDriveType::Swing];
        return joint->D6Drive[(int32)D6JointDriveType::Slerp];
    }

    void ApplyD6JointState(JointBox3D* joint)
    {
        if (!IsJointValid(joint))
            return;
        switch (joint->D6Kind)
        {
        case D6JointKindBox3D::Prismatic:
        {
            const int32 axisIndex = (int32)joint->D6Axis;
            b3PrismaticJoint_EnableLimit(joint->Handle, joint->D6Motion[axisIndex] == D6JointMotion::Limited);
            b3PrismaticJoint_SetLimits(joint->Handle, -Math::Max(joint->D6LimitLinear.Extent, 0.0f), Math::Max(joint->D6LimitLinear.Extent, 0.0f));
            const D6JointDrive& drive = joint->D6Drive[axisIndex];
            b3PrismaticJoint_EnableSpring(joint->Handle, drive.Stiffness > 0.0f || drive.Damping > 0.0f);
            b3PrismaticJoint_SetTargetTranslation(joint->Handle, GetAxisValue(joint->D6DrivePosition, joint->D6Axis));
            b3PrismaticJoint_SetSpringHertz(joint->Handle, Math::Max(drive.Stiffness, 0.0f));
            b3PrismaticJoint_SetSpringDampingRatio(joint->Handle, Math::Max(drive.Damping, 0.0f));
            b3PrismaticJoint_EnableMotor(joint->Handle, drive.ForceLimit > 0.0f);
            b3PrismaticJoint_SetMotorSpeed(joint->Handle, GetAxisValue(joint->D6DriveLinearVelocity, joint->D6Axis));
            b3PrismaticJoint_SetMaxMotorForce(joint->Handle, Math::Max(drive.ForceLimit, 0.0f));
            break;
        }
        case D6JointKindBox3D::Revolute:
        {
            const int32 axisIndex = (int32)joint->D6Axis;
            b3RevoluteJoint_EnableLimit(joint->Handle, joint->D6Motion[axisIndex] == D6JointMotion::Limited);
            if (joint->D6Axis == D6JointAxis::Twist)
            {
                b3RevoluteJoint_SetLimits(joint->Handle, ClampAngularLimit(joint->D6LimitTwist.Lower), ClampAngularLimit(Math::Max(joint->D6LimitTwist.Upper, joint->D6LimitTwist.Lower)));
            }
            else
            {
                const float angle = joint->D6Axis == D6JointAxis::SwingY ? joint->D6LimitSwing.YLimitAngle : joint->D6LimitSwing.ZLimitAngle;
                b3RevoluteJoint_SetLimits(joint->Handle, -ClampConeLimit(angle), ClampConeLimit(angle));
            }
            const D6JointDrive drive = GetD6AngularDrive(joint, joint->D6Axis);
            b3RevoluteJoint_EnableMotor(joint->Handle, drive.ForceLimit > 0.0f);
            b3RevoluteJoint_SetMotorSpeed(joint->Handle, GetAxisValue(joint->D6DriveAngularVelocity, joint->D6Axis));
            b3RevoluteJoint_SetMaxMotorTorque(joint->Handle, Math::Max(drive.ForceLimit, 0.0f));
            break;
        }
        case D6JointKindBox3D::Spherical:
        {
            b3SphericalJoint_EnableConeLimit(joint->Handle, joint->D6Motion[(int32)D6JointAxis::SwingY] == D6JointMotion::Limited || joint->D6Motion[(int32)D6JointAxis::SwingZ] == D6JointMotion::Limited);
            b3SphericalJoint_SetConeLimit(joint->Handle, Math::Min(ClampConeLimit(joint->D6LimitSwing.YLimitAngle), ClampConeLimit(joint->D6LimitSwing.ZLimitAngle)));
            b3SphericalJoint_EnableTwistLimit(joint->Handle, joint->D6Motion[(int32)D6JointAxis::Twist] == D6JointMotion::Limited);
            b3SphericalJoint_SetTwistLimits(joint->Handle, ClampAngularLimit(joint->D6LimitTwist.Lower), ClampAngularLimit(Math::Max(joint->D6LimitTwist.Upper, joint->D6LimitTwist.Lower)));
            b3SphericalJoint_EnableSpring(joint->Handle, joint->D6Drive[(int32)D6JointDriveType::Slerp].Stiffness > 0.0f || joint->D6Drive[(int32)D6JointDriveType::Slerp].Damping > 0.0f);
            b3SphericalJoint_SetSpringHertz(joint->Handle, Math::Max(joint->D6Drive[(int32)D6JointDriveType::Slerp].Stiffness, 0.0f));
            b3SphericalJoint_SetSpringDampingRatio(joint->Handle, Math::Max(joint->D6Drive[(int32)D6JointDriveType::Slerp].Damping, 0.0f));
            b3SphericalJoint_SetTargetRotation(joint->Handle, C2B(joint->D6DriveRotation));
            b3SphericalJoint_EnableMotor(joint->Handle, joint->D6Drive[(int32)D6JointDriveType::Slerp].ForceLimit > 0.0f);
            b3SphericalJoint_SetMotorVelocity(joint->Handle, C2BVec(joint->D6DriveAngularVelocity));
            b3SphericalJoint_SetMaxMotorTorque(joint->Handle, Math::Max(joint->D6Drive[(int32)D6JointDriveType::Slerp].ForceLimit, 0.0f));
            break;
        }
        default:
            break;
        }
    }

    void RecreateJointHandle(JointBox3D* joint)
    {
        DestroyJointHandle(joint);
        if (!joint->Scene || !IsActorValid(joint->Actor0) || !IsActorValid(joint->Actor1))
            return;

        switch (joint->Type)
        {
        case JointTypeBox3D::Fixed:
        {
            b3WeldJointDef def = b3DefaultWeldJointDef();
            SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
            joint->Handle = b3CreateWeldJoint(joint->Scene->World, &def);
            break;
        }
        case JointTypeBox3D::Distance:
        {
            b3DistanceJointDef def = b3DefaultDistanceJointDef();
            SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
            def.length = Math::Max(joint->DistanceLength, (float)B3_LINEAR_SLOP);
            joint->Handle = b3CreateDistanceJoint(joint->Scene->World, &def);
            ApplyDistanceJointState(joint);
            break;
        }
        case JointTypeBox3D::Hinge:
        {
            b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
            SetupJointBase(def.base, joint, GetRevoluteFrame(joint->LocalFrame0, D6JointAxis::Twist), GetRevoluteFrame(joint->LocalFrame1, D6JointAxis::Twist));
            joint->Handle = b3CreateRevoluteJoint(joint->Scene->World, &def);
            ApplyHingeJointState(joint);
            break;
        }
        case JointTypeBox3D::Slider:
        {
            b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
            SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
            joint->Handle = b3CreatePrismaticJoint(joint->Scene->World, &def);
            ApplySliderJointState(joint);
            break;
        }
        case JointTypeBox3D::Spherical:
        {
            b3SphericalJointDef def = b3DefaultSphericalJointDef();
            SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
            joint->Handle = b3CreateSphericalJoint(joint->Scene->World, &def);
            ApplySphericalJointState(joint);
            break;
        }
        case JointTypeBox3D::D6:
        {
            D6JointAxis linearAxis = D6JointAxis::X;
            D6JointAxis angularAxis = D6JointAxis::Twist;
            const int32 linearCount = CountD6Unlocked(joint, 0, 3, linearAxis);
            const int32 angularCount = CountD6Unlocked(joint, 3, 3, angularAxis);
            if (linearCount == 0 && angularCount == 0)
            {
                b3WeldJointDef def = b3DefaultWeldJointDef();
                SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
                joint->Handle = b3CreateWeldJoint(joint->Scene->World, &def);
                joint->D6Kind = D6JointKindBox3D::Weld;
            }
            else if (linearCount == 1 && angularCount == 0)
            {
                b3PrismaticJointDef def = b3DefaultPrismaticJointDef();
                SetupJointBase(def.base, joint, GetPrismaticFrame(joint->LocalFrame0, linearAxis), GetPrismaticFrame(joint->LocalFrame1, linearAxis));
                joint->Handle = b3CreatePrismaticJoint(joint->Scene->World, &def);
                joint->D6Kind = D6JointKindBox3D::Prismatic;
                joint->D6Axis = linearAxis;
                ApplyD6JointState(joint);
            }
            else if (linearCount == 0 && angularCount == 1)
            {
                b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
                SetupJointBase(def.base, joint, GetRevoluteFrame(joint->LocalFrame0, angularAxis), GetRevoluteFrame(joint->LocalFrame1, angularAxis));
                joint->Handle = b3CreateRevoluteJoint(joint->Scene->World, &def);
                joint->D6Kind = D6JointKindBox3D::Revolute;
                joint->D6Axis = angularAxis;
                ApplyD6JointState(joint);
            }
            else if (linearCount == 0 && angularCount == 3)
            {
                b3SphericalJointDef def = b3DefaultSphericalJointDef();
                SetupJointBase(def.base, joint, joint->LocalFrame0, joint->LocalFrame1);
                joint->Handle = b3CreateSphericalJoint(joint->Scene->World, &def);
                joint->D6Kind = D6JointKindBox3D::Spherical;
                ApplyD6JointState(joint);
            }
            break;
        }
        }
    }

    JointBox3D* CreateJointBox(const PhysicsJointDesc& desc, JointTypeBox3D type)
    {
        auto result = New<JointBox3D>();
        result->Owner = desc.Joint;
        result->Actor0 = (ActorBox3D*)desc.Actor0;
        result->Actor1 = (ActorBox3D*)desc.Actor1;
        result->Scene = result->Actor0 ? result->Actor0->Scene : (result->Actor1 ? result->Actor1->Scene : nullptr);
        result->LocalFrame0 = C2BTransform(desc.Pos0, desc.Rot0);
        result->LocalFrame1 = C2BTransform(desc.Pos1, desc.Rot1);
        result->Type = type;
        result->DistanceLength = (float)Vector3::Distance(desc.Pos0, desc.Pos1);
        return result;
    }
}

void* PhysicsBackend::CreateFixedJoint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::Fixed);
    RecreateJointHandle(result);
    return result;
}

void* PhysicsBackend::CreateDistanceJoint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::Distance);
    RecreateJointHandle(result);
    return result;
}

void* PhysicsBackend::CreateHingeJoint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::Hinge);
    RecreateJointHandle(result);
    return result;
}

void* PhysicsBackend::CreateSliderJoint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::Slider);
    RecreateJointHandle(result);
    return result;
}

void* PhysicsBackend::CreateSphericalJoint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::Spherical);
    RecreateJointHandle(result);
    return result;
}

void* PhysicsBackend::CreateD6Joint(const PhysicsJointDesc& desc)
{
    auto result = CreateJointBox(desc, JointTypeBox3D::D6);
    RecreateJointHandle(result);
    return result;
}

void PhysicsBackend::SetDistanceJointFlags(void* joint, DistanceJointFlag flags)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->DistanceFlags = flags;
    ApplyDistanceJointState(jointBox3D);
}

void PhysicsBackend::SetDistanceJointMinDistance(void* joint, float value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->DistanceMin = value;
    ApplyDistanceJointState(jointBox3D);
}

void PhysicsBackend::SetDistanceJointMaxDistance(void* joint, float value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->DistanceMax = value;
    ApplyDistanceJointState(jointBox3D);
}

void PhysicsBackend::SetDistanceJointTolerance(void* joint, float value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (jointBox3D)
        jointBox3D->DistanceTolerance = value;
}

void PhysicsBackend::SetDistanceJointSpring(void* joint, const SpringParameters& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->DistanceSpring = value;
    ApplyDistanceJointState(jointBox3D);
}

float PhysicsBackend::GetDistanceJointDistance(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle) ? b3DistanceJoint_GetCurrentLength(jointBox3D->Handle) : 0.0f;
}

void PhysicsBackend::SetHingeJointFlags(void* joint, HingeJointFlag value, bool driveFreeSpin)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->HingeFlags = value;
    jointBox3D->HingeDriveFreeSpin = driveFreeSpin;
    ApplyHingeJointState(jointBox3D);
}

void PhysicsBackend::SetHingeJointLimit(void* joint, const LimitAngularRange& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->HingeLimit = value;
    ApplyHingeJointState(jointBox3D);
}

void PhysicsBackend::SetHingeJointDrive(void* joint, const HingeJointDrive& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->HingeDrive = value;
    jointBox3D->HingeDriveFreeSpin = value.FreeSpin;
    ApplyHingeJointState(jointBox3D);
}

float PhysicsBackend::GetHingeJointAngle(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle) ? b3RevoluteJoint_GetAngle(jointBox3D->Handle) : 0.0f;
}

float PhysicsBackend::GetHingeJointVelocity(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D && IsJointValid(jointBox3D, b3_revoluteJoint) ? b3RevoluteJoint_GetMotorSpeed(jointBox3D->Handle) : 0.0f;
}

void PhysicsBackend::SetSliderJointFlags(void* joint, SliderJointFlag value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->SliderFlags = value;
    ApplySliderJointState(jointBox3D);
}

void PhysicsBackend::SetSliderJointLimit(void* joint, const LimitLinearRange& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->SliderLimit = value;
    ApplySliderJointState(jointBox3D);
}

float PhysicsBackend::GetSliderJointPosition(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle) ? b3PrismaticJoint_GetTranslation(jointBox3D->Handle) : 0.0f;
}

float PhysicsBackend::GetSliderJointVelocity(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D && B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle) ? b3PrismaticJoint_GetSpeed(jointBox3D->Handle) : 0.0f;
}

void PhysicsBackend::SetSphericalJointFlags(void* joint, SphericalJointFlag value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->SphericalFlags = value;
    ApplySphericalJointState(jointBox3D);
}

void PhysicsBackend::SetSphericalJointLimit(void* joint, const LimitConeRange& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->SphericalLimit = value;
    ApplySphericalJointState(jointBox3D);
}

void PhysicsBackend::SetD6JointMotion(void* joint, D6JointAxis axis, D6JointMotion value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6Motion[(int32)axis] = value;
    RecreateJointHandle(jointBox3D);
}

void PhysicsBackend::SetD6JointDrive(void* joint, const D6JointDriveType index, const D6JointDrive& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6Drive[(int32)index] = value;
    ApplyD6JointState(jointBox3D);
}

void PhysicsBackend::SetD6JointLimitLinear(void* joint, const LimitLinear& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6LimitLinear = value;
    ApplyD6JointState(jointBox3D);
}

void PhysicsBackend::SetD6JointLimitTwist(void* joint, const LimitAngularRange& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6LimitTwist = value;
    ApplyD6JointState(jointBox3D);
}

void PhysicsBackend::SetD6JointLimitSwing(void* joint, const LimitConeRange& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6LimitSwing = value;
    ApplyD6JointState(jointBox3D);
}

Vector3 PhysicsBackend::GetD6JointDrivePosition(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D ? jointBox3D->D6DrivePosition : Vector3::Zero;
}

void PhysicsBackend::SetD6JointDrivePosition(void* joint, const Vector3& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6DrivePosition = value;
    ApplyD6JointState(jointBox3D);
}

Quaternion PhysicsBackend::GetD6JointDriveRotation(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    return jointBox3D ? jointBox3D->D6DriveRotation : Quaternion::Identity;
}

void PhysicsBackend::SetD6JointDriveRotation(void* joint, const Quaternion& value)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6DriveRotation = value;
    ApplyD6JointState(jointBox3D);
}

void PhysicsBackend::GetD6JointDriveVelocity(void* joint, Vector3& linear, Vector3& angular)
{
    auto jointBox3D = (JointBox3D*)joint;
    linear = jointBox3D ? jointBox3D->D6DriveLinearVelocity : Vector3::Zero;
    angular = jointBox3D ? jointBox3D->D6DriveAngularVelocity : Vector3::Zero;
}

void PhysicsBackend::SetD6JointDriveVelocity(void* joint, const Vector3& linear, const Vector3& angular)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    jointBox3D->D6DriveLinearVelocity = linear;
    jointBox3D->D6DriveAngularVelocity = angular;
    ApplyD6JointState(jointBox3D);
}

float PhysicsBackend::GetD6JointTwist(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (jointBox3D && IsJointValid(jointBox3D, b3_sphericalJoint))
        return b3SphericalJoint_GetTwistAngle(jointBox3D->Handle);
    if (jointBox3D && IsJointValid(jointBox3D, b3_revoluteJoint) && jointBox3D->D6Axis == D6JointAxis::Twist)
        return b3RevoluteJoint_GetAngle(jointBox3D->Handle);
    return 0.0f;
}

float PhysicsBackend::GetD6JointSwingY(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (jointBox3D && IsJointValid(jointBox3D, b3_sphericalJoint))
        return b3SphericalJoint_GetConeAngle(jointBox3D->Handle);
    if (jointBox3D && IsJointValid(jointBox3D, b3_revoluteJoint) && jointBox3D->D6Axis == D6JointAxis::SwingY)
        return b3RevoluteJoint_GetAngle(jointBox3D->Handle);
    return 0.0f;
}

float PhysicsBackend::GetD6JointSwingZ(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (jointBox3D && IsJointValid(jointBox3D, b3_sphericalJoint))
        return b3SphericalJoint_GetConeAngle(jointBox3D->Handle);
    if (jointBox3D && IsJointValid(jointBox3D, b3_revoluteJoint) && jointBox3D->D6Axis == D6JointAxis::SwingZ)
        return b3RevoluteJoint_GetAngle(jointBox3D->Handle);
    return 0.0f;
}

namespace
{
    struct ControllerMoveContext
    {
        ShapeBox3D* Self = nullptr;
        Vector3 Up = Vector3::Up;
        Array<b3CollisionPlane, InlinedAllocation<8>> Planes;
        int32 Flags = 0;
        bool CollectFlags = false;
    };

    bool ControllerShouldCollide(b3ShapeId shapeId, ControllerMoveContext* context)
    {
        if (context->Self && IsShapeValid(context->Self) && B3_ID_EQUALS(shapeId, context->Self->Shape))
            return false;
        if (B3_IS_NULL(shapeId) || !b3Shape_IsValid(shapeId) || b3Shape_IsSensor(shapeId))
            return false;
        return true;
    }

    void ControllerAddFlags(const b3Plane& plane, ControllerMoveContext* context)
    {
        Vector3 normal = B2C(plane.normal);
        if (normal.LengthSquared() < ZeroTolerance)
            return;
        normal.Normalize();

        const float vertical = (float)Vector3::Dot(normal, context->Up);
        if (vertical > 0.5f)
            context->Flags |= (int32)CharacterController::CollisionFlags::Below;
        else if (vertical < -0.5f)
            context->Flags |= (int32)CharacterController::CollisionFlags::Above;
        else
            context->Flags |= (int32)CharacterController::CollisionFlags::Sides;
    }

    bool ControllerPlaneResult(b3ShapeId shapeId, const b3PlaneResult* planeResults, int planeCount, void* userContext)
    {
        auto context = (ControllerMoveContext*)userContext;
        if (!ControllerShouldCollide(shapeId, context))
            return true;

        for (int32 i = 0; i < planeCount; i++)
        {
            if (context->CollectFlags)
                ControllerAddFlags(planeResults[i].plane, context);
            auto& plane = context->Planes.AddOne();
            plane.plane = planeResults[i].plane;
            plane.pushLimit = MAX_float;
            plane.push = 0.0f;
            plane.clipVelocity = true;
        }
        return true;
    }

    bool ControllerMoverFilter(b3ShapeId shapeId, void* userContext)
    {
        return ControllerShouldCollide(shapeId, (ControllerMoveContext*)userContext);
    }

    b3HeightFieldData* BuildHeightFieldData(MeshBox3D* heightField)
    {
        return BuildHeightFieldData(heightField, b3Vec3_one);
    }

    b3HeightFieldData* BuildHeightFieldData(MeshBox3D* heightField, const b3Vec3& scale)
    {
        if (!heightField || heightField->Columns <= 1 || heightField->Rows <= 1 || heightField->HeightSamples.Count() != heightField->Columns * heightField->Rows)
            return nullptr;

        const int32 samplesCount = heightField->Columns * heightField->Rows;
        heightField->Heights.Resize(samplesCount, false);
        heightField->HeightMaterials.Resize((heightField->Rows - 1) * (heightField->Columns - 1), false);

        float minHeight = MAX_float;
        float maxHeight = -MAX_float;
        for (int32 z = 0; z < heightField->Columns; z++)
        for (int32 x = 0; x < heightField->Rows; x++)
        {
            // Flax height fields are row-major in X then Z (the PhysX layout), while
            // Box3D expects rows in Z then X. Convert at the backend boundary.
            const int32 sourceIndex = x * heightField->Columns + z;
            const int32 destinationIndex = z * heightField->Rows + x;
            const float h = (float)heightField->HeightSamples[sourceIndex].Height;
            heightField->Heights[destinationIndex] = h;
            minHeight = Math::Min(minHeight, h);
            maxHeight = Math::Max(maxHeight, h);
        }
        for (int32 z = 0; z < heightField->Columns - 1; z++)
        for (int32 x = 0; x < heightField->Rows - 1; x++)
        {
            const auto& sample = heightField->HeightSamples[x * heightField->Columns + z];
            heightField->HeightMaterials[z * (heightField->Rows - 1) + x] = sample.MaterialIndex0 == (uint8)PhysicsBackend::HeightFieldMaterial::Hole ? B3_HEIGHT_FIELD_HOLE : sample.MaterialIndex0;
        }

        b3HeightFieldDef def = {};
        def.heights = heightField->Heights.Get();
        def.materialIndices = heightField->HeightMaterials.Get();
        def.countX = heightField->Rows;
        def.countZ = heightField->Columns;
        def.scale = scale;
        def.globalMinimumHeight = minHeight;
        def.globalMaximumHeight = maxHeight;
        return b3CreateHeightField(&def);
    }
}

void* PhysicsBackend::CreateController(void* scene, IPhysicsActor* actor, PhysicsColliderActor* collider, float contactOffset, const Vector3& position, float slopeLimit, int32 nonWalkableMode, JsonAsset* material, float radius, float height, float stepOffset, void*& shape)
{
    auto sceneBox3D = (SceneBox3D*)scene;
    auto controller = New<ControllerBox3D>();
    controller->Scene = sceneBox3D;
    controller->Owner = actor;
    controller->Radius = radius;
    controller->Height = height;
    controller->StepOffset = stepOffset;
    controller->SlopeLimit = slopeLimit;
    controller->Position = position;

    controller->Actor = (ActorBox3D*)CreateActor(actor, position, Quaternion::Identity, sceneBox3D, b3_kinematicBody);
    controller->Actor->DynamicFlags = RigidDynamicFlags::Kinematic;
    controller->Actor->AddedToScene = true;
    if (b3Body_IsValid(controller->Actor->Body))
        b3Body_Enable(controller->Actor->Body);

    CollisionShape geometry;
    geometry.SetCapsule(radius, height * 0.5f);
    controller->Shape = (ShapeBox3D*)CreateShape(collider, geometry, Span<JsonAsset*>(&material, 1), true, false);
    controller->Shape->ContactOffset = contactOffset;
    AttachShapeInternal(controller->Shape, controller->Actor);
    shape = controller->Shape;
    return controller;
}

void* PhysicsBackend::GetControllerRigidDynamicActor(void* controller)
{
    return ((ControllerBox3D*)controller)->Actor;
}

void PhysicsBackend::SetControllerSize(void* controller, float radius, float height)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    controllerBox3D->Radius = radius;
    controllerBox3D->Height = height;
    CollisionShape geometry;
    geometry.SetCapsule(radius, height * 0.5f);
    SetShapeGeometry(controllerBox3D->Shape, geometry);
}

void PhysicsBackend::SetControllerSlopeLimit(void* controller, float value)
{
    ((ControllerBox3D*)controller)->SlopeLimit = value;
}

void PhysicsBackend::SetControllerNonWalkableMode(void* controller, int32 value)
{
}

void PhysicsBackend::SetControllerStepOffset(void* controller, float value)
{
    ((ControllerBox3D*)controller)->StepOffset = value;
}

Vector3 PhysicsBackend::GetControllerBasePosition(void* controller)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    return controllerBox3D->Position - controllerBox3D->Up * (controllerBox3D->Height * 0.5f + controllerBox3D->Radius);
}

void PhysicsBackend::SetControllerBasePosition(void* controller, const Vector3& value)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    SetControllerPosition(controller, value + controllerBox3D->Up * (controllerBox3D->Height * 0.5f + controllerBox3D->Radius));
}

Vector3 PhysicsBackend::GetControllerUpDirection(void* controller)
{
    return ((ControllerBox3D*)controller)->Up;
}

void PhysicsBackend::SetControllerUpDirection(void* controller, const Vector3& value)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    controllerBox3D->Up = value.IsZero() ? Vector3::Up : Vector3::Normalize(value);
}

Vector3 PhysicsBackend::GetControllerPosition(void* controller)
{
    return ((ControllerBox3D*)controller)->Position;
}

void PhysicsBackend::SetControllerPosition(void* controller, const Vector3& value)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    controllerBox3D->Position = value;
    SetRigidActorPose(controllerBox3D->Actor, value, Quaternion::Identity);
}

int32 PhysicsBackend::MoveController(void* controller, void* shape, const Vector3& displacement, float minMoveDistance, float deltaTime)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    if (!controllerBox3D || displacement.LengthSquared() <= minMoveDistance * minMoveDistance)
        return (int32)CharacterController::CollisionFlags::None;

    const b3Vec3 up = C2BVec(controllerBox3D->Up);
    b3Capsule mover;
    mover.center1 = b3MulSV(-controllerBox3D->Height * 0.5f, up);
    mover.center2 = b3MulSV(controllerBox3D->Height * 0.5f, up);
    mover.radius = controllerBox3D->Radius;

    ControllerMoveContext context;
    context.Self = (ShapeBox3D*)shape;
    context.Up = controllerBox3D->Up;
    const b3QueryFilter filter = MakeQueryFilter(context.Self ? context.Self->Mask1 : MAX_uint32);
    b3Vec3 targetDelta = C2BVec(displacement);
    b3Pos position = C2BPos(controllerBox3D->Position);

    const float tolerance = Math::Max(minMoveDistance, 0.01f);
    for (int32 i = 0; i < 5; i++)
    {
        context.Planes.Clear();
        b3World_CollideMover(controllerBox3D->Scene->World, position, &mover, filter, ControllerPlaneResult, &context);

        const b3PlaneSolverResult solved = b3SolvePlanes(targetDelta, context.Planes.Get(), context.Planes.Count());
        b3Vec3 delta = solved.delta;
        const float fraction = b3World_CastMover(controllerBox3D->Scene->World, position, &mover, delta, filter, ControllerMoverFilter, &context);
        delta = b3MulSV(Math::Clamp(fraction, 0.0f, 1.0f), delta);
        position = b3OffsetPos(position, delta);
        targetDelta = b3Sub(targetDelta, delta);

        if (fraction < 1.0f)
        {
            const float vertical = (float)Vector3::Dot(displacement, controllerBox3D->Up);
            if (vertical > ZeroTolerance)
                context.Flags |= (int32)CharacterController::CollisionFlags::Above;
            else if (vertical < -ZeroTolerance)
                context.Flags |= (int32)CharacterController::CollisionFlags::Below;
            else
                context.Flags |= (int32)CharacterController::CollisionFlags::Sides;
        }
        if (b3LengthSquared(delta) < tolerance * tolerance || b3LengthSquared(targetDelta) < tolerance * tolerance)
            break;
    }

    context.Planes.Clear();
    context.CollectFlags = true;
    b3World_CollideMover(controllerBox3D->Scene->World, position, &mover, filter, ControllerPlaneResult, &context);
    SetControllerPosition(controller, B2C(position));
    return context.Flags;
}

void* PhysicsBackend::CreateConvexMesh(byte* data, int32 dataSize, BoundingBox& localBounds)
{
    if (!data || dataSize < sizeof(Box3DCookedHeader))
        return nullptr;
    const auto header = (const Box3DCookedHeader*)data;
    if (header->Magic != BOX3D_COOKED_MAGIC || header->Version != BOX3D_COOKED_VERSION || header->Type != (uint32)CollisionDataType::ConvexMesh)
        return nullptr;

    if (header->VertexCount < BOX3D_CONVEX_VERTEX_MIN || header->VertexCount > MAX_int32)
        return nullptr;
    const int32 vertexCount = (int32)header->VertexCount;
    const int64 expectedSize = (int64)sizeof(Box3DCookedHeader) + (int64)vertexCount * sizeof(Float3);
    if (dataSize < expectedSize)
        return nullptr;

    auto mesh = New<MeshBox3D>();
    mesh->Type = CollisionShape::Types::ConvexMesh;
    mesh->LocalBounds = header->Bounds;
    mesh->Vertices.Resize(vertexCount, false);
    Platform::MemoryCopy(mesh->Vertices.Get(), data + sizeof(Box3DCookedHeader), vertexCount * sizeof(Float3));

    Array<b3Vec3> points;
    points.Resize(mesh->Vertices.Count(), false);
    for (int32 i = 0; i < mesh->Vertices.Count(); i++)
        points[i] = C2BVec(mesh->Vertices[i]);
    mesh->Hull = CreateBox3DConvexHull(points.Get(), points.Count(), vertexCount);
    if (!mesh->Hull)
    {
        LOG(Warning, "Failed to create Box3D convex hull from cooked collision data.");
        Delete(mesh);
        return nullptr;
    }
    localBounds = mesh->LocalBounds;
    return mesh;
}

void* PhysicsBackend::CreateTriangleMesh(byte* data, int32 dataSize, BoundingBox& localBounds)
{
    if (!data || dataSize < sizeof(Box3DCookedHeader))
        return nullptr;
    const auto header = (const Box3DCookedHeader*)data;
    if (header->Magic != BOX3D_COOKED_MAGIC || header->Version != BOX3D_COOKED_VERSION || header->Type != (uint32)CollisionDataType::TriangleMesh)
        return nullptr;

    if (header->VertexCount == 0 || header->VertexCount > MAX_int32 || header->IndexCount < 3 || header->IndexCount > MAX_int32)
        return nullptr;
    const int32 vertexCount = (int32)header->VertexCount;
    const int32 indexCount = (int32)header->IndexCount;
    const int64 expectedSize = (int64)sizeof(Box3DCookedHeader) + (int64)vertexCount * sizeof(Float3) + (int64)indexCount * sizeof(int32);
    if (dataSize < expectedSize)
        return nullptr;

    auto mesh = New<MeshBox3D>();
    mesh->Type = CollisionShape::Types::TriangleMesh;
    mesh->LocalBounds = header->Bounds;
    mesh->Vertices.Resize(vertexCount, false);
    mesh->Indices.Resize(indexCount, false);
    byte* ptr = data + sizeof(Box3DCookedHeader);
    Platform::MemoryCopy(mesh->Vertices.Get(), ptr, vertexCount * sizeof(Float3));
    ptr += vertexCount * sizeof(Float3);
    Platform::MemoryCopy(mesh->Indices.Get(), ptr, indexCount * sizeof(int32));

    Array<b3Vec3> vertices;
    vertices.Resize(mesh->Vertices.Count(), false);
    for (int32 i = 0; i < mesh->Vertices.Count(); i++)
        vertices[i] = C2BVec(mesh->Vertices[i]);

    const int32 rawTriangleCount = mesh->Indices.Count() / 3;
    Array<int32> validIndices;
    Array<uint32> validRemap;
    validIndices.EnsureCapacity(rawTriangleCount * 3);
    validRemap.EnsureCapacity(rawTriangleCount);
    for (int32 triangleIndex = 0; triangleIndex < rawTriangleCount; triangleIndex++)
    {
        const int32 i0 = mesh->Indices[triangleIndex * 3 + 0];
        const int32 i1 = mesh->Indices[triangleIndex * 3 + 1];
        const int32 i2 = mesh->Indices[triangleIndex * 3 + 2];
        if (i0 < 0 || i0 >= vertexCount || i1 < 0 || i1 >= vertexCount || i2 < 0 || i2 >= vertexCount || i0 == i1 || i1 == i2 || i2 == i0)
            continue;
        const b3Vec3 e1 = b3Sub(vertices[i1], vertices[i0]);
        const b3Vec3 e2 = b3Sub(vertices[i2], vertices[i0]);
        if (b3LengthSquared(b3Cross(e1, e2)) <= ZeroTolerance * ZeroTolerance)
            continue;
        validIndices.Add(i0);
        validIndices.Add(i1);
        validIndices.Add(i2);
        validRemap.Add((uint32)triangleIndex);
    }
    if (!validIndices.HasItems())
    {
        Delete(mesh);
        return nullptr;
    }
    mesh->Indices.Set(validIndices.Get(), validIndices.Count());

    b3MeshDef def = {};
    def.vertices = vertices.Get();
    def.indices = mesh->Indices.Get();
    def.vertexCount = vertices.Count();
    def.triangleCount = mesh->Indices.Count() / 3;
    def.weldTolerance = 0.1f;
    def.weldVertices = true;
    def.identifyEdges = true;
    mesh->Mesh = b3CreateMesh(&def, nullptr, 0);
    if (!mesh->Mesh)
    {
        Delete(mesh);
        return nullptr;
    }

    mesh->Remap.Set(validRemap.Get(), validRemap.Count());
    localBounds = mesh->LocalBounds;
    return mesh;
}

void* PhysicsBackend::CreateHeightField(byte* data, int32 dataSize)
{
    if (!data || dataSize < sizeof(Box3DHeightFieldHeader))
        return nullptr;
    const auto header = (const Box3DHeightFieldHeader*)data;
    if (header->Magic != BOX3D_COOKED_MAGIC || header->Version != BOX3D_COOKED_VERSION || header->Type != (uint32)CollisionShape::Types::HeightField)
        return nullptr;

    const int32 samplesCount = header->Columns * header->Rows;
    const int32 expectedSize = sizeof(Box3DHeightFieldHeader) + samplesCount * sizeof(HeightFieldSample);
    if (dataSize < expectedSize || header->Columns <= 1 || header->Rows <= 1)
        return nullptr;

    auto heightField = New<MeshBox3D>();
    heightField->Type = CollisionShape::Types::HeightField;
    heightField->Columns = header->Columns;
    heightField->Rows = header->Rows;
    heightField->HeightSamples.Resize(samplesCount, false);
    Platform::MemoryCopy(heightField->HeightSamples.Get(), data + sizeof(Box3DHeightFieldHeader), samplesCount * sizeof(HeightFieldSample));
    heightField->HeightField = BuildHeightFieldData(heightField);
    if (!heightField->HeightField)
    {
        Delete(heightField);
        return nullptr;
    }
    return heightField;
}

void PhysicsBackend::GetConvexMeshTriangles(void* contextMesh, Array<Float3, HeapAllocation>& vertexBuffer, Array<int32, HeapAllocation>& indexBuffer)
{
    PROFILE_CPU();
    auto mesh = (MeshBox3D*)contextMesh;
    if (!mesh || !mesh->Hull)
        return;

    const b3HullData* hull = mesh->Hull;
    const b3Vec3* points = b3GetHullPoints(hull);
    const b3HullHalfEdge* edges = b3GetHullEdges(hull);
    const b3HullFace* faces = b3GetHullFaces(hull);
    if (!points || !edges || !faces || hull->vertexCount <= 0 || hull->faceCount <= 0)
        return;

    vertexBuffer.Resize(hull->vertexCount, false);
    for (int32 i = 0; i < hull->vertexCount; i++)
        vertexBuffer[i] = Float3(points[i].x, points[i].y, points[i].z);

    int32 indexCount = 0;
    for (int32 faceIndex = 0; faceIndex < hull->faceCount; faceIndex++)
    {
        const uint8 startEdge = faces[faceIndex].edge;
        uint8 edge = startEdge;
        int32 loopLength = 0;
        do
        {
            loopLength++;
            edge = edges[edge].next;
            if (loopLength > 256)
                return;
        } while (edge != startEdge);
        if (loopLength >= 3)
            indexCount += (loopLength - 2) * 3;
    }

    indexBuffer.Resize(indexCount, false);
    int32 outIndex = 0;
    for (int32 faceIndex = 0; faceIndex < hull->faceCount; faceIndex++)
    {
        const uint8 startEdge = faces[faceIndex].edge;
        uint8 loop[256];
        uint8 edge = startEdge;
        int32 loopLength = 0;
        do
        {
            if (loopLength >= 256)
                return;
            loop[loopLength++] = edges[edge].origin;
            edge = edges[edge].next;
        } while (edge != startEdge);

        for (int32 i = 1; i < loopLength - 1; i++)
        {
            indexBuffer[outIndex++] = loop[0];
            indexBuffer[outIndex++] = loop[i];
            indexBuffer[outIndex++] = loop[i + 1];
        }
    }
}

void PhysicsBackend::GetTriangleMeshTriangles(void* triangleMesh, Array<Float3, HeapAllocation>& vertexBuffer, Array<int32, HeapAllocation>& indexBuffer)
{
    auto mesh = (MeshBox3D*)triangleMesh;
    if (!mesh)
        return;
    vertexBuffer.Resize(mesh->Vertices.Count(), false);
    indexBuffer.Resize(mesh->Indices.Count(), false);
    if (mesh->Vertices.HasItems())
        Platform::MemoryCopy(vertexBuffer.Get(), mesh->Vertices.Get(), mesh->Vertices.Count() * sizeof(Float3));
    if (mesh->Indices.HasItems())
        Platform::MemoryCopy(indexBuffer.Get(), mesh->Indices.Get(), mesh->Indices.Count() * sizeof(int32));
}

const uint32* PhysicsBackend::GetTriangleMeshRemap(void* triangleMesh, uint32& count)
{
    auto mesh = (MeshBox3D*)triangleMesh;
    if (!mesh)
    {
        count = 0;
        return nullptr;
    }
    count = mesh->Remap.Count();
    return mesh->Remap.Get();
}

void PhysicsBackend::GetHeightFieldSize(void* heightField, int32& rows, int32& columns)
{
    auto mesh = (MeshBox3D*)heightField;
    rows = mesh ? mesh->Rows : 0;
    columns = mesh ? mesh->Columns : 0;
}

float PhysicsBackend::GetHeightFieldHeight(void* heightField, int32 x, int32 z)
{
    auto mesh = (MeshBox3D*)heightField;
    if (!mesh || x < 0 || z < 0 || x >= mesh->Rows || z >= mesh->Columns)
        return 0.0f;
    return (float)mesh->HeightSamples[x * mesh->Columns + z].Height;
}

PhysicsBackend::HeightFieldSample PhysicsBackend::GetHeightFieldSample(void* heightField, int32 x, int32 z)
{
    auto mesh = (MeshBox3D*)heightField;
    if (!mesh || x < 0 || z < 0 || x >= mesh->Rows || z >= mesh->Columns)
        return HeightFieldSample();
    return mesh->HeightSamples[x * mesh->Columns + z];
}

bool PhysicsBackend::ModifyHeightField(void* heightField, int32 startCol, int32 startRow, int32 cols, int32 rows, const HeightFieldSample* data)
{
    auto mesh = (MeshBox3D*)heightField;
    if (!mesh || !data || startCol < 0 || startRow < 0 || cols <= 0 || rows <= 0 || startCol + cols > mesh->Columns || startRow + rows > mesh->Rows)
        return true;

    for (int32 row = 0; row < rows; row++)
    {
        const int32 dstRow = startRow + row;
        const int32 dstIndex = dstRow * mesh->Columns + startCol;
        const int32 srcIndex = row * cols;
        Platform::MemoryCopy(mesh->HeightSamples.Get() + dstIndex, data + srcIndex, cols * sizeof(HeightFieldSample));
    }

    b3HeightFieldData* oldHeightField = mesh->HeightField;
    b3HeightFieldData* newHeightField = BuildHeightFieldData(mesh);
    if (!newHeightField)
        return true;
    mesh->HeightField = newHeightField;

    Array<ShapeBox3D*> shapes;
    shapes.Set(mesh->HeightFieldShapes.Get(), mesh->HeightFieldShapes.Count());
    for (auto shape : shapes)
    {
        if (shape)
            RecreateRuntimeShape(shape);
    }

    if (oldHeightField)
        b3DestroyHeightField(oldHeightField);
    return false;
}

void PhysicsBackend::FlushRequests()
{
}

void PhysicsBackend::FlushRequests(void* scene)
{
}

void PhysicsBackend::DestroyActor(void* actor)
{
    auto actorBox3D = (ActorBox3D*)actor;
    if (!actorBox3D)
        return;
    ClearPendingActorRequests(actorBox3D);
    for (auto shape : actorBox3D->Shapes)
    {
        if (shape->HeightFieldOwner)
        {
            shape->HeightFieldOwner->HeightFieldShapes.Remove(shape);
            shape->HeightFieldOwner = nullptr;
        }
        shape->Actor = nullptr;
        shape->Shape = b3_nullShapeId;
    }
    if (b3Body_IsValid(actorBox3D->Body))
        b3DestroyBody(actorBox3D->Body);
    Delete(actorBox3D);
}

void PhysicsBackend::DestroyShape(void* shape)
{
    auto shapeBox3D = (ShapeBox3D*)shape;
    if (!shapeBox3D)
        return;
    DestroyRuntimeShape(shapeBox3D);
    if (shapeBox3D->Actor)
        shapeBox3D->Actor->Shapes.Remove(shapeBox3D);
    Delete(shapeBox3D);
}

void PhysicsBackend::DestroyJoint(void* joint)
{
    auto jointBox3D = (JointBox3D*)joint;
    if (!jointBox3D)
        return;
    if (B3_IS_NON_NULL(jointBox3D->Handle) && b3Joint_IsValid(jointBox3D->Handle))
        b3DestroyJoint(jointBox3D->Handle, true);
    Delete(jointBox3D);
}

void PhysicsBackend::DestroyController(void* controller)
{
    auto controllerBox3D = (ControllerBox3D*)controller;
    if (!controllerBox3D)
        return;
    DestroyShape(controllerBox3D->Shape);
    DestroyActor(controllerBox3D->Actor);
    Delete(controllerBox3D);
}

void PhysicsBackend::DestroyMaterial(void* material)
{
    Delete((MaterialBox3D*)material);
}

void PhysicsBackend::DestroyObject(void* object)
{
    Delete((MeshBox3D*)object);
}

void PhysicsBackend::RemoveCollider(PhysicsColliderActor* collider)
{
}

void PhysicsBackend::RemoveJoint(Joint* joint)
{
}

#endif
