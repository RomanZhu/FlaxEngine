// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.Options;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG;
using FlaxEditor.Tools.CSG.HitTesting;
using FlaxEditor.Tools.CSG.Selection;
using FlaxEditor.Viewport.Modes;
using FlaxEngine;
using NUnit.Framework;

namespace FlaxEditor.Tests
{
    [TestFixture]
    public class TestCSGAuthoringMode
    {
        [Test]
        public void TestModeCollectionCancelsBeforeActivationChange()
        {
            var owner = new TestGizmoOwner();
            var first = new TestMode();
            var second = new TestMode();
            owner.Gizmos.AddMode(first);
            owner.Gizmos.AddMode(second);

            owner.Gizmos.ActiveMode = first;
            Assert.AreEqual(1, first.ActivationCount);
            Assert.AreEqual(0, first.DeactivationCount);

            owner.Gizmos.ActiveMode = second;
            Assert.AreEqual(EditorGizmoModeCancelReason.ModeChanged, first.LastCancelReason);
            Assert.AreEqual(1, first.DeactivationCount);
            Assert.AreEqual(1, second.ActivationCount);

            owner.Gizmos.Clear();
        }

        [Test]
        public void TestDefaultModeInputHooksDoNotConsumeViewportInput()
        {
            var mode = new TestMode();
            Assert.IsFalse(mode.OnMouseMove(Float2.Zero));
            Assert.IsFalse(mode.OnMouseDown(Float2.Zero, MouseButton.Left));
            Assert.IsFalse(mode.OnMouseUp(Float2.Zero, MouseButton.Left));
            Assert.IsFalse(mode.OnMouseDoubleClick(Float2.Zero, MouseButton.Left));
            Assert.IsFalse(mode.OnKeyDown(KeyboardKeys.Q));
            Assert.IsFalse(mode.OnKeyUp(KeyboardKeys.Q));
        }

        [Test]
        public void TestControllerStateRoundTripsAndUnavailableFeaturesAreSanitized()
        {
            var source = new CSGToolController();
            Assert.IsTrue(source.SetTool(CSGTool.Draw));
            Assert.IsTrue(source.SetOperation(CSGOperation.Subtractive));
            source.SetWorkingPlaneLocked(true);
            source.SetSnappingEnabled(false);
            source.SetSnapIncrement(25.0f);
            source.SetVisibility(CSGVisibility.SourceBrushes | CSGVisibility.HiddenBrushes);

            var restored = new CSGToolController();
            restored.ApplyState(source.CaptureState());
            Assert.AreEqual(CSGTool.Draw, restored.Tool);
            Assert.AreEqual(CSGOperation.Subtractive, restored.Operation);
            Assert.IsTrue(restored.WorkingPlaneLocked);
            Assert.IsFalse(restored.SnappingEnabled);
            Assert.AreEqual(25.0f, restored.SnapIncrement);
            Assert.AreEqual(CSGVisibility.SourceBrushes | CSGVisibility.HiddenBrushes, restored.Visibility);

            var unavailable = source.CaptureState();
            unavailable.Tool = CSGTool.Clip;
            unavailable.Operation = CSGOperation.Intersecting;
            unavailable.SnapIncrement = 0.0f;
            restored.ApplyState(unavailable);
            Assert.AreEqual(CSGTool.SelectPlace, restored.Tool);
            Assert.AreEqual(CSGOperation.Additive, restored.Operation);
            Assert.Greater(restored.SnapIncrement, 0.0f);
            Assert.IsFalse(restored.SetTool(CSGTool.Clip));
            Assert.IsFalse(restored.SetOperation(CSGOperation.Intersecting));
        }

        [Test]
        public void TestInteractionCancelsForEveryAuthoringContextBoundary()
        {
            var controller = new CSGToolController();
            var reasons = new[]
            {
                EditorGizmoModeCancelReason.FocusLost,
                EditorGizmoModeCancelReason.ToolChanged,
                EditorGizmoModeCancelReason.SceneChanged,
                EditorGizmoModeCancelReason.PlayModeBeginning,
                EditorGizmoModeCancelReason.ModeChanged,
                EditorGizmoModeCancelReason.User,
            };

            foreach (var reason in reasons)
            {
                controller.BeginInteraction();
                controller.SetTransientModifiers(true, true, true, true);
                Assert.IsTrue(controller.TryCancel(reason));
                Assert.IsFalse(controller.HasActiveInteraction);
                Assert.IsFalse(controller.SnapOverrideActive);
                Assert.IsFalse(controller.SquareConstraintActive);
                Assert.IsFalse(controller.SymmetricConstraintActive);
                Assert.IsFalse(controller.DuplicateModifierActive);
                Assert.AreEqual(reason, controller.LastCancelReason);
            }

            controller.BeginInteraction();
            Assert.IsTrue(controller.SetTool(CSGTool.Edit));
            Assert.IsFalse(controller.HasActiveInteraction);
            Assert.AreEqual(EditorGizmoModeCancelReason.ToolChanged, controller.LastCancelReason);
        }

