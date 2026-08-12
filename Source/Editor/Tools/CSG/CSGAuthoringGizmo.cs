// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.Gizmo.Snapping;
using FlaxEditor.GUI;
using FlaxEditor.Modules;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG.HitTesting;
using FlaxEditor.Tools.CSG.Placement;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEditor.Tools.CSG.Rendering;
using FlaxEditor.Tools.CSG.Selection;
using FlaxEditor.Tools.CSG.Snapping;
using FlaxEditor.Tools.CSG.Tools;
using FlaxEditor.Tools.CSG.Transactions;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEditor.Viewport.Cameras;
using FlaxEditor.Viewport.Modes;
using FlaxEditor.Viewport.Widgets;
using FlaxEngine;
using FlaxEngine.Gizmo;
using FlaxEngine.GUI;

namespace FlaxEditor.Tools.CSG
{
    /// <summary>
    /// CSG authoring gizmo. Owns CSG-only viewport selection, source-brush visualization, and tool overlays.
    /// </summary>
    [HideInEditor]
    public sealed class CSGAuthoringGizmo : GizmoBase, IViewportRubberBandSelection
    {
        private static readonly int[] BrushBoxEdges =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };

        private readonly CSGToolController _controller;
        private readonly CSGHitTestService _hitTest = new CSGHitTestService();
        private readonly CSGSelectionModel _selection = new CSGSelectionModel();
        private readonly CSGWorkingPlaneService _workingPlane = new CSGWorkingPlaneService();
        private readonly ViewportSnapService _snapService = new ViewportSnapService();
        private readonly CSGOverlayRenderer _overlayRenderer = new CSGOverlayRenderer();
        private readonly CSGBoxDrawTool _boxDrawTool = new CSGBoxDrawTool();
        private readonly CSGTransaction _transaction = new CSGTransaction();
        private readonly List<CSGHit> _hits = new List<CSGHit>(64);
        private readonly List<CSGHit> _selectableHits = new List<CSGHit>(16);
        private readonly List<CSGHit> _cycleSignature = new List<CSGHit>(16);
        private readonly List<SceneGraphNode> _selectionBuffer = new List<SceneGraphNode>(16);
        private readonly List<ActorNode> _actorBuffer = new List<ActorNode>(64);
        private readonly List<BoxBrush> _transactionBrushes = new List<BoxBrush>(8);
        private readonly List<ViewportSnapCandidate> _snapCandidates = new List<ViewportSnapCandidate>(128);
        private readonly Vector3[] _brushCorners = new Vector3[8];
        private Float2 _cyclePointer;
        private Vector3 _cycleViewPosition;
        private Quaternion _cycleViewOrientation;
        private Matrix _cycleViewProjection;
        private CSGTool _cycleTool;
        private int _cycleIndex;
        private bool _hasCycle;
        private bool _modeActive;
        private bool _selectionEntered;
        private bool _applyingSelection;
        private bool _hasSnap;
        private ViewportSnapResult _snapResult;
        private CSGOperation _lastControllerOperation;
        private CSGOperation? _drawOperationOverride;
        private bool _boxCreated;
        private bool _consumeDrawMouseUp;
        private string _planeStatus = "World";
        private string _rebuildStatus = "Build Auto";
        private string _statusText = "CSG Authoring";

        /// <summary>
        /// Gets the reusable authoring transaction used by concrete CSG tools.
        /// </summary>
        internal CSGTransaction Transaction => _transaction;

        /// <inheritdoc />
        public override bool IsControllingMouse => _controller.HasActiveInteraction;

        /// <summary>
        /// Initializes a new instance of the <see cref="CSGAuthoringGizmo"/> class.
        /// </summary>
        /// <param name="owner">The gizmo owner.</param>
        /// <param name="controller">The CSG tool controller.</param>
        public CSGAuthoringGizmo(IGizmoOwner owner, CSGToolController controller)
        : base(owner)
        {
            _controller = controller;
            _controller.Changed += OnControllerChanged;
            _controller.InteractionStarted += OnInteractionStarted;
            _controller.InteractionCommitted += OnInteractionCommitted;
            _controller.InteractionCancelled += OnInteractionCancelled;
            _controller.PickWorkingPlaneRequested += OnPickWorkingPlaneRequested;
            _controller.ResetWorkingPlaneRequested += OnResetWorkingPlaneRequested;
            _controller.OffsetWorkingPlaneRequested += OnOffsetWorkingPlaneRequested;
            _controller.RotateWorkingPlaneRequested += OnRotateWorkingPlaneRequested;
            _workingPlane.Reset(_controller.SnapIncrement);
            _lastControllerOperation = _controller.Operation;
        }

