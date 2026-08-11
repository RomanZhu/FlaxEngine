// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.SceneGraph;
using FlaxEditor.Viewport;
using FlaxEngine.GUI;

namespace FlaxEngine.Gizmo;

/// <summary>
/// Optional contract for gizmos that constrain viewport marquee selection to a custom domain.
/// </summary>
public interface IViewportRubberBandSelection
{
    /// <summary>
    /// Gets whether an actor node can participate in the current marquee selection.
    /// </summary>
    bool CanSelectWithRubberBand(ActorNode node);

    /// <summary>
    /// Resolves the scene graph node that should be selected for a marquee hit.
    /// </summary>
    SceneGraphNode ResolveRubberBandSelection(ActorNode node);
}

/// <summary>
/// Class for adding viewport rubber band selection.
/// </summary>
public sealed class ViewportRubberBandSelector
{
    private bool _isMosueCaptured;
    private bool _isRubberBandSpanning;
    private bool _tryStartRubberBand;
    private Float2 _cachedStartingMousePosition;
    private Rectangle _rubberBandRect;
    private Rectangle _lastRubberBandRect;
    private SceneGraphNode[] _rubberBandSelectionBefore;
    private List<ActorNode> _nodesCache;
    private List<SceneGraphNode> _hitsCache;
    private IGizmoOwner _owner;

    /// <summary>
    /// Constructs a rubber band selector with a designated gizmo owner.
    /// </summary>
    /// <param name="owner">The gizmo owner.</param>
    public ViewportRubberBandSelector(IGizmoOwner owner)
    {
        _owner = owner;
    }

    /// <summary>
    /// Triggers the start of a rubber band selection.
    /// </summary>
    /// <returns>True if selection started, otherwise false.</returns>
    public bool TryStartingRubberBandSelection(Float2 mousePosition)
    {
        if (!_isRubberBandSpanning && CanStartRubberBand())
        {
            _tryStartRubberBand = true;
            _cachedStartingMousePosition = mousePosition;
            return true;
        }
        return false;
    }

    /// <summary>
    /// Release the rubber band selection.
    /// </summary>
    /// <returns>Returns true if rubber band is currently spanning</returns>
    public bool ReleaseRubberBandSelection()
    {
        return EndRubberBandSelection();
    }

    /// <summary>
    /// Tries to create a rubber band selection.
    /// </summary>
    /// <param name="canStart">Whether the creation can start.</param>
    /// <param name="mousePosition">The current mouse position.</param>
    public void TryCreateRubberBand(bool canStart, Float2 mousePosition)
    {
        canStart &= CanStartRubberBand();

        if (_tryStartRubberBand && !canStart)
        {
            _tryStartRubberBand = false;
            return;
        }

        if (_isRubberBandSpanning && !canStart)
        {
            EndRubberBandSelection();
            return;
        }

        if (_tryStartRubberBand && canStart)
        {
            var delta = mousePosition - _cachedStartingMousePosition;
            if (Mathf.Abs(delta.X) > 0.1f || Mathf.Abs(delta.Y) > 0.1f)
            {
                _isRubberBandSpanning = true;
                var currentSelection = _owner.SceneGraphRoot.SceneContext.Selection;
                if (_owner.Gizmos.Active is IViewportRubberBandSelection selectionFilter)
                {
                    var filteredSelection = new List<SceneGraphNode>(currentSelection.Count);
                    for (int i = 0; i < currentSelection.Count; i++)
                    {
                        var node = currentSelection[i];
                        var actorNode = node as ActorNode ?? node?.ParentNode as ActorNode;
                        if (actorNode != null && selectionFilter.CanSelectWithRubberBand(actorNode))
                            filteredSelection.Add(node);
                    }
                    _rubberBandSelectionBefore = filteredSelection.ToArray();
                }
                else
                {
                    _rubberBandSelectionBefore = currentSelection.ToArray();
                }
                _rubberBandRect = new Rectangle(_cachedStartingMousePosition, Float2.Zero);
                _lastRubberBandRect = Rectangle.Empty;
                _tryStartRubberBand = false;
            }
        }
        else if (_isRubberBandSpanning && _owner.Gizmos.Active != null && !_owner.Gizmos.Active.IsControllingMouse && !_owner.IsRightMouseButtonDown)
        {
            _rubberBandRect.Width = mousePosition.X - _cachedStartingMousePosition.X;
            _rubberBandRect.Height = mousePosition.Y - _cachedStartingMousePosition.Y;
            if (_lastRubberBandRect != _rubberBandRect)
            {
                if (!_isMosueCaptured)
                {
                    _isMosueCaptured = true;
                    _owner.Viewport.StartMouseCapture();
                }
                UpdateRubberBand();
            }
        }
    }

