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
using FlaxEditor.Viewport;
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
        private readonly CSGSelectTool _selectTool = new CSGSelectTool();
        private readonly CSGBoxFaceEditTool _faceEditTool = new CSGBoxFaceEditTool();
        private readonly CSGTransaction _transaction = new CSGTransaction();
        private readonly List<CSGHit> _hits = new List<CSGHit>(64);
        private readonly List<CSGHit> _selectableHits = new List<CSGHit>(16);
        private readonly List<CSGHit> _cycleSignature = new List<CSGHit>(16);
        private readonly List<SceneGraphNode> _selectionBuffer = new List<SceneGraphNode>(16);
        private readonly List<ActorNode> _actorBuffer = new List<ActorNode>(64);
        private readonly List<BoxBrush> _transactionBrushes = new List<BoxBrush>(8);
        private readonly List<BoxBrush> _selectBrushes = new List<BoxBrush>(8);
        private readonly List<SceneGraphNode> _selectExclusionNodes = new List<SceneGraphNode>(8);
        private readonly List<SceneGraphNode> _directEditComponents = new List<SceneGraphNode>(16);
        private readonly List<Transform> _directEditStartTransforms = new List<Transform>(16);
        private readonly List<ViewportSnapCandidate> _snapCandidates = new List<ViewportSnapCandidate>(128);
        private readonly Vector3[] _brushCorners = new Vector3[8];
        private readonly Vector3[] _drawAdjustmentCorners = new Vector3[4];
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
        private BoxBrushNode _boxCreatedNode;
        private bool _consumeDrawMouseUp;
        private bool _selectClickAppliedOnDown;
        private bool _selectRaySnapActive;
        private bool _hasSelectSurfaceTarget;
        private Vector3 _selectSurfaceNormal;
        private int _hoveredFaceHandle = -1;
        private int _hoveredEdgeHandle = -1;
        private int _hoveredCornerHandle = -1;
        private int _hoveredDrawCornerHandle = -1;
        private int _activeDrawCornerHandle = -1;
        private int _hoveredDrawHeightDirection;
        private int _activeDrawHeightDirection;
        private BoxBrushNode _hoveredFaceBrush;
        private SceneGraphNode _activeDirectEditComponent;
        private BoxBrush _activeDirectEditBrush;
        private string _planeStatus = "World";
        private string _rebuildStatus = "Build Auto";
        private string _statusText = "CSG Authoring";

        /// <summary>
        /// Gets the reusable authoring transaction used by concrete CSG tools.
        /// </summary>
        internal CSGTransaction Transaction => _transaction;

        /// <summary>Gets whether a Select/Place click is armed but has not crossed the drag threshold.</summary>
        internal bool HasArmedSelectDrag => _selectTool.Stage == CSGSelectDragStage.Armed;

        /// <summary>Gets whether a direct brush mutation owns a CSG transaction.</summary>
        internal bool HasActiveDirectBrushMutation =>
            _selectTool.Stage == CSGSelectDragStage.Dragging || _faceEditTool.IsInteracting;

        /// <inheritdoc />
        public override bool IsControllingMouse
        {
            get
            {
                if (_selectTool.IsInteracting || _faceEditTool.IsInteracting)
                    return true;
                if (_controller.Tool == CSGTool.Draw)
                {
                    if (_boxDrawTool.Stage == CSGBoxDrawStage.Footprint)
                        return true;
                    if (_boxDrawTool.Stage == CSGBoxDrawStage.Height)
                        return _activeDrawHeightDirection != 0 || _activeDrawCornerHandle >= 0;
                }
                var transform = GetSupplementalTransformGizmo();
                return transform != null && transform.IsControllingMouse;
            }
        }

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
            if (Owner is MainEditorGizmoViewport viewport)
            {
                viewport.TransformGizmo.SupplementalActive = false;
                viewport.TransformGizmo.SupplementalTranslationSnapEnabled = false;
                viewport.TransformGizmo.Visible = true;
            }
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
            UpdateSupplementalTransformGizmo();
            _workingPlane.SetSpacing(_controller.SnapIncrement);
            _workingPlane.SetLocked(_controller.WorkingPlaneLocked);
            TryEnterSelectionContext();
            ResetDeepSelectionCycle();
            base.OnActivated();
        }

        /// <inheritdoc />
        public override void OnDeactivated()
        {
            if (Owner is MainEditorGizmoViewport viewport)
            {
                viewport.TransformGizmo.SupplementalActive = false;
                viewport.TransformGizmo.SupplementalTranslationSnapEnabled = false;
                // Draw mode hides the shared transform gizmo while CSG owns input. Restore it
                // before Object mode activates the same instance.
                viewport.TransformGizmo.Visible = true;
            }
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
            ResetDrawAdjustmentHandles();
            _selectTool.Reset();
            ResetSelectSurfacePlacement();
            _faceEditTool.Reset();
            ResetDirectEditComponents();
            _selectExclusionNodes.Clear();
            ResetDeepSelectionCycle();
            base.OnDeactivated();
        }

        /// <inheritdoc />
        public override void OnSelectionChanged(List<SceneGraphNode> newSelection)
        {
            if (!_modeActive || !_selectionEntered || _applyingSelection)
                return;
            if (_selectTool.Stage == CSGSelectDragStage.Armed)
            {
                _selectTool.Reset();
                ResetSelectSurfacePlacement();
                _selectClickAppliedOnDown = false;
            }
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
            AddSubtractiveVolumeHits(ref ray);
            var pointer = Owner.Viewport.ContinuousViewMousePosition;
            if (_controller.Tool == CSGTool.SelectPlace || _controller.Tool == CSGTool.Edit)
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
            if (_controller.Tool == CSGTool.Edit && _faceEditTool.IsInteracting)
            {
                UpdateFaceEdit();
                return true;
            }
            if (_controller.Tool == CSGTool.SelectPlace && _selectTool.IsInteracting)
            {
                try
                {
                    if (_selectTool.TryBeginDrag(location, Owner.Viewport.DpiScale))
                        BeginSelectDrag();
                    if (_selectTool.Stage == CSGSelectDragStage.Dragging)
                        UpdateSelectDrag();
                }
                catch (System.Exception ex)
                {
                    Debug.LogError("CSG direct drag failed. " + ex.Message);
                    if (_controller.HasActiveInteraction)
                        _controller.TryCancel(EditorGizmoModeCancelReason.User);
                    else
                        TryCancelArmedSelectDrag();
                }
                return true;
            }
            return _controller.Tool == CSGTool.Draw &&
                   (_boxDrawTool.Stage == CSGBoxDrawStage.Footprint ||
                    _activeDrawHeightDirection != 0 ||
                    _activeDrawCornerHandle >= 0);
        }

        /// <summary>Begins a footprint or operates the post-footprint adjustment stage.</summary>
        internal bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button != MouseButton.Left || Owner.IsAltKeyDown)
                return false;

            if (_controller.Tool == CSGTool.SelectPlace || _controller.Tool == CSGTool.Edit)
            {
                var transform = GetSupplementalTransformGizmo();
                if (transform != null && transform.ActiveMode != TransformGizmoBase.Mode.Select && transform.HoveredHandle.IsValid)
                    return false;
                if (_controller.Tool == CSGTool.SelectPlace)
                    return ArmSelectDrag(location);
            }
            if (_controller.Tool == CSGTool.Edit)
                return BeginFaceEdit();
            if (_controller.Tool != CSGTool.Draw)
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
            {
                UpdateDrawAdjustmentHover(location);
                if (_hoveredDrawHeightDirection != 0)
                {
                    var ray = Owner.MouseRay;
                    if (_boxDrawTool.BeginHeightAdjustment(_hoveredDrawHeightDirection, ref ray, Owner.ViewDirection))
                        _activeDrawHeightDirection = _hoveredDrawHeightDirection;
                    UpdateStatusText();
                    return true;
                }
                if (_hoveredDrawCornerHandle >= 0)
                {
                    _activeDrawCornerHandle = _hoveredDrawCornerHandle;
                    _boxDrawTool.BeginFootprintAdjustment(_activeDrawCornerHandle);
                    UpdateStatusText();
                    return true;
                }
                // The idle extrusion stage is deliberately non-modal. Clicking away cancels the
                // unextruded footprint instead of silently creating a default-volume brush.
                _controller.TryCancel(EditorGizmoModeCancelReason.User);
            }
            return true;
        }

        /// <summary>Locks the footprint after the initial drag.</summary>
        internal bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (_controller.Tool == CSGTool.Edit && button == MouseButton.Left && _faceEditTool.IsInteracting)
            {
                UpdateFaceEdit();
                _controller.TryCommit();
                return true;
            }
            if (_controller.Tool == CSGTool.SelectPlace && button == MouseButton.Left && _selectTool.IsInteracting)
            {
                if (_selectTool.Stage == CSGSelectDragStage.Dragging)
                {
                    UpdateSelectDrag();
                    _controller.TryCommit();
                }
                else
                {
                    _selectTool.Reset();
                    if (!_selectClickAppliedOnDown)
                        Pick();
                }
                _selectClickAppliedOnDown = false;
                return true;
            }
            if (_controller.Tool == CSGTool.Draw && button == MouseButton.Left && _boxDrawTool.Stage == CSGBoxDrawStage.Height &&
                (_activeDrawHeightDirection != 0 || _activeDrawCornerHandle >= 0))
            {
                bool completedHeightGesture = _activeDrawHeightDirection != 0;
                UpdateDrawAdjustment();
                _boxDrawTool.EndHeightAdjustment();
                _activeDrawHeightDirection = 0;
                _activeDrawCornerHandle = -1;
                UpdateDrawAdjustmentHover(location);
                UpdateStatusText();
                if (completedHeightGesture)
                    TryCommitBoxDraw();
                return true;
            }
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

        /// <summary>Cancels an armed click that has not opened a transaction.</summary>
        internal bool TryCancelArmedSelectDrag()
        {
            if (_selectTool.Stage != CSGSelectDragStage.Armed)
                return false;
            _selectTool.Reset();
            ResetSelectSurfacePlacement();
            _selectClickAppliedOnDown = false;
            UpdateStatusText();
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
            // Selection changes elsewhere in the viewport can re-enable the regular transform
            // gizmo, so enforce CSG ownership every frame (including staged Draw).
            UpdateSupplementalTransformGizmo();
            UpdateWorkingPlaneAndSnap();
            UpdateBoxDrawTool();
            var plane = _workingPlane.ActivePlane;
            AlignDrawPlaneToGrid(ref plane);
            bool showSnap = _controller.HasActiveInteraction &&
                            (_controller.Tool != CSGTool.Draw ||
                             _boxDrawTool.Stage == CSGBoxDrawStage.Footprint ||
                             _activeDrawHeightDirection != 0 || _activeDrawCornerHandle >= 0);
            var drawSnap = _snapResult;
            ProjectDrawPointToPlane(ref plane, ref drawSnap.Point);
            float snapMarkerSize = GetScreenSpaceCursorWorldSize(drawSnap.Point, plane.Spacing);
            _overlayRenderer.Draw(ref plane, Owner.ViewPosition, !_workingPlane.IsLocked && _workingPlane.HasHover, showSnap && _hasSnap, ref drawSnap, snapMarkerSize);
            DrawSelectDragFeedback();
            UpdateAndDrawFaceHandles();

            bool showSourceBrushes = (_controller.Visibility & CSGVisibility.SourceBrushes) != 0;
            for (int i = 0; i < _actorBuffer.Count; i++)
            {
                if (_actorBuffer[i] is not BoxBrushNode node)
                    continue;
                bool selected = IsBrushSelected(node);
                if (!showSourceBrushes && !selected)
                    continue;
                bool hidden = !node.IsActiveInHierarchy;
                if (hidden && (_controller.Visibility & CSGVisibility.HiddenBrushes) == 0 && !selected)
                    continue;

                var brush = (BoxBrush)node.Actor;
                if (showSourceBrushes)
                {
                    bool subtractive = brush.Mode == BrushMode.Subtractive;
                    var color = subtractive ? new Color(1.0f, 0.12f, 0.1f, 0.95f) : new Color(0.18f, 0.76f, 1.0f, 0.9f);
                    if (hidden)
                        color.A = 0.45f;
                    var xrayColor = color;
                    xrayColor.A *= 0.2f;
                    DrawDashedWireBox(brush.OrientedBox, xrayColor, false);
                    DebugDraw.DrawWireBox(brush.OrientedBox, color, 0.0f, true);
                }
                if (selected)
                {
                    if (IsBrushBodySelected(node))
                        DebugDraw.DrawBox(brush.OrientedBox, new Color(1.0f, 0.68f, 0.08f, 0.5f), 0.0f, true);
                    DrawDashedWireBox(brush.OrientedBox, new Color(1.0f, 1.0f, 1.0f, 0.22f), false);
                    DebugDraw.DrawWireBox(brush.OrientedBox, Color.White, 0.0f, true);
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
                IReadOnlyList<SceneGraphNode> excluded = _selectTool.Stage == CSGSelectDragStage.Dragging
                    ? _selectExclusionNodes
                    : _controller.HasActiveInteraction ? _selection.CSGSelection : null;
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
                UpdateDrawAdjustment();
                UpdateDrawAdjustmentHover(Owner.Viewport.ContinuousViewMousePosition);
            }
            _boxDrawTool.Draw(hoverMarkerSize);
            DrawDrawAdjustmentHandles();
            UpdateStatusText();
        }

        private void UpdateDrawAdjustment()
        {
            if (_boxDrawTool.Stage != CSGBoxDrawStage.Height)
                return;
            if (_activeDrawHeightDirection != 0)
            {
                var ray = Owner.MouseRay;
                _boxDrawTool.UpdateHeight(ref ray, Owner.ViewDirection, _controller.EffectiveSnappingEnabled, _controller.SnapIncrement);
            }
            else if (_activeDrawCornerHandle >= 0)
            {
                var plane = _workingPlane.ActivePlane;
                AlignDrawPlaneToGrid(ref plane);
                if (TryGetDrawPoint(ref plane, out var point))
                    _boxDrawTool.UpdateFootprintAdjustment(point);
            }
        }

        private void UpdateDrawAdjustmentHover(Float2 pointer)
        {
            if (_activeDrawHeightDirection != 0 || _activeDrawCornerHandle >= 0)
                return;
            _hoveredDrawHeightDirection = 0;
            _hoveredDrawCornerHandle = -1;
            if (!_boxDrawTool.TryGetAdjustmentFrame(out _, out _, out var positiveTip, out var negativeTip,
                                                    out var corner0, out var corner1, out var corner2, out var corner3))
                return;

            float threshold = 13.0f * Owner.Viewport.DpiScale;
            float bestDistance = threshold * threshold;
            _drawAdjustmentCorners[0] = corner0;
            _drawAdjustmentCorners[1] = corner1;
            _drawAdjustmentCorners[2] = corner2;
            _drawAdjustmentCorners[3] = corner3;
            for (int i = 0; i < _drawAdjustmentCorners.Length; i++)
            {
                if (!TryProjectPoint(_drawAdjustmentCorners[i], out var screen))
                    continue;
                float distance = (pointer - screen).LengthSquared;
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    _hoveredDrawCornerHandle = i;
                    _hoveredDrawHeightDirection = 0;
                }
            }

            if (TryProjectPoint(positiveTip, out var positiveScreen))
            {
                float distance = (pointer - positiveScreen).LengthSquared;
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    _hoveredDrawCornerHandle = -1;
                    _hoveredDrawHeightDirection = 1;
                }
            }
            if (TryProjectPoint(negativeTip, out var negativeScreen))
            {
                float distance = (pointer - negativeScreen).LengthSquared;
                if (distance <= bestDistance)
                {
                    _hoveredDrawCornerHandle = -1;
                    _hoveredDrawHeightDirection = -1;
                }
            }
        }

        private void DrawDrawAdjustmentHandles()
        {
            if (!_boxDrawTool.TryGetAdjustmentFrame(out var plane, out var origin, out var positiveTip, out var negativeTip,
                                                    out var corner0, out var corner1, out var corner2, out var corner3))
                return;

            float markerSize = GetScreenSpaceCursorWorldSize(origin, plane.Spacing) * 0.65f;
            var orientation = Quaternion.LookRotation(-plane.Bitangent, plane.Normal);
            var axisColor = new Color(0.35f, 0.92f, 0.28f, 1.0f);
            DebugDraw.DrawLine(negativeTip, positiveTip, axisColor, 0.0f, false);
            DrawAdjustmentArrow(positiveTip, plane.Normal, plane.Tangent, markerSize, axisColor);
            DrawAdjustmentArrow(negativeTip, -plane.Normal, plane.Tangent, markerSize, axisColor);
            DrawEditHandleCube(origin, orientation, markerSize * 0.55f, new Color(0.72f, 1.0f, 0.55f, 1.0f));
            DrawEditHandleCube(positiveTip, orientation, markerSize,
                               _activeDrawHeightDirection > 0 || _hoveredDrawHeightDirection > 0 ? Color.Yellow : axisColor);
            DrawEditHandleCube(negativeTip, orientation, markerSize,
                               _activeDrawHeightDirection < 0 || _hoveredDrawHeightDirection < 0 ? Color.Yellow : axisColor);

            _drawAdjustmentCorners[0] = corner0;
            _drawAdjustmentCorners[1] = corner1;
            _drawAdjustmentCorners[2] = corner2;
            _drawAdjustmentCorners[3] = corner3;
            for (int i = 0; i < _drawAdjustmentCorners.Length; i++)
            {
                bool active = _activeDrawCornerHandle == i || _hoveredDrawCornerHandle == i;
                DrawEditHandleCube(_drawAdjustmentCorners[i], orientation, markerSize * 0.82f,
                                   active ? Color.Yellow : new Color(0.2f, 0.72f, 0.92f, 1.0f));
            }
        }

        private static void DrawAdjustmentArrow(Vector3 tip, Vector3 direction, Vector3 tangent, float size, Color color)
        {
            var root = tip - direction * size * 1.6f;
            var wing = tangent * size * 0.7f;
            DebugDraw.DrawLine(tip, root + wing, color, 0.0f, false);
            DebugDraw.DrawLine(tip, root - wing, color, 0.0f, false);
        }

        private void ResetDrawAdjustmentHandles()
        {
            _hoveredDrawCornerHandle = -1;
            _activeDrawCornerHandle = -1;
            _hoveredDrawHeightDirection = 0;
            _activeDrawHeightDirection = 0;
        }

        private bool ArmSelectDrag(Float2 location)
        {
            if (!_modeActive || !TryEnterSelectionContext() || Owner.SceneGraphRoot == null)
                return false;

            var ray = Owner.MouseRay;
            var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
            var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                        SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                        SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
            _hitTest.Gather(Owner.SceneGraphRoot, ref ray, ref view, _hits, flags);
            AddSubtractiveVolumeHits(ref ray);
            AddBrushOutlineHits(location);

            BoxBrushNode hitBrush = null;
            Vector3 hitPoint = Vector3.Zero;
            for (int i = 0; i < _hits.Count; i++)
            {
                if (_hits[i].Kind != CSGHitKind.Brush || _hits[i].Brush?.Actor is not BoxBrush)
                    continue;
                hitBrush = _hits[i].Brush;
                hitPoint = _hits[i].Point;
                break;
            }
            if (hitBrush == null)
                return false;

            _selectClickAppliedOnDown = false;
            if (!IsBrushSelected(hitBrush))
            {
                _selection.ApplyClick(hitBrush, Owner.IsControlDown, Owner.IsShiftDown, _selectionBuffer);
                ApplySelectionBuffer(true);
                _selectClickAppliedOnDown = true;
            }

            CollectSelectedBrushes(_selectBrushes);
            // Direct placement always starts on a horizontal world plane through the grabbed point.
            // Construction-plane orientation and camera angle must not change basic object movement.
            var plane = CSGSelectTool.CreateHorizontalDragPlane(_controller.SnapIncrement, hitPoint);
            if (!_selectTool.Arm(ref plane, location, hitPoint, _selectBrushes))
                return false;
            ResetSelectSurfacePlacement();
            ResetDeepSelectionCycle();
            UpdateStatusText();
            return true;
        }

        private void BeginSelectDrag()
        {
            var dragPlane = _selectTool.Plane;
            _workingPlane.Freeze(ref dragPlane);
            IUndoAction duplicateAction = null;
            if (_selectTool.TryConsumeDuplicate(_controller.DuplicateModifierActive))
            {
                _selectionBuffer.Clear();
                var selection = _selection.CSGSelection;
                for (int i = 0; i < selection.Count; i++)
                {
                    var brushNode = selection[i] as BoxBrushNode ?? selection[i]?.ParentNode as BoxBrushNode;
                    if (brushNode != null && !_selectionBuffer.Contains(brushNode))
                        _selectionBuffer.Add(brushNode);
                }
                ApplySelectionBuffer(false);

                if (Owner.TryDuplicateForTransform(out var createdObjects, out duplicateAction))
                {
                    _selection.Observe(createdObjects);
                    CollectBrushes(createdObjects, _selectBrushes);
                    if (!_selectTool.Rebind(_selectBrushes))
                    {
                        duplicateAction.Undo();
                        duplicateAction.Dispose();
                        duplicateAction = null;
                    }
                }
            }

            _controller.BeginInteraction();
            if (duplicateAction != null)
                _transaction.RegisterPerformedAction(duplicateAction);
            RebuildSelectExclusionNodes();
            _selectRaySnapActive = Owner.IsShiftDown;
            UpdateSelectDrag();
            UpdateStatusText();
        }

        private void UpdateSelectDrag()
        {
            if (_selectTool.Stage != CSGSelectDragStage.Dragging)
                return;

            bool useRaySnapping = Owner.IsShiftDown;
            if (useRaySnapping != _selectRaySnapActive)
            {
                var rebasePlane = _selectTool.Plane;
                var rebaseRay = Owner.MouseRay;
                if (CSGWorkingPlaneService.TryIntersect(ref rebasePlane, ref rebaseRay, out var rebaseAnchor))
                    _selectTool.Rebase(ref rebasePlane, rebaseAnchor);
                _selectRaySnapActive = useRaySnapping;
                _hasSelectSurfaceTarget = false;
                UpdateStatusText();
            }

            if (useRaySnapping)
            {
                if (!TryGetSelectSurfaceTarget(out var surfaceTarget, out var surfaceNormal))
                    return;
                _hasSelectSurfaceTarget = true;
                _selectSurfaceNormal = surfaceNormal;
                var cameraUp = Vector3.Transform(Vector3.Up, Owner.ViewOrientation);
                if (_selectTool.ApplySurfaceTarget(surfaceTarget, surfaceNormal,
                                                   _controller.RayPlacementAlignment, _controller.RayPlacementFront,
                                                   _controller.EffectiveSnappingEnabled, _controller.SnapIncrement,
                                                   cameraUp, Owner.ViewDirection))
                {
                    _transaction.RecordPreview(0.0, 0);
                    UpdateStatusText();
                }
                return;
            }

            _hasSelectSurfaceTarget = false;
            var plane = _selectTool.Plane;
            var ray = Owner.MouseRay;
            if (!CSGWorkingPlaneService.TryIntersect(ref plane, ref ray, out var target))
                return;

            if (_selectTool.ApplyTarget(target, false, _controller.EffectiveSnappingEnabled, _controller.SnapIncrement))
            {
                _transaction.RecordPreview(0.0, 0);
                UpdateStatusText();
            }
        }

        private bool TryGetSelectSurfaceTarget(out Vector3 target, out Vector3 normal)
        {
            target = normal = Vector3.Zero;
            var ray = Owner.MouseRay;
            var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
            var flags = SceneGraphNode.RayCastData.FlagTypes.SkipColliders |
                        SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives |
                        SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
            _hitTest.Gather(Owner.SceneGraphRoot, ref ray, ref view, _hits, flags);
            for (int i = 0; i < _hits.Count; i++)
            {
                var hit = _hits[i];
                if (hit.Kind != CSGHitKind.Face && hit.Kind != CSGHitKind.Placement)
                    continue;
                if (hit.Brush != null && IsBrushSelected(hit.Brush))
                    continue;
                if (hit.Normal.LengthSquared <= 0.000001f)
                    continue;
                target = hit.Point;
                normal = hit.Normal;
                normal.Normalize();
                return true;
            }
            return false;
        }

        private void DrawSelectDragFeedback()
        {
            if (_selectTool.Stage != CSGSelectDragStage.Dragging)
                return;

            var plane = _selectTool.Plane;
            float markerSize = GetScreenSpaceCursorWorldSize(_selectTool.Target, plane.Spacing) * 0.65f;
            var anchor = _selectTool.Anchor + plane.Normal * Mathf.Max(0.12f, plane.Spacing * 0.012f);
            var target = _selectTool.Target + plane.Normal * Mathf.Max(0.12f, plane.Spacing * 0.012f);
            DebugDraw.DrawLine(anchor, target, new Color(1.0f, 0.78f, 0.08f, 1.0f), 0.0f, false);
            DebugDraw.DrawSphere(new BoundingSphere(anchor, markerSize * 0.35f), new Color(1.0f, 0.55f, 0.05f, 1.0f), 0.0f, false);
            DebugDraw.DrawWireSphere(new BoundingSphere(target, markerSize * 0.5f), Color.Yellow, 0.0f, false);
            if (_selectRaySnapActive && _hasSelectSurfaceTarget)
            {
                var normalEnd = target + _selectSurfaceNormal * markerSize * 2.5f;
                DebugDraw.DrawLine(target, normalEnd, new Color(0.2f, 0.95f, 1.0f, 1.0f), 0.0f, false);
                DebugDraw.DrawWireSphere(new BoundingSphere(target, markerSize * 0.7f), new Color(0.2f, 0.95f, 1.0f, 1.0f), 0.0f, false);
            }
        }

        private void ResetSelectSurfacePlacement()
        {
            _selectRaySnapActive = false;
            _hasSelectSurfaceTarget = false;
            _selectSurfaceNormal = Vector3.Zero;
        }

        private void UpdateAndDrawFaceHandles()
        {
            if (_controller.Tool != CSGTool.Edit)
            {
                _hoveredFaceHandle = -1;
                _hoveredEdgeHandle = -1;
                _hoveredCornerHandle = -1;
                _hoveredFaceBrush = null;
                return;
            }

            var selection = _selection.CSGSelection;
            var pointer = Owner.Viewport.ContinuousViewMousePosition;
            float hoverThreshold = 10.0f * Owner.Viewport.DpiScale;
            float bestDistance = hoverThreshold * hoverThreshold;
            _hoveredFaceHandle = -1;
            _hoveredEdgeHandle = -1;
            _hoveredCornerHandle = -1;
            _hoveredFaceBrush = null;

            // Prefer the smaller topology domains when handles overlap: vertex, then edge, then face.
            for (int actorIndex = 0; actorIndex < _actorBuffer.Count; actorIndex++)
            {
                if (_actorBuffer[actorIndex] is not BoxBrushNode brushNode || !IsBrushSelected(brushNode) || brushNode.Actor is not BoxBrush brush)
                    continue;
                _hoveredFaceBrush ??= brushNode;
                brush.OrientedBox.GetCorners(_brushCorners);
                UpdateHoveredBrushComponents(brushNode, pointer, ref bestDistance);
            }

            for (int actorIndex = 0; actorIndex < _actorBuffer.Count; actorIndex++)
            {
                if (_actorBuffer[actorIndex] is not BoxBrushNode brushNode || !IsBrushSelected(brushNode) || brushNode.Actor is not BoxBrush brush)
                    continue;
                DrawBrushComponentHandles(brushNode, brush, selection);
            }
        }

        private void UpdateHoveredBrushComponents(BoxBrushNode brushNode, Float2 pointer, ref float bestDistance)
        {
            if (_faceEditTool.IsInteracting)
                return;
            for (int face = 0; face < 6; face++)
            {
                Owner.Viewport.ProjectPoint(GetFaceCenter(_brushCorners, face), out var screen);
                float distance = (pointer - screen).LengthSquared;
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    _hoveredFaceBrush = brushNode;
                    _hoveredFaceHandle = face;
                    _hoveredEdgeHandle = _hoveredCornerHandle = -1;
                }
            }
            for (int edge = 0; edge < 12; edge++)
            {
                Owner.Viewport.ProjectPoint(GetEdgeCenter(_brushCorners, edge), out var screen);
                float distance = (pointer - screen).LengthSquared;
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    _hoveredFaceBrush = brushNode;
                    _hoveredEdgeHandle = edge;
                    _hoveredFaceHandle = _hoveredCornerHandle = -1;
                }
            }
            for (int corner = 0; corner < _brushCorners.Length; corner++)
            {
                Owner.Viewport.ProjectPoint(_brushCorners[corner], out var screen);
                float distance = (pointer - screen).LengthSquared;
                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    _hoveredFaceBrush = brushNode;
                    _hoveredCornerHandle = corner;
                    _hoveredFaceHandle = _hoveredEdgeHandle = -1;
                }
            }
        }

        private void DrawBrushComponentHandles(BoxBrushNode brushNode, BoxBrush brush, IReadOnlyList<SceneGraphNode> selection)
        {
            brush.OrientedBox.GetCorners(_brushCorners);
            var brushCenter = brush.Transform.LocalToWorld(brush.Center);
            float markerSize = GetScreenSpaceCursorWorldSize(brushCenter, _controller.SnapIncrement) * 0.65f;
            var orientation = brush.Transform.Orientation;
            bool interactingBrush = _activeDirectEditBrush == brush;
            for (int face = 0; face < 6; face++)
            {
                var node = brushNode.ChildNodes[face];
                bool selected = SelectionContains(selection, node);
                bool active = interactingBrush && _faceEditTool.FaceIndex == face || !_faceEditTool.IsInteracting && _hoveredFaceBrush == brushNode && _hoveredFaceHandle == face;
                var color = active ? Color.Yellow : selected ? new Color(1.0f, 0.55f, 0.08f, 1.0f) : new Color(0.35f, 0.78f, 0.32f, 1.0f);
                DrawEditHandleCube(GetFaceCenter(_brushCorners, face), orientation, markerSize, color);
            }
            for (int edge = 0; edge < 12; edge++)
            {
                var node = brushNode.ChildNodes[6 + edge];
                bool selected = SelectionContains(selection, node);
                bool active = interactingBrush && _faceEditTool.EdgeIndex == edge || !_faceEditTool.IsInteracting && _hoveredFaceBrush == brushNode && _hoveredEdgeHandle == edge;
                var color = active ? Color.Yellow : selected ? new Color(1.0f, 0.55f, 0.08f, 1.0f) : new Color(0.2f, 0.72f, 0.92f, 1.0f);
                DrawEditHandleCube(GetEdgeCenter(_brushCorners, edge), orientation, markerSize * 0.82f, color);
            }
            for (int corner = 0; corner < _brushCorners.Length; corner++)
            {
                var node = brushNode.ChildNodes[18 + corner];
                bool selected = SelectionContains(selection, node);
                bool active = interactingBrush && _faceEditTool.CornerIndex == corner || !_faceEditTool.IsInteracting && _hoveredFaceBrush == brushNode && _hoveredCornerHandle == corner;
                var color = active ? Color.Yellow : selected ? new Color(1.0f, 0.55f, 0.08f, 1.0f) : new Color(0.5f, 0.42f, 0.94f, 1.0f);
                DrawEditHandleCube(_brushCorners[corner], orientation, markerSize * 0.68f, color);
            }
        }

        private bool BeginFaceEdit()
        {
            if ((_hoveredFaceHandle < 0 && _hoveredEdgeHandle < 0 && _hoveredCornerHandle < 0) || _hoveredFaceBrush?.Actor is not BoxBrush brush)
                return false;
            SceneGraphNode component = _hoveredCornerHandle >= 0
                ? _hoveredFaceBrush.ChildNodes[18 + _hoveredCornerHandle]
                : _hoveredEdgeHandle >= 0
                    ? _hoveredFaceBrush.ChildNodes[6 + _hoveredEdgeHandle]
                    : _hoveredFaceBrush.ChildNodes[_hoveredFaceHandle];
            bool modifiedSelection = Owner.IsControlDown || Owner.IsShiftDown || !SelectionContains(_selection.CSGSelection, component);
            if (modifiedSelection)
            {
                _selection.ApplyClick(component, Owner.IsControlDown, Owner.IsShiftDown, _selectionBuffer);
                ApplySelectionBuffer(true);
            }
            if (!SelectionContains(_selection.CSGSelection, component))
                return true;
            CaptureDirectEditComponents(component, brush);
            bool began;
            if (_hoveredCornerHandle >= 0)
                began = _faceEditTool.BeginCorner(brush, _hoveredCornerHandle, Owner.MouseRay, Owner.ViewDirection);
            else if (_hoveredEdgeHandle >= 0)
                began = _faceEditTool.BeginEdge(brush, _hoveredEdgeHandle, Owner.MouseRay, Owner.ViewDirection);
            else
                began = _faceEditTool.Begin(brush, _hoveredFaceHandle, Owner.MouseRay);
            if (!began)
            {
                ResetDirectEditComponents();
                return false;
            }
            _controller.BeginInteraction();
            UpdateStatusText();
            return true;
        }

        private void UpdateFaceEdit()
        {
            if (_faceEditTool.Update(Owner.MouseRay, _controller.EffectiveSnappingEnabled, _controller.SnapIncrement))
            {
                ApplyDirectEditComponentGroup();
                _transaction.RecordPreview(0.0, 0);
                UpdateStatusText();
            }
        }

        private void CaptureDirectEditComponents(SceneGraphNode active, BoxBrush brush)
        {
            ResetDirectEditComponents();
            _activeDirectEditComponent = active;
            _activeDirectEditBrush = brush;
            var selection = _selection.CSGSelection;
            for (int i = 0; i < selection.Count; i++)
            {
                var node = selection[i];
                var kind = node?.CSGViewportSelection ?? CSGViewportSelectionKind.None;
                if (kind != CSGViewportSelectionKind.Face && kind != CSGViewportSelectionKind.Edge && kind != CSGViewportSelectionKind.Vertex)
                    continue;
                _directEditComponents.Add(node);
                _directEditStartTransforms.Add(node.Transform);
            }
        }

        private void ApplyDirectEditComponentGroup()
        {
            if (_activeDirectEditBrush == null || _directEditComponents.Count != _directEditStartTransforms.Count)
                return;
            var worldDelta = _activeDirectEditBrush.Transform.LocalToWorldVector(_faceEditTool.DeltaVector);
            for (int i = 0; i < _directEditComponents.Count; i++)
            {
                var node = _directEditComponents[i];
                if (node == null || node == _activeDirectEditComponent)
                    continue;
                var transform = _directEditStartTransforms[i];
                transform.Translation += worldDelta;
                node.Transform = transform;
            }
        }

        private void ResetDirectEditComponents()
        {
            _directEditComponents.Clear();
            _directEditStartTransforms.Clear();
            _activeDirectEditComponent = null;
            _activeDirectEditBrush = null;
        }

        private static bool SelectionContains(IReadOnlyList<SceneGraphNode> selection, SceneGraphNode node)
        {
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] == node)
                    return true;
            }
            return false;
        }

        private static Vector3 GetFaceCenter(Vector3[] corners, int face)
        {
            int offset = face * 4;
            int i0 = FaceCornerIndices[offset];
            int i1 = FaceCornerIndices[offset + 1];
            int i2 = FaceCornerIndices[offset + 2];
            int i3 = FaceCornerIndices[offset + 3];
            return (corners[i0] + corners[i1] + corners[i2] + corners[i3]) * 0.25f;
        }

        private static Vector3 GetEdgeCenter(Vector3[] corners, int edge)
        {
            int offset = edge * 2;
            return (corners[BrushBoxEdges[offset]] + corners[BrushBoxEdges[offset + 1]]) * 0.5f;
        }

        private static readonly int[] FaceCornerIndices =
        {
            0, 1, 4, 5,
            2, 3, 6, 7,
            0, 1, 3, 2,
            4, 5, 7, 6,
            0, 3, 4, 7,
            1, 2, 5, 6,
        };

        private static void DrawEditHandleCube(Vector3 point, Quaternion orientation, float size, Color color)
        {
            var box = new OrientedBoundingBox(new Vector3(-size * 0.5f), new Vector3(size * 0.5f))
            {
                Transformation = new Transform(point, orientation),
            };
            DebugDraw.DrawBox(box, color, 0.0f, false);
            DebugDraw.DrawWireBox(box, Color.White, 0.0f, false);
        }

        private void CollectSelectedBrushes(List<BoxBrush> result)
        {
            result.Clear();
            var selection = _selection.CSGSelection;
            for (int i = 0; i < selection.Count; i++)
            {
                var brushNode = selection[i] as BoxBrushNode ?? selection[i]?.ParentNode as BoxBrushNode;
                if (brushNode?.Actor is BoxBrush brush && !result.Contains(brush))
                    result.Add(brush);
            }
        }

        private static void CollectBrushes(IReadOnlyList<SceneGraphNode> nodes, List<BoxBrush> result)
        {
            result.Clear();
            for (int i = 0; nodes != null && i < nodes.Count; i++)
            {
                var brushNode = nodes[i] as BoxBrushNode ?? nodes[i]?.ParentNode as BoxBrushNode;
                if (brushNode?.Actor is BoxBrush brush && !result.Contains(brush))
                    result.Add(brush);
            }
        }

        private void RebuildSelectExclusionNodes()
        {
            _selectExclusionNodes.Clear();
            for (int i = 0; i < _selectTool.Brushes.Count; i++)
            {
                var node = SceneGraphFactory.FindNode(_selectTool.Brushes[i].ID) as BoxBrushNode ?? SceneGraphFactory.GetNode(_selectTool.Brushes[i].ID) as BoxBrushNode;
                if (node != null)
                    _selectExclusionNodes.Add(node);
            }
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
            if (sceneEditing == null || !sceneEditing.TrySpawnForCSGTransaction(brush, out var node, out var action))
            {
                FlaxEngine.Object.Destroy(brush);
                return false;
            }

            _transaction.Touch(brush);
            _transaction.RegisterPerformedAction(action);
            _boxCreated = true;
            _boxCreatedNode = node as BoxBrushNode;
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

        private void AddSubtractiveVolumeHits(ref Ray ray)
        {
            if ((_controller.Visibility & CSGVisibility.SourceBrushes) == 0)
                return;

            _actorBuffer.Clear();
            Owner.SceneGraphRoot.GetAllChildActorNodes(_actorBuffer);
            for (int actorIndex = 0; actorIndex < _actorBuffer.Count; actorIndex++)
            {
                if (_actorBuffer[actorIndex] is not BoxBrushNode node || node.Actor is not BoxBrush brush || brush.Mode != BrushMode.Subtractive)
                    continue;
                bool hidden = !node.IsActiveInHierarchy;
                if (hidden && (_controller.Visibility & CSGVisibility.HiddenBrushes) == 0)
                    continue;

                bool alreadyHit = false;
                for (int hitIndex = 0; hitIndex < _hits.Count; hitIndex++)
                {
                    if (_hits[hitIndex].Brush == node)
                    {
                        alreadyHit = true;
                        break;
                    }
                }
                if (alreadyHit)
                    continue;

                var box = brush.OrientedBox;
                if (!box.Intersects(ref ray, out Real distance) || distance < 0.0f)
                    continue;
                _hits.Add(new CSGHit
                {
                    Node = node,
                    Brush = node,
                    Kind = CSGHitKind.Brush,
                    ComponentIndex = -1,
                    Distance = distance,
                    Normal = Vector3.Zero,
                    Point = ray.Position + ray.Direction * distance,
                });
            }
            _hitTest.Sort(_hits);
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

            DrawBoxMeasurements();

            var font = Style.Current.FontSmall;
            float width = Mathf.Clamp(font.MeasureText(_statusText).X + 18.0f, 196.0f, Mathf.Max(196.0f, Owner.Viewport.Width - 20.0f));
            var rect = new Rectangle(10.0f, ViewportWidgetsContainer.WidgetsHeight + 14.0f, width, 22.0f);
            Render2D.FillRectangle(rect, new Color(0.08f, 0.11f, 0.16f, 0.88f));
            Render2D.DrawText(font, _statusText, rect, Color.White, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap);
        }

        private void DrawBoxMeasurements()
        {
            if (_controller.Tool == CSGTool.Draw &&
                _boxDrawTool.TryGetMeasurementFrame(out var widthStart, out var widthEnd, out var depthStart, out var depthEnd, out var heightStart, out var heightEnd, out var dimensions))
            {
                DrawDimensionGuide(widthStart, widthEnd, $"X  {FormatDimension((float)dimensions.X)}", new Color(0.92f, 0.28f, 0.22f, 1.0f), 0.0f);
                DrawDimensionGuide(depthStart, depthEnd, $"Z  {FormatDimension((float)dimensions.Z)}", new Color(0.24f, 0.55f, 1.0f, 1.0f), 0.0f);
                if (_boxDrawTool.Stage == CSGBoxDrawStage.Height)
                    DrawDimensionGuide(heightStart, heightEnd, $"Y  {FormatDimension((float)dimensions.Y)}", new Color(0.32f, 0.9f, 0.36f, 1.0f), 0.0f);
                return;
            }

            if (_controller.Tool != CSGTool.Edit || _hoveredFaceBrush?.Actor is not BoxBrush brush)
                return;
            brush.OrientedBox.GetCorners(_brushCorners);
            float sizeX = (float)brush.Transform.LocalToWorldVector(Vector3.Right * brush.Size.X).Length;
            float sizeY = (float)brush.Transform.LocalToWorldVector(Vector3.Up * brush.Size.Y).Length;
            float sizeZ = (float)brush.Transform.LocalToWorldVector(Vector3.Forward * brush.Size.Z).Length;
            DrawDimensionGuide(_brushCorners[6], _brushCorners[5], $"X  {FormatDimension(sizeX)}", new Color(0.92f, 0.28f, 0.22f, 1.0f), 0.0f);
            DrawDimensionGuide(_brushCorners[6], _brushCorners[2], $"Y  {FormatDimension(sizeY)}", new Color(0.32f, 0.9f, 0.36f, 1.0f), 0.0f);
            DrawDimensionGuide(_brushCorners[6], _brushCorners[7], $"Z  {FormatDimension(sizeZ)}", new Color(0.24f, 0.55f, 1.0f, 1.0f), 0.0f);
        }

        private void DrawDimensionGuide(Vector3 worldStart, Vector3 worldEnd, string label, Color color, float sideOffset)
        {
            if (!TryProjectPoint(worldStart, out var start) || !TryProjectPoint(worldEnd, out var end))
                return;
            var line = end - start;
            float length = line.Length;
            if (length < 8.0f)
                return;
            var direction = line / length;
            var normal = new Float2(-direction.Y, direction.X);
            start += normal * sideOffset;
            end += normal * sideOffset;

            var shadow = Color.Black.AlphaMultiplied(0.72f);
            Render2D.DrawLine(start, end, shadow, 4.0f);
            Render2D.DrawLine(start, end, color, 1.7f);
            DrawDimensionArrow(start, direction, normal, color);
            DrawDimensionArrow(end, -direction, normal, color);

            var font = Style.Current.FontSmall;
            var textSize = font.MeasureText(label);
            var pillSize = textSize + new Float2(14.0f, 6.0f);
            var center = (start + end) * 0.5f + normal * (sideOffset < 0.0f ? -2.0f : 2.0f);
            var rect = new Rectangle(center - pillSize * 0.5f, pillSize);
            StyleRendering.FillRoundedRectangle(rect, new Color(0.06f, 0.08f, 0.12f, 0.94f), 3.0f);
            Render2D.DrawText(font, label, rect, Color.White, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap);
        }

        private static void DrawDimensionArrow(Float2 tip, Float2 direction, Float2 normal, Color color)
        {
            var left = tip + direction * 8.0f + normal * 4.0f;
            var right = tip + direction * 8.0f - normal * 4.0f;
            Render2D.DrawLine(tip, left, Color.Black.AlphaMultiplied(0.72f), 4.0f);
            Render2D.DrawLine(tip, right, Color.Black.AlphaMultiplied(0.72f), 4.0f);
            Render2D.DrawLine(tip, left, color, 1.7f);
            Render2D.DrawLine(tip, right, color, 1.7f);
        }

        private bool TryProjectPoint(Vector3 worldPoint, out Float2 screenPoint)
        {
            screenPoint = Float2.Zero;
            if (Vector3.Dot(worldPoint - Owner.ViewPosition, (Vector3)Owner.ViewDirection) <= 0.0f)
                return false;
            Owner.Viewport.ProjectPoint(worldPoint, out screenPoint);
            return screenPoint.X > -10000.0f && screenPoint.Y > -10000.0f;
        }

        private static string FormatDimension(float value)
        {
            value = Mathf.Abs(value);
            return value >= 100.0f ? $"{value * 0.01f:0.##} m" : $"{value:0.##} cm";
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
            if (_controller.Tool != CSGTool.SelectPlace && _selectTool.Stage == CSGSelectDragStage.Armed)
            {
                _selectTool.Reset();
                _selectClickAppliedOnDown = false;
            }
            if (_boxDrawTool.IsInteracting && _controller.Operation != _lastControllerOperation)
                _drawOperationOverride = _controller.Operation;
            _lastControllerOperation = _controller.Operation;
            UpdateSupplementalTransformGizmo();
            _workingPlane.SetSpacing(_controller.SnapIncrement);
            _workingPlane.SetLocked(_controller.WorkingPlaneLocked);
            if (_controller.HasActiveInteraction && !_workingPlane.IsFrozen)
                _workingPlane.Freeze();
            else if (!_controller.HasActiveInteraction && _workingPlane.IsFrozen)
                _workingPlane.Unfreeze();
            ResetDeepSelectionCycle();
        }

        /// <summary>Synchronizes visibility of the supplemental Select-mode transform handles.</summary>
        internal void RefreshSupplementalTransformGizmo()
        {
            UpdateSupplementalTransformGizmo();
        }

        private TransformGizmo GetSupplementalTransformGizmo()
        {
            if (!_modeActive || (_controller.Tool != CSGTool.SelectPlace && _controller.Tool != CSGTool.Edit) || Owner is not MainEditorGizmoViewport viewport)
                return null;
            return viewport.TransformGizmo;
        }

        private void UpdateSupplementalTransformGizmo()
        {
            if (Owner is not MainEditorGizmoViewport viewport)
                return;
            bool enabled = _modeActive && Visible && (_controller.Tool == CSGTool.SelectPlace || _controller.Tool == CSGTool.Edit);
            viewport.TransformGizmo.Visible = enabled;
            viewport.TransformGizmo.SupplementalActive = enabled;
            viewport.TransformGizmo.SupplementalTranslationSnapEnabled = enabled && _controller.EffectiveSnappingEnabled;
            viewport.TransformGizmo.SupplementalTranslationSnapValue = _controller.SnapIncrement;
            if (enabled && (_controller.Tool == CSGTool.Edit || viewport.TransformGizmo.ActiveMode == TransformGizmoBase.Mode.Select))
                viewport.TransformGizmo.ActiveMode = TransformGizmoBase.Mode.Translate;
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
            _boxCreatedNode = null;
        }

        private void OnInteractionCommitted()
        {
            bool createdBox = _controller.Tool == CSGTool.Draw && _boxDrawTool.IsInteracting;
            try
            {
                if (createdBox && !CreateBoxBrush())
                {
                    _transaction.Rollback("Invalid box placement");
                    return;
                }
                string action = _controller.Tool == CSGTool.Draw ? "Create CSG Box" : _controller.Tool == CSGTool.Edit ? "Resize CSG Box" : "Edit CSG";
                _transaction.Commit(Editor.Instance?.Undo, action);
                // Extrusion release finalizes the real brush and immediately exposes topology
                // editing. There is no extra confirmation click between creation and editing.
                if (createdBox && _boxCreatedNode != null)
                    _controller.SetTool(CSGTool.Edit);
            }
            catch (System.Exception ex)
            {
                Debug.LogError("CSG interaction commit failed. " + ex.Message);
                _transaction.Rollback("Commit failed");
            }
            finally
            {
                _boxDrawTool.Reset();
                ResetDrawAdjustmentHandles();
                _selectTool.Reset();
                ResetSelectSurfacePlacement();
                _faceEditTool.Reset();
                ResetDirectEditComponents();
                _selectExclusionNodes.Clear();
                _selectClickAppliedOnDown = false;
                _drawOperationOverride = null;
                _consumeDrawMouseUp = false;
                _boxCreatedNode = null;
            }
        }

        private void OnInteractionCancelled(EditorGizmoModeCancelReason reason)
        {
            _transaction.Invalidate(reason.ToString());
            _boxDrawTool.Reset();
            ResetDrawAdjustmentHandles();
            _selectTool.Reset();
            ResetSelectSurfacePlacement();
            _faceEditTool.Reset();
            ResetDirectEditComponents();
            _selectExclusionNodes.Clear();
            _selectClickAppliedOnDown = false;
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
            string move = _selectTool.Stage == CSGSelectDragStage.Armed
                ? "  |  Drag armed (XZ; hold Shift for surface placement)"
                : _selectTool.Stage == CSGSelectDragStage.Dragging
                    ? _selectRaySnapActive
                        ? $"  |  Shift Surface  X {_selectTool.Delta.X:0.###}  Y {_selectTool.Delta.Y:0.###}  Z {_selectTool.Delta.Z:0.###}"
                        : $"  |  Move X {_selectTool.Delta.X:0.###}  Z {_selectTool.Delta.Z:0.###}"
                    : string.Empty;
            string resize = _faceEditTool.IsInteracting
                ? _faceEditTool.CornerIndex >= 0
                    ? $"  |  Vertex {_faceEditTool.CornerIndex + 1}  Offset {_faceEditTool.DeltaVector}"
                    : _faceEditTool.EdgeIndex >= 0
                        ? $"  |  Edge {_faceEditTool.EdgeIndex + 1}  Offset {_faceEditTool.DeltaVector}"
                        : $"  |  Face {_faceEditTool.FaceIndex + 1}  Offset {_faceEditTool.Delta:0.###}"
                : string.Empty;
            _statusText = _hasCycle && _selectableHits.Count > 1
                ? $"CSG {tool}  |  Hit {_cycleIndex + 1}/{_selectableHits.Count}  |  Plane {_planeStatus}  |  {_rebuildStatus}{draw}{move}{resize}{transaction}"
                : $"CSG {tool}  |  Plane {_planeStatus}  |  {_rebuildStatus}{draw}{move}{resize}{transaction}";
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