        [Test]
        public void TestInputBindingConflictsAreReported()
        {
            var input = new InputOptions
            {
                CSGDrawTool = new InputBinding(KeyboardKeys.B),
                CSGEditTool = new InputBinding(KeyboardKeys.B),
            };

            var conflicts = CSGAuthoringGizmoMode.FindInputConflicts(input);
            Assert.AreEqual(1, conflicts.Count);
            StringAssert.Contains("Draw Tool", conflicts[0]);
            StringAssert.Contains("Edit Tool", conflicts[0]);

            input.CSGEditTool = new InputBinding(KeyboardKeys.E);
            Assert.AreEqual(0, CSGAuthoringGizmoMode.FindInputConflicts(input).Count);
        }

        [Test]
        public void TestAllHitTraversalAppendsEveryHitAndPreservesNearestHit()
        {
            var root = new TestRayNode(Guid.NewGuid(), 5.0f);
            var near = new TestRayNode(Guid.NewGuid(), 2.0f);
            var far = new TestRayNode(Guid.NewGuid(), 7.0f);
            root.AddChild(near);
            root.AddChild(far);
            var ray = new SceneGraphNode.RayCastData
            {
                Ray = new Ray(Vector3.Zero, Vector3.Forward),
                View = new Ray(Vector3.Zero, Vector3.Forward),
            };
            var hits = new List<SceneGraphNode.RayCastHit>(3);

            root.RayCastAll(ref ray, hits);

            Assert.AreEqual(3, hits.Count);
            Assert.AreSame(root, hits[0].Node);
            Assert.AreSame(near, hits[1].Node);
            Assert.AreSame(far, hits[2].Node);
            Assert.AreSame(near, root.RayCast(ref ray, out var distance, out _));
            Assert.AreEqual(2.0f, distance);
            root.Dispose();
        }

        [Test]
        public void TestAllHitTraversalAllocatesNothingAfterWarmup()
        {
            var root = new TestRayNode(Guid.NewGuid(), 1.0f);
            root.AddChild(new TestRayNode(Guid.NewGuid(), 2.0f));
            var ray = new SceneGraphNode.RayCastData
            {
                Ray = new Ray(Vector3.Zero, Vector3.Forward),
                View = new Ray(Vector3.Zero, Vector3.Forward),
            };
            var hits = new List<SceneGraphNode.RayCastHit>(2);
            root.RayCastAll(ref ray, hits);
            hits.Clear();

            long before = System.GC.GetAllocatedBytesForCurrentThread();
            for (int i = 0; i < 100; i++)
            {
                root.RayCastAll(ref ray, hits);
                hits.Clear();
            }
            long after = System.GC.GetAllocatedBytesForCurrentThread();

            Assert.AreEqual(before, after);
            root.Dispose();
        }

        [Test]
        public void TestSelectionModelKeepsObjectAndCSGMemoriesSeparate()
        {
            var root = new TestRootNode();
            var objectNode = new TestRayNode(Guid.NewGuid(), 1.0f);
            var brushNode = new TestRayNode(Guid.NewGuid(), 2.0f, CSGViewportSelectionKind.Brush);
            root.AddChild(objectNode);
            root.AddChild(brushNode);
            var model = new CSGSelectionModel();
            var result = new List<SceneGraphNode>();

            model.Enter(new[] { objectNode }, root, result);
            Assert.AreEqual(0, result.Count);
            model.ApplyClick(brushNode, false, false, result);
            CollectionAssert.AreEqual(new[] { brushNode }, result);
            model.Leave(result, root, result);
            CollectionAssert.AreEqual(new[] { objectNode }, result);
            model.Enter(result, root, result);
            CollectionAssert.AreEqual(new[] { brushNode }, result);

            model.Observe(new[] { objectNode });
            model.Leave(new[] { objectNode }, root, result);
            model.Enter(result, root, result);
            CollectionAssert.AreEqual(new[] { brushNode }, result, "Passive Scene Tree selection must not erase CSG memory.");
            root.Dispose();
        }

