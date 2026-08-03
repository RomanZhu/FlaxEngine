// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Tree
{
    /// <summary>
    /// Tree control.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    [HideInEditor]
    public class Tree : ContainerControl
    {
        /// <summary>
        /// The key updates timeout in seconds.
        /// </summary>
        public static float KeyUpdateTimeout = 0.25f;

        /// <summary>
        /// Delegate for selected tree nodes collection change.
        /// </summary>
        /// <param name="before">The before state.</param>
        /// <param name="after">The after state.</param>
        public delegate void SelectionChangedDelegate(List<TreeNode> before, List<TreeNode> after);

        /// <summary>
        /// Delegate for node click events.
        /// </summary>
        /// <param name="node">The node.</param>
        /// <param name="location">The location.</param>
        public delegate void NodeClickDelegate(TreeNode node, Float2 location);

        private float _keyUpdateTime;
        private readonly bool _supportMultiSelect;
        private Margin _margin;
        private bool _autoSize = true;
        private bool _deferLayoutUpdate = false;
        private TreeNode _lastSelectedNode;

        /// <summary>
        /// The TreeNode that is being dragged over. This could have a value when not dragging.
        /// </summary>
        internal TreeNode DraggedOverNode = null;

        /// <summary>
        /// Action fired when tree nodes selection gets changed.
        /// </summary>
        public event SelectionChangedDelegate SelectedChanged;

        /// <summary>
        /// Action fired when mouse goes right click up on node.
        /// </summary>
        public event NodeClickDelegate RightClick;

        /// <summary>
        /// List with all selected nodes
        /// </summary>
        [HideInEditor, NoSerialize]
        public readonly List<TreeNode> Selection = new List<TreeNode>();

        /// <summary>
        /// Gets the first selected node or null.
        /// </summary>
        public TreeNode SelectedNode => Selection.Count > 0 ? Selection[0] : null;

        /// <summary>
        /// Allow nodes to Draw the root tree line.
        /// </summary>
        public bool DrawRootTreeLine = true;

        /// <summary>
        /// Occurs when the deferred layout operation was performed.
        /// </summary>
        public event Action AfterDeferredLayout;

        /// <summary>
        /// Gets or sets the margin for the child tree nodes.
        /// </summary>
        [EditorOrder(0), Tooltip("The margin applied to the child tree nodes.")]
        public Margin Margin
        {
            get => _margin;
            set
            {
                _margin = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// Gets or sets the value indicating whenever the tree will auto-size to the tree nodes dimensions.
        /// </summary>
        [EditorOrder(10), Tooltip("If checked, the tree will auto-size to the tree nodes dimensions.")]
        public bool AutoSize
        {
            get => _autoSize;
            set
            {
                _autoSize = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="Tree"/> class.
        /// </summary>
        public Tree()
        : this(false)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="Tree"/> class.
        /// </summary>
        /// <param name="supportMultiSelect">True if support multi selection for tree nodes, otherwise false.</param>
        public Tree(bool supportMultiSelect)
        : base(0, 0, 100, 100)
        {
            IsScrollable = true;
            AutoFocus = false;

            _supportMultiSelect = supportMultiSelect;
            _keyUpdateTime = KeyUpdateTimeout;
        }

        internal void OnRightClickInternal(TreeNode node, ref Float2 location)
        {
            RightClick?.Invoke(node, location);
        }

        private static bool CanSelectNode(TreeNode node)
        {
            return node != null && node.IsSelectable;
        }

        /// <summary>
        /// Selects single tree node.
        /// </summary>
        /// <param name="node">Node to select.</param>
        /// <param name="additive">If set to <c>true</c> item will be added to the current selection. Otherwise, selection will be cleared before.</param>
        public void Select(TreeNode node, bool additive = false)
        {
            if (node == null)
                throw new ArgumentNullException();
            if (!CanSelectNode(node))
                return;

            // Check if won't change
            if (Selection.Count == 1 && SelectedNode == node)
                return;

            // Cache previous state
            var prev = new List<TreeNode>(Selection);

            // Update selection
            if (additive)
            {
                if (!Selection.Contains(node))
                    Selection.Add(node);
            }
            else
            {
                Selection.Clear();
                Selection.Add(node);
            }

            // Ensure that node can be visible (all it's parents are expanded)
            node.ExpandAllParents();

            node.Focus();

            // Fire event
            SelectedChanged?.Invoke(prev, Selection);
        }

        /// <summary>
        /// Selects tree nodes.
        /// </summary>
        /// <param name="nodes">Nodes to select.</param>
        public void Select(List<TreeNode> nodes)
        {
            if (nodes == null)
                throw new ArgumentNullException();

            var selectableNodes = new List<TreeNode>(nodes.Count);
            for (int i = 0; i < nodes.Count; i++)
            {
                if (CanSelectNode(nodes[i]))
                    selectableNodes.Add(nodes[i]);
            }

            // Check if won't change
            if (Selection.Count == selectableNodes.Count && Selection.SequenceEqual(selectableNodes))
                return;

            // Cache previous state
            var prev = new List<TreeNode>(Selection);

            // Update selection
            Selection.Clear();
            if (_supportMultiSelect)
                Selection.AddRange(selectableNodes);
            else if (selectableNodes.Count > 0)
                Selection.Add(selectableNodes[0]);

            // Ensure that every selected node can be visible (all it's parents are expanded)
            // TODO: maybe use faster tree walk or faster algorythm?
            for (int i = 0; i < Selection.Count; i++)
            {
                Selection[i].ExpandAllParents();
            }

            // Fire event
            SelectedChanged?.Invoke(prev, Selection);
        }

        /// <summary>
        /// Clears the selection.
        /// </summary>
        public void Deselect()
        {
            // Check if won't change
            if (Selection.Count == 0)
                return;

            // Cache previous state
            var prev = new List<TreeNode>(Selection);

            // Update selection
            Selection.Clear();

            // Fire event
            SelectedChanged?.Invoke(prev, Selection);
        }

        /// <summary>
        /// Adds or removes node to/from the selection
        /// </summary>
        /// <param name="node">The node.</param>
        public void AddOrRemoveSelection(TreeNode node)
        {
            if (!CanSelectNode(node))
                return;

            // Cache previous state
            var prev = new List<TreeNode>(Selection);

            // Check if is selected
            int index = Selection.IndexOf(node);
            if (index != -1)
            {
                // Remove
                Selection.RemoveAt(index);
            }
            else
            {
                if (!_supportMultiSelect)
                    Selection.Clear();

                // Add
                Selection.Add(node);
            }

            // Fire event
            SelectedChanged?.Invoke(prev, Selection);
        }

        private void WalkSelectRangeExpandedTree(List<TreeNode> selection, TreeNode node, ref Rectangle range)
        {
            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is TreeNode child && child.Visible)
                {
                    var pos = child.PointToParent(this, Float2.One);
                    if (CanSelectNode(child) && range.Contains(pos))
                    {
                        selection.Add(child);
                    }

                    var nodeArea = new Rectangle(pos, child.Size);
                    if (child.IsExpanded && range.Intersects(ref nodeArea))
                        WalkSelectRangeExpandedTree(selection, child, ref range);
                }
            }
        }

        private Rectangle CalcNodeRangeRect(TreeNode node)
        {
            var pos = node.PointToParent(this, Float2.One);
            return new Rectangle(pos, new Float2(10000, 4));
        }

        /// <summary>
        /// Selects tree nodes range (used to select part of the tree using Shift+Mouse).
        /// </summary>
        /// <param name="endNode">End range node</param>
        public void SelectRange(TreeNode endNode)
        {
            if (!CanSelectNode(endNode))
                return;

            if (_supportMultiSelect && Selection.Count > 0)
            {
                // Cache previous state
                var prev = new List<TreeNode>(Selection);

                // Update selection
                var selectionRect = CalcNodeRangeRect(Selection[0]);
                for (int i = 1; i < Selection.Count; i++)
                {
                    selectionRect = Rectangle.Union(selectionRect, CalcNodeRangeRect(Selection[i]));
                }
                var endNodeRect = CalcNodeRangeRect(endNode);
                if (endNodeRect.Top - Mathf.Epsilon <= selectionRect.Top)
                {
                    float diff = selectionRect.Top - endNodeRect.Top;
                    selectionRect.Location.Y -= diff;
                    selectionRect.Size.Y += diff;
                }
                else if (endNodeRect.Bottom + Mathf.Epsilon >= selectionRect.Bottom)
                {
                    float diff = endNodeRect.Bottom - selectionRect.Bottom;
                    selectionRect.Size.Y += diff;
                }
                Selection.Clear();
                WalkSelectRangeExpandedTree(Selection, _children[0] as TreeNode, ref selectionRect);

                // Check if changed
                if (Selection.Count != prev.Count || !Selection.SequenceEqual(prev))
                {
                    // Fire event
                    SelectedChanged?.Invoke(prev, Selection);
                }
            }
            else
            {
                Select(endNode);
            }
        }

        private void WalkSelectExpandedTree(List<TreeNode> selection, TreeNode node)
        {
            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is TreeNode child)
                {
                    if (CanSelectNode(child))
                        selection.Add(child);
                    if (child.IsExpanded)
                        WalkSelectExpandedTree(selection, child);
                }
            }
        }

        private TreeNode FindFirstSelectableInSubtree(TreeNode node)
        {
            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is TreeNode child && child.Visible)
                {
                    if (CanSelectNode(child))
                        return child;
                    if (child.IsExpanded)
                    {
                        var result = FindFirstSelectableInSubtree(child);
                        if (result != null)
                            return result;
                    }
                }
            }
            return null;
        }

        private TreeNode FindLastSelectableInSubtree(TreeNode node)
        {
            if (node.IsExpanded)
            {
                for (int i = node.ChildrenCount - 1; i >= 0; i--)
                {
                    if (node.GetChild(i) is TreeNode child && child.Visible)
                    {
                        var result = FindLastSelectableInSubtree(child);
                        if (result != null)
                            return result;
                    }
                }
            }
            return CanSelectNode(node) ? node : null;
        }

        private TreeNode FindPreviousSelectable(TreeNode node)
        {
            if (node == null)
                return null;

            var parent = node.Parent;
            var parentNode = parent as TreeNode;
            int nodeIndex = parent?.GetChildIndex(node) ?? -1;
            if (nodeIndex == -1)
                return null;

            for (int i = nodeIndex - 1; i >= 0; i--)
            {
                if (parent.GetChild(i) is TreeNode sibling && sibling.Visible)
                {
                    var result = FindLastSelectableInSubtree(sibling);
                    if (result != null)
                        return result;
                }
            }

            return CanSelectNode(parentNode) ? parentNode : FindPreviousSelectable(parentNode);
        }

        private TreeNode FindNextSelectable(TreeNode node)
        {
            if (node == null)
                return null;

            if (node.IsExpanded)
            {
                var child = FindFirstSelectableInSubtree(node);
                if (child != null)
                    return child;
            }

            Control current = node;
            while (current != null)
            {
                var parent = current.Parent;
                int currentIndex = parent?.GetChildIndex(current) ?? -1;
                if (currentIndex == -1)
                    return null;

                for (int i = currentIndex + 1; i < parent.ChildrenCount; i++)
                {
                    if (parent.GetChild(i) is TreeNode sibling && sibling.Visible)
                    {
                        if (CanSelectNode(sibling))
                            return sibling;
                        if (sibling.IsExpanded)
                        {
                            var result = FindFirstSelectableInSubtree(sibling);
                            if (result != null)
                                return result;
                        }
                    }
                }

                current = parent as TreeNode;
            }

            return null;
        }

        private TreeNode FindSelectableParent(TreeNode node)
        {
            var parent = node?.Parent as TreeNode;
            while (parent != null)
            {
                if (CanSelectNode(parent))
                    return parent;
                parent = parent.Parent as TreeNode;
            }
            return null;
        }

        private void BulkSelectUpdateExpanded(bool select = true)
        {
            if (_supportMultiSelect)
            {
                // Cache previous state
                var prev = new List<TreeNode>(Selection);

                // Update selection
                Selection.Clear();
                if (select)
                    WalkSelectExpandedTree(Selection, _children[0] as TreeNode);

                // Check if changed
                if (Selection.Count != prev.Count || !Selection.SequenceEqual(prev))
                {
                    // Fire event
                    SelectedChanged?.Invoke(prev, Selection);
                }
            }
        }

        /// <summary>
        /// Select all expanded nodes
        /// </summary>
        public void SelectAllExpanded()
        {
            BulkSelectUpdateExpanded(true);
        }

        /// <summary>
        /// Deselect all nodes
        /// </summary>
        public void DeselectAll()
        {
            BulkSelectUpdateExpanded(false);
        }

        /// <summary>
        /// Flushes any pending layout perming action that has been delayed until next update to optimize performance of the complex tree hierarchy.
        /// </summary>
        public void FlushPendingPerformLayout()
        {
            if (_deferLayoutUpdate)
            {
                base.PerformLayout();
                AfterDeferredLayout?.Invoke();
                _deferLayoutUpdate = false;
            }
        }

        /// <inheritdoc />
        public override void PerformLayout(bool force = false)
        {
            if (_isLayoutLocked && !force)
                return;

            // In case the tree was fully expanded or collapsed along its children, avoid calculating the layout multiple times for each child
            _deferLayoutUpdate = true;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            if (_deferLayoutUpdate)
                FlushPendingPerformLayout();
            var window = Root;
            bool shiftDown = window.GetKey(KeyboardKeys.Shift);
            bool keyUpArrow = window.GetKey(KeyboardKeys.ArrowUp);
            bool keyDownArrow = window.GetKey(KeyboardKeys.ArrowDown);

            // Use last selection for last selected node if sift is down
            if (Selection.Count < 2)
                _lastSelectedNode = null;
            else if (shiftDown)
                _lastSelectedNode ??= Selection[^1];

            // Skip nodes that cannot be selected by keyboard navigation
            if (_lastSelectedNode != null && !CanSelectNode(_lastSelectedNode))
                _lastSelectedNode = null;

            var node = _lastSelectedNode ?? SelectedNode;
            if (!CanSelectNode(node))
                node = null;

            // Check if has focus and if any selectable node is focused
            if (ContainsFocus && node != null && node.AutoFocus)
            {
                if (window.GetKeyDown(KeyboardKeys.ArrowUp) || window.GetKeyDown(KeyboardKeys.ArrowDown))
                    _keyUpdateTime = KeyUpdateTimeout;
                if (_keyUpdateTime >= KeyUpdateTimeout && window is WindowRootControl windowRoot && windowRoot.Window.IsFocused)
                {
                    // Check if arrow flags are different
                    if (keyDownArrow != keyUpArrow)
                    {
                        List<TreeNode> toSelect = new List<TreeNode>();
                        if (shiftDown && _supportMultiSelect)
                        {
                            toSelect.AddRange(Selection);
                        }

                        var select = keyUpArrow ? FindPreviousSelectable(node) : FindNextSelectable(node);
                        if (select != null)
                        {
                            if (shiftDown && _supportMultiSelect)
                            {
                                if (toSelect.Contains(select))
                                    toSelect.Remove(node);
                                else
                                    toSelect.Add(select);
                            }
                            else
                            {
                                toSelect.Add(select);
                            }

                            if (toSelect.Count > 0)
                            {
                                // Select
                                Select(toSelect);
                                _lastSelectedNode = select;
                                _lastSelectedNode.Focus();
                            }
                        }

                        // Reset time
                        _keyUpdateTime = 0.0f;
                    }
                }
                else
                {
                    // Update time
                    _keyUpdateTime += deltaTime;
                }

                if (window.GetKeyDown(KeyboardKeys.ArrowRight))
                {
                    if (node.IsExpanded)
                    {
                        // Select first child if has
                        var child = FindFirstSelectableInSubtree(node);
                        if (child != null)
                        {
                            Select(child);
                            child.Focus();
                        }
                    }
                    else
                    {
                        // Expand selected node
                        node.Expand();
                    }
                }
                else if (window.GetKeyDown(KeyboardKeys.ArrowLeft))
                {
                    if (node.IsCollapsed)
                    {
                        // Select parent if has a selectable parent
                        var nodeParentNode = FindSelectableParent(node);
                        if (nodeParentNode != null)
                        {
                            Select(nodeParentNode);
                            nodeParentNode.Focus();
                        }
                    }
                    else
                    {
                        // Collapse selected node
                        node.Collapse();
                    }
                }
            }

            base.Update(deltaTime);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            // Expansion can queue layout from input/update after the update-time flush.
            // Do not render one frame with stale child widths/positions.
            if (_deferLayoutUpdate)
                FlushPendingPerformLayout();

            base.Draw();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            // Check if can use multi selection
            if (_supportMultiSelect)
            {
                InputOptions options = Editor.Instance.Options.Options.Input;

                // Select all expanded nodes
                if (options.SelectAll.Process(this))
                {
                    SelectAllExpanded();
                    return true;
                }
                else if (options.DeselectAll.Process(this))
                {
                    DeselectAll();
                    return true;
                }
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override void OnGotFocus()
        {
            // Reset timer
            _keyUpdateTime = 0;

            base.OnGotFocus();
        }

        /// <inheritdoc />
        public override void OnParentResized()
        {
            PerformLayout();

            base.OnParentResized();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            if (_autoSize)
            {
                // Use max of parent clint area width and root node width
                var parent = Parent;
                var width = parent != null ? Mathf.Max(parent.GetClientArea().Width, 0) : 0.0f;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node && node.Visible)
                        width = Mathf.Max(width, node.MinimumWidth);
                }
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node && node.Visible)
                        node.Width = width;
                }
                Width = width + _margin.Width;
            }

            base.PerformLayoutBeforeChildren();
        }

        /// <inheritdoc />
        protected override void PerformLayoutAfterChildren()
        {
            base.PerformLayoutAfterChildren();

            // Arrange children
            float y = _margin.Top;
            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i] is TreeNode node && node.Visible)
                {
                    node.Location = new Float2(_margin.Left, y);
                    y += node.Height + TreeNode.DefaultNodeOffsetY;
                }
            }

            if (_autoSize)
            {
                // Update height based on the nodes
                var bottom = 0.0f;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node && node.Visible)
                        bottom = Mathf.Max(bottom, node.Bottom);
                }
                Height = bottom + _margin.Bottom;
            }
        }
    }
}
