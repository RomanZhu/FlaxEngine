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
using FlaxEditor.Gizmo.Snapping;
using FlaxEditor.Options;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG;
using FlaxEditor.Tools.CSG.HitTesting;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEditor.Tools.CSG.Selection;
using FlaxEditor.Tools.CSG.Transactions;
using FlaxEditor.Tools.CSG.WorkingPlane;
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
        public void TestDefaultToolBindingsUseNumberRow()
        {
            var input = new InputOptions();
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha1), input.CSGSelectPlaceTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha2), input.CSGDrawTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha3), input.CSGEditTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha4), input.CSGSurfaceTool);
        }

        [Test]
        public void TestControllerPublishesTransactionLifecycle()
        {
            var controller = new CSGToolController();
            int started = 0;
            int committed = 0;
            EditorGizmoModeCancelReason cancelled = default;
            controller.InteractionStarted += () => started++;
            controller.InteractionCommitted += () => committed++;
            controller.InteractionCancelled += reason => cancelled = reason;

            controller.BeginInteraction();
            Assert.AreEqual(1, started);
            Assert.IsTrue(controller.TryCommit());
            Assert.AreEqual(1, committed);
            controller.BeginInteraction();
            Assert.IsTrue(controller.TryCancel(EditorGizmoModeCancelReason.FocusLost));
            Assert.AreEqual(EditorGizmoModeCancelReason.FocusLost, cancelled);
        }

        [Test]
        public void TestRebuildQueueCoalescesPreviewAndRejectsStaleCompletion()
        {
            var queue = new CSGRebuildQueue { PreviewIntervalSeconds = 0.1 };
            var sceneId = Guid.NewGuid();

            long first = queue.Request(sceneId, CSGRebuildRequestKind.Preview, true, 50.0f, 0.0, out var dispatch);
            Assert.AreEqual(first, dispatch.Revision);
            long second = queue.Request(sceneId, CSGRebuildRequestKind.Preview, true, 50.0f, 0.01, out dispatch);
            Assert.AreEqual(0, dispatch.Revision);
            long third = queue.Request(sceneId, CSGRebuildRequestKind.Preview, true, 50.0f, 0.02, out dispatch);
            Assert.AreEqual(0, dispatch.Revision);
            Assert.IsFalse(queue.TryDequeue(sceneId, true, 0.09, out dispatch));
            Assert.IsTrue(queue.TryDequeue(sceneId, true, 0.1, out dispatch));
            Assert.AreEqual(third, dispatch.Revision);
            Assert.IsFalse(queue.TryAcknowledge(sceneId, first));
            Assert.IsTrue(queue.TryAcknowledge(sceneId, third));

            var status = queue.GetStatus(sceneId);
            Assert.AreEqual(3, status.RequestCount);
            Assert.AreEqual(2, status.DispatchCount);
            Assert.AreEqual(CSGRebuildVisualState.UpToDate, status.State);
            long final = queue.Request(sceneId, CSGRebuildRequestKind.Final, true, 50.0f, 0.11, out dispatch);
            Assert.AreEqual(final, dispatch.Revision);
            Assert.AreEqual(0.0f, dispatch.TimeoutMs);
            long stale = queue.Request(sceneId, CSGRebuildRequestKind.Preview, false, 50.0f, 0.12, out dispatch);
            Assert.Greater(stale, final);
            Assert.AreEqual(0, dispatch.Revision);
            Assert.AreEqual(CSGRebuildVisualState.Stale, queue.GetStatus(sceneId).State);
        }

        [Test]
        public void TestTransactionOwnsPerformedActionsUntilCommitOrRollback()
        {
            var undo = new Undo();
            var committedAction = new TestUndoAction();
            using (var transaction = new CSGTransaction())
            {
                transaction.Begin();
                transaction.RegisterPerformedAction(committedAction);
                transaction.RecordPreview(0.25, 0);
                Assert.IsTrue(transaction.Commit(undo));
                Assert.AreEqual(1, undo.UndoOperationsStack.HistoryCount);
                Assert.AreEqual(CSGTransactionState.Committed, transaction.Telemetry.State);
            }
            undo.PerformUndo();
            Assert.AreEqual(1, committedAction.UndoCount);
            undo.PerformRedo();
            Assert.AreEqual(1, committedAction.DoCount);

            var rolledBackAction = new TestUndoAction();
            using (var transaction = new CSGTransaction())
            {
                transaction.Begin();
                transaction.RegisterPerformedAction(rolledBackAction);
                Assert.IsTrue(transaction.Invalidate("FocusLost"));
                Assert.AreEqual(CSGTransactionState.RolledBack, transaction.Telemetry.State);
                Assert.AreEqual("FocusLost", transaction.Telemetry.InvalidationReason);
            }
            Assert.AreEqual(1, rolledBackAction.UndoCount);
            Assert.AreEqual(1, rolledBackAction.DisposeCount);
            undo.Dispose();
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
            Assert.IsTrue(Vector3.NearEqual(new Vector3(0.0f, 0.0f, 2.0f), hits[0].Point));
            root.Dispose();
        }

        [Test]
        public void TestWorkingPlaneBasisIsStableAcrossCoplanarHits()
        {
            var normal = new Vector3(0.25f, 0.8f, 0.45f);
            normal.Normalize();
            var preferredTangent = Vector3.Right;
            var pointA = normal * 125.0f;
            var alongPlane = Vector3.Cross(normal, Vector3.Right);
            alongPlane.Normalize();
            var pointB = pointA + alongPlane * 73.0f;

            Assert.IsTrue(CSGWorkingPlaneService.TryDerive(pointA, normal, preferredTangent, 10.0f, 10, Guid.Empty, -1, out var planeA));
            Assert.IsTrue(CSGWorkingPlaneService.TryDerive(pointB, normal, preferredTangent, 10.0f, 10, Guid.Empty, -1, out var planeB));
            Assert.IsTrue(Vector3.NearEqual(planeA.Origin, planeB.Origin));
            Assert.IsTrue(Vector3.NearEqual(planeA.Tangent, planeB.Tangent));
            Assert.IsTrue(Vector3.NearEqual(planeA.Bitangent, planeB.Bitangent));
            Assert.AreEqual(0.0f, (float)Vector3.Dot(planeA.Normal, planeA.Tangent), 0.0001f);
            Assert.AreEqual(0.0f, (float)Vector3.Dot(planeA.Normal, planeA.Bitangent), 0.0001f);
        }

        [Test]
        public void TestWorkingPlaneRejectsGrazingHoverAndFreezesTransactionPlane()
        {
            var service = new CSGWorkingPlaneService();
            var grazingRay = new Ray(new Vector3(0.0f, 10.0f, 0.0f), Vector3.Right);
            Assert.IsFalse(service.TrySetHover(Vector3.Zero, Vector3.Up, Vector3.Right, grazingRay, 10.0f, Guid.Empty, -1));

            var ray = new Ray(new Vector3(0.0f, 10.0f, 0.0f), Vector3.Down);
            Assert.IsTrue(service.TrySetHover(Vector3.Zero, Vector3.Up, Vector3.Right, ray, 10.0f, Guid.NewGuid(), 2));
            service.Freeze();
            var frozen = service.ActivePlane;
            service.ClearHover();
            service.SetSpacing(25.0f);
            Assert.IsTrue(service.IsFrozen);
            Assert.IsTrue(Vector3.NearEqual(frozen.Origin, service.ActivePlane.Origin));
            Assert.AreEqual(25.0f, service.ActivePlane.Spacing);
            service.Unfreeze();
        }

        [Test]
        public void TestPlaneGridSnapHandlesNegativeCoordinatesExactly()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var point = plane.ToWorld(new Float2(-14.9f, 26.1f));
            var snapped = ViewportSnapService.SnapToGrid(ref plane, point, out var coordinates);

            Assert.AreEqual(-10.0f, coordinates.X, 0.0001f);
            Assert.AreEqual(30.0f, coordinates.Y, 0.0001f);
            Assert.IsTrue(Vector3.NearEqual(plane.ToWorld(coordinates), snapped));
        }

        [Test]
        public void TestGeometrySnapUsesScreenThresholdAndStableFeaturePriority()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var candidates = new List<ViewportSnapCandidate>
            {
                new ViewportSnapCandidate { Point = Vector3.Right * 20.0f, Kind = ViewportSnapTargetKind.CSGFace, ActorId = Guid.NewGuid(), ComponentIndex = 1, ScreenDistance = 4.0f },
                new ViewportSnapCandidate { Point = Vector3.Right * 10.0f, Kind = ViewportSnapTargetKind.CSGVertex, ActorId = Guid.NewGuid(), ComponentIndex = 3, ScreenDistance = 4.0f },
            };
            var solver = new ViewportSnapService();

            solver.Solve(ref plane, Vector3.Zero, true, 3.0f, candidates, out var gridResult);
            Assert.AreEqual(ViewportSnapTargetKind.Grid, gridResult.Kind);
            solver.Solve(ref plane, Vector3.Zero, true, 5.0f, candidates, out var geometryResult);
            Assert.AreEqual(ViewportSnapTargetKind.CSGVertex, geometryResult.Kind);
            Assert.IsTrue(Vector3.NearEqual(Vector3.Right * 10.0f, geometryResult.Point));
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

        private sealed class TestUndoAction : IUndoAction
        {
            public int DoCount;
            public int UndoCount;
            public int DisposeCount;

            public string ActionString => "Test CSG action";

            public void Do()
            {
                DoCount++;
            }

            public void Undo()
            {
                UndoCount++;
            }

            public void Dispose()
            {
                DisposeCount++;
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