    private bool CanStartRubberBand()
    {
        var activeGizmo = _owner.Gizmos.Active;
        if (activeGizmo == null || activeGizmo.IsControllingMouse || _owner.IsRightMouseButtonDown)
            return false;
        if (activeGizmo is TransformGizmoBase transformGizmo && transformGizmo.ActiveAxis != TransformGizmoBase.Axis.None)
            return false;
        return true;
    }

    private struct ViewportProjection
    {
        private Matrix _viewProjection;
        private BoundingFrustum _frustum;
        private Viewport _viewport;
        private Vector3 _origin;

        public void Init(EditorViewport editorViewport)
        {
            // Inline EditorViewport.ProjectPoint to save on calculation for large set of points
            _viewport = new Viewport(0, 0, editorViewport.Width, editorViewport.Height);
            _frustum = editorViewport.ViewFrustum;
            _viewProjection = _frustum.Matrix;
            _origin = editorViewport.Task.View.Origin;
        }

        public void ProjectPoint(Vector3 worldSpaceLocation, out Float2 viewportSpaceLocation)
        {
            worldSpaceLocation -= _origin;
            _viewport.Project(ref worldSpaceLocation, ref _viewProjection, out var projected);
            viewportSpaceLocation = new Float2((float)projected.X, (float)projected.Y);
        }

        public ContainmentType FrustumCull(ref BoundingBox bounds)
        {
            bounds.Minimum -= _origin;
            bounds.Maximum -= _origin;
            return _frustum.Contains(ref bounds);
        }
    }

    private void UpdateRubberBand()
    {
        Profiler.BeginEvent("UpdateRubberBand");

        // Select rubberbanded rect actor nodes
        var adjustedRect = _rubberBandRect;
        _lastRubberBandRect = _rubberBandRect;
        if (adjustedRect.Width < 0 || adjustedRect.Height < 0)
        {
            // Make sure we have a well-formed rectangle i.e. size is positive and X/Y is upper left corner
            var size = adjustedRect.Size;
            adjustedRect.X = Mathf.Min(adjustedRect.X, adjustedRect.X + adjustedRect.Width);
            adjustedRect.Y = Mathf.Min(adjustedRect.Y, adjustedRect.Y + adjustedRect.Height);
            size.X = Mathf.Abs(size.X);
            size.Y = Mathf.Abs(size.Y);
            adjustedRect.Size = size;
        }

        // Get hits from graph nodes
        if (_nodesCache == null)
            _nodesCache = new List<ActorNode>();
        else
            _nodesCache.Clear();
        var nodes = _nodesCache;
        _owner.SceneGraphRoot.GetAllChildActorNodes(nodes);
        if (_hitsCache == null)
            _hitsCache = new List<SceneGraphNode>();
        else
            _hitsCache.Clear();
        var hits = _hitsCache;
        var selectionFilter = _owner.Gizmos.Active as IViewportRubberBandSelection;

        // Process all nodes
        var projection = new ViewportProjection();
        projection.Init(_owner.Viewport);
        foreach (var node in nodes)
        {
            // Skip actors that cannot be selected
            if (!node.CanSelectInViewport || (selectionFilter != null && !selectionFilter.CanSelectWithRubberBand(node)))
                continue;
            var a = node.Actor;

            // Skip actor if outside of view frustum
            var actorBox = a.EditorBox;
            if (projection.FrustumCull(ref actorBox) == ContainmentType.Disjoint)
                continue;

            // Get valid selection points
            var points = node.GetActorSelectionPoints();
            if (LoopOverPoints(points, ref adjustedRect, ref projection))
            {
                SceneGraphNode hit;
                if (selectionFilter != null)
                    hit = selectionFilter.ResolveRubberBandSelection(node);
                else if (_owner.Gizmos.Active is TransformGizmo transformGizmo)
                    hit = transformGizmo.ResolveSelectionTarget(node, _owner.Viewport.Task.View.Mode, _owner is not PrefabWindowViewport);
                else if (a.HasPrefabLink && _owner is not PrefabWindowViewport)
                    hit = _owner.SceneGraphRoot.Find(a.GetPrefabRoot());
                else
                    hit = node;
                if (hit != null && !hits.Contains(hit))
                    hits.Add(hit);
            }
        }

        // Process selection
        if (_owner.IsControlDown)
        {
            var newSelection = GetRubberBandSelectionBefore();
            for (int i = newSelection.Count - 1; i >= 0; i--)
            {
                if (!hits.Contains(newSelection[i]))
                    newSelection.RemoveAt(i);
            }
            _owner.Select(newSelection, false);
        }
        else if (_owner.IsShiftDown)
        {
            var newSelection = GetRubberBandSelectionBefore();
            foreach (var hit in hits)
            {
                if (newSelection.Contains(hit))
                    newSelection.Remove(hit);
                else
                    newSelection.Add(hit);
            }
            _owner.Select(newSelection, false);
        }
        else
        {
            _owner.Select(hits, false);
        }

        Profiler.EndEvent();
    }