        /// <inheritdoc />
        public override void Destroy()
        {
            _controller.Changed -= OnControllerChanged;
            _controller.InteractionStarted -= OnInteractionStarted;
            _controller.InteractionCommitted -= OnInteractionCommitted;
            _controller.InteractionCancelled -= OnInteractionCancelled;
            _controller.PickWorkingPlaneRequested -= OnPickWorkingPlaneRequested;
            _controller.ResetWorkingPlaneRequested -= OnResetWorkingPlaneRequested;
            _controller.OffsetWorkingPlaneRequested -= OnOffsetWorkingPlaneRequested;
            _controller.RotateWorkingPlaneRequested -= OnRotateWorkingPlaneRequested;
            _transaction.Dispose();
            base.Destroy();
        }

        /// <inheritdoc />
        public override void OnActivated()
        {
            _modeActive = true;
            _workingPlane.SetSpacing(_controller.SnapIncrement);
            _workingPlane.SetLocked(_controller.WorkingPlaneLocked);
            TryEnterSelectionContext();
            ResetDeepSelectionCycle();
            base.OnActivated();
        }

        /// <inheritdoc />
        public override void OnDeactivated()
        {
            var root = Owner.SceneGraphRoot;
            if (_selectionEntered && root?.SceneContext != null)
            {
                _selection.Leave(root.SceneContext.Selection, root, _selectionBuffer);
                ApplySelectionBuffer(false);
            }
            _selectionEntered = false;
            _modeActive = false;
            _workingPlane.Unfreeze();
            _workingPlane.ClearHover();
            _hasSnap = false;
            _boxDrawTool.Reset();
            ResetDeepSelectionCycle();
            base.OnDeactivated();
        }

        /// <inheritdoc />
        public override void OnSelectionChanged(List<SceneGraphNode> newSelection)
        {
            if (!_modeActive || !_selectionEntered || _applyingSelection)
                return;
            _selection.Observe(newSelection);
            ResetDeepSelectionCycle();
        }

        /// <inheritdoc />
        public override void Pick()
        {
            if (!_modeActive || !TryEnterSelectionContext() || Owner.SceneGraphRoot == null)
                return;

            Profiler.BeginEvent("CSG.Pick");
            var ray = Owner.MouseRay;
            var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
            var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                        SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                        SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
            _hitTest.Gather(Owner.SceneGraphRoot, ref ray, ref view, _hits, flags);
            var pointer = Owner.Viewport.ContinuousViewMousePosition;
            if (_controller.Tool == CSGTool.SelectPlace)
                AddBrushOutlineHits(pointer);
            _selectableHits.Clear();
            for (int i = 0; i < _hits.Count; i++)
            {
                var hit = _hits[i];
                if (CSGHitTestService.IsSelectable(_controller.Tool, ref hit))
                    _selectableHits.Add(hit);
            }

            if (_selectableHits.Count == 0)
            {
                _selection.ApplyClick(null, Owner.IsControlDown, Owner.IsShiftDown, _selectionBuffer);
                ApplySelectionBuffer(true);
                ResetDeepSelectionCycle();
                Profiler.EndEvent();
                return;
            }

            var projection = Owner.Viewport.ViewFrustum.Matrix;
            float tolerance = 4.0f * Owner.Viewport.DpiScale;
            bool continueCycle = _hasCycle &&
                                 (pointer - _cyclePointer).LengthSquared <= tolerance * tolerance &&
                                 Owner.ViewPosition.Equals(_cycleViewPosition) &&
                                 Owner.ViewOrientation.Equals(_cycleViewOrientation) &&
                                 projection.Equals(_cycleViewProjection) &&
                                 _controller.Tool == _cycleTool &&
                                 HasSameHitSignature(_selectableHits, _cycleSignature);
            _cycleIndex = continueCycle ? (_cycleIndex + 1) % _selectableHits.Count : 0;
            SaveDeepSelectionCycle(pointer, projection);

            var candidate = _selectableHits[_cycleIndex].SelectionNode;
            _selection.ApplyClick(candidate, Owner.IsControlDown, Owner.IsShiftDown, _selectionBuffer);
            ApplySelectionBuffer(true);
            Profiler.EndEvent();
        }

