// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.Gizmo;
using FlaxEditor.SceneGraph;
using FlaxEngine;
using NUnit.Framework;

namespace FlaxEditor.Tests
{
    [TestFixture]
    public class TestTransformGizmoInteraction
    {
        [Test]
        public void TestProjectionSizingUsesForwardDepthAndDpiParity()
        {
            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(false, 10.0f, 60.0f, 1.0f, 1000.0f, 1.0f, 96.0f, 0.1f, out var perspectivePixelSize, out var perspectiveRadius));
            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(false, 10.0f, 60.0f, 1.0f, 1000.0f, 2.0f, 96.0f, 0.1f, out var highDpiPixelSize, out var highDpiRadius));
            Assert.AreEqual(perspectivePixelSize * 0.5f, highDpiPixelSize, 0.00001f);
            Assert.AreEqual(perspectiveRadius, highDpiRadius, 0.00001f);

            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(false, 20.0f, 60.0f, 1.0f, 1000.0f, 1.0f, 96.0f, 0.1f, out _, out var fartherRadius));
            Assert.AreEqual(perspectiveRadius * 2.0f, fartherRadius, 0.00001f);

            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(false, 0.001f, 60.0f, 1.0f, 1000.0f, 1.0f, 96.0f, 0.1f, out _, out var nearClampedRadius));
            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(false, 0.1f, 60.0f, 1.0f, 1000.0f, 1.0f, 96.0f, 0.1f, out _, out var nearPlaneRadius));
            Assert.AreEqual(nearPlaneRadius, nearClampedRadius, 0.00001f);
            Assert.IsFalse(TransformGizmoBase.TryCalculateProjectionSizing(false, -1.0f, 60.0f, 1.0f, 1000.0f, 1.0f, 96.0f, 0.1f, out _, out _));

            Assert.IsFalse(TransformGizmoBase.TryCalculateProjectionSizing(true, -100.0f, 60.0f, 0.02f, 1000.0f, 2.0f, 96.0f, 0.1f, out _, out _));
            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(true, 10.0f, 60.0f, 0.02f, 1000.0f, 2.0f, 96.0f, 0.1f, out _, out var orthographicRadius));
            Assert.IsTrue(TransformGizmoBase.TryCalculateProjectionSizing(true, 100.0f, 60.0f, 0.02f, 1000.0f, 2.0f, 96.0f, 0.1f, out _, out var orthographicFarRadius));
            Assert.AreEqual(orthographicRadius, orthographicFarRadius, 0.00001f);
        }

        [Test]
        public void TestPureTransformMathIsSymmetricAndAnchorBased()
        {
            Assert.AreEqual(2.0f, TransformGizmoBase.SolveExponentialScaleFactor(120.0f, TransformGizmoBase.ScalePixelsPerDoubling), 0.00001f);
            Assert.AreEqual(0.5f, TransformGizmoBase.SolveExponentialScaleFactor(-120.0f, TransformGizmoBase.ScalePixelsPerDoubling), 0.00001f);
            Assert.AreEqual(2.0f, TransformGizmoBase.SolvePointerScaleFactor(1200.0f), 0.00001f);
            Assert.AreEqual(0.5f, TransformGizmoBase.SolvePointerScaleFactor(-120.0f), 0.00001f);
            Assert.AreEqual(1.0f, TransformGizmoBase.SolvePointerScaleFactor(0.0f), 0.00001f);
            float halfDragFactor = TransformGizmoBase.SolveExponentialScaleFactor(60.0f, TransformGizmoBase.ScalePixelsPerDoubling);
            Assert.AreEqual(2.0f, halfDragFactor * halfDragFactor, 0.00001f);
            Assert.Greater(TransformGizmoBase.SolveExponentialScaleFactor(-100000.0f, TransformGizmoBase.ScalePixelsPerDoubling), 0.0f);

            var anchorRay = new Ray(new Vector3(0, 0, -10), Vector3.Forward);
            var currentRay = new Ray(new Vector3(2, 0, -10), Vector3.Forward);
            Assert.IsTrue(TransformGizmoBase.TrySolveAxisTranslation(anchorRay, currentRay, Vector3.Zero, Vector3.UnitX, out var axisDelta));
            Assert.AreEqual(new Vector3(2, 0, 0), axisDelta);

            currentRay = new Ray(new Vector3(2, 3, -10), Vector3.Forward);
            var plane = new Plane(Vector3.Zero, Vector3.Forward);
            Assert.IsTrue(TransformGizmoBase.TrySolvePlaneTranslation(anchorRay, currentRay, plane, out var planeDelta));
            Assert.AreEqual(new Vector3(2, 3, 0), planeDelta);

            float previous = 170.0f * Mathf.DegreesToRadians;
            float current = -170.0f * Mathf.DegreesToRadians;
            float unwrapped = TransformGizmoBase.UnwrapAngle(previous, previous, current);
            Assert.AreEqual(190.0f, unwrapped * Mathf.RadiansToDegrees, 0.0001f);

            Quaternion arcball = TransformGizmoBase.SolveArcballRotation(Vector3.UnitX, Vector3.UnitY, Quaternion.Identity);
            Vector3 rotated = Vector3.UnitX * arcball;
            Assert.AreEqual(0.0f, (float)rotated.X, 0.0001f);
            Assert.AreEqual(1.0f, (float)rotated.Y, 0.0001f);

            Quaternion oppositeArcball = TransformGizmoBase.SolveArcballRotation(Vector3.UnitX, -Vector3.UnitX, Quaternion.Identity);
            Vector3 oppositeRotated = Vector3.UnitX * oppositeArcball;
            Assert.AreEqual(-1.0f, (float)oppositeRotated.X, 0.0001f);

            Vector3 scaledPosition = TransformGizmoBase.ScalePositionAroundPivot(new Vector3(3, 2, 0), new Vector3(1, 0, 0), Quaternion.Identity, new Vector3(2, 0.5f, 1));
            Assert.AreEqual(new Vector3(5, 1, 0), scaledPosition);

            var bounds = new BoundingBox(Vector3.Zero, new Vector3(10, 20, 30));
            Assert.AreEqual(new Vector3(0, 10, 15), TransformGizmoBase.GetBoundsResizePivot(bounds, TransformGizmoBase.Axis.XPositive));
            Assert.AreEqual(new Vector3(10, 10, 15), TransformGizmoBase.GetBoundsResizePivot(bounds, TransformGizmoBase.Axis.XNegative));
            Assert.AreEqual(1.5f, TransformGizmoBase.SolveBoundsResizeFactor(1.0f, 5.0f, 10.0f, 1.0f), 0.00001f);
            Assert.AreEqual(1.5f, TransformGizmoBase.SolveBoundsResizeFactor(1.0f, -5.0f, 10.0f, -1.0f), 0.00001f);
            Assert.AreEqual(0.0001f, TransformGizmoBase.SolveBoundsResizeFactor(1.0f, -20.0f, 10.0f, 1.0f), 0.00001f);

            Quaternion rotated = Quaternion.RotationZ(90.0f * Mathf.DegreesToRadians);
            Float3 rotatedScale = TransformGizmoBase.ApplyWorldScaleDelta(new Float3(1.0f, 0.25f, 1.0f), rotated, new Vector3(10.0f, 0.0f, 0.0f));
            Assert.AreEqual(1.0f, rotatedScale.X, 0.0001f);
            Assert.AreEqual(2.75f, rotatedScale.Y, 0.0001f);
            Assert.AreEqual(1.0f, rotatedScale.Z, 0.0001f);
        }

        [Test]
        public void TestTransformSnappingUsesLinearGridUnits()
        {
            var step = new Vector3(10.0f);
            Vector3 relative = TransformGizmoBase.SnapTranslationToGrid(
                new Vector3(6, 3, 0),
                new Vector3(3, 0, 0),
                Quaternion.Identity,
                TransformGizmoBase.TransformSpace.World,
                TransformGizmoBase.Axis.X,
                step,
                false);
            Assert.AreEqual(new Vector3(10, 3, 0), relative);

            Vector3 absolute = TransformGizmoBase.SnapTranslationToGrid(
                new Vector3(6, 3, 0),
                new Vector3(3, 0, 0),
                Quaternion.Identity,
                TransformGizmoBase.TransformSpace.World,
                TransformGizmoBase.Axis.X,
                step,
                true);
            Assert.AreEqual(new Vector3(7, 3, 0), absolute);

            var bounds = new BoundingBox(new Vector3(-50), new Vector3(50));
            Vector3 uniform = TransformGizmoBase.SnapScaleFactorsToGrid(
                new Vector3(1.24f),
                bounds,
                Vector3.Zero,
                Quaternion.Identity,
                TransformGizmoBase.Axis.Center,
                new Vector3(25.0f));
            Assert.AreEqual(new Vector3(1.25f), uniform);
            Assert.AreEqual(1.25f, TransformGizmoBase.SnapScaleFactorToGrid(1.24f, 100.0f, 25.0f), 0.00001f);

            Vector3 roundedPosition = ActorTransformEditor.PositionEditor.RoundPositionToGrid(new Vector3(-5147.25534f, 50.0f, -843.796185f), 10.0f);
            Assert.AreEqual(new Vector3(-5150.0f, 50.0f, -840.0f), roundedPosition);
        }

        [Test]
        public void TestOriginPreviewScaleAndReanchorAreDeterministic()
        {
            var owner = new TestGizmoOwner();
            var node = new TestNode(Guid.NewGuid())
            {
                Transform = new Transform(new Vector3(4, 5, 6), Quaternion.Identity, new Float3(2, 3, 4))
            };
            var start = node.Transform;
            var gizmo = new TestGizmo(owner, node);

            try
            {
                gizmo.StartTransforming(false);
                Assert.AreEqual(InteractionState.Dragging, gizmo.State);

                gizmo.ApplyDelta(new Vector3(2, 0, 0), Quaternion.Identity, new Vector3(0.25f, 0.25f, 0.25f));
                Assert.AreEqual(new Vector3(6, 5, 6), node.Transform.Translation);
                Assert.AreEqual(new Float3(2.5f, 3.75f, 5.0f), node.Transform.Scale);

                gizmo.ApplyDelta(new Vector3(3, 0, 0), Quaternion.Identity, new Vector3(0.25f, 0.25f, 0.25f));
                Assert.AreEqual(new Vector3(9, 5, 6), node.Transform.Translation);
                Assert.AreEqual(new Float3(3, 4.5f, 6), node.Transform.Scale);

                var singleFrameNode = new TestNode(Guid.NewGuid())
                {
                    Transform = start
                };
                var singleFrameGizmo = new TestGizmo(owner, singleFrameNode);
                try
                {
                    singleFrameGizmo.StartTransforming(false);
                    singleFrameGizmo.ApplyDelta(new Vector3(5, 0, 0), Quaternion.Identity, new Vector3(0.5f, 0.5f, 0.5f));
                    Assert.AreEqual(node.Transform, singleFrameNode.Transform);
                }
                finally
                {
                    singleFrameGizmo.Destroy();
                    singleFrameNode.OnDispose();
                }

                var beforeReanchor = node.Transform;
                Assert.IsTrue(gizmo.ReanchorInteraction());
                Assert.AreEqual(beforeReanchor, node.Transform);

                gizmo.ApplyDelta(new Vector3(1, 0, 0), Quaternion.Identity, new Vector3(0.1f, 0.1f, 0.1f));
                Assert.AreEqual(new Vector3(10, 5, 6), node.Transform.Translation);
                Assert.AreEqual(new Float3(3.2f, 4.8f, 6.4f), node.Transform.Scale);

                Assert.IsTrue(gizmo.CancelTransforming());
                Assert.AreEqual(start, node.Transform);
                Assert.AreEqual(InteractionState.Inactive, gizmo.State);
                Assert.AreEqual(0, owner.Undo.UndoOperationsStack.HistoryCount);
            }
            finally
            {
                gizmo.Destroy();
                node.OnDispose();
            }
        }

        [Test]
        public void TestLifecycleClutchNumericEntryAndCaptureLoss()
        {
            var owner = new TestGizmoOwner();
            var node = new TestNode(Guid.NewGuid())
            {
                Transform = new Transform(new Vector3(1, 2, 3))
            };
            var gizmo = new TestGizmo(owner, node);
            var trace = new List<string>();
            gizmo.InteractionStateChanged += (previous, current) => trace.Add(previous + "->" + current);

            try
            {
                gizmo.StartTransforming(false);
                Assert.IsTrue(gizmo.BeginCameraClutch());
                Assert.AreEqual(InteractionState.Clutched, gizmo.State);
                Assert.IsTrue(gizmo.EndCameraClutch());
                Assert.AreEqual(InteractionState.Dragging, gizmo.State);

                Assert.IsTrue(gizmo.BeginNumericEntry());
                Assert.AreEqual(InteractionState.NumericEntry, gizmo.State);
                Assert.IsTrue(gizmo.EndNumericEntry(new Vector3(7, 8, 9), Quaternion.Identity, Vector3.One));
                Assert.AreEqual(InteractionState.Dragging, gizmo.State);
                Assert.AreEqual(new Vector3(8, 10, 12), node.Transform.Translation);

                owner.LeftMouseButtonDown = false;
                gizmo.OnInteractionMouseCaptureLost();
                Assert.AreEqual(InteractionState.Inactive, gizmo.State);
                Assert.AreEqual(1, owner.Undo.UndoOperationsStack.HistoryCount);
                CollectionAssert.Contains(trace, "Inactive->Hovering");
                CollectionAssert.Contains(trace, "Hovering->Armed");
                CollectionAssert.Contains(trace, "Armed->Dragging");
                CollectionAssert.Contains(trace, "Dragging->Clutched");
                CollectionAssert.Contains(trace, "Clutched->Dragging");
                CollectionAssert.Contains(trace, "Dragging->NumericEntry");
                CollectionAssert.Contains(trace, "NumericEntry->Dragging");
                CollectionAssert.Contains(trace, "Dragging->Committing");
                CollectionAssert.Contains(trace, "Committing->Inactive");
            }
            finally
            {
                gizmo.Destroy();
                node.OnDispose();
            }
        }

        [Test]
        public void TestFocusLossFreezesAndCancellationRestoresOrigin()
        {
            var owner = new TestGizmoOwner();
            var node = new TestNode(Guid.NewGuid())
            {
                Transform = new Transform(new Vector3(3, 4, 5))
            };
            var start = node.Transform;
            var gizmo = new TestGizmo(owner, node);

            try
            {
                gizmo.StartTransforming(false);
                gizmo.ApplyDelta(new Vector3(4, 0, 0), Quaternion.Identity, Vector3.Zero);
                gizmo.OnInteractionFocusLost();

                Assert.AreEqual(InteractionState.Clutched, gizmo.State);
                Assert.IsTrue(gizmo.HasActiveTransaction);
                Assert.IsTrue(gizmo.CancelTransforming());
                Assert.AreEqual(start, node.Transform);
                Assert.AreEqual(InteractionState.Inactive, gizmo.State);
                Assert.AreEqual(0, owner.Undo.UndoOperationsStack.HistoryCount);
            }
            finally
            {
                gizmo.Destroy();
                node.OnDispose();
            }
        }

        [Test]
        public void TestTransactionDuplicateRollbackDeletesCreatedObjects()
        {
            var owner = new TestGizmoOwner
            {
                UseDuplicateValue = true
            };
            var original = new TestNode(Guid.NewGuid())
            {
                Transform = new Transform(new Vector3(2, 3, 4))
            };
            var gizmo = new TestGizmo(owner, original);
            TestNode duplicate = null;

            try
            {
                gizmo.StartTransforming();
                duplicate = gizmo.CurrentNode;
                Assert.AreNotSame(original, duplicate);
                gizmo.ApplyDelta(new Vector3(1, 0, 0), Quaternion.Identity, Vector3.Zero);

                Assert.IsTrue(gizmo.CancelTransforming());
                Assert.IsFalse(duplicate.Active);
                Assert.AreEqual(InteractionState.Inactive, gizmo.State);
                Assert.AreEqual(0, owner.Undo.UndoOperationsStack.HistoryCount);
                Assert.AreEqual(new Vector3(2, 3, 4), original.Transform.Translation);
            }
            finally
            {
                gizmo.Destroy();
                duplicate?.OnDispose();
                original.OnDispose();
            }
        }

        [Test]
        public void TestNoOpCommitDoesNotAddUndo()
        {
            var owner = new TestGizmoOwner();
            var node = new TestNode(Guid.NewGuid());
            var gizmo = new TestGizmo(owner, node);

            try
            {
                gizmo.StartTransforming(false);
                gizmo.EndTransforming();
                Assert.AreEqual(InteractionState.Inactive, gizmo.State);
                Assert.AreEqual(0, owner.Undo.UndoOperationsStack.HistoryCount);
            }
            finally
            {
                gizmo.Destroy();
                node.OnDispose();
            }
        }

        [Test]
        public void TestScalePreviewDoesNotProduceSingularScale()
        {
            var owner = new TestGizmoOwner();
            var node = new TestNode(Guid.NewGuid())
            {
                Transform = new Transform(Vector3.Zero, Quaternion.Identity, new Float3(2, -3, 4))
            };
            var gizmo = new TestGizmo(owner, node);

            try
            {
                gizmo.StartTransforming(false);
                gizmo.ApplyDelta(Vector3.Zero, Quaternion.Identity, new Vector3(-1));
                Assert.AreEqual(new Float3(0.0001f, -0.0001f, 0.0001f), node.Transform.Scale);
            }
            finally
            {
                gizmo.Destroy();
                node.OnDispose();
            }
        }

        [Test]
        public void TestTransformActionReacquiresRecreatedNodeOnRedo()
        {
            var id = Guid.NewGuid();
            var original = new TestNode(id)
            {
                Transform = new Transform(new Vector3(2, 0, 0))
            };
            var before = new List<Transform> { original.Transform };
            original.Transform = new Transform(new Vector3(12, 0, 0));
            var bounds = BoundingBox.Empty;
            var action = new TransformObjectsAction(new List<SceneGraphNode> { original }, before, ref bounds, false);

            original.Active = false;
            var recreated = new TestNode(id)
            {
                Transform = new Transform(new Vector3(-4, 0, 0))
            };

            try
            {
                action.Do();
                Assert.AreEqual(new Vector3(12, 0, 0), recreated.Transform.Translation);
                action.Undo();
                Assert.AreEqual(new Vector3(2, 0, 0), recreated.Transform.Translation);
            }
            finally
            {
                action.Dispose();
                recreated.OnDispose();
                original.OnDispose();
            }
        }

        private sealed class TestGizmo : TransformGizmoBase
        {
            private TestNode _node;

            public TestGizmo(IGizmoOwner owner, TestNode node)
            : base(owner)
            {
                _node = node;
            }

            public void ApplyDelta(Vector3 translation, Quaternion rotation, Vector3 scale)
            {
                ApplyInteractionDelta(ref translation, ref rotation, ref scale);
            }

            public TestNode CurrentNode => _node;

            protected override int SelectionCount => 1;

            protected override SceneGraphNode GetSelectedObject(int index)
            {
                return _node;
            }

            protected override Transform GetSelectedTransform(int index)
            {
                return _node.Transform;
            }

            protected override void GetSelectedObjectsBounds(out BoundingBox bounds, out bool navigationDirty)
            {
                bounds = BoundingBox.Empty;
                navigationDirty = false;
            }

            protected override bool IsSelected(SceneGraphNode obj)
            {
                return obj == _node;
            }

            protected override bool UsesOriginAuthoritativePreview => true;

            protected override void OnDuplicate()
            {
                var duplicate = new TestNode(Guid.NewGuid())
                {
                    Transform = _node.Transform
                };
                _node = duplicate;
                RegisterDuplicatedObjects(new[] { duplicate }, new TestDuplicateUndoAction(duplicate));
            }

            protected override void OnApplyTransformation(ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta)
            {
                var transform = _node.Transform;
                transform.Translation += translationDelta;
                transform.Scale = ApplyScaleDelta(transform.Scale, scaleDelta);
                _node.Transform = transform;
            }

            protected override void OnEndTransforming()
            {
                base.OnEndTransforming();
                Owner.Undo.AddAction(new TestUndoAction());
            }
        }

        private sealed class TestUndoAction : IUndoAction
        {
            public string ActionString => "Transform gizmo test";

            public void Do()
            {
            }

            public void Undo()
            {
            }

            public void Dispose()
            {
            }
        }

        private sealed class TestDuplicateUndoAction : IUndoAction
        {
            private readonly TestNode _node;

            public TestDuplicateUndoAction(TestNode node)
            {
                _node = node;
            }

            public string ActionString => "Duplicate test node";

            public void Do()
            {
                _node.Active = true;
            }

            public void Undo()
            {
                _node.Active = false;
            }

            public void Dispose()
            {
            }
        }

        private sealed class TestNode : SceneGraphNode
        {
            public TestNode(Guid id)
            : base(id)
            {
            }

            public bool Active = true;

            public override string Name => "Transform gizmo test node";
            public override SceneNode ParentScene => null;
            public override Transform Transform { get; set; }
            public override bool IsActive => Active;
            public override bool IsActiveInHierarchy => Active;
            public override int OrderInParent { get; set; }
        }

        private sealed class TestGizmoOwner : IGizmoOwner
        {
            public TestGizmoOwner()
            {
                Undo = new Undo();
                Gizmos = new GizmosCollection(this);
            }

            public GizmosCollection Gizmos { get; }
            public Undo Undo { get; }
            public bool LeftMouseButtonDown { get; set; } = true;
            public bool UseDuplicateValue { get; set; }

            public FlaxEditor.Viewport.EditorViewport Viewport => null;
            public SceneRenderTask RenderTask => null;
            public bool IsLeftMouseButtonDown => LeftMouseButtonDown;
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
            public float ViewFarPlane => 10000.0f;
            public Ray MouseRay => new Ray(Vector3.Zero, Vector3.Forward);
            public Float2 MouseDelta => Float2.Zero;
            public bool UseSnapping => false;
            public bool UseDuplicate => UseDuplicateValue;
            public SceneGraph.RootNode SceneGraphRoot => null;

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
