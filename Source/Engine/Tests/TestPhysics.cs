// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Runtime.CompilerServices;
#if USE_NETCORE
using FlaxEngine.Interop;
#endif
using NUnit.Framework;

namespace FlaxEngine.Tests
{
#if USE_NETCORE
    /// <summary>
    /// Managed physics interop tests that run against the native test scene.
    /// </summary>
    [TestFixture]
    public class TestPhysics
    {
        public static void RunManagedCastInteropTests(IntPtr scenePtr, IntPtr floorPtr, IntPtr spherePtr, IntPtr materialPtr)
        {
            var scene = ReinterpretManagedReference<PhysicsScene>(scenePtr);
            var floorCollider = ReinterpretManagedReference<PhysicsColliderActor>(floorPtr);
            var sphereCollider = ReinterpretManagedReference<PhysicsColliderActor>(spherePtr);
            var material = ReinterpretManagedReference<PhysicalMaterial>(materialPtr);
            Vector3 origin = new Vector3(0.0f, 300.0f, 0.0f);
            Vector3 direction = Vector3.Down;

            Assert.AreEqual(0, Physics.RayCastAllNonAlloc(origin, direction, null, 400.0f, uint.MaxValue, true));
            Assert.AreEqual(0, Physics.RayCastAllNonAlloc(origin, direction, Array.Empty<RayCastHit>(), 400.0f, uint.MaxValue, true));
            Assert.AreEqual(0, scene.RayCastAllNonAlloc(origin, direction, null, 400.0f, uint.MaxValue, true));
            Assert.AreEqual(0, scene.RayCastAllNonAlloc(origin, direction, Array.Empty<RayCastHit>(), 400.0f, uint.MaxValue, true));

            var staticRayHits = new RayCastHit[2];
            staticRayHits[1].Distance = 12345.0f;
            int staticRayCount = Physics.RayCastAllNonAlloc(origin, direction, staticRayHits, 400.0f, uint.MaxValue, false);
            Assert.AreEqual(1, staticRayCount);
            AssertFloorHit(staticRayHits[0], floorCollider, material);
            Assert.AreEqual(12345.0f, staticRayHits[1].Distance);

            var filteredRayHits = new RayCastHit[2];
            int filteredRayCount = Physics.RayCastAllNonAlloc(origin, direction, filteredRayHits, 400.0f, uint.MaxValue, false);
            int triggerRayCount = Physics.RayCastAllNonAlloc(origin, direction, filteredRayHits, 400.0f, uint.MaxValue, true);
            Assert.AreEqual(1, filteredRayCount);
            Assert.AreEqual(2, triggerRayCount);
            Assert.IsTrue(ContainsCollider(filteredRayHits, triggerRayCount, sphereCollider));

            Vector3 lineEnd = new Vector3(0.0f, -100.0f, 0.0f);
            var sceneRayHits = new RayCastHit[2];
            sceneRayHits[1].Distance = 23456.0f;
            int sceneRayCount = scene.LineCastAllNonAlloc(origin, lineEnd, sceneRayHits, uint.MaxValue, false);
            Assert.AreEqual(1, sceneRayCount);
            AssertFloorHit(sceneRayHits[0], floorCollider, material);
            Assert.AreEqual(23456.0f, sceneRayHits[1].Distance);

            var staticSweepHits = new RayCastHit[2];
            int staticSweepCount = Physics.SphereCastAllNonAlloc(new Vector3(0.0f, 200.0f, 0.0f), 10.0f,
                direction, staticSweepHits, 100.0f, uint.MaxValue, true);
            Assert.IsTrue(staticSweepCount >= 1);
            Assert.IsTrue(ContainsCollider(staticSweepHits, staticSweepCount, sphereCollider));

            var sceneSweepHits = new RayCastHit[2];
            int sceneSweepCount = scene.CapsuleCastAllNonAlloc(new Vector3(0.0f, 200.0f, 0.0f), 10.0f, 20.0f,
                direction, sceneSweepHits, Quaternion.Identity, 100.0f, uint.MaxValue, true);
            Assert.IsTrue(sceneSweepCount >= 1);
            Assert.IsTrue(ContainsCollider(sceneSweepHits, sceneSweepCount, sphereCollider));

            var allocationHits = new RayCastHit[2];
            Physics.RayCastAllNonAlloc(origin, direction, allocationHits, 400.0f, uint.MaxValue, false);
            long allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
            int repeatedCount = 0;
            for (int i = 0; i < 64; i++)
                repeatedCount = Physics.RayCastAllNonAlloc(origin, direction, allocationHits, 400.0f, uint.MaxValue, false);
            long allocatedAfter = GC.GetAllocatedBytesForCurrentThread();
            Assert.AreEqual(1, repeatedCount);
            Assert.AreEqual(0, allocatedAfter - allocatedBefore);
        }

        private static void AssertFloorHit(RayCastHit hit, PhysicsColliderActor floorCollider, PhysicalMaterial material)
        {
            Assert.AreSame(floorCollider, hit.Collider);
            Assert.AreSame(material, hit.Material);
            Assert.That(hit.Distance, Is.EqualTo(300.0f).Within(0.5f));
            Assert.That(hit.Point.Y, Is.EqualTo(0.0f).Within(0.5f));
            Assert.That(hit.Normal.Y, Is.EqualTo(1.0f).Within(0.01f));
        }

        private static bool ContainsCollider(RayCastHit[] hits, int count, PhysicsColliderActor collider)
        {
            for (int i = 0; i < count; i++)
            {
                if (hits[i].Collider == collider)
                    return true;
            }
            return false;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static T ReinterpretManagedReference<T>(IntPtr pointer) where T : class
        {
            return pointer == IntPtr.Zero ? null : (T)ManagedHandle.FromIntPtr(pointer).Target;
        }
    }
#endif
}
