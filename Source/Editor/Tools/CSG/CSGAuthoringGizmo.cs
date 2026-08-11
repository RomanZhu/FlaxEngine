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
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG.HitTesting;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEditor.Tools.CSG.Rendering;
using FlaxEditor.Tools.CSG.Selection;
using FlaxEditor.Tools.CSG.Snapping;
using FlaxEditor.Tools.CSG.Transactions;
using FlaxEditor.Tools.CSG.WorkingPlane;
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

        /// <inheritdoc />
        public override void Update(float dt)
        {
            if (_modeActive && !_selectionEntered)
                TryEnterSelectionContext();
            if (!IsActive || !Visible || Owner.SceneGraphRoot == null)
                return;

            _actorBuffer.Clear();
            Owner.SceneGraphRoot.GetAllChildActorNodes(_actorBuffer);
            CSGRebuildScheduler.Shared.Update();
            UpdateRebuildStatus();
            UpdateWorkingPlaneAndSnap();
            var plane = _workingPlane.ActivePlane;
            _overlayRenderer.Draw(ref plane, Owner.ViewPosition, !_workingPlane.IsLocked && _workingPlane.HasHover, _hasSnap, ref _snapResult);

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
                    if (subtractive)
                        DrawDashedWireBox(brush.OrientedBox, color);
                    else
                        DebugDraw.DrawWireBox(brush.OrientedBox, color, 0.0f, true);
                    if (selected)
                        DebugDraw.DrawWireBox(brush.OrientedBox, Color.White, 0.0f, false);
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
                    if (_workingPlane.TrySetHover(hit.Point, hit.Normal, preferredTangent, pointerRay, _controller.SnapIncrement, hit.Brush?.ID ?? System.Guid.Empty, hit.ComponentIndex))
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
                CSGSnapProviders.Gather(_actorBuffer, excluded, Owner.Viewport, Owner.Viewport.ContinuousViewMousePosition, threshold, _snapCandidates, _brushCorners);
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

        private Vector3 GetPreferredSurfaceTangent(ref CSGHit hit)
        {
            if (hit.Kind != CSGHitKind.Face || hit.Brush == null)
                return Vector3.Zero;
            var transform = hit.Brush.Actor.Transform;
            var localTangent = hit.ComponentIndex <= 1 ? Vector3.Up : Vector3.Right;
            return transform.LocalToWorldVector(localTangent);
        }

        private void DrawDashedWireBox(OrientedBoundingBox box, Color color)
        {
            box.GetCorners(_brushCorners);
            var viewport = Owner.Viewport;
            var viewPosition = Owner.ViewPosition;
            var viewDirection = (Vector3)Owner.ViewDirection;
            const float dashLength = 6.0f;
            const float gapLength = 4.0f;

            for (int i = 0; i < BrushBoxEdges.Length; i += 2)
            {
                var start = _brushCorners[BrushBoxEdges[i]];
                var end = _brushCorners[BrushBoxEdges[i + 1]];
                if (Vector3.Dot(start - viewPosition, viewDirection) <= 0.0f ||
                    Vector3.Dot(end - viewPosition, viewDirection) <= 0.0f)
                    continue;

                viewport.ProjectPoint(start, out var startScreen);
                viewport.ProjectPoint(end, out var endScreen);
                float projectedLength = (endScreen - startScreen).Length;
                if (projectedLength < 1.0f)
                    continue;

                var edge = end - start;
                for (float distance = 0.0f; distance < projectedLength; distance += dashLength + gapLength)
                {
                    float dashStart = distance / projectedLength;
                    float dashEnd = Mathf.Min(distance + dashLength, projectedLength) / projectedLength;
                    DebugDraw.DrawLine(start + edge * dashStart, start + edge * dashEnd, color, 0.0f, true);
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
        }

        private void OnInteractionCommitted()
        {
            _transaction.Commit(Editor.Instance?.Undo);
        }

        private void OnInteractionCancelled(EditorGizmoModeCancelReason reason)
        {
            _transaction.Invalidate(reason.ToString());
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
            _statusText = _hasCycle && _selectableHits.Count > 1
                ? $"CSG {tool}  |  Hit {_cycleIndex + 1}/{_selectableHits.Count}  |  Plane {_planeStatus}  |  {_rebuildStatus}{transaction}"
                : $"CSG {tool}  |  Plane {_planeStatus}  |  {_rebuildStatus}{transaction}";
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