        /// <summary>Handles CSG-owned pointer movement before viewport selection behavior.</summary>
        internal bool OnMouseMove(Float2 location)
        {
            return _controller.Tool == CSGTool.Draw && _boxDrawTool.IsInteracting;
        }

        /// <summary>Begins a footprint or commits the height stage.</summary>
        internal bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (_controller.Tool != CSGTool.Draw || button != MouseButton.Left || Owner.IsAltKeyDown)
                return false;

            if (_boxDrawTool.Stage == CSGBoxDrawStage.Hover)
            {
                var plane = _workingPlane.ActivePlane;
                AlignDrawPlaneToGrid(ref plane);
                if (!TryGetDrawPoint(ref plane, out var point) ||
                    !_boxDrawTool.Begin(ref plane, point, _controller.SquareConstraintActive, false, _controller.SymmetricConstraintActive))
                    return true;
                _drawOperationOverride = null;
                _boxCreated = false;
                _controller.BeginInteraction();
                UpdateStatusText();
                return true;
            }

            if (_boxDrawTool.Stage == CSGBoxDrawStage.Height)
                TryCommitBoxDraw(true);
            return true;
        }

        /// <summary>Locks the footprint after the initial drag.</summary>
        internal bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _consumeDrawMouseUp)
            {
                _consumeDrawMouseUp = false;
                return true;
            }
            if (_controller.Tool != CSGTool.Draw || button != MouseButton.Left || !_boxDrawTool.IsInteracting)
                return false;
            if (_boxDrawTool.Stage == CSGBoxDrawStage.Footprint)
            {
                var plane = _workingPlane.ActivePlane;
                AlignDrawPlaneToGrid(ref plane);
                if (TryGetDrawPoint(ref plane, out var point))
                    _boxDrawTool.UpdateFootprint(point);
                _boxDrawTool.CompleteFootprint();
                UpdateStatusText();
            }
            return true;
        }

        /// <summary>Handles numeric box dimensions before mode shortcut processing.</summary>
        internal bool OnKeyDown(KeyboardKeys key)
        {
            if (_controller.Tool != CSGTool.Draw || !_boxDrawTool.OnKeyDown(key, out bool requestCommit))
                return false;
            if (requestCommit)
                TryCommitBoxDraw();
            UpdateStatusText();
            return true;
        }

        /// <summary>Advances or commits the staged draw interaction.</summary>
        internal bool TryCommitDrawStage()
        {
            if (_controller.Tool != CSGTool.Draw || !_boxDrawTool.IsInteracting)
                return false;
            if (_boxDrawTool.Stage == CSGBoxDrawStage.Footprint)
                _boxDrawTool.CompleteFootprint();
            else
                TryCommitBoxDraw();
            UpdateStatusText();
            return true;
        }

        /// <inheritdoc />
        public override void Update(float dt)
        {
            if (_modeActive && !_selectionEntered)
                TryEnterSelectionContext();
            if (!IsActive || !Visible || Owner.SceneGraphRoot == null)
            {
                return;
            }

            _actorBuffer.Clear();
            Owner.SceneGraphRoot.GetAllChildActorNodes(_actorBuffer);
            CSGRebuildScheduler.Shared.Update();
            UpdateRebuildStatus();
            UpdateWorkingPlaneAndSnap();
            UpdateBoxDrawTool();
            var plane = _workingPlane.ActivePlane;
            AlignDrawPlaneToGrid(ref plane);
            bool showSnap = _controller.HasActiveInteraction;
            var drawSnap = _snapResult;
            ProjectDrawPointToPlane(ref plane, ref drawSnap.Point);
            float snapMarkerSize = GetScreenSpaceCursorWorldSize(drawSnap.Point, plane.Spacing);
            _overlayRenderer.Draw(ref plane, Owner.ViewPosition, !_workingPlane.IsLocked && _workingPlane.HasHover, showSnap && _hasSnap, ref drawSnap, snapMarkerSize);

            if ((_controller.Visibility & CSGVisibility.SourceBrushes) != 0)
            {
                for (int i = 0; i < _actorBuffer.Count; i++)
                {
                    if (_actorBuffer[i] is not BoxBrushNode node)
                        continue;
                    bool hidden = !node.IsActiveInHierarchy;
                    if (hidden && (_controller.Visibility & CSGVisibility.HiddenBrushes) == 0)
                        continue;

                    var brush = (BoxBrush)node.Actor;
                    bool selected = IsBrushBodySelected(node);
                    bool subtractive = brush.Mode == BrushMode.Subtractive;
                    var color = subtractive ? new Color(1.0f, 0.12f, 0.1f, 0.95f) : new Color(0.18f, 0.76f, 1.0f, 0.9f);
                    if (hidden)
                        color.A = 0.45f;
                    var xrayColor = color;
                    xrayColor.A *= 0.2f;
                    if (subtractive)
                    {
                        DrawDashedWireBox(brush.OrientedBox, xrayColor, false);
                        DrawDashedWireBox(brush.OrientedBox, color, true);
                    }
                    else
                    {
                        DebugDraw.DrawWireBox(brush.OrientedBox, xrayColor, 0.0f, false);
                        DebugDraw.DrawWireBox(brush.OrientedBox, color, 0.0f, true);
                    }
                    if (selected)
                    {
                        DebugDraw.DrawBox(brush.OrientedBox, new Color(1.0f, 0.68f, 0.08f, 0.5f), 0.0f, true);
                        DebugDraw.DrawWireBox(brush.OrientedBox, Color.White, 0.0f, true);
                    }
                }
            }

        }

        private void UpdateWorkingPlaneAndSnap()
        {
            _workingPlane.SetSpacing(_controller.SnapIncrement);
            var pointerRay = Owner.MouseRay;
            if (!_workingPlane.IsLocked && !_workingPlane.IsFrozen)
            {
                var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
                var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                            SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                            SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
                _hitTest.Gather(Owner.SceneGraphRoot, ref pointerRay, ref view, _hits, flags);
                bool found = false;
                for (int i = 0; i < _hits.Count; i++)
                {
                    var hit = _hits[i];
                    if (hit.Kind != CSGHitKind.Face && hit.Kind != CSGHitKind.Placement)
                        continue;
                    if (_controller.HasActiveInteraction && hit.Brush != null && IsBrushSelected(hit.Brush))
                        continue;
                    var preferredTangent = GetPreferredSurfaceTangent(ref hit);
                    if (_workingPlane.TrySetHover(hit.Point, hit.Normal, preferredTangent, pointerRay, _controller.SnapIncrement, hit.Brush?.ID ?? System.Guid.Empty, hit.ComponentIndex, true))
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    _workingPlane.ClearHover();
            }

            var plane = _workingPlane.ActivePlane;
            if (CSGWorkingPlaneService.TryIntersect(ref plane, ref pointerRay, out var point))
            {
                float threshold = 12.0f * Owner.Viewport.DpiScale;
                IReadOnlyList<SceneGraphNode> excluded = _controller.HasActiveInteraction ? _selection.CSGSelection : null;
                _snapCandidates.Clear();
                if (_controller.Tool != CSGTool.Draw)
                    CSGSnapProviders.Gather(_actorBuffer, excluded, Owner.Viewport, ref plane, Owner.Viewport.ContinuousViewMousePosition, threshold, _snapCandidates, _brushCorners);
                _snapService.Solve(ref plane, point, _controller.EffectiveSnappingEnabled, threshold, _snapCandidates, out _snapResult);
                _hasSnap = _snapResult.IsSnapped;
            }
            else
            {
                _snapCandidates.Clear();
                _hasSnap = false;
            }

            string planeStatus = _workingPlane.IsLocked ? "Locked" : _workingPlane.HasHover ? "Surface" : "World";
            if (_planeStatus != planeStatus)
            {
                _planeStatus = planeStatus;
                UpdateStatusText();
            }
        }

        private void UpdateBoxDrawTool()
        {
            if (_controller.Tool != CSGTool.Draw)
                return;

            _boxDrawTool.SetModifiers(_controller.SquareConstraintActive, false, _controller.SymmetricConstraintActive);
            var plane = _workingPlane.ActivePlane;
            AlignDrawPlaneToGrid(ref plane);
            float hoverMarkerSize = 0.0f;
            if (_boxDrawTool.Stage == CSGBoxDrawStage.Hover)
            {
                if (TryGetDrawPoint(ref plane, out var point))
                {
                    _boxDrawTool.UpdateHover(ref plane, point);
                    hoverMarkerSize = GetScreenSpaceCursorWorldSize(point, plane.Spacing);
                }
            }
            else if (_boxDrawTool.Stage == CSGBoxDrawStage.Footprint)
            {
                if (TryGetDrawPoint(ref plane, out var point))
                    _boxDrawTool.UpdateFootprint(point);
            }
            else
            {
                var ray = Owner.MouseRay;
                _boxDrawTool.UpdateHeight(ref ray, Owner.ViewDirection, _controller.EffectiveSnappingEnabled, _controller.SnapIncrement);
            }
            _boxDrawTool.Draw(hoverMarkerSize);
            UpdateStatusText();
        }

        private float GetScreenSpaceCursorWorldSize(Vector3 point, float fallbackSpacing)
        {
            var viewport = Owner.Viewport;
            float forwardDepth = (float)Vector3.Dot(point - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
            float verticalFov = viewport.FieldOfView;
            if (viewport.ViewportCamera is FPSCamera fpsCamera)
                verticalFov += fpsCamera.AdditionalZoomFOV;
            if (TransformGizmoBase.TryCalculateProjectionSizing(
                    viewport.UseOrthographicProjection,
                    forwardDepth,
                    verticalFov,
                    viewport.OrthographicScale,
                    viewport.Height,
                    viewport.DpiScale,
                    6.0f,
                    viewport.NearPlane,
                    out _,
                    out var halfSize))
                return halfSize * 2.0f;
            return Mathf.Clamp(fallbackSpacing * 0.15f, 0.75f, 7.5f);
        }

        private bool TryGetDrawPoint(ref CSGWorkingPlane plane, out Vector3 point)
        {
            if (_hasSnap)
            {
                point = _snapResult.Point;
                ProjectDrawPointToPlane(ref plane, ref point);
                return true;
            }
            var ray = Owner.MouseRay;
            return CSGWorkingPlaneService.TryIntersect(ref plane, ref ray, out point);
        }

        private void AlignDrawPlaneToGrid(ref CSGWorkingPlane plane)
        {
            if (_controller.Tool == CSGTool.Draw && _controller.EffectiveSnappingEnabled)
                CSGWorkingPlaneService.AlignAxisAlignedOriginToGrid(ref plane, _controller.SnapIncrement);
        }

        private static void ProjectDrawPointToPlane(ref CSGWorkingPlane plane, ref Vector3 point)
        {
            point += plane.Normal * (float)Vector3.Dot(plane.Origin - point, plane.Normal);
        }

        private void TryCommitBoxDraw(bool consumeMouseUp = false)
        {
            if (!_boxDrawTool.TryGetPlacement(out _))
                return;
            if (_controller.TryCommit() && consumeMouseUp)
                _consumeDrawMouseUp = true;
        }

        private bool CreateBoxBrush()
        {
            if (_boxCreated)
                return true;
            if (!_boxDrawTool.TryGetPlacement(out var placement))
                return false;

            var operation = _drawOperationOverride ?? GetInferredOperation(ref placement);
            var brush = new BoxBrush
            {
                Name = "Box Brush",
                Transform = new Transform(placement.Center, placement.Orientation),
                Center = Vector3.Zero,
                Size = placement.Size,
                Mode = operation == CSGOperation.Subtractive ? BrushMode.Subtractive : BrushMode.Additive,
            };
            var sceneEditing = Editor.Instance?.SceneEditing;
            if (sceneEditing == null || !sceneEditing.TrySpawnForCSGTransaction(brush, out _, out var action))
            {
                FlaxEngine.Object.Destroy(brush);
                return false;
            }

            _transaction.Touch(brush);
            _transaction.RegisterPerformedAction(action);
            _boxCreated = true;
            return true;
        }

        private CSGOperation GetInferredOperation(ref CSGBoxPlacement placement)
        {
            if (placement.InferredOperation == CSGBoxOperationInference.Additive)
                return CSGOperation.Additive;
            if (placement.InferredOperation == CSGBoxOperationInference.Subtractive)
                return CSGOperation.Subtractive;
            return _controller.Operation;
        }

        private Vector3 GetPreferredSurfaceTangent(ref CSGHit hit)
        {
            if (hit.Kind != CSGHitKind.Face || hit.Brush == null)
                return Vector3.Zero;
            var transform = hit.Brush.Actor.Transform;
            var localTangent = hit.ComponentIndex <= 1 ? Vector3.Up : Vector3.Right;
            return transform.LocalToWorldVector(localTangent);
        }

        private void DrawDashedWireBox(OrientedBoundingBox box, Color color, bool depthTest)
        {
            box.GetCorners(_brushCorners);
            const int dashCount = 6;
            const float dashFraction = 0.6f;

            for (int i = 0; i < BrushBoxEdges.Length; i += 2)
            {
                var start = _brushCorners[BrushBoxEdges[i]];
                var end = _brushCorners[BrushBoxEdges[i + 1]];
                var edge = end - start;
                for (int dash = 0; dash < dashCount; dash++)
                {
                    float dashStart = (float)dash / dashCount;
                    float dashEnd = (dash + dashFraction) / dashCount;
                    DebugDraw.DrawLine(start + edge * dashStart, start + edge * dashEnd, color, 0.0f, depthTest);
                }
            }
        }

        private void AddBrushOutlineHits(Float2 pointer)
        {
            if ((_controller.Visibility & CSGVisibility.SourceBrushes) == 0)
                return;

            _actorBuffer.Clear();
            Owner.SceneGraphRoot.GetAllChildActorNodes(_actorBuffer);
            float tolerance = 7.0f * Owner.Viewport.DpiScale;
            float toleranceSquared = tolerance * tolerance;
            var viewPosition = Owner.ViewPosition;
            var viewDirection = (Vector3)Owner.ViewDirection;

            for (int actorIndex = 0; actorIndex < _actorBuffer.Count; actorIndex++)
            {
                if (_actorBuffer[actorIndex] is not BoxBrushNode node)
                    continue;
                bool hidden = !node.IsActiveInHierarchy;
                if (hidden && (_controller.Visibility & CSGVisibility.HiddenBrushes) == 0)
                    continue;

                bool alreadyHit = false;
                for (int hitIndex = 0; hitIndex < _hits.Count; hitIndex++)
                {
                    if (_hits[hitIndex].Kind == CSGHitKind.Brush && _hits[hitIndex].Brush == node)
                    {
                        alreadyHit = true;
                        break;
                    }
                }
                if (alreadyHit)
                    continue;

                var brush = (BoxBrush)node.Actor;
                brush.OrientedBox.GetCorners(_brushCorners);
                float closestDistanceSquared = float.MaxValue;
                Real closestDepth = Real.MaxValue;
                Vector3 closestPoint = Vector3.Zero;
                for (int edgeIndex = 0; edgeIndex < BrushBoxEdges.Length; edgeIndex += 2)
                {
                    var start = _brushCorners[BrushBoxEdges[edgeIndex]];
                    var end = _brushCorners[BrushBoxEdges[edgeIndex + 1]];
                    if (Vector3.Dot(start - viewPosition, viewDirection) <= 0.0f ||
                        Vector3.Dot(end - viewPosition, viewDirection) <= 0.0f)
                        continue;

                    Owner.Viewport.ProjectPoint(start, out var startScreen);
                    Owner.Viewport.ProjectPoint(end, out var endScreen);
                    float distanceSquared = DistanceSquaredToSegment(pointer, startScreen, endScreen, out float edgeAmount);
                    if (distanceSquared >= closestDistanceSquared)
                        continue;

                    closestDistanceSquared = distanceSquared;
                    var edgePoint = start + (end - start) * edgeAmount;
                    closestDepth = Vector3.Dot(edgePoint - viewPosition, viewDirection);
                    closestPoint = edgePoint;
                }

                if (closestDistanceSquared <= toleranceSquared)
                {
                    _hits.Add(new CSGHit
                    {
                        Node = node,
                        Brush = node,
                        Kind = CSGHitKind.Brush,
                        ComponentIndex = -1,
                        Distance = closestDepth,
                        Normal = Vector3.Zero,
                        Point = closestPoint,
                    });
                }
            }

            _hitTest.Sort(_hits);
        }

        private static float DistanceSquaredToSegment(Float2 point, Float2 start, Float2 end, out float amount)
        {
            var edge = end - start;
            float lengthSquared = edge.LengthSquared;
            if (lengthSquared <= Mathf.Epsilon)
            {
                amount = 0.0f;
                return (point - start).LengthSquared;
            }

            amount = Mathf.Saturate(Float2.Dot(point - start, edge) / lengthSquared);
            return (point - (start + edge * amount)).LengthSquared;
        }

        /// <inheritdoc />
        public bool CanSelectWithRubberBand(ActorNode node)
        {
            return node is BoxBrushNode;
        }

        /// <inheritdoc />
        public SceneGraphNode ResolveRubberBandSelection(ActorNode node)
        {
            return node as BoxBrushNode;
        }

        /// <inheritdoc />
        public override void Draw()
        {
            if (!IsActive || !Visible)
                return;

            var font = Style.Current.FontSmall;
            float width = Mathf.Clamp(font.MeasureText(_statusText).X + 18.0f, 196.0f, Mathf.Max(196.0f, Owner.Viewport.Width - 20.0f));
            var rect = new Rectangle(10.0f, ViewportWidgetsContainer.WidgetsHeight + 14.0f, width, 22.0f);
            Render2D.FillRectangle(rect, new Color(0.08f, 0.11f, 0.16f, 0.88f));
            Render2D.DrawText(font, _statusText, rect, Color.White, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap);
        }

        private void ApplySelectionBuffer(bool recordUndo)
        {
            _applyingSelection = true;
            Owner.Select(_selectionBuffer, recordUndo);
            _applyingSelection = false;
        }

        private bool TryEnterSelectionContext()
        {
            if (_selectionEntered)
                return true;
            var root = Owner.SceneGraphRoot;
            var context = root?.SceneContext;
            if (!_modeActive || context == null)
                return false;
            _selection.Enter(context.Selection, root, _selectionBuffer);
            _selectionEntered = true;
            ApplySelectionBuffer(false);
            return true;
        }

        private bool IsBrushSelected(BoxBrushNode brush)
        {
            var selection = _selection.CSGSelection;
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] == brush || selection[i]?.ParentNode == brush)
                    return true;
            }
            return false;
        }

        private bool IsBrushBodySelected(BoxBrushNode brush)
        {
            var selection = _selection.CSGSelection;
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] == brush)
                    return true;
            }
            return false;
        }

        private static bool HasSameHitSignature(List<CSGHit> current, List<CSGHit> previous)
        {
            if (current.Count != previous.Count)
                return false;
            for (int i = 0; i < current.Count; i++)
            {
                var a = current[i];
                var b = previous[i];
                if (a.Node != b.Node || a.Kind != b.Kind || a.ComponentIndex != b.ComponentIndex ||
                    !Mathf.NearEqual((float)a.Distance, (float)b.Distance) || !Vector3.NearEqual(a.Normal, b.Normal) || !Vector3.NearEqual(a.Point, b.Point))
                    return false;
            }
            return true;
        }

        private void SaveDeepSelectionCycle(Float2 pointer, Matrix projection)
        {
            _cyclePointer = pointer;
            _cycleViewPosition = Owner.ViewPosition;
            _cycleViewOrientation = Owner.ViewOrientation;
            _cycleViewProjection = projection;
            _cycleTool = _controller.Tool;
            _cycleSignature.Clear();
            _cycleSignature.AddRange(_selectableHits);
            _hasCycle = true;
            UpdateStatusText();
        }

        private void ResetDeepSelectionCycle()
        {
            _hasCycle = false;
            _cycleIndex = 0;
            _cycleSignature.Clear();
            UpdateStatusText();
        }

        private void OnControllerChanged()
        {
            if (_boxDrawTool.IsInteracting && _controller.Operation != _lastControllerOperation)
                _drawOperationOverride = _controller.Operation;
            _lastControllerOperation = _controller.Operation;
            _workingPlane.SetSpacing(_controller.SnapIncrement);
            _workingPlane.SetLocked(_controller.WorkingPlaneLocked);
            if (_controller.HasActiveInteraction && !_workingPlane.IsFrozen)
                _workingPlane.Freeze();
            else if (!_controller.HasActiveInteraction && _workingPlane.IsFrozen)
                _workingPlane.Unfreeze();
            ResetDeepSelectionCycle();
        }

        private void OnInteractionStarted()
        {
            _transactionBrushes.Clear();
            var selection = _selection.CSGSelection;
            for (int i = 0; i < selection.Count; i++)
            {
                var brushNode = selection[i] as BoxBrushNode ?? selection[i]?.ParentNode as BoxBrushNode;
                if (brushNode?.Actor is BoxBrush brush && !_transactionBrushes.Contains(brush))
                    _transactionBrushes.Add(brush);
            }
            _transaction.Begin(_transactionBrushes);
            _boxCreated = false;
        }

        private void OnInteractionCommitted()
        {
            try
            {
                if (_controller.Tool == CSGTool.Draw && _boxDrawTool.IsInteracting && !CreateBoxBrush())
                {
                    _transaction.Rollback("Invalid box placement");
                    return;
                }
                _transaction.Commit(Editor.Instance?.Undo, _controller.Tool == CSGTool.Draw ? "Create CSG Box" : "Edit CSG");
            }
            catch (System.Exception ex)
            {
                Debug.LogError("CSG interaction commit failed. " + ex.Message);
                _transaction.Rollback("Commit failed");
            }
            finally
            {
                _boxDrawTool.Reset();
                _drawOperationOverride = null;
                _consumeDrawMouseUp = false;
            }
        }

        private void OnInteractionCancelled(EditorGizmoModeCancelReason reason)
        {
            _transaction.Invalidate(reason.ToString());
            _boxDrawTool.Reset();
            _drawOperationOverride = null;
        }

        private void OnPickWorkingPlaneRequested()
        {
            if (_workingPlane.PickHovered())
                _controller.SetWorkingPlaneLocked(true);
        }

        private void OnResetWorkingPlaneRequested()
        {
            _workingPlane.Reset(_controller.SnapIncrement);
            _hasSnap = false;
            _planeStatus = "World";
            UpdateStatusText();
        }

        private void OnOffsetWorkingPlaneRequested(float distance)
        {
            _workingPlane.Offset(distance);
            _controller.SetWorkingPlaneLocked(true);
            _planeStatus = "Locked";
            UpdateStatusText();
        }

        private void OnRotateWorkingPlaneRequested(float angleDegrees)
        {
            _workingPlane.Rotate(angleDegrees);
            _controller.SetWorkingPlaneLocked(true);
            _planeStatus = "Locked";
            UpdateStatusText();
        }

        private void UpdateStatusText()
        {
            string tool = _controller.Tool == CSGTool.SelectPlace ? "Select / Place" : _controller.Tool.ToString();
            string transaction = _transaction.IsActive ? $"  |  Txn Preview {_transaction.Telemetry.TouchedBrushCount}" : string.Empty;
            string draw = _controller.Tool == CSGTool.Draw ? $"  |  {_boxDrawTool.StatusText}" : string.Empty;
            _statusText = _hasCycle && _selectableHits.Count > 1
                ? $"CSG {tool}  |  Hit {_cycleIndex + 1}/{_selectableHits.Count}  |  Plane {_planeStatus}  |  {_rebuildStatus}{draw}{transaction}"
                : $"CSG {tool}  |  Plane {_planeStatus}  |  {_rebuildStatus}{draw}{transaction}";
        }

        private void UpdateRebuildStatus()
        {
            var editor = Editor.Instance;
            string status = "Build Auto";
            if (editor != null && !editor.Options.Options.General.AutoRebuildCSG)
            {
                status = "Build Off";
            }
            else
            {
                Scene scene = null;
                for (int i = 0; i < _actorBuffer.Count; i++)
                {
                    if (_actorBuffer[i]?.Actor is BoxBrush brush && brush.Scene != null)
                    {
                        scene = brush.Scene;
                        break;
                    }
                }
                var rebuild = CSGRebuildScheduler.Shared.GetStatus(scene);
                if (rebuild.State == CSGRebuildVisualState.Pending)
                    status = "Build Pending";
                else if (rebuild.State == CSGRebuildVisualState.Submitted)
                    status = $"Build Queued r{rebuild.SubmittedRevision}";
                else if (rebuild.State == CSGRebuildVisualState.Stale)
                    status = "Build Stale";
            }
            if (_rebuildStatus == status)
                return;
            _rebuildStatus = status;
            UpdateStatusText();
        }
    }
}
