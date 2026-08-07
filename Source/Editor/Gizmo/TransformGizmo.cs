// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    /// <summary>
    /// The most import gizmo tool used to move, rotate, scale and select scene objects in editor viewport.
    /// </summary>
    /// <seealso cref="TransformGizmoBase" />
    [HideInEditor]
    public class TransformGizmo : TransformGizmoBase
    {
        /// <summary>
        /// Applies scale to the selected objects pool.
        /// </summary>
        /// <param name="selection">The selected objects pool.</param>
        /// <param name="translationDelta">The translation delta.</param>
        /// <param name="rotationDelta">The rotation delta.</param>
        /// <param name="scaleDelta">The scale delta.</param>
        public delegate void ApplyTransformationDelegate(List<SceneGraphNode> selection, ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta);

        private readonly List<SceneGraphNode> _selection = new List<SceneGraphNode>();
        private readonly List<SceneGraphNode> _selectionParents = new List<SceneGraphNode>();
        private readonly List<SceneGraphNode> _selectionScopes = new List<SceneGraphNode>();
        private readonly List<SceneGraphNode> _pickAncestry = new List<SceneGraphNode>();
        private readonly List<SceneGraphNode> _pickSelectionChain = new List<SceneGraphNode>();

        /// <summary>
        /// The event to apply objects transformation.
        /// </summary>
        public ApplyTransformationDelegate ApplyTransformation;

        /// <summary>
        /// The event to duplicate selected objects.
        /// </summary>
        public Action Duplicate;

        /// <summary>
        /// Gets the array of selected objects.
        /// </summary>
        public List<SceneGraphNode> Selection => _selection;

        /// <summary>
        /// Gets the array of selected parent objects (as actors).
        /// </summary>
        public List<SceneGraphNode> SelectedParents => _selectionParents;

        /// <summary>
        /// Initializes a new instance of the <see cref="TransformGizmo" /> class.
        /// </summary>
        /// <param name="owner">The gizmos owner.</param>
        public TransformGizmo(IGizmoOwner owner)
        : base(owner)
        {
        }

        private static bool HasSelectionRelationship(SceneGraphNode node, ViewportSelectionRelationship relationship)
        {
            return (node.ViewportSelection & relationship) != 0;
        }

        private static SceneGraphNode ResolveRawSelectionTarget(SceneGraphNode hit)
        {
            if (hit is ActorChildNode actorChildNode && !actorChildNode.CanBeSelectedDirectly)
                hit = actorChildNode.ParentNode;

            // Scene roots are hierarchy/navigation objects, never viewport pick targets.
            if (hit is SceneNode)
                return null;

            if (hit is ActorNode hitActorNode && !hitActorNode.Actor)
                return null;
            if (hit is ActorChildNode hitChildNode && hitChildNode.ParentNode is ActorNode hitChildActorNode && !hitChildActorNode.Actor)
                return null;

            return hit;
        }

        private void BuildSelectionChain(SceneGraphNode rawHit, ViewMode viewMode, bool includePrefabBoundaries)
        {
            _pickSelectionChain.Clear();
            _pickAncestry.Clear();

            var directTarget = ResolveRawSelectionTarget(rawHit);
            SceneGraphNode requiredParent = null;
            if (directTarget != null && HasSelectionRelationship(directTarget, ViewportSelectionRelationship.SelectionProxy))
            {
                for (var parent = directTarget.ParentNode; parent != null; parent = parent.ParentNode)
                {
                    if (parent is not ActorNode parentActorNode)
                        continue;
                    if (!parentActorNode.Actor)
                        break;
                    if (parent is not SceneNode && !HasSelectionRelationship(parent, ViewportSelectionRelationship.SelectionProxy))
                    {
                        requiredParent = parent;
                        break;
                    }
                }
            }
            for (var node = directTarget; node != null; node = node.ParentNode)
            {
                // Do not allow an object's ancestry to promote a viewport hit to its Scene root.
                if (node is SceneNode)
                    break;
                if (node is ActorNode actorNode && !actorNode.Actor)
                    break;
                _pickAncestry.Add(node);
            }

            for (int i = _pickAncestry.Count - 1; i >= 0; i--)
            {
                var node = _pickAncestry[i];
                var relationship = node.ViewportSelection;
                bool isBoundary = (relationship & (ViewportSelectionRelationship.SemanticBoundary | ViewportSelectionRelationship.RuntimeOwner)) != 0;
                bool isPrefabBoundary = includePrefabBoundaries && (relationship & ViewportSelectionRelationship.PrefabBoundary) != 0;
                if (node == directTarget || node == requiredParent || isBoundary || isPrefabBoundary)
                    _pickSelectionChain.Add(node);
            }
        }

        private int FindActiveScopeIndex()
        {
            int result = -1;
            for (int i = 0; i < _selectionScopes.Count; i++)
                result = Mathf.Max(result, _pickSelectionChain.IndexOf(_selectionScopes[i]));
            return result;
        }

        private void RemoveScopesOutsideCurrentChain()
        {
            for (int i = 0; i < _selectionScopes.Count; i++)
            {
                if (_pickSelectionChain.Contains(_selectionScopes[i]))
                    continue;
                _selectionScopes.RemoveRange(i, _selectionScopes.Count - i);
                break;
            }
        }

        private SceneGraphNode ResolveSelectionChainTarget()
        {
            if (_pickSelectionChain.Count == 0)
                return null;
            int scopeIndex = FindActiveScopeIndex();
            return _pickSelectionChain[Mathf.Min(scopeIndex + 1, _pickSelectionChain.Count - 1)];
        }

        /// <summary>
        /// Resolves a raw scene graph hit through viewport selection relationships.
        /// </summary>
        /// <param name="rawHit">The raw hit node.</param>
        /// <param name="viewMode">The viewport view mode.</param>
        /// <param name="includePrefabBoundaries">True to include prefab instance roots as selection boundaries.</param>
        /// <returns>The node to select, or null.</returns>
        internal SceneGraphNode ResolveSelectionTarget(SceneGraphNode rawHit, ViewMode viewMode, bool includePrefabBoundaries = true)
        {
            if (rawHit == null)
                return null;
            BuildSelectionChain(rawHit, viewMode, includePrefabBoundaries);
            RemoveScopesOutsideCurrentChain();
            return ResolveSelectionChainTarget();
        }

        /// <summary>
        /// Resolves the target to preview while hovering a raw viewport hit.
        /// </summary>
        internal SceneGraphNode ResolveHoverTarget(SceneGraphNode rawHit, ViewMode viewMode, IList<SceneGraphNode> selection, bool includePrefabBoundaries = true)
        {
            return ResolveHoverTarget(rawHit, viewMode, selection, out _, includePrefabBoundaries);
        }

        /// <summary>
        /// Resolves the target to preview while hovering a raw viewport hit.
        /// </summary>
        internal SceneGraphNode ResolveHoverTarget(SceneGraphNode rawHit, ViewMode viewMode, IList<SceneGraphNode> selection, out bool isLeafTarget, bool includePrefabBoundaries = true)
        {
            isLeafTarget = false;
            if (rawHit == null)
                return null;
            BuildSelectionChain(rawHit, viewMode, includePrefabBoundaries);
            var target = ResolveSelectionChainTarget();
            int targetIndex = _pickSelectionChain.IndexOf(target);
            if (selection != null && selection.Count == 1 && selection[0] == target)
            {
                if (targetIndex + 1 < _pickSelectionChain.Count)
                {
                    target = _pickSelectionChain[++targetIndex];
                }
                else
                {
                    return null;
                }
            }
            isLeafTarget = targetIndex >= 0 && targetIndex == _pickSelectionChain.Count - 1;
            return target;
        }

        /// <summary>
        /// Enters the currently selected relationship scope and gets the next target in the hit chain.
        /// </summary>
        internal bool TryGetDrillTarget(SceneGraphNode rawHit, ViewMode viewMode, IList<SceneGraphNode> selection, out SceneGraphNode target, bool includePrefabBoundaries = true)
        {
            target = null;
            if (rawHit == null || selection == null || selection.Count != 1)
                return false;

            BuildSelectionChain(rawHit, viewMode, includePrefabBoundaries);
            int selectedIndex = _pickSelectionChain.IndexOf(selection[0]);
            if (selectedIndex < 0 || selectedIndex + 1 >= _pickSelectionChain.Count)
                return false;

            for (int i = _selectionScopes.Count - 1; i >= 0; i--)
            {
                int scopeIndex = _pickSelectionChain.IndexOf(_selectionScopes[i]);
                if (scopeIndex < 0 || scopeIndex >= selectedIndex)
                    _selectionScopes.RemoveAt(i);
            }
            _selectionScopes.Add(selection[0]);
            target = _pickSelectionChain[selectedIndex + 1];
            return true;
        }

        /// <summary>
        /// Exits the deepest active relationship scope.
        /// </summary>
        internal bool TryExitSelectionScope(out SceneGraphNode target)
        {
            while (_selectionScopes.Count != 0)
            {
                int index = _selectionScopes.Count - 1;
                target = _selectionScopes[index];
                _selectionScopes.RemoveAt(index);
                if (target != null)
                    return true;
            }
            target = null;
            return false;
        }

        /// <inheritdoc />
        public override void SnapToGround()
        {
            if (Owner.SceneGraphRoot == null)
                return;
            var ray = new Ray(Position, Vector3.Down);
            while (true)
            {
                var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
                var rayCastFlags = SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives | SceneGraphNode.RayCastData.FlagTypes.SkipTriggers;
                var hit = Owner.SceneGraphRoot.RayCast(ref ray, ref view, out var distance, out _, rayCastFlags);
                if (hit != null)
                {
                    // Skip snapping selection to itself
                    bool isSelected = false;
                    for (var e = hit; e != null && !isSelected; e = e.ParentNode)
                        isSelected |= IsSelected(e);
                    if (isSelected)
                    {
                        GetSelectedObjectsBounds(out var selectionBounds, out _);
                        var offset = Mathf.Max(selectionBounds.Size.Y * 0.5f, 1.0f);
                        ray.Position = ray.GetPoint(offset);
                        continue;
                    }

                    // Include objects bounds into target snap location
                    var editorBounds = BoundingBox.Empty;
                    Real bottomToCenter = 100000.0f;
                    for (int i = 0; i < _selectionParents.Count; i++)
                    {
                        if (_selectionParents[i] is ActorNode actorNode)
                        {
                            var b = actorNode.Actor.EditorBoxChildren;
                            BoundingBox.Merge(ref editorBounds, ref b, out editorBounds);
                            bottomToCenter = Mathf.Min(bottomToCenter, actorNode.Actor.Position.Y - editorBounds.Minimum.Y);
                        }
                    }
                    var newPosition = ray.GetPoint(distance) + new Vector3(0, bottomToCenter, 0);

                    // Snap
                    var translationDelta = newPosition - Position;
                    var rotationDelta = Quaternion.Identity;
                    var scaleDelta = Vector3.Zero;
                    if (translationDelta.IsZero)
                        break;
                    StartTransforming(false);
                    if (State != InteractionState.Dragging)
                        break;
                    ApplyInteractionDelta(ref translationDelta, ref rotationDelta, ref scaleDelta);
                    EndTransforming();
                }
                break;
            }
        }

        /// <inheritdoc />
        public override void Pick()
        {
            // Ensure player is not moving objects
            if (ActiveAxis != Axis.None)
                return;
            Profiler.BeginEvent("Pick");

            // Get mouse ray and try to hit any object
            var ray = Owner.MouseRay;
            var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
            var renderView = Owner.RenderTask.View;
            Pick(ref ray, ref view, renderView.Flags, renderView.Mode, Owner.IsShiftDown);

            Profiler.EndEvent();
        }

        internal void Pick(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode, bool addRemove)
        {
            var hit = GetPickTarget(ref ray, ref view, viewFlags, viewMode);

            // Update selection
            var sceneEditing = Editor.Instance.SceneEditing;
            if (hit != null)
            {
                bool isSelected = sceneEditing.Selection.Contains(hit);

                if (addRemove)
                {
                    if (isSelected)
                        sceneEditing.Deselect(hit);
                    else
                        sceneEditing.Select(hit, true);
                }
                else
                {
                    sceneEditing.Select(hit);
                }
            }
            else
            {
                sceneEditing.Deselect();
            }
        }

        /// <summary>
        /// Gets the scene graph node that would be selected by a pick at the given ray.
        /// </summary>
        /// <param name="ray">The mouse ray.</param>
        /// <param name="view">The view ray.</param>
        /// <param name="viewFlags">The view flags.</param>
        /// <param name="viewMode">The view mode.</param>
        /// <returns>The node that would be selected, or null if nothing was hit.</returns>
        internal SceneGraphNode GetPickTarget(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode)
        {
            var hit = GetRawPickTarget(ref ray, ref view, viewFlags, viewMode);
            return ResolveSelectionTarget(hit, viewMode);
        }

        /// <summary>
        /// Gets the scene graph node that should be previewed by a hover at the given ray.
        /// </summary>
        internal SceneGraphNode GetHoverTarget(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode)
        {
            return GetHoverTarget(ref ray, ref view, viewFlags, viewMode, out _);
        }

        /// <summary>
        /// Gets the scene graph node that should be previewed by a hover at the given ray.
        /// </summary>
        internal SceneGraphNode GetHoverTarget(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode, out bool isLeafTarget)
        {
            var hit = GetRawPickTarget(ref ray, ref view, viewFlags, viewMode);
            return ResolveHoverTarget(hit, viewMode, _selection, out isLeafTarget);
        }

        /// <summary>
        /// Tries to enter the current viewport selection scope at the given ray.
        /// </summary>
        internal bool TryDrillPick(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode, out SceneGraphNode target, bool includePrefabBoundaries = true)
        {
            var hit = GetRawPickTarget(ref ray, ref view, viewFlags, viewMode);
            return TryGetDrillTarget(hit, viewMode, _selection, out target, includePrefabBoundaries);
        }

        private SceneGraphNode GetRawPickTarget(ref Ray ray, ref Ray view, ViewFlags viewFlags, ViewMode viewMode)
        {
            bool selectColliders = (viewFlags & ViewFlags.PhysicsDebug) == ViewFlags.PhysicsDebug || viewMode == ViewMode.PhysicsColliders;
            SceneGraphNode.RayCastData.FlagTypes rayCastFlags = SceneGraphNode.RayCastData.FlagTypes.None;
            if (!selectColliders)
                rayCastFlags |= SceneGraphNode.RayCastData.FlagTypes.SkipColliders;
            var root = Owner?.SceneGraphRoot;
            if (root == null)
                return null;

            var hit = root.RayCast(ref ray, ref view, out _, rayCastFlags);
            if (hit != null && _selection.Count == 1)
            {
                var selected = _selection[0];
                bool selectedIsValid = selected != null && (selected is not ActorNode selectedActorNode || selectedActorNode.Actor);
                if (selectedIsValid && (hit == selected || selected.ContainsInHierarchy(hit)))
                {
                    var rayCastData = new SceneGraphNode.RayCastData
                    {
                        Ray = ray,
                        View = view,
                        Flags = rayCastFlags & ~SceneGraphNode.RayCastData.FlagTypes.SkipColliders,
                    };
                    var childHit = selected.RayCastChildren(ref rayCastData, out _, out _);
                    if (childHit != null)
                        hit = childHit;
                }
            }
            return hit;
        }

        /// <inheritdoc />
        public override void OnSelectionChanged(List<SceneGraphNode> newSelection)
        {
            // An external selection change invalidates the transaction. The
            // selection change produced by transaction-aware duplication is
            // the one intentional exception.
            if (HasActiveTransaction && !IsExpectingTransactionSelectionChange)
                CancelTransforming();

            if (_selectionScopes.Count != 0)
            {
                var scope = _selectionScopes[_selectionScopes.Count - 1];
                if (newSelection.Count != 1 || (newSelection[0] != scope && !scope.ContainsInHierarchy(newSelection[0])))
                    _selectionScopes.Clear();
            }

            // Prepare collections
            _selection.Clear();
            _selectionParents.Clear();
            int count = newSelection.Count;
            if (_selection.Capacity < count)
            {
                _selection.Capacity = Mathf.NextPowerOfTwo(count);
                _selectionParents.Capacity = Mathf.NextPowerOfTwo(count);
            }

            // Cache selected objects
            _selection.AddRange(newSelection);

            // Build selected objects parents list.
            // Note: because selection may contain objects and their children we have to split them and get only parents.
            // Later during transformation we apply translation/scale/rotation only on them (children inherit transformations)
            SceneGraphTools.BuildNodesParents(_selection, _selectionParents);

            base.OnSelectionChanged(newSelection);
        }

        /// <inheritdoc />
        protected override int SelectionCount => _selectionParents.Count;

        /// <inheritdoc />
        protected override SceneGraphNode GetSelectedObject(int index)
        {
            return _selectionParents[index];
        }

        /// <inheritdoc />
        protected override Transform GetSelectedTransform(int index)
        {
            return _selectionParents[index].Transform;
        }

        /// <inheritdoc />
        protected override void GetSelectedObjectsBounds(out BoundingBox bounds, out bool navigationDirty)
        {
            bounds = BoundingBox.Empty;
            navigationDirty = false;
            for (int i = 0; i < _selectionParents.Count; i++)
            {
                if (_selectionParents[i] is ActorNode actorNode)
                {
                    bounds = BoundingBox.Merge(bounds, actorNode.Actor.EditorBoxChildren);
                    navigationDirty |= actorNode.AffectsNavigationWithChildren;
                }
            }
        }

        /// <inheritdoc />
        protected override bool IsSelected(SceneGraphNode obj)
        {
            return _selection.Contains(obj);
        }

        /// <inheritdoc />
        protected override void OnApplyTransformation(ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta)
        {
            base.OnApplyTransformation(ref translationDelta, ref rotationDelta, ref scaleDelta);

            ApplyTransformation(_selectionParents, ref translationDelta, ref rotationDelta, ref scaleDelta);
        }

        /// <inheritdoc />
        protected override void OnApplyInteractionResult(InteractionResult result)
        {
            var origin = TransactionOrigin;
            if (origin != null && origin.InitialPivot != PivotType.ObjectCenter && result.Scale != Vector3.One)
            {
                for (int i = 0; i < _selectionParents.Count; i++)
                {
                    var node = _selectionParents[i];
                    var transform = node.Transform;
                    transform.Translation = ScalePositionAroundPivot(transform.Translation, origin.PivotPosition, origin.InitialBasis, result.Scale);
                    node.Transform = transform;
                }
            }
            base.OnApplyInteractionResult(result);
        }

        /// <inheritdoc />
        protected override bool UsesOriginAuthoritativePreview => true;

        /// <inheritdoc />
        protected override void OnEndTransforming()
        {
            base.OnEndTransforming();

            if (!HasTransformChanges || TransactionObjects.Count == 0)
                return;

            // Record one transform action. Transaction-aware duplication is
            // composed into the same history item by the lifecycle layer.
            var selection = new List<SceneGraphNode>(TransactionObjects);
            var action = new TransformObjectsAction(selection, _startTransforms, ref _startBounds, _navigationDirty);
            AddTransformUndoAction(action);
        }

        /// <inheritdoc />
        protected override void OnDuplicate()
        {
            base.OnDuplicate();

            if (Owner.TryDuplicateForTransform(out var createdObjects, out var undoAction))
            {
                RegisterDuplicatedObjects(createdObjects, undoAction);
            }
        }
    }
}
