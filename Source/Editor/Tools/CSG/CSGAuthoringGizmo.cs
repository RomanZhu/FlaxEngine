// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.GUI;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG.HitTesting;
using FlaxEditor.Tools.CSG.Selection;
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
        private readonly List<CSGHit> _hits = new List<CSGHit>(64);
        private readonly List<CSGHit> _selectableHits = new List<CSGHit>(16);
        private readonly List<CSGHit> _cycleSignature = new List<CSGHit>(16);
        private readonly List<SceneGraphNode> _selectionBuffer = new List<SceneGraphNode>(16);
        private readonly List<ActorNode> _actorBuffer = new List<ActorNode>(64);
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
        private string _statusText = "CSG Authoring";

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
        }

        /// <inheritdoc />
        public override void Destroy()
        {
            _controller.Changed -= OnControllerChanged;
            base.Destroy();
        }

        /// <inheritdoc />
        public override void OnActivated()
        {
            _modeActive = true;
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
            if (!IsActive || !Visible || Owner.SceneGraphRoot == null || (_controller.Visibility & CSGVisibility.SourceBrushes) == 0)
                return;

            _actorBuffer.Clear();
            Owner.SceneGraphRoot.GetAllChildActorNodes(_actorBuffer);
            for (int i = 0; i < _actorBuffer.Count; i++)
            {
                if (_actorBuffer[i] is not BoxBrushNode node)
                    continue;
                bool hidden = !node.IsActiveInHierarchy;
                if (hidden && (_controller.Visibility & CSGVisibility.HiddenBrushes) == 0)
                    continue;

                var brush = (BoxBrush)node.Actor;
                bool selected = IsBrushSelected(node);
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

            var rect = new Rectangle(10.0f, ViewportWidgetsContainer.WidgetsHeight + 14.0f, 196.0f, 22.0f);
            Render2D.FillRectangle(rect, new Color(0.08f, 0.11f, 0.16f, 0.88f));
            Render2D.DrawText(Style.Current.FontSmall, _statusText, rect, Color.White, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap);
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

        private static bool HasSameHitSignature(List<CSGHit> current, List<CSGHit> previous)
        {
            if (current.Count != previous.Count)
                return false;
            for (int i = 0; i < current.Count; i++)
            {
                var a = current[i];
                var b = previous[i];
                if (a.Node != b.Node || a.Kind != b.Kind || a.ComponentIndex != b.ComponentIndex ||
                    !Mathf.NearEqual((float)a.Distance, (float)b.Distance) || !Vector3.NearEqual(a.Normal, b.Normal))
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
            ResetDeepSelectionCycle();
        }

        private void UpdateStatusText()
        {
            string tool = _controller.Tool == CSGTool.SelectPlace ? "Select / Place" : _controller.Tool.ToString();
            _statusText = _hasCycle && _selectableHits.Count > 1
                ? $"CSG {tool}  |  Hit {_cycleIndex + 1}/{_selectableHits.Count}"
                : $"CSG {tool}";
        }
    }
}
