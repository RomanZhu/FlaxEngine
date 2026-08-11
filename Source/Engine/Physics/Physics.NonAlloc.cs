// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEngine
{
    public static partial class Physics
    {
        /// <summary>Finds box overlaps and writes up to the supplied buffer length without allocating.</summary>
        public static int OverlapBoxNonAlloc(Vector3 center, Vector3 halfExtents, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapBoxNonAlloc(ref center, ref halfExtents, results, ref rotation, layerMask, hitTriggers);
        }

        /// <summary>Finds sphere overlaps and writes up to the supplied buffer length without allocating.</summary>
        public static int OverlapSphereNonAlloc(Vector3 center, float radius, PhysicsColliderActor[] results, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapSphereNonAlloc(ref center, radius, results, layerMask, hitTriggers);
        }

        /// <summary>Finds capsule overlaps and writes up to the supplied buffer length without allocating.</summary>
        public static int OverlapCapsuleNonAlloc(Vector3 center, float radius, float height, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapCapsuleNonAlloc(ref center, radius, height, results, ref rotation, layerMask, hitTriggers);
        }

        /// <summary>Finds convex overlaps and writes up to the supplied buffer length without allocating.</summary>
        public static int OverlapConvexNonAlloc(Vector3 center, CollisionData convexMesh, Vector3 scale, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapConvexNonAlloc(ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, results, ref rotation, layerMask, hitTriggers);
        }
    }

    public partial class PhysicsScene
    {
        /// <summary>Finds box overlaps and writes up to the supplied buffer length without allocating.</summary>
        public int OverlapBoxNonAlloc(Vector3 center, Vector3 halfExtents, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapBoxNonAlloc(__unmanagedPtr, ref center, ref halfExtents, results, ref rotation, layerMask, hitTriggers);
        }

        /// <summary>Finds sphere overlaps and writes up to the supplied buffer length without allocating.</summary>
        public int OverlapSphereNonAlloc(Vector3 center, float radius, PhysicsColliderActor[] results, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapSphereNonAlloc(__unmanagedPtr, ref center, radius, results, layerMask, hitTriggers);
        }

        /// <summary>Finds capsule overlaps and writes up to the supplied buffer length without allocating.</summary>
        public int OverlapCapsuleNonAlloc(Vector3 center, float radius, float height, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapCapsuleNonAlloc(__unmanagedPtr, ref center, radius, height, results, ref rotation, layerMask, hitTriggers);
        }

        /// <summary>Finds convex overlaps and writes up to the supplied buffer length without allocating.</summary>
        public int OverlapConvexNonAlloc(Vector3 center, CollisionData convexMesh, Vector3 scale, PhysicsColliderActor[] results, Quaternion rotation, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            return Internal_OverlapConvexNonAlloc(__unmanagedPtr, ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, results, ref rotation, layerMask, hitTriggers);
        }
    }
}