        [Test]
        public void TestToolSpecificCSGHitEligibilitySeparatesBodiesAndFaces()
        {
            var brush = new CSGHit { Kind = CSGHitKind.Brush };
            var face = new CSGHit { Kind = CSGHitKind.Face };
            var placement = new CSGHit { Kind = CSGHitKind.Placement };

            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.SelectPlace, ref brush));
            Assert.IsFalse(CSGHitTestService.IsSelectable(CSGTool.SelectPlace, ref face));
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Edit, ref face));
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Surface, ref face));
            Assert.IsFalse(CSGHitTestService.IsSelectable(CSGTool.Edit, ref placement));
        }

        [Test]
        public void TestCSGHitsSortFrontToBackWithStableOrderTieBreak()
        {
            var root = new TestRootNode();
            var far = new TestRayNode(Guid.NewGuid(), 4.0f, CSGViewportSelectionKind.Brush) { OrderInParent = 0 };
            var tiedSecond = new TestRayNode(Guid.NewGuid(), 2.0f, CSGViewportSelectionKind.Brush) { OrderInParent = 2 };
            var tiedFirst = new TestRayNode(Guid.NewGuid(), 2.0f, CSGViewportSelectionKind.Brush) { OrderInParent = 1 };
            root.AddChild(far);
            root.AddChild(tiedSecond);
            root.AddChild(tiedFirst);
            var service = new CSGHitTestService();
            var hits = new List<CSGHit>();
            var ray = new Ray(Vector3.Zero, Vector3.Forward);
            var view = ray;

            service.Gather(root, ref ray, ref view, hits, SceneGraphNode.RayCastData.FlagTypes.None);

            Assert.AreEqual(3, hits.Count);
            Assert.AreSame(tiedFirst, hits[0].Node);
            Assert.AreSame(tiedSecond, hits[1].Node);
            Assert.AreSame(far, hits[2].Node);
            root.Dispose();
        }

        private sealed class TestMode : EditorGizmoMode
        {
            public int ActivationCount;
            public int DeactivationCount;
            public EditorGizmoModeCancelReason LastCancelReason;

            public override void OnActivated()
            {
                ActivationCount++;
                base.OnActivated();
            }

            public override void OnDeactivated()
            {
                DeactivationCount++;
                base.OnDeactivated();
            }

            public override bool TryCancel(EditorGizmoModeCancelReason reason)
            {
                LastCancelReason = reason;
                return true;
            }
        }

        private sealed class TestRayNode : SceneGraphNode
        {
            private readonly Real _distance;
            private readonly CSGViewportSelectionKind _csgKind;
            private Transform _transform;
            private int _order;

            public TestRayNode(Guid id, Real distance, CSGViewportSelectionKind csgKind = CSGViewportSelectionKind.None)
            : base(id)
            {
                _distance = distance;
                _csgKind = csgKind;
                _transform = Transform.Identity;
            }

            public override string Name => "Test";
            public override SceneNode ParentScene => null;
            public override Transform Transform { get => _transform; set => _transform = value; }
            public override bool IsActive => true;
            public override bool IsActiveInHierarchy => true;
            public override int OrderInParent { get => _order; set => _order = value; }
            public override CSGViewportSelectionKind CSGViewportSelection => _csgKind;

            public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
            {
                distance = _distance;
                normal = Vector3.Backward;
                return true;
            }
        }

        private sealed class TestRootNode : RootNode
        {
            public override Undo Undo => null;
            public override ISceneEditingContext SceneContext => null;

            public override void Spawn(Actor actor, Actor parent, int orderInParent = -1)
            {
            }
        }

        private sealed class TestGizmoOwner : IGizmoOwner
        {
            public FlaxEditor.Viewport.EditorViewport Viewport => null;
            public GizmosCollection Gizmos { get; }
            public SceneRenderTask RenderTask => null;
            public bool IsLeftMouseButtonDown => false;
            public bool IsRightMouseButtonDown => false;
            public bool IsMiddleMouseButtonDown => false;
            public bool IsAltKeyDown => false;
            public bool IsControlDown => false;
            public bool IsShiftDown => false;
            public bool SnapToGround => false;
            public bool SnapToVertex => false;
            public Float3 ViewDirection => Float3.Forward;
            public Vector3 ViewPosition => Vector3.Zero;
            public Quaternion ViewOrientation => Quaternion.Identity;
            public float ViewFarPlane => 1000.0f;
            public Ray MouseRay => new Ray(Vector3.Zero, Vector3.Forward);
            public Float2 MouseDelta => Float2.Zero;
            public bool UseSnapping => false;
            public bool UseDuplicate => false;
            public Undo Undo => null;
            public RootNode SceneGraphRoot => null;

            public TestGizmoOwner()
            {
                Gizmos = new GizmosCollection(this);
            }

            public bool TryDuplicateForTransform(out List<SceneGraphNode> createdObjects, out IUndoAction undoAction)
            {
                createdObjects = null;
                undoAction = null;
                return false;
            }

            public void Select(List<SceneGraphNode> nodes, bool recordUndo = true)
            {
            }

            public void Spawn(Actor actor)
            {
            }

            public void OpenContextMenu()
            {
            }
        }
    }
}
#endif
