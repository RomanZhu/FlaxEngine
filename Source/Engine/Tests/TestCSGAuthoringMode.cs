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
using FlaxEditor.Tools.CSG.Placement;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEditor.Tools.CSG.Selection;
using FlaxEditor.Tools.CSG.Snapping;
using FlaxEditor.Tools.CSG.Tools;
using FlaxEditor.Tools.CSG.Transactions;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEditor.Viewport.Modes;
using FlaxEditor.Viewport.Overlays;
using FlaxEngine;
using FlaxEngine.GUI;
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
            Assert.AreEqual(CSGTool.Draw, source.Tool);
            Assert.IsTrue(source.SetTool(CSGTool.Draw));
            Assert.IsTrue(source.SetOperation(CSGOperation.Subtractive));
            source.SetWorkingPlaneLocked(true);
            source.SetSnappingEnabled(false);
            source.SetBrushAlignmentSnappingEnabled(true);
            source.SetSnapIncrement(25.0f);
            source.SetVisibility(CSGVisibility.SourceBrushes | CSGVisibility.HiddenBrushes);
            source.SetRayPlacementAlignment(CSGRayPlacementAlignment.AlignSurfaceUp);
            source.SetRayPlacementFront(CSGRayPlacementFront.Bottom);
            source.SetBrushMaterialAutoPick(true);

            var restored = new CSGToolController();
            restored.ApplyState(source.CaptureState());
            Assert.AreEqual(CSGTool.Draw, restored.Tool);
            Assert.AreEqual(CSGOperation.Subtractive, restored.Operation);
            Assert.IsTrue(restored.WorkingPlaneLocked);
            Assert.IsFalse(restored.SnappingEnabled);
            Assert.IsTrue(restored.BrushAlignmentSnappingEnabled);
            Assert.AreEqual(25.0f, restored.SnapIncrement);
            Assert.AreEqual(CSGVisibility.SourceBrushes | CSGVisibility.HiddenBrushes, restored.Visibility);
            Assert.AreEqual(CSGRayPlacementAlignment.AlignSurfaceUp, restored.RayPlacementAlignment);
            Assert.AreEqual(CSGRayPlacementFront.Bottom, restored.RayPlacementFront);
            Assert.IsTrue(restored.BrushMaterialAutoPick);

            var unavailable = source.CaptureState();
            unavailable.Tool = (CSGTool)999;
            unavailable.Operation = CSGOperation.Intersecting;
            unavailable.SnapIncrement = 0.0f;
            restored.ApplyState(unavailable);
            Assert.AreEqual(CSGTool.Draw, restored.Tool);
            Assert.AreEqual(CSGOperation.Additive, restored.Operation);
            Assert.Greater(restored.SnapIncrement, 0.0f);
            Assert.IsTrue(restored.SetTool(CSGTool.Brush));
            Assert.IsFalse(restored.SetTool((CSGTool)999));
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
            Assert.IsTrue(controller.SetTool(CSGTool.Surface));
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

            input.CSGSnapOverride = new InputBinding(KeyboardKeys.Control);
            conflicts = CSGAuthoringGizmoMode.FindInputConflicts(input);
            Assert.AreEqual(1, conflicts.Count);
            StringAssert.Contains("temporary Draw", conflicts[0]);
            StringAssert.Contains("move snap override", conflicts[0]);
        }

        [Test]
        public void TestDefaultToolBindingsUseNumberRow()
        {
            var input = new InputOptions();
            Assert.AreEqual(new InputBinding(KeyboardKeys.None), input.CSGSelectPlaceTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha1), input.CSGDrawTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.None), input.CSGEditTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha2), input.CSGSurfaceTool);
            Assert.AreEqual(new InputBinding(KeyboardKeys.Alpha3), input.CSGBrushTool);
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
            Assert.AreEqual(0.0f, dispatch.TimeoutMs);
            long second = queue.Request(sceneId, CSGRebuildRequestKind.Preview, true, 50.0f, 0.01, out dispatch);
            Assert.AreEqual(0, dispatch.Revision);
            long third = queue.Request(sceneId, CSGRebuildRequestKind.Preview, true, 50.0f, 0.02, out dispatch);
            Assert.AreEqual(0, dispatch.Revision);
            Assert.IsFalse(queue.TryDequeue(sceneId, true, 0.09, out dispatch));
            Assert.IsTrue(queue.TryDequeue(sceneId, true, 0.1, out dispatch));
            Assert.AreEqual(third, dispatch.Revision);
            Assert.AreEqual(0.0f, dispatch.TimeoutMs);
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
        public void TestRebuildQueueCanDeferFinalPublishUntilNavigationReleases()
        {
            var queue = new CSGRebuildQueue();
            var sceneId = Guid.NewGuid();

            long revision = queue.Request(sceneId, CSGRebuildRequestKind.Final, true, 50.0f, 1.0, out var dispatch, true);
            Assert.AreEqual(0, dispatch.Revision);
            Assert.AreEqual(CSGRebuildVisualState.Pending, queue.GetStatus(sceneId).State);
            Assert.IsTrue(queue.TryDequeue(sceneId, true, 1.0, out dispatch));
            Assert.AreEqual(revision, dispatch.Revision);
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
        public void TestSelectToolUsesDpiScaledThresholdAndConsumesDuplicateOnce()
        {
            Assert.IsFalse(CSGSelectTool.HasExceededDragThreshold(Float2.Zero, new Float2(7.9f, 0.0f), 2.0f));
            Assert.IsTrue(CSGSelectTool.HasExceededDragThreshold(Float2.Zero, new Float2(8.0f, 0.0f), 2.0f));

            var plane = CSGWorkingPlane.World(10.0f);
            var brush = new BoxBrush();
            try
            {
                var tool = new CSGSelectTool();
                Assert.IsTrue(tool.Arm(ref plane, Float2.Zero, Vector3.Zero, new[] { brush }));
                Assert.IsFalse(tool.TryBeginDrag(new Float2(3.9f, 0.0f), 1.0f));
                Assert.IsTrue(tool.TryBeginDrag(new Float2(4.0f, 0.0f), 1.0f));
                Assert.IsTrue(tool.TryConsumeDuplicate(true));
                Assert.IsFalse(tool.TryConsumeDuplicate(true));
            }
            finally
            {
                FlaxEngine.Object.Destroy(brush);
            }
        }

        [Test]
        public void TestBoxBrushCanFlipGeneratedNormals()
        {
            var brush = new BoxBrush();
            try
            {
                Assert.IsFalse(brush.FlipNormals);
                brush.FlipNormals = true;
                Assert.IsTrue(brush.FlipNormals);
                Assert.AreEqual(BrushMode.Additive, brush.Mode, "Flipping normals must not change the CSG operation.");
            }
            finally
            {
                FlaxEngine.Object.Destroy(brush);
            }
        }

        [Test]
        public void TestSelectToolAppliesRigidPlaneDeltaAndPreservesOrientation()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var firstRotation = Quaternion.Euler(10.0f, 20.0f, 30.0f);
            var secondRotation = Quaternion.Euler(-15.0f, 35.0f, 5.0f);
            var first = new Transform(new Vector3(10.0f, 20.0f, 30.0f), firstRotation, new Float3(1.0f, 2.0f, 3.0f));
            var second = new Transform(new Vector3(-40.0f, 50.0f, 60.0f), secondRotation, new Float3(2.0f, 1.0f, 0.5f));
            var delta = plane.Tangent * 25.0f + plane.Bitangent * -15.0f;

            var movedFirst = CSGSelectTool.ApplyTranslation(first, delta);
            var movedSecond = CSGSelectTool.ApplyTranslation(second, delta);

            Assert.IsTrue(Vector3.NearEqual(first.Translation + delta, movedFirst.Translation));
            Assert.IsTrue(Vector3.NearEqual(second.Translation + delta, movedSecond.Translation));
            Assert.AreEqual(first.Orientation, movedFirst.Orientation);
            Assert.AreEqual(second.Orientation, movedSecond.Orientation);
            Assert.AreEqual(first.Scale, movedFirst.Scale);
            Assert.AreEqual(second.Scale, movedSecond.Scale);
            Assert.IsTrue(Vector3.NearEqual(second.Translation - first.Translation, movedSecond.Translation - movedFirst.Translation));
        }

        [Test]
        public void TestSelectToolExplicitNormalAlignmentIsOptIn()
        {
            var initial = new Transform(Vector3.Zero, Quaternion.Euler(15.0f, 25.0f, 35.0f), new Float3(2.0f, 3.0f, 4.0f));
            var normal = new Vector3(0.25f, 0.9f, -0.35f);
            normal.Normalize();

            var preserved = CSGSelectTool.ApplyTranslation(initial, Vector3.Zero);
            var aligned = CSGSelectTool.AlignToNormal(initial, normal);
            var pivot = new Vector3(10.0f, -20.0f, 30.0f);
            var groupRotation = Quaternion.FindBetween(Vector3.Up, normal);
            var rigid = CSGSelectTool.ApplyRigidTransform(initial, pivot, new Vector3(5.0f, 6.0f, 7.0f), groupRotation);

            Assert.AreEqual(initial.Orientation, preserved.Orientation);
            Assert.AreEqual(initial.Scale, aligned.Scale);
            var alignedUp = Vector3.Transform(Vector3.Up, aligned.Orientation);
            alignedUp.Normalize();
            Assert.IsTrue(Vector3.NearEqual(normal, alignedUp));
            Assert.IsTrue(Vector3.NearEqual(
                pivot + Vector3.Transform(initial.Translation - pivot, groupRotation) + new Vector3(5.0f, 6.0f, 7.0f),
                rigid.Translation));
            Assert.AreEqual(initial.Scale, rigid.Scale);
        }

        [Test]
        public void TestSelectToolSurfacePlacementAndPlanarRebaseDoNotJump()
        {
            var plane = CSGSelectTool.CreateHorizontalDragPlane(10.0f, new Vector3(999.0f, 25.0f, -999.0f));
            Assert.AreEqual(Vector3.Up, plane.Normal);
            Assert.AreEqual(Vector3.Right, plane.Tangent);
            Assert.AreEqual(25.0f, (float)plane.Origin.Y, 0.0001f);
            var brush = new BoxBrush
            {
                Transform = new Transform(new Vector3(10.0f, 25.0f, 30.0f)),
                Size = new Vector3(20.0f),
            };
            try
            {
                var tool = new CSGSelectTool();
                Assert.IsTrue(tool.Arm(ref plane, Float2.Zero, brush.Transform.Translation, new[] { brush }));
                Assert.IsTrue(tool.TryBeginDrag(new Float2(CSGSelectTool.DragThreshold, 0.0f), 1.0f));

                var surfaceTarget = new Vector3(50.0f, 80.0f, -20.0f);
                Assert.IsTrue(tool.ApplySurfaceTarget(surfaceTarget, Vector3.Up, false));
                Assert.IsTrue(Vector3.NearEqual(surfaceTarget + Vector3.Up * 10.0f, brush.Transform.Translation));

                var planarPointer = new Vector3(50.0f, 25.0f, -20.0f);
                Assert.IsTrue(tool.Rebase(ref plane, planarPointer));
                Assert.IsTrue(Vector3.NearEqual(surfaceTarget + Vector3.Up * 10.0f, brush.Transform.Translation));
                Assert.IsTrue(tool.ApplyTarget(new Vector3(65.0f, 25.0f, -5.0f)));
                Assert.IsTrue(Vector3.NearEqual(new Vector3(65.0f, 90.0f, -5.0f), brush.Transform.Translation));
            }
            finally
            {
                FlaxEngine.Object.Destroy(brush);
            }
        }

        [Test]
        public void TestSelectToolSnapsGeometryDeltaInsteadOfGrabPoint()
        {
            var plane = CSGSelectTool.CreateHorizontalDragPlane(10.0f, new Vector3(37.0f, 20.0f, 43.0f));
            var gridAlignedCorners = new[]
            {
                new Vector3(0.0f, 20.0f, 0.0f),
                new Vector3(100.0f, 20.0f, 0.0f),
                new Vector3(0.0f, 20.0f, 100.0f),
                new Vector3(100.0f, 20.0f, 100.0f),
            };

            var snapped = CSGSelectTool.SnapDeltaToGrid(new Vector3(13.0f, 0.0f, 17.0f), gridAlignedCorners, 10.0f, ref plane);
            Assert.IsTrue(Vector3.NearEqual(new Vector3(10.0f, 0.0f, 20.0f), snapped));
            for (int i = 0; i < gridAlignedCorners.Length; i++)
            {
                var moved = gridAlignedCorners[i] + snapped;
                Assert.AreEqual(0.0f, (float)moved.X % 10.0f, 0.0001f);
                Assert.AreEqual(0.0f, (float)moved.Z % 10.0f, 0.0001f);
            }

            // A tiny drag stays put instead of snapping the arbitrary clicked point to a grid line.
            snapped = CSGSelectTool.SnapDeltaToGrid(new Vector3(2.0f, 0.0f, 3.0f), gridAlignedCorners, 10.0f, ref plane);
            Assert.IsTrue(Vector3.NearEqual(Vector3.Zero, snapped));
        }

        [Test]
        public void TestSurfacePlacementSeatsAndSnapsBrushBoundsInsteadOfPivot()
        {
            var plane = CSGSelectTool.CreateHorizontalDragPlane(10.0f, new Vector3(5.0f, 50.0f, 5.0f));
            var brush = new BoxBrush
            {
                Transform = new Transform(new Vector3(5.0f, 50.0f, 5.0f)),
                Size = new Vector3(40.0f, 20.0f, 60.0f),
            };
            try
            {
                var tool = new CSGSelectTool();
                Assert.IsTrue(tool.Arm(ref plane, Float2.Zero, brush.Transform.Translation, new[] { brush }));
                Assert.IsTrue(tool.TryBeginDrag(new Float2(CSGSelectTool.DragThreshold, 0.0f), 1.0f));
                Assert.IsTrue(tool.ApplySurfaceTarget(new Vector3(23.0f, 0.0f, 27.0f), Vector3.Up,
                                                     CSGRayPlacementAlignment.KeepRotation, CSGRayPlacementFront.Top,
                                                     true, 10.0f, Vector3.Up, Vector3.Forward));

                var corners = new Vector3[8];
                brush.OrientedBox.GetCorners(corners);
                float minimumY = float.MaxValue;
                for (int i = 0; i < corners.Length; i++)
                {
                    minimumY = Mathf.Min(minimumY, (float)corners[i].Y);
                    Assert.AreEqual(0.0f, (float)corners[i].X % 10.0f, 0.0001f);
                    Assert.AreEqual(0.0f, (float)corners[i].Z % 10.0f, 0.0001f);
                }
                Assert.AreEqual(0.0f, minimumY, 0.0001f);
            }
            finally
            {
                FlaxEngine.Object.Destroy(brush);
            }
        }

        [Test]
        public void TestSurfacePlacementFrontMapsChosenLocalSideToSurfaceNormal()
        {
            var normal = new Vector3(1.0f, 1.0f, 0.0f);
            normal.Normalize();
            var orientation = CSGSelectTool.CalculateSurfaceOrientation(normal, CSGRayPlacementAlignment.AlignToSurface,
                                                                        CSGRayPlacementFront.Top, Vector3.Up, Vector3.Forward);
            var placedTop = Vector3.Transform(Vector3.Up, orientation);
            placedTop.Normalize();
            Assert.IsTrue(Vector3.NearEqual(normal, placedTop));

            orientation = CSGSelectTool.CalculateSurfaceOrientation(Vector3.Right, CSGRayPlacementAlignment.AlignSurfaceUp,
                                                                    CSGRayPlacementFront.Front, Vector3.Up, Vector3.Forward);
            var placedFront = Vector3.Transform(Vector3.Forward, orientation);
            placedFront.Normalize();
            Assert.IsTrue(Vector3.NearEqual(Vector3.Right, placedFront));
            var placedUp = Vector3.Transform(Vector3.Up, orientation);
            placedUp.Normalize();
            Assert.IsTrue(Vector3.NearEqual(Vector3.Up, placedUp));
        }

        [Test]
        public void TestBoxFaceEditKeepsOppositeFaceFixed()
        {
            var center = new Vector3(10.0f, 20.0f, 30.0f);
            var size = new Vector3(100.0f, 200.0f, 300.0f);
            Assert.IsTrue(CSGBoxFaceEditTool.TrySolve(center, size, 0, 50.0f, 0.001f, out var positiveCenter, out var positiveSize));
            Assert.AreEqual(150.0f, (float)positiveSize.X, 0.0001f);
            Assert.AreEqual(35.0f, (float)positiveCenter.X, 0.0001f);
            Assert.AreEqual(-40.0f, (float)(positiveCenter.X - positiveSize.X * 0.5f), 0.0001f);

            Assert.IsTrue(CSGBoxFaceEditTool.TrySolve(center, size, 1, 50.0f, 0.001f, out var negativeCenter, out var negativeSize));
            Assert.AreEqual(150.0f, (float)negativeSize.X, 0.0001f);
            Assert.AreEqual(-15.0f, (float)negativeCenter.X, 0.0001f);
            Assert.AreEqual(60.0f, (float)(negativeCenter.X + negativeSize.X * 0.5f), 0.0001f);
        }

        [Test]
        public void TestBoxFaceEditCanAlignToAnotherWorldSpaceFacePlane()
        {
            var brush = new BoxBrush
            {
                Center = Vector3.Zero,
                Size = new Vector3(100.0f),
                Transform = new Transform(new Vector3(250.0f, 75.0f, -120.0f), Quaternion.Euler(15.0f, 30.0f, 5.0f), new Float3(2.0f, 2.0f, 2.0f)),
            };
            try
            {
                var tool = new CSGBoxFaceEditTool();
                Assert.IsTrue(tool.Begin(brush, 2, new Ray(Vector3.Zero, Vector3.Forward)));
                var target = tool.FaceCenter + tool.AxisWorld * 50.0f;
                Assert.IsTrue(tool.SnapFaceTo(target));
                Assert.AreEqual(125.0f, (float)brush.Size.Y, 0.0001f);
                Assert.AreEqual(12.5f, (float)brush.Center.Y, 0.0001f);
                Assert.IsTrue(Vector3.NearEqual(target, tool.FaceCenter));
            }
            finally
            {
                FlaxEngine.Object.Destroy(brush);
            }
        }

        [Test]
        public void TestFaceAlignmentThresholdDoesNotDependOnGridIncrement()
        {
            float fallback = CSGAuthoringGizmo.GetFaceAlignmentFallbackThreshold(1000.0f);
            float threshold = CSGAuthoringGizmo.GetFaceAlignmentThreshold(10.0f, 0.5f, fallback);
            Assert.AreEqual(120.0f, fallback, 0.0001f);
            Assert.AreEqual(20.0f, threshold, 0.0001f);
        }

        [Test]
        public void TestBoxCornerEditKeepsOppositeCornerFixed()
        {
            var center = new Vector3(10.0f, 20.0f, 30.0f);
            var size = new Vector3(100.0f, 200.0f, 300.0f);
            var opposite = center - size * 0.5f;
            Assert.IsTrue(CSGBoxFaceEditTool.TrySolveCorner(center, size, 0, new Vector3(50.0f, 25.0f, 75.0f), 0.001f, out var newCenter, out var newSize));
            Assert.AreEqual(new Vector3(150.0f, 225.0f, 375.0f), newSize);
            Assert.IsTrue(Vector3.NearEqual(opposite, newCenter - newSize * 0.5f));
        }

        [Test]
        public void TestBoxEdgeEditOffsetsTwoPlanesAndKeepsOppositeExtentsFixed()
        {
            var center = new Vector3(10.0f, 20.0f, 30.0f);
            var size = new Vector3(100.0f, 200.0f, 300.0f);
            CSGBoxFaceEditTool.GetEdgeSigns(0, out var signs, out int edgeAxis);
            Assert.AreEqual(2, edgeAxis);
            Assert.AreEqual(new Vector3(1.0f, 1.0f, 0.0f), signs);

            var oppositeX = center.X - size.X * 0.5f;
            var oppositeY = center.Y - size.Y * 0.5f;
            Assert.IsTrue(CSGBoxFaceEditTool.TrySolveEdge(center, size, 0, new Vector3(40.0f, -25.0f, 999.0f), 0.001f, out var newCenter, out var newSize));
            Assert.AreEqual(140.0f, (float)newSize.X, 0.0001f);
            Assert.AreEqual(175.0f, (float)newSize.Y, 0.0001f);
            Assert.AreEqual(300.0f, (float)newSize.Z, 0.0001f);
            Assert.AreEqual((float)oppositeX, (float)(newCenter.X - newSize.X * 0.5f), 0.0001f);
            Assert.AreEqual((float)oppositeY, (float)(newCenter.Y - newSize.Y * 0.5f), 0.0001f);
        }

        [Test]
        public void TestBoxComponentNodesOffsetGeometryWithoutChangingActorScale()
        {
            var actor = new BoxBrush
            {
                Center = new Vector3(10.0f, 20.0f, 30.0f),
                Size = new Vector3(100.0f, 200.0f, 300.0f),
                Transform = new Transform(new Vector3(1000.0f, 2000.0f, 3000.0f), Quaternion.Euler(10.0f, 25.0f, 5.0f), new Float3(2.0f, 3.0f, 4.0f)),
            };
            var brush = new BoxBrushNode(actor);
            try
            {
                var initialScale = actor.Transform.Scale;
                var positiveFace = (BoxBrushNode.SideLinkNode)brush.ChildNodes[0];
                var faceTarget = positiveFace.Transform;
                faceTarget.Translation += actor.Transform.LocalToWorldVector(Vector3.Right * 50.0f);
                positiveFace.Transform = faceTarget;
                Assert.AreEqual(initialScale, actor.Transform.Scale);
                Assert.AreEqual(150.0f, (float)actor.Size.X, 0.0001f);
                Assert.AreEqual(-40.0f, (float)(actor.Center.X - actor.Size.X * 0.5f), 0.0001f);

                var edge = (BoxBrushNode.EdgeLinkNode)brush.ChildNodes[6];
                var edgeTarget = edge.Transform;
                edgeTarget.Translation += actor.Transform.LocalToWorldVector(new Vector3(25.0f, 40.0f, 90.0f));
                edge.Transform = edgeTarget;
                Assert.AreEqual(initialScale, actor.Transform.Scale);
                Assert.AreEqual(175.0f, (float)actor.Size.X, 0.0001f);
                Assert.AreEqual(240.0f, (float)actor.Size.Y, 0.0001f);
                Assert.AreEqual(300.0f, (float)actor.Size.Z, 0.0001f);

                var vertex = (BoxBrushNode.VertexLinkNode)brush.ChildNodes[18];
                var vertexTarget = vertex.Transform;
                vertexTarget.Translation += actor.Transform.LocalToWorldVector(new Vector3(10.0f, 20.0f, 30.0f));
                vertex.Transform = vertexTarget;
                Assert.AreEqual(initialScale, actor.Transform.Scale);
                Assert.AreEqual(185.0f, (float)actor.Size.X, 0.0001f);
                Assert.AreEqual(260.0f, (float)actor.Size.Y, 0.0001f);
                Assert.AreEqual(330.0f, (float)actor.Size.Z, 0.0001f);
            }
            finally
            {
                brush.Dispose();
                FlaxEngine.Object.Destroy(actor);
            }
        }

        [Test]
        public void TestCSGSnapProviderExcludesMovingBrushAndItsFaceSelection()
        {
            var actor = new BoxBrush();
            var brush = new BoxBrushNode(actor);
            try
            {
                var face = brush.ChildNodes[0];
                Assert.IsFalse(CSGSnapProviders.IsExcluded(brush, null));
                Assert.IsTrue(CSGSnapProviders.IsExcluded(brush, new SceneGraphNode[] { brush }));
                Assert.IsTrue(CSGSnapProviders.IsExcluded(brush, new[] { face }));
            }
            finally
            {
                brush.Dispose();
                FlaxEngine.Object.Destroy(actor);
            }
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
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Edit, ref brush));
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Edit, ref face));
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Surface, ref face));
            Assert.IsTrue(CSGHitTestService.IsSelectable(CSGTool.Brush, ref face));
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
        public void TestCSGSurfaceWorkingPlaneCanUseStableBrushLocalGridOrigin()
        {
            var hitPoint = new Vector3(17.0f, 23.0f, 41.0f);
            var gridOrigin = new Vector3(5.0f, 20.0f, 5.0f);
            var ray = new Ray(hitPoint + Vector3.Up * 100.0f, Vector3.Down);
            var service = new CSGWorkingPlaneService();
            Assert.IsTrue(service.TrySetHover(hitPoint, Vector3.Up, Vector3.Right, ray, 10.0f, Guid.NewGuid(), 2, true, gridOrigin));
            var plane = service.ActivePlane;
            Assert.IsTrue(Vector3.NearEqual(new Vector3(5.0f, hitPoint.Y, 5.0f), plane.Origin));
            var snapped = ViewportSnapService.SnapToGrid(ref plane, hitPoint, out _);
            Assert.IsTrue(Vector3.NearEqual(new Vector3(15.0f, hitPoint.Y, 45.0f), snapped));
            Assert.IsTrue(plane.IsSurfaceDerived);
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
        public void TestWorkingPlaneCanFreezeCapturedThresholdAnchorPlane()
        {
            var service = new CSGWorkingPlaneService();
            service.Reset(10.0f);
            var captured = CSGWorkingPlane.World(25.0f);
            captured.Origin = new Vector3(0.0f, 125.0f, 0.0f);

            service.Freeze(ref captured);
            var frozen = service.ActivePlane;
            Assert.IsTrue(captured.NearlyEquals(ref frozen));

            service.Unfreeze();
            Assert.IsFalse(service.IsFrozen);
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
                new ViewportSnapCandidate { Point = new Vector3(10.0f, 30.0f, 0.0f), Kind = ViewportSnapTargetKind.CSGVertex, ActorId = Guid.NewGuid(), ComponentIndex = 3, ScreenDistance = 4.0f },
            };
            var solver = new ViewportSnapService();

            solver.Solve(ref plane, Vector3.Zero, true, 3.0f, candidates, out var gridResult);
            Assert.AreEqual(ViewportSnapTargetKind.Grid, gridResult.Kind);
            solver.Solve(ref plane, Vector3.Zero, true, 5.0f, candidates, out var geometryResult);
            Assert.AreEqual(ViewportSnapTargetKind.CSGVertex, geometryResult.Kind);
            Assert.IsTrue(Vector3.NearEqual(Vector3.Right * 10.0f, geometryResult.Point));
        }

        [Test]
        public void TestBoxPlacementHandlesEveryDragQuadrantWithPositiveSize()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var endpoints = new[]
            {
                new Float2(30.0f, 40.0f),
                new Float2(-30.0f, 40.0f),
                new Float2(-30.0f, -40.0f),
                new Float2(30.0f, -40.0f),
            };

            for (int i = 0; i < endpoints.Length; i++)
            {
                Assert.IsTrue(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, endpoints[i], 20.0f, false, false, false, out var placement));
                Assert.AreEqual(30.0f, (float)placement.Size.X, 0.0001f);
                Assert.AreEqual(20.0f, (float)placement.Size.Y, 0.0001f);
                Assert.AreEqual(40.0f, (float)placement.Size.Z, 0.0001f);
                Assert.Greater((float)placement.Size.X, 0.0f);
                Assert.Greater((float)placement.Size.Y, 0.0f);
                Assert.Greater((float)placement.Size.Z, 0.0f);
            }
        }

        [Test]
        public void TestBoxPlacementPreservesRotatedPlaneBasisAndSignedHeight()
        {
            var normal = new Vector3(0.2f, 0.9f, -0.35f);
            normal.Normalize();
            Assert.IsTrue(CSGWorkingPlaneService.TryDerive(normal * 25.0f, normal, Vector3.Right, 5.0f, 10, Guid.NewGuid(), 2, out var plane));
            Assert.IsTrue(CSGBoxPlacementSolver.TrySolve(ref plane, new Float2(-10.0f, -15.0f), new Float2(20.0f, 25.0f), -12.0f, false, false, false, out var placement));

            var transform = new Transform(placement.Center, placement.Orientation);
            var localUp = transform.LocalToWorldVector(Vector3.Up);
            localUp.Normalize();
            Assert.IsTrue(Vector3.NearEqual(plane.Normal, localUp));
            Assert.AreEqual(-12.0f, placement.SignedHeight, 0.0001f);
            Assert.AreEqual(12.0f, (float)placement.Size.Y, 0.0001f);
            Assert.AreEqual(CSGBoxOperationInference.Subtractive, placement.InferredOperation);
            var expectedPlaneCenter = plane.ToWorld(new Float2(5.0f, 5.0f));
            var expectedCenter = expectedPlaneCenter + plane.Normal * -6.0f;
            Assert.IsTrue(Vector3.NearEqual(expectedCenter, placement.Center));
        }

        [Test]
        public void TestGeneratedSurfacePlacementKeepsExactAuthoredDimensions()
        {
            var service = new CSGWorkingPlaneService();
            var ray = new Ray(new Vector3(0.0f, 10.0f, 0.0f), Vector3.Down);
            Assert.IsTrue(service.TrySetHover(Vector3.Zero, Vector3.Up, Vector3.Right, ray, 10.0f, Guid.Empty, -1, true));
            var plane = service.ActivePlane;
            Assert.IsTrue(plane.IsSurfaceDerived);
            Assert.AreEqual(Guid.Empty, plane.SourceActorId);

            Assert.IsTrue(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, new Float2(20.0f, 30.0f), -25.0f, false, false, false, out var placement));
            Assert.AreEqual(25.0f, (float)placement.Size.Y, 0.0001f);
            Assert.AreEqual(CSGBoxOperationInference.Subtractive, placement.InferredOperation);
            float positiveExtent = (float)Vector3.Dot(placement.Center, plane.Normal) + (float)placement.Size.Y * 0.5f;
            Assert.AreEqual(0.0f, positiveExtent, 0.0001f);
            float negativeExtent = (float)Vector3.Dot(placement.Center, plane.Normal) - (float)placement.Size.Y * 0.5f;
            Assert.AreEqual(-25.0f, negativeExtent, 0.0001f);
        }

        [Test]
        public void TestAxisAlignedSurfacePlaneUsesExactWorldGridCoordinate()
        {
            Assert.IsTrue(CSGWorkingPlaneService.TryDerive(new Vector3(0.0f, 498.58435f, 0.0f), Vector3.Up, Vector3.Right, 50.0f, 10, Guid.Empty, -1, out var plane));
            CSGWorkingPlaneService.AlignAxisAlignedOriginToGrid(ref plane, 50.0f);
            Assert.AreEqual(500.0f, (float)plane.Origin.Y, 0.0001f);

            Assert.IsTrue(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, new Float2(400.0f, 100.0f), -200.0f, false, false, false, out var placement));
            Assert.AreEqual(400.0f, (float)placement.Center.Y, 0.0001f);
            Assert.AreEqual(200.0f, (float)placement.Size.Y, 0.0001f);
        }

        [Test]
        public void TestSlopedSurfacePlaneRetainsSurfaceCoordinate()
        {
            var normal = Vector3.Normalize(new Vector3(1.0f, 1.0f, 0.0f));
            Assert.IsTrue(CSGWorkingPlaneService.TryDerive(new Vector3(17.0f, 23.0f, 0.0f), normal, Vector3.Forward, 50.0f, 10, Guid.Empty, -1, out var plane));
            var originalOrigin = plane.Origin;

            CSGWorkingPlaneService.AlignAxisAlignedOriginToGrid(ref plane, 50.0f);

            Assert.AreEqual(originalOrigin, plane.Origin);
        }

        [Test]
        public void TestBoxPlacementSquareSymmetricAndEpsilonRules()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            Assert.IsTrue(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, new Float2(20.0f, 50.0f), 15.0f, true, false, true, out var placement));
            Assert.AreEqual(50.0f, (float)placement.Size.X, 0.0001f);
            Assert.AreEqual(50.0f, (float)placement.Size.Z, 0.0001f);
            Assert.AreEqual(30.0f, (float)placement.Size.Y, 0.0001f);
            Assert.AreEqual(0.0f, (float)Vector3.Dot(placement.Center, plane.Normal), 0.0001f);
            Assert.AreEqual(CSGBoxOperationInference.None, placement.InferredOperation);

            Assert.IsFalse(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, new Float2(0.0001f, 10.0f), 10.0f, false, false, false, out _));
            Assert.IsFalse(CSGBoxPlacementSolver.TrySolve(ref plane, Float2.Zero, new Float2(10.0f), 0.0001f, false, false, false, out _));
            Assert.AreEqual(-25.0f, CSGBoxPlacementSolver.SnapDimension(-24.9f, 5.0f), 0.0001f);
        }

        [Test]
        public void TestBoxHeightProjectionUsesCameraFacingExtrusionPlane()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var direction = new Vector3(0.0f, 15.0f, 10.0f);
            direction.Normalize();
            var ray = new Ray(new Vector3(0.0f, 10.0f, -10.0f), direction);
            var viewDirection = new Vector3(0.0f, -1.0f, 1.0f);
            viewDirection.Normalize();

            Assert.IsTrue(CSGBoxPlacementSolver.TrySolveHeight(ref plane, Vector3.Zero, ref ray, viewDirection, out float height));
            Assert.AreEqual(25.0f, height, 0.0001f);
        }

        [Test]
        public void TestBoxDrawToolNumericOverridesProduceExactDimensions()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var tool = new CSGBoxDrawTool();
            Assert.IsTrue(tool.Begin(ref plane, Vector3.Zero, false, false, false));
            tool.UpdateFootprint(plane.ToWorld(new Float2(-30.0f, 40.0f)));
            Assert.IsTrue(tool.SetNumericOverride(CSGBoxNumericDimension.Width, 125.0f));
            Assert.IsTrue(tool.SetNumericOverride(CSGBoxNumericDimension.Depth, 75.0f));
            Assert.IsTrue(tool.CompleteFootprint());
            Assert.IsTrue(tool.SetNumericOverride(CSGBoxNumericDimension.Height, -35.0f));
            Assert.IsTrue(tool.TryGetPlacement(out var placement));
            Assert.AreEqual(125.0f, (float)placement.Size.X, 0.0001f);
            Assert.AreEqual(35.0f, (float)placement.Size.Y, 0.0001f);
            Assert.AreEqual(75.0f, (float)placement.Size.Z, 0.0001f);

            tool.Reset();
            Assert.AreEqual(CSGBoxDrawStage.Hover, tool.Stage);
            Assert.IsFalse(tool.SetNumericOverride(CSGBoxNumericDimension.Width, 0.0f));
        }

        [Test]
        public void TestBoxDrawAdjustmentStageKeepsOppositeCornerAndSupportsEitherHeightDirection()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var tool = new CSGBoxDrawTool();
            Assert.IsTrue(tool.Begin(ref plane, Vector3.Zero, false, false, false));
            tool.UpdateFootprint(plane.ToWorld(new Float2(40.0f, 30.0f)));
            Assert.IsTrue(tool.CompleteFootprint());
            Assert.IsFalse(tool.TryGetPlacement(out _));
            Assert.IsTrue(tool.TryGetAdjustmentFrame(out _, out var zeroOrigin, out var zeroPositiveTip, out var zeroNegativeTip,
                                                     out _, out _, out _, out _));
            Assert.AreEqual(10.0f, (float)Vector3.Dot(zeroPositiveTip - zeroOrigin, plane.Normal), 0.0001f);
            Assert.AreEqual(-10.0f, (float)Vector3.Dot(zeroNegativeTip - zeroOrigin, plane.Normal), 0.0001f);
            Assert.IsTrue(tool.BeginFootprintAdjustment(2));
            Assert.IsTrue(tool.UpdateFootprintAdjustment(plane.ToWorld(new Float2(60.0f, 50.0f))));
            Assert.IsTrue(tool.SetHeightDirection(-1));
            Assert.IsTrue(tool.TryGetPlacement(out var placement));
            Assert.AreEqual(60.0f, (float)placement.Size.X, 0.0001f);
            Assert.AreEqual(50.0f, (float)placement.Size.Z, 0.0001f);
            Assert.Less(Vector3.Dot(placement.Center, plane.Normal), 0.0f);

            Assert.IsTrue(tool.SetHeightDirection(1));
            Assert.IsTrue(tool.TryGetPlacement(out placement));
            Assert.Greater(Vector3.Dot(placement.Center, plane.Normal), 0.0f);
            Assert.AreEqual(CSGBoxDrawStage.Height, tool.Stage);

            Assert.IsTrue(tool.SetNumericOverride(CSGBoxNumericDimension.Height, 250.0f));
            Assert.IsTrue(tool.TryGetAdjustmentFrame(out _, out var origin, out var positiveTip, out var negativeTip,
                                                     out _, out _, out _, out _));
            Assert.AreEqual(260.0f, (float)Vector3.Dot(positiveTip - origin, plane.Normal), 0.0001f);
            Assert.AreEqual(-10.0f, (float)Vector3.Dot(negativeTip - origin, plane.Normal), 0.0001f);
        }

        [Test]
        public void TestBoxHeightHandleGrabPreservesHeightUntilPointerMoves()
        {
            var plane = CSGWorkingPlane.World(10.0f);
            var tool = new CSGBoxDrawTool();
            Assert.IsTrue(tool.Begin(ref plane, plane.ToWorld(new Float2(-20.0f, -15.0f)), false, false, false));
            tool.UpdateFootprint(plane.ToWorld(new Float2(20.0f, 15.0f)));
            Assert.IsTrue(tool.CompleteFootprint());

            var direction = new Vector3(0.0f, 15.0f, 10.0f);
            direction.Normalize();
            var ray = new Ray(new Vector3(0.0f, 10.0f, -10.0f), direction);
            var viewDirection = new Vector3(0.0f, -1.0f, 1.0f);
            viewDirection.Normalize();

            Assert.IsTrue(tool.BeginHeightAdjustment(-1, ref ray, viewDirection));
            Assert.IsTrue(tool.UpdateHeight(ref ray, viewDirection, false, plane.Spacing));
            Assert.IsTrue(tool.TryGetPlacement(out var placement));
            Assert.AreEqual(-10.0f, placement.SignedHeight, 0.0001f);
            tool.EndHeightAdjustment();
        }

        [Test]
        public void TestViewportOverlayLayoutRestoresDockPresentationAndVisibility()
        {
            var host = new ViewportOverlayHost();
            host.ApplyLayout("Test.Overlay@8@3@125.5@88.25@0|Test.Toolbar@9@1@340@0@1@220@70");
            var overlay = host.AddOverlay("Test.Overlay", "Test", new Control(0, 0, 160, 40), new Float2(160, 40));
            var toolbar = host.AddOverlay("Test.Toolbar", "Toolbar", new Control(0, 0, 220, 28), new Float2(220, 28));

            Assert.AreEqual(ViewportOverlayDock.BottomRight, overlay.Dock);
            Assert.AreEqual(ViewportOverlayLayoutMode.Collapsed, overlay.LayoutMode);
            Assert.IsFalse(overlay.UserVisible);
            StringAssert.Contains("Test.Overlay@8@3@125.5@88.25@0", host.CaptureLayout());
            Assert.AreEqual(ViewportOverlayDock.Toolbar, toolbar.Dock);
            Assert.AreEqual(ViewportOverlayLayoutMode.Horizontal, toolbar.LayoutMode);
            Assert.AreEqual(220.0f, toolbar.Content.Width, 0.001f);
            Assert.AreEqual(70.0f, toolbar.Content.Height, 0.001f);
            StringAssert.Contains("Test.Toolbar@9@1@340@0@1@220@70", host.CaptureLayout());

            overlay.UserVisible = true;
            overlay.SetContextVisible(false);
            Assert.IsTrue(overlay.UserVisible);
            Assert.IsFalse(overlay.Visible);
            overlay.SetContextVisible(true);
            Assert.IsTrue(overlay.Visible);
            host.Dispose();
        }

        [Test]
        public void TestCSGAuthoringGizmoModeTryCancelExitsEditContext()
        {
            var owner = new TestGizmoOwner();
            var mode = new CSGAuthoringGizmoMode();
            owner.Gizmos.AddMode(mode);
            owner.Gizmos.ActiveMode = mode;

            var actor = new BoxBrush();
            var brush = new BoxBrushNode(actor);
            try
            {
                Assert.IsNotNull(mode.Gizmo);
                Assert.IsTrue(mode.Gizmo.EnterEditContext(brush));
                Assert.IsTrue(mode.Gizmo.IsEditingContext);

                // Cancel in edit mode should exit edit mode cleanly
                Assert.IsTrue(mode.TryCancel(EditorGizmoModeCancelReason.User));
                Assert.IsFalse(mode.Gizmo.IsEditingContext);
            }
            finally
            {
                brush.Dispose();
                FlaxEngine.Object.Destroy(actor);
                mode.Dispose();
                owner.Gizmos.Clear();
            }
        }

        [Test]
        public void TestCSGAuthoringGizmoMouseDownInEditModeReturnsFalseOutsideHandles()
        {
            var owner = new TestGizmoOwner();
            var mode = new CSGAuthoringGizmoMode();
            owner.Gizmos.AddMode(mode);
            owner.Gizmos.ActiveMode = mode;

            var actor = new BoxBrush();
            var brush = new BoxBrushNode(actor);
            try
            {
                Assert.IsTrue(mode.Gizmo.EnterEditContext(brush));
                Assert.IsTrue(mode.Gizmo.IsEditingContext);

                // Clicking outside handles in edit mode should return false so rubberband marquee can start
                Assert.IsFalse(mode.Gizmo.OnMouseDown(new Float2(100, 100), MouseButton.Left));
            }
            finally
            {
                brush.Dispose();
                FlaxEngine.Object.Destroy(actor);
                mode.Dispose();
                owner.Gizmos.Clear();
            }
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
