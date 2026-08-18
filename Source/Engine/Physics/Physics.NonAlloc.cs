// Copyright (c) Wojciech Figat. All rights reserved.

using System;
#if USE_NETCORE
using FlaxEngine.Interop;
#endif

namespace FlaxEngine
{
#if USE_NETCORE
    internal static unsafe class PhysicsCastInterop
    {
        [ThreadStatic]
        internal static RayCastHitMarshaller.RayCastHitInternal[] CastScratch;

        internal static RayCastHitMarshaller.RayCastHitInternal[] GetScratch(int capacity)
        {
            if (CastScratch == null || CastScratch.Length < capacity)
                CastScratch = new RayCastHitMarshaller.RayCastHitInternal[capacity];
            return CastScratch;
        }

        internal static int CopyResults(RayCastHitMarshaller.RayCastHitInternal[] scratch, RayCastHit[] results, int count)
        {
            for (int i = 0; i < count; i++)
                results[i] = RayCastHitMarshaller.ToManaged(scratch[i]);
            return count;
        }
    }
#endif

    public static partial class Physics
    {
        /// <summary>Performs a line cast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int LineCastAllNonAlloc(Vector3 start, Vector3 end, RayCastHit[] results, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_LineCastAllNonAlloc(ref start, ref end, (IntPtr)scratchPtr, results.Length, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_LineCastAllNonAlloc(ref start, ref end, results, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a raycast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int RayCastAllNonAlloc(Vector3 origin, Vector3 direction, RayCastHit[] results, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_RayCastAllNonAlloc(ref origin, ref direction, (IntPtr)scratchPtr, results.Length, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_RayCastAllNonAlloc(ref origin, ref direction, results, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a box cast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int BoxCastAllNonAlloc(Vector3 center, Vector3 halfExtents, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_BoxCastAllNonAlloc(ref center, ref halfExtents, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_BoxCastAllNonAlloc(ref center, ref halfExtents, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a sphere cast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int SphereCastAllNonAlloc(Vector3 center, float radius, Vector3 direction, RayCastHit[] results, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_SphereCastAllNonAlloc(ref center, radius, ref direction, (IntPtr)scratchPtr, results.Length, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_SphereCastAllNonAlloc(ref center, radius, ref direction, results, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a capsule cast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int CapsuleCastAllNonAlloc(Vector3 center, float radius, float height, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_CapsuleCastAllNonAlloc(ref center, radius, height, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_CapsuleCastAllNonAlloc(ref center, radius, height, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a convex cast and writes up to the supplied buffer length without allocating.</summary>
        public static unsafe int ConvexCastAllNonAlloc(Vector3 center, CollisionData convexMesh, Vector3 scale, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_ConvexCastAllNonAlloc(ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_ConvexCastAllNonAlloc(ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

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
        /// <summary>Performs a line cast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int LineCastAllNonAlloc(Vector3 start, Vector3 end, RayCastHit[] results, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_LineCastAllNonAlloc(__unmanagedPtr, ref start, ref end, (IntPtr)scratchPtr, results.Length, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_LineCastAllNonAlloc(__unmanagedPtr, ref start, ref end, results, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a raycast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int RayCastAllNonAlloc(Vector3 origin, Vector3 direction, RayCastHit[] results, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_RayCastAllNonAlloc(__unmanagedPtr, ref origin, ref direction, (IntPtr)scratchPtr, results.Length, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_RayCastAllNonAlloc(__unmanagedPtr, ref origin, ref direction, results, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a box cast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int BoxCastAllNonAlloc(Vector3 center, Vector3 halfExtents, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_BoxCastAllNonAlloc(__unmanagedPtr, ref center, ref halfExtents, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_BoxCastAllNonAlloc(__unmanagedPtr, ref center, ref halfExtents, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a sphere cast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int SphereCastAllNonAlloc(Vector3 center, float radius, Vector3 direction, RayCastHit[] results, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_SphereCastAllNonAlloc(__unmanagedPtr, ref center, radius, ref direction, (IntPtr)scratchPtr, results.Length, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_SphereCastAllNonAlloc(__unmanagedPtr, ref center, radius, ref direction, results, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a capsule cast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int CapsuleCastAllNonAlloc(Vector3 center, float radius, float height, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_CapsuleCastAllNonAlloc(__unmanagedPtr, ref center, radius, height, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_CapsuleCastAllNonAlloc(__unmanagedPtr, ref center, radius, height, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

        /// <summary>Performs a convex cast and writes up to the supplied buffer length without allocating.</summary>
        public unsafe int ConvexCastAllNonAlloc(Vector3 center, CollisionData convexMesh, Vector3 scale, Vector3 direction, RayCastHit[] results, Quaternion rotation, float maxDistance = float.MaxValue, uint layerMask = uint.MaxValue, bool hitTriggers = true)
        {
            if (results == null || results.Length == 0)
                return 0;
#if USE_NETCORE
            var scratch = PhysicsCastInterop.GetScratch(results.Length);
            int count;
            fixed (RayCastHitMarshaller.RayCastHitInternal* scratchPtr = scratch)
            {
                count = Internal_ConvexCastAllNonAlloc(__unmanagedPtr, ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, ref direction, (IntPtr)scratchPtr, results.Length, ref rotation, maxDistance, layerMask, hitTriggers);
            }
            return PhysicsCastInterop.CopyResults(scratch, results, count);
#else
            return Internal_ConvexCastAllNonAlloc(__unmanagedPtr, ref center, Object.GetUnmanagedPtr(convexMesh), ref scale, ref direction, results, ref rotation, maxDistance, layerMask, hitTriggers);
#endif
        }

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