    private bool EndRubberBandSelection()
    {
        if (_isMosueCaptured)
        {
            _isMosueCaptured = false;
            _owner.Viewport.EndMouseCapture();
        }

        var wasSpanning = _isRubberBandSpanning;
        _tryStartRubberBand = false;
        _isRubberBandSpanning = false;

        if (wasSpanning)
            CommitRubberBandSelectionChange();
        _rubberBandSelectionBefore = null;
        return wasSpanning;
    }

    private void CommitRubberBandSelectionChange()
    {
        var before = _rubberBandSelectionBefore;
        if (before == null)
            return;

        var after = _owner.SceneGraphRoot.SceneContext.Selection.ToArray();
        if (AreSelectionsEqual(before, after))
            return;

        Select(before, false);
        Select(after, true);
    }

    private List<SceneGraphNode> GetRubberBandSelectionBefore()
    {
        return GetValidSelection(_rubberBandSelectionBefore);
    }

    private void Select(SceneGraphNode[] nodes, bool recordUndo)
    {
        _owner.Select(GetValidSelection(nodes), recordUndo);
    }

    private static List<SceneGraphNode> GetValidSelection(SceneGraphNode[] nodes)
    {
        var result = new List<SceneGraphNode>(nodes?.Length ?? 0);
        if (nodes != null)
        {
            for (int i = 0; i < nodes.Length; i++)
            {
                if (nodes[i] != null)
                    result.Add(nodes[i]);
            }
        }
        return result;
    }

    private static bool AreSelectionsEqual(SceneGraphNode[] a, SceneGraphNode[] b)
    {
        if (a == b)
            return true;
        if (a == null || b == null || a.Length != b.Length)
            return false;
        for (int i = 0; i < a.Length; i++)
        {
            if (a[i] != b[i])
                return false;
        }
        return true;
    }

    private bool LoopOverPoints(Vector3[] points, ref Rectangle adjustedRect, ref ViewportProjection projection)
    {
        Profiler.BeginEvent("LoopOverPoints");
        bool containsAllPoints = points.Length != 0;
        for (int i = 0; i < points.Length; i++)
        {
            projection.ProjectPoint(points[i], out var loc);
            if (!adjustedRect.Contains(loc))
            {
                containsAllPoints = false;
                break;
            }
        }
        Profiler.EndEvent();
        return containsAllPoints;
    }

    /// <summary>
    /// Draws the ruber band during owner viewport UI drawing.
    /// </summary>
    public void Draw()
    {
        if (!_isRubberBandSpanning)
            return;
        var style = Style.Current;
        var selectionBorder = style.SelectionBorder;
        Render2D.FillRectangle(_rubberBandRect, selectionBorder.AlphaMultiplied(0.18f));
        Render2D.DrawRectangle(_rubberBandRect, selectionBorder);
    }

    /// <summary>
    /// Immediately stops the rubber band.
    /// </summary>
    /// <returns>True if rubber band was active before stopping.</returns>
    public bool StopRubberBand()
    {
        var wasActive = _tryStartRubberBand || _isRubberBandSpanning;
        EndRubberBandSelection();
        return wasActive;
    }
}
