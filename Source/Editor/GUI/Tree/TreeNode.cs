// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Tree
{
    /// <summary>
    /// Tree node control.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    [HideInEditor]
    public class TreeNode : ContainerControl
    {
        /// <summary>
        /// The default drag insert position margin.
        /// </summary>
        public const float DefaultDragInsertPositionMargin = 3.0f;

        /// <summary>
        /// The default node offset on Y axis.
        /// </summary>
        public const float DefaultNodeOffsetY = 0;

        private const float _targetHighlightScale = 1.25f;
        private const float _highlightScaleAnimDuration = 0.85f;

        private Tree _tree;

        private bool _opened, _canChangeOrder;
        private float _animationProgress, _cachedHeight;
        private bool _isHightlighted;
        private float _targetHighlightTimeSec;
        private float _currentHighlightTimeSec;
        // Used to prevent showing highlight on double mouse click
        private float _debounceHighlightTime;
        private float _highlightScale;
        private static TreeNode _lastHighlightedNode;
        private bool _mouseOverArrow, _mouseOverHeader;
        private float _xOffset, _textWidth;
        private float _headerHeight = 16.0f;
        private bool _showHeader = true;

        private float HeaderIconSize => Mathf.Min(Mathf.Max(0.0f, Style.Current.GetTreeIconSize()), Mathf.Max(0.0f, _headerHeight - 2.0f));

        private float HeaderIconSlotWidth => HeaderIconSize > 0.0f ? HeaderIconSize + 4.0f : 0.0f;
        private float LayoutHeaderHeight => _showHeader ? _headerHeight : 0.0f;
        private Rectangle _headerRect;
        private SpriteHandle _iconCollaped, _iconOpened;
        private Margin _margin = new Margin(2.0f);
        private string _text;
        private bool _textChanged;
        private bool _isMouseDown;
        private bool _mouseDownOverArrow;
        private bool _mouseDownRecursiveToggle;
        private float _mouseDownTime;
        private Float2 _mouseDownPos;
        private float _suppressLeftMouseUpUntil = -1.0f;
        private bool _arrowMouseCaptureActive;
        private bool _arrowMouseCaptureSuppressActions;
        private bool _arrowMouseCaptureExpand;
        private bool _arrowMouseCaptureRecursive;
        private ContainerControl _arrowMouseCaptureParent;
        private HashSet<TreeNode> _arrowMouseCaptureProcessedNodes;

        private DragItemPositioning _dragOverMode;
        private bool _isDragOverHeader;
        private static ulong _dragEndFrame;

        /// <summary>
        /// Gets or sets the text.
        /// </summary>
        [EditorOrder(10), Tooltip("The node text.")]
        public string Text
        {
            get => _text;
            set
            {
                _text = value;
                _textChanged = true;
                PerformLayout();
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether this node is expanded.
        /// </summary>
        [EditorOrder(20), Tooltip("If checked, node is expanded.")]
        public bool IsExpanded
        {
            get => _opened;
            set
            {
                if (value)
                    Expand(true);
                else
                    Collapse(true);
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether this node is collapsed.
        /// </summary>
        [HideInEditor, NoSerialize]
        public bool IsCollapsed
        {
            get => !_opened;
            set
            {
                if (value)
                    Collapse(true);
                else
                    Expand(true);
            }
        }

        /// <summary>
        /// Gets a value indicating whether the node is collapsed in the hierarchy (is collapsed or any of its parents is collapsed).
        /// </summary>
        public bool IsCollapsedInHierarchy => IsCollapsed || (Parent is TreeNode parentNode && parentNode.IsCollapsedInHierarchy);

        /// <summary>
        /// Gets or sets the text margin.
        /// </summary>
        [EditorOrder(30), Tooltip("The margin of the text area.")]
        public Margin TextMargin
        {
            get => _margin;
            set
            {
                _margin = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// Gets or sets the color of the text.
        /// </summary>
        [EditorDisplay("Style"), EditorOrder(2000)]
        public Color TextColor { get; set; }

        /// <summary>
        /// Gets or sets the font used to render text.
        /// </summary>
        [EditorDisplay("Style"), EditorOrder(2000)]
        public FontReference TextFont { get; set; }

        /// <summary>
        /// Gets or sets the color of the background when tree node is selected.
        /// </summary>
        [EditorDisplay("Style"), EditorOrder(2000)]
        public Color BackgroundColorSelected { get; set; }

        /// <summary>
        /// Gets or sets the color of the background when tree node is highlighted.
        /// </summary>
        [EditorDisplay("Style"), EditorOrder(2000)]
        public Color BackgroundColorHighlighted { get; set; }

        /// <summary>
        /// Gets or sets the color of the background when tree node is selected but not focused.
        /// </summary>
        [EditorDisplay("Style"), EditorOrder(2000)]
        public Color BackgroundColorSelectedUnfocused { get; set; }

        /// <summary>
        /// Gets the parent tree control.
        /// </summary>
        public Tree ParentTree
        {
            get
            {
                if (_tree == null)
                {
                    if (Parent is TreeNode upNode)
                        _tree = upNode.ParentTree;
                    else if (Parent is Tree tree)
                        _tree = tree;
                }
                return _tree;
            }
        }

        /// <summary>
        /// Gets a value indicating whether this node is root.
        /// </summary>
        public bool IsRoot => !(Parent is TreeNode);

        /// <summary>
        /// Gets or sets a value indicating whether this node can be selected by the parent tree.
        /// </summary>
        [HideInEditor, NoSerialize]
        public bool IsSelectable { get; set; } = true;

        /// <summary>
        /// Gets or sets a value indicating whether this node renders and accepts input on its header row.
        /// </summary>
        [HideInEditor, NoSerialize]
        public bool ShowHeader
        {
            get => _showHeader;
            set
            {
                if (_showHeader == value)
                    return;

                _showHeader = value;
                if (!_showHeader)
                {
                    _mouseOverArrow = false;
                    _mouseOverHeader = false;
                    _isMouseDown = false;
                    _mouseDownOverArrow = false;
                    EndArrowMouseCapture();
                }
                _headerRect = new Rectangle(0, 0, Width, LayoutHeaderHeight);
                PerformLayout();
            }
        }

        /// <summary>
        /// Gets the minimum width of the node sub-tree.
        /// </summary>
        public virtual float MinimumWidth
        {
            get
            {
                UpdateTextWidth();

                float minWidth = 0.0f;
                if (_showHeader)
                {
                    minWidth = _xOffset + _textWidth + 6 + 16 + HeaderTextLeftOffset + HeaderTextRightOffset;
                    if (_iconCollaped.IsValid)
                        minWidth += 16;
                }

                if (_opened || _animationProgress < 1.0f)
                {
                    for (int i = 0; i < _children.Count; i++)
                    {
                        if (_children[i] is TreeNode node && node.Visible)
                        {
                            minWidth = Mathf.Max(minWidth, node.MinimumWidth);
                        }
                    }
                }

                return minWidth;
            }
        }

        /// <summary>
        /// The indent applied to the child nodes.
        /// </summary>
        [EditorOrder(30), Tooltip("The indentation applied to the child nodes.")]
        public float ChildrenIndent { get; set; } = 12.0f;

        /// <summary>
        /// The height of the tree node header area.
        /// </summary>
        [EditorOrder(40), Limit(1, 10000, 0.1f), Tooltip("The height of the tree node header area.")]
        public float HeaderHeight
        {
            get => _headerHeight;
            set
            {
                if (!Mathf.NearEqual(_headerHeight, value))
                {
                    _headerHeight = value;
                    _headerRect = new Rectangle(0, 0, Width, LayoutHeaderHeight);
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the color of the icon.
        /// </summary>
        [EditorOrder(50), Tooltip("The color of the icon.")]
        public Color IconColor { get; set; } = Color.White;

        /// <summary>
        /// Gets the arrow rectangle.
        /// </summary>
        public Rectangle ArrowRect => _showHeader ? (CustomArrowRect.HasValue ? CustomArrowRect.Value : new Rectangle(_xOffset + 2 + _margin.Left, 2, 12, 12)) : Rectangle.Empty;

        /// <summary>
        /// Gets the header rectangle.
        /// </summary>
        public Rectangle HeaderRect => _headerRect;

        /// <summary>
        /// Gets the header text rectangle.
        /// </summary>
        public Rectangle TextRect
        {
            get
            {
                if (!_showHeader)
                    return Rectangle.Empty;

                var left = _xOffset + 16; // offset + arrow
                var textRect = new Rectangle(left, 0, Width - left, _headerHeight);

                // Margin
                _margin.ShrinkRectangle(ref textRect);

                // Icon
                if (_iconCollaped.IsValid && HeaderIconSize > 0.0f)
                {
                    textRect.X += HeaderIconSlotWidth;
                    textRect.Width -= HeaderIconSlotWidth;
                }

                ApplyHeaderTextLeftOffset(ref textRect);
                ApplyHeaderTextRightOffset(ref textRect);
                return textRect;
            }
        }

        /// <summary>
        /// Gets the extra space reserved to the left of the header text.
        /// </summary>
        protected virtual float HeaderTextLeftOffset => 0.0f;

        /// <summary>
        /// Gets the extra space reserved to the right of the header text.
        /// </summary>
        protected virtual float HeaderTextRightOffset => 0.0f;

        /// <summary>
        /// Gets a value indicating whether the mouse is over the node header.
        /// </summary>
        protected bool IsMouseOverHeader => _mouseOverHeader;

        /// <summary>
        /// Gets a value indicating whether the node header should be highlighted by an external source.
        /// </summary>
        protected virtual bool IsHeaderExternallyHighlighted => false;

        private void ApplyHeaderTextLeftOffset(ref Rectangle textRect)
        {
            var offset = HeaderTextLeftOffset;
            if (offset <= 0.0f)
                return;
            textRect.X += offset;
            textRect.Width = Mathf.Max(0.0f, textRect.Width - offset);
        }

        private void ApplyHeaderTextRightOffset(ref Rectangle textRect)
        {
            var offset = HeaderTextRightOffset;
            if (offset > 0.0f)
                textRect.Width = Mathf.Max(0.0f, textRect.Width - offset);
        }

        /// <summary>
        /// Custom arrow rectangle within node.
        /// </summary>
        [HideInEditor, NoSerialize]
        public Rectangle? CustomArrowRect;

        /// <summary>
        /// Gets or sets a value indicating whether this node has children that are loaded on expansion.
        /// </summary>
        [HideInEditor, NoSerialize]
        public bool HasDeferredChildren { get; set; }

        /// <summary>
        /// Gets the drag over action type.
        /// </summary>
        public DragItemPositioning DragOverMode => _dragOverMode;

        /// <summary>
        /// Gets a value indicating whether this node has any visible child. Returns false if it has no children.
        /// </summary>
        public bool HasAnyVisibleChild
        {
            get
            {
                bool result = HasDeferredChildren;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node && node.Visible)
                    {
                        result = true;
                        break;
                    }
                }
                return result;
            }
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="TreeNode"/> class.
        /// </summary>
        public TreeNode()
        : this(false, SpriteHandle.Invalid, SpriteHandle.Invalid)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="TreeNode"/> class.
        /// </summary>
        /// <param name="canChangeOrder">Enable/disable changing node order in parent tree node.</param>
        public TreeNode(bool canChangeOrder)
        : this(canChangeOrder, SpriteHandle.Invalid, SpriteHandle.Invalid)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="TreeNode"/> class.
        /// </summary>
        /// <param name="canChangeOrder">Enable/disable changing node order in parent tree node.</param>
        /// <param name="iconCollapsed">The icon for node collapsed.</param>
        /// <param name="iconOpened">The icon for node opened.</param>
        public TreeNode(bool canChangeOrder, SpriteHandle iconCollapsed, SpriteHandle iconOpened)
        : base(0, 0, 64, 16)
        {
            AutoFocus = true;

            var style = Style.Current;
            if (style.TreeRowHeight > 0.0f)
            {
                _headerHeight = style.TreeRowHeight;
                Height = _headerHeight;
            }
            _canChangeOrder = canChangeOrder;
            _animationProgress = 1.0f;
            _cachedHeight = _headerHeight;
            _iconCollaped = iconCollapsed;
            _iconOpened = iconOpened;
            _mouseDownTime = -1;

            TextColor = style.Foreground;
            BackgroundColorSelected = style.BackgroundSelected;
            BackgroundColorHighlighted = style.BackgroundHighlighted;
            BackgroundColorSelectedUnfocused = style.BackgroundSelected.AlphaMultiplied(0.72f);
            TextFont = new FontReference(style.FontSmall);
        }

        /// <summary>
        /// Expand node.
        /// </summary>
        /// <param name="noAnimation">True if skip node expanding animation.</param>
        public void Expand(bool noAnimation = false)
        {
            // Parents first
            ExpandAllParents(noAnimation);

            // Change state
            if (_opened && _animationProgress >= 1.0f)
                return;
            bool prevState = _opened;
            _opened = true;
            if (noAnimation)
                _animationProgress = 1.0f;
            else if (prevState != _opened)
                _animationProgress = 1.0f - _animationProgress;

            // Update
            OnExpandedChanged();
            OnExpandAnimationChanged();
        }

        /// <summary>
        /// Collapse node.
        /// </summary>
        /// <param name="noAnimation">True if skip node expanding animation.</param>
        public void Collapse(bool noAnimation = false)
        {
            // Change state
            if (!_opened && _animationProgress >= 1.0f)
                return;
            bool prevState = _opened;
            _opened = false;
            if (noAnimation)
                _animationProgress = 1.0f;
            else if (prevState != _opened)
                _animationProgress = 1.0f - _animationProgress;

            // Update
            OnExpandedChanged();
            OnExpandAnimationChanged();
        }

        /// <summary>
        /// Expand node and all the children.
        /// </summary>
        /// <param name="noAnimation">True if skip node expanding animation.</param>
        public void ExpandAll(bool noAnimation = false)
        {
            bool wasLayoutLocked = IsLayoutLocked;
            IsLayoutLocked = true;

            Expand(noAnimation);

            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i] is TreeNode node)
                {
                    node.ExpandAll(noAnimation);
                }
            }

            IsLayoutLocked = wasLayoutLocked;
            PerformLayout();
        }

        /// <summary>
        /// Collapse node and all the children.
        /// </summary>
        /// <param name="noAnimation">True if skip node expanding animation.</param>
        public void CollapseAll(bool noAnimation = false)
        {
            bool wasLayoutLocked = IsLayoutLocked;
            IsLayoutLocked = true;

            Collapse(noAnimation);

            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i] is TreeNode node)
                {
                    node.CollapseAll(noAnimation);
                }
            }

            IsLayoutLocked = wasLayoutLocked;
            PerformLayout();
        }

        private void BeginArrowMouseCapture()
        {
            _arrowMouseCaptureSuppressActions = true;
            _arrowMouseCaptureActive = true;
            _arrowMouseCaptureRecursive = IsRecursiveToggleModifierDown();
            _arrowMouseCaptureExpand = ShouldExpandRecursiveToggle(_arrowMouseCaptureRecursive);
            _arrowMouseCaptureParent = Parent;

            if (_arrowMouseCaptureProcessedNodes == null)
                _arrowMouseCaptureProcessedNodes = new HashSet<TreeNode>();
            else
                _arrowMouseCaptureProcessedNodes.Clear();

            StartMouseCapture();
            ApplyArrowMouseCapture(this);
        }

        private bool IsVisibleRootNode => Parent is TreeNode parentNode && !parentNode.ShowHeader;

        /// <summary>
        /// Gets a value indicating whether recursive collapse animation should be skipped for this node.
        /// </summary>
        protected virtual bool SkipRecursiveCollapseAnimation => false;

        private bool IsRecursiveToggleModifierDown()
        {
            return (ParentTree?.Root?.GetKey(KeyboardKeys.Alt) ?? false) || FlaxEngine.Input.GetKey(KeyboardKeys.Alt);
        }

        private bool IsHierarchyFullyExpanded()
        {
            if (!_opened)
                return false;

            for (int i = 0; i < _children.Count; i++)
            {
                if (_children[i] is TreeNode node && node.Visible && node.HasAnyVisibleChild && !node.IsHierarchyFullyExpanded())
                    return false;
            }

            return true;
        }

        private bool ShouldExpandRecursiveToggle(bool recursive)
        {
            if (recursive && IsVisibleRootNode)
                return !IsHierarchyFullyExpanded();
            return !_opened;
        }

        private void ToggleRecursive()
        {
            if (ShouldExpandRecursiveToggle(true))
                ExpandAll();
            else
                CollapseAll(SkipRecursiveCollapseAnimation && IsVisibleRootNode);
        }

        private void ClearArrowMouseCapture()
        {
            _arrowMouseCaptureActive = false;
            _arrowMouseCaptureSuppressActions = false;
            _arrowMouseCaptureParent = null;
            _arrowMouseCaptureProcessedNodes?.Clear();
        }

        private void EndArrowMouseCapture()
        {
            if (!_arrowMouseCaptureActive && !_arrowMouseCaptureSuppressActions)
                return;

            var wasCapturing = _arrowMouseCaptureActive;
            ClearArrowMouseCapture();
            if (wasCapturing)
                EndMouseCapture();
        }

        private void UpdateArrowMouseCapture(Float2 location)
        {
            var parent = _arrowMouseCaptureParent;
            if (parent == null)
                return;

            var parentLocation = PointToParent(parent, location);
            var children = parent.Children;
            for (int i = children.Count - 1; i >= 0; i--)
            {
                if (children[i] is TreeNode node && node.Visible && node.Enabled && node.HasAnyVisibleChild)
                {
                    var nodeLocation = node.PointFromParent(parent, parentLocation);
                    if (node.HeaderRect.Contains(nodeLocation))
                    {
                        ApplyArrowMouseCapture(node);
                        break;
                    }
                }
            }
        }

        private void ApplyArrowMouseCapture(TreeNode node)
        {
            if (node == null || node.Parent != _arrowMouseCaptureParent || !node.HasAnyVisibleChild)
                return;
            if (!_arrowMouseCaptureProcessedNodes.Add(node))
                return;

            if (_arrowMouseCaptureRecursive)
            {
                if (_arrowMouseCaptureExpand)
                    node.ExpandAll();
                else
                    node.CollapseAll();
            }
            else
            {
                if (_arrowMouseCaptureExpand)
                    node.Expand();
                else
                    node.Collapse();
            }
        }

        /// <summary>
        /// Ensure that all node parents are expanded.
        /// </summary>
        /// <param name="noAnimation">True if skip node expanding animation.</param>
        public void ExpandAllParents(bool noAnimation = false)
        {
            (Parent as TreeNode)?.Expand(noAnimation);
        }

        /// <summary>
        /// Ends open/close animation by force.
        /// </summary>
        public void EndAnimation()
        {
            if (_animationProgress < 1.0f)
            {
                _animationProgress = 1.0f;
                OnExpandAnimationChanged();
            }
        }

        /// <summary>
        /// Select node in the tree.
        /// </summary>
        public void Select()
        {
            ParentTree.Select(this);
        }

        /// <summary>
        /// Called when drag and drop enters the node header area.
        /// </summary>
        /// <param name="data">The data.</param>
        /// <returns>Drag action response.</returns>
        protected virtual DragDropEffect OnDragEnterHeader(DragData data)
        {
            return DragDropEffect.None;
        }

        /// <summary>
        /// Called when drag and drop moves over the node header area.
        /// </summary>
        /// <param name="data">The data.</param>
        /// <returns>Drag action response.</returns>
        protected virtual DragDropEffect OnDragMoveHeader(DragData data)
        {
            return DragDropEffect.None;
        }

        /// <summary>
        /// Called when drag and drop performs over the node header area.
        /// </summary>
        /// <param name="data">The data.</param>
        /// <returns>Drag action response.</returns>
        protected virtual DragDropEffect OnDragDropHeader(DragData data)
        {
            return DragDropEffect.None;
        }

        /// <summary>
        /// Called when drag and drop leaves the node header area.
        /// </summary>
        protected virtual void OnDragLeaveHeader()
        {
        }

        /// <summary>
        /// Begins the drag drop operation.
        /// </summary>
        protected virtual void DoDragDrop()
        {
        }

        /// <summary>
        /// Called when mouse double clicks header.
        /// </summary>
        /// <param name="location">The mouse location.</param>
        /// <param name="button">The button.</param>
        /// <returns>True if event has been handled.</returns>
        protected virtual bool OnMouseDoubleClickHeader(ref Float2 location, MouseButton button)
        {
            if (HasAnyVisibleChild)
            {
                // Toggle open state
                if (_opened)
                    Collapse();
                else
                    Expand();
            }

            // Handled
            return true;
        }

        /// <summary>
        /// Called when mouse clicks a header that was already the sole selected node.
        /// </summary>
        protected virtual void OnSelectedClickHeader()
        {
        }

        /// <summary>
        /// Called when mouse is pressing node header for a long time.
        /// </summary>
        protected virtual void OnLongPress()
        {
        }

        /// <summary>
        /// Called when expanded/collapsed state changes.
        /// </summary>
        protected virtual void OnExpandedChanged()
        {
        }

        /// <summary>
        /// Called when expand/collapse animation progress changes.
        /// </summary>
        protected virtual void OnExpandAnimationChanged()
        {
            if (ParentTree != null)
                ParentTree.PerformLayout();
            else if (Parent != null)
                Parent.PerformLayout();
            else
                PerformLayout();
        }

        /// <summary>
        /// Tests the header hit.
        /// </summary>
        /// <param name="location">The location.</param>
        /// <returns>True if hits it.</returns>
        protected virtual bool TestHeaderHit(ref Float2 location)
        {
            return _showHeader && _headerRect.Contains(ref location);
        }

        /// <summary>
        /// Updates the drag over mode based on the given mouse location.
        /// </summary>
        /// <param name="location">The location.</param>
        private void UpdateDragPositioning(ref Float2 location)
        {
            if (!_canChangeOrder)
            {
                _dragOverMode = TestHeaderHit(ref location) ? DragItemPositioning.At : DragItemPositioning.None;
            }
            // Check collision with drag areas
            else if (new Rectangle(_headerRect.X, _headerRect.Y - DefaultDragInsertPositionMargin - DefaultNodeOffsetY, _headerRect.Width, DefaultDragInsertPositionMargin * 2.0f).Contains(location))
                _dragOverMode = DragItemPositioning.Above;
            else if ((IsCollapsed || !HasAnyVisibleChild) && new Rectangle(_headerRect.X, _headerRect.Bottom - DefaultDragInsertPositionMargin, _headerRect.Width, DefaultDragInsertPositionMargin * 2.0f).Contains(location))
                _dragOverMode = DragItemPositioning.Below;
            else
                _dragOverMode = DragItemPositioning.At;

            // Update DraggedOverNode
            var tree = ParentTree;
            if (_dragOverMode == DragItemPositioning.None)
            {
                if (tree != null && tree.DraggedOverNode == this)
                    tree.DraggedOverNode = null;
            }
            else if (tree != null)
                tree.DraggedOverNode = this;
        }

        private void ClearDragPositioning()
        {
            _dragOverMode = DragItemPositioning.None;
            var tree = ParentTree;
            if (tree != null && tree.DraggedOverNode == this)
                tree.DraggedOverNode = null;
        }

        /// <summary>
        /// Caches the color of the text for this node. Called during update before children nodes but after parent node so it can reuse parent tree node data.
        /// </summary>
        /// <returns>Text color.</returns>
        protected virtual Color CacheTextColor()
        {
            return Enabled ? TextColor : TextColor * 0.6f;
        }

        /// <summary>
        /// Updates the cached width of the text.
        /// </summary>
        protected void UpdateTextWidth()
        {
            if (_textChanged)
            {
                var font = TextFont.GetFont();
                if (font)
                {
                    _textWidth = font.MeasureText(_text).X;
                    _textChanged = false;
                }
            }
        }

        /// <summary>
        /// Adds a box around the text to highlight the node.
        /// </summary>
        /// <param name="durationSec">The duration of the highlight in seconds.</param>
        public void StartHighlight(float durationSec = 0.5f)
        {
            if (_lastHighlightedNode != null && _lastHighlightedNode != this && !_lastHighlightedNode.IsDisposing)
                _lastHighlightedNode.StopHighlight();

            _isHightlighted = true;
            _targetHighlightTimeSec = durationSec;
            _currentHighlightTimeSec = 0;
            _debounceHighlightTime = 0;
            _highlightScale = 2f;
            _lastHighlightedNode = this;
        }

        /// <summary>
        /// Stops any current highlight.
        /// </summary>
        public void StopHighlight()
        {
            _isHightlighted = false;
            _targetHighlightTimeSec = 0;
            _currentHighlightTimeSec = 0;
            _debounceHighlightTime = 0;
            if (_lastHighlightedNode == this)
                _lastHighlightedNode = null;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // Highlight animations
            if (_isHightlighted)
            {
                _debounceHighlightTime += deltaTime;
                _currentHighlightTimeSec += deltaTime;

                // In the first second, animate the highlight to shrink into it's resting position
                if (_currentHighlightTimeSec < _highlightScaleAnimDuration)
                    _highlightScale = Mathf.Lerp(_highlightScale, _targetHighlightScale, _currentHighlightTimeSec);

                if (_currentHighlightTimeSec >= _targetHighlightTimeSec)
                    StopHighlight();
            }

            // Drop/down animation
            if (_animationProgress < 1.0f)
            {
                bool isDeltaSlow = deltaTime > (1 / 20.0f);

                // Update progress
                if (isDeltaSlow)
                {
                    _animationProgress = 1.0f;
                }
                else
                {
                    const float openCloseAnimationTime = 0.1f;
                    _animationProgress += deltaTime / openCloseAnimationTime;
                    if (_animationProgress > 1.0f)
                        _animationProgress = 1.0f;
                }

                // Arrange controls
                OnExpandAnimationChanged();
            }

            // Check for long press
            const float longPressTimeSeconds = 0.6f;
            if (_isMouseDown && !_mouseDownOverArrow && !_arrowMouseCaptureSuppressActions && Time.UnscaledGameTime - _mouseDownTime > longPressTimeSeconds)
            {
                OnLongPress();
            }

            // Don't update collapsed children
            if (_opened)
            {
                base.Update(deltaTime);
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            var visibleHeaderHeight = LayoutHeaderHeight;
            if (_showHeader)
            {
                // Cache data
                var style = Style.Current;
                var tree = ParentTree;
                bool isSelected = tree.Selection.Contains(this);
                bool isFocused = tree.ContainsFocus;
                bool isHeaderHighlighted = _mouseOverHeader || IsHeaderExternallyHighlighted;
                var left = _xOffset + 16; // offset + arrow
                var textRect = new Rectangle(left, 0, Width - left, _headerHeight);
                _margin.ShrinkRectangle(ref textRect);

                // Draw background
                if (!isSelected && !isHeaderHighlighted && Editor.Instance?.Options?.Options?.Interface?.AlternatingTreeRows == true)
                {
                    var treeY = PointToParent(tree, Float2.Zero).Y;
                    var row = (int)Mathf.Floor(treeY / Mathf.Max(1.0f, _headerHeight));
                    if ((row & 1) != 0)
                        Render2D.FillRectangle(_headerRect, Color.Lerp(style.Background, style.Foreground, 0.02f));
                }
                if (isSelected || isHeaderHighlighted)
                {
                    Render2D.FillRectangle(_headerRect, (isSelected && isFocused) ? BackgroundColorSelected : (isHeaderHighlighted ? BackgroundColorHighlighted : BackgroundColorSelectedUnfocused));
                    if (isSelected && isFocused)
                        Render2D.FillRectangle(new Rectangle(_headerRect.X, _headerRect.Y + 2.0f, 2.0f, _headerRect.Height - 4.0f), style.BorderSelected);
                }

                // Draw arrow
                if (HasAnyVisibleChild)
                {
                    Render2D.DrawSprite(_opened ? style.ArrowDown : style.ArrowRight, ArrowRect, isHeaderHighlighted ? style.Foreground : style.ForegroundGrey);
                }

                // Draw icon
                if (_iconCollaped.IsValid)
                {
                    var iconSize = HeaderIconSize;
                    if (iconSize > 0.0f)
                    {
                        Render2D.DrawSprite(_opened ? _iconOpened : _iconCollaped, new Rectangle(textRect.Left, (_headerHeight - iconSize) * 0.5f, iconSize, iconSize), IconColor);
                        textRect.X += HeaderIconSlotWidth;
                        textRect.Width -= HeaderIconSlotWidth;
                    }
                }
                ApplyHeaderTextLeftOffset(ref textRect);
                ApplyHeaderTextRightOffset(ref textRect);

                float textWidth = TextFont.GetFont().MeasureText(_text).X;
                Rectangle trueTextRect = textRect;
                trueTextRect.Width = textWidth;
                trueTextRect.Scale(_highlightScale);

                if (_isHightlighted && _debounceHighlightTime > 0.1f)
                {
                    Color highlightBackgroundColor = Editor.Instance.Options.Options.Visual.HighlightColor;
                    highlightBackgroundColor = highlightBackgroundColor.AlphaMultiplied(0.3f);
                    Render2D.FillRectangle(trueTextRect, highlightBackgroundColor);
                }

                // Draw text
                Color textColor = CacheTextColor();
                Render2D.DrawText(TextFont.GetFont(), _text, textRect, textColor, TextAlignment.Near, TextAlignment.Center);

                // Draw drag and drop effect
                if (IsDragOver && _tree.DraggedOverNode == this)
                {
                    switch (_dragOverMode)
                    {
                    case DragItemPositioning.At:
                        Render2D.FillRectangle(textRect, style.Selection);
                        Render2D.DrawRectangle(textRect, style.SelectionBorder);
                        break;
                    case DragItemPositioning.Above:
                        Render2D.DrawRectangle(new Rectangle(textRect.X, textRect.Top - DefaultDragInsertPositionMargin * 0.5f - DefaultNodeOffsetY - _margin.Top, textRect.Width, DefaultDragInsertPositionMargin), style.SelectionBorder);
                        break;
                    case DragItemPositioning.Below:
                        Render2D.DrawRectangle(new Rectangle(textRect.X, textRect.Bottom + _margin.Bottom - DefaultDragInsertPositionMargin * 0.5f, textRect.Width, DefaultDragInsertPositionMargin), style.SelectionBorder);
                        break;
                    }
                }

                // Show tree guidelines
                if (Editor.Instance.Options.Options.Interface.ShowTreeLines)
                {
                    ContainerControl parent = Parent;
                    TreeNode parentNode = parent as TreeNode;
                    bool thisNodeIsLast = false;
                    while (parentNode != null && (parentNode != tree.Children[0] || tree.DrawRootTreeLine))
                    {
                        float bottomOffset = 0;
                        float topOffset = 0;

                        if (parent == parentNode && this == parent.Children[0])
                            topOffset = 2;

                        if (thisNodeIsLast && parentNode.Children.Count == 1)
                            bottomOffset = topOffset != 0 ? 4 : 2;

                        if (parent == parentNode && this == parent.Children[^1] && !_opened)
                        {
                            thisNodeIsLast = true;
                            bottomOffset = topOffset != 0 ? 4 : 2;
                        }

                        // Derive the guide from the actual disclosure arrow instead of the
                        // text/icon offsets. Those offsets vary with content view scale, which
                        // otherwise leaves the guide one or more pixels away from the arrow.
                        var arrowRect = parentNode.ArrowRect;
                        var lineX = Mathf.Round(arrowRect.X + arrowRect.Width * 0.5f - 0.5f);
                        var lineRect1 = new Rectangle(lineX, parentNode.HeaderRect.Top + topOffset, 1, parentNode.HeaderRect.Height - bottomOffset);
                        if (HasAnyVisibleChild && CustomArrowRect.HasValue && CustomArrowRect.Value.Intersects(lineRect1))
                            lineRect1 = Rectangle.Empty; // Skip drawing line if it's overlapping the arrow rectangle
                        Render2D.FillRectangle(lineRect1, Color.FromRGB(0x343538));
                        parentNode = parentNode.Parent as TreeNode;
                    }
                }

                if (_isHightlighted && _debounceHighlightTime > 0.1f)
                {
                    // Draw highlights
                    Render2D.DrawRectangle(trueTextRect, Editor.Instance.Options.Options.Visual.HighlightColor, 3);
                }
            }

            // Base
            if (_opened)
            {
                if (ClipChildren)
                {
                    Render2D.PushClip(new Rectangle(0, visibleHeaderHeight, Width, Height - visibleHeaderHeight));
                    base.Draw();
                    Render2D.PopClip();
                }
                else
                {
                    base.Draw();
                }
            }
        }

        /// <inheritdoc />
        protected override void DrawChildren()
        {
            // Draw all visible child controls
            var children = _children;
            if (children.Count == 0)
                return;
            var last = children.Count - 1;

            if (CullChildren)
            {
                Render2D.PeekClip(out var globalClipping);
                Render2D.PeekTransform(out var globalTransform);

                // Try to estimate the rough location of the first and the last nodes, assuming the node height is constant
                var firstChildGlobalRect = GetChildGlobalRectangle(children[0], ref globalTransform);
                var firstVisibleChild = Math.Clamp((int)Math.Floor((globalClipping.Top - firstChildGlobalRect.Top) / _headerHeight) + 1, 0, last);
                if (GetChildGlobalRectangle(children[firstVisibleChild], ref globalTransform).Top > globalClipping.Top || !children[firstVisibleChild].Visible)
                {
                    // Estimate overshoot, either it's partially visible or hidden in the tree
                    for (; firstVisibleChild > 0; firstVisibleChild--)
                    {
                        var child = children[firstVisibleChild];
                        if (!child.Visible)
                            continue;
                        if (GetChildGlobalRectangle(child, ref globalTransform).Top < globalClipping.Top)
                            break;
                    }
                }
                var lastVisibleChild = Math.Clamp((int)Math.Ceiling((globalClipping.Bottom - firstChildGlobalRect.Top) / _headerHeight) + 1, firstVisibleChild, last);
                if (GetChildGlobalRectangle(children[lastVisibleChild], ref globalTransform).Top < globalClipping.Bottom || !children[lastVisibleChild].Visible)
                {
                    // Estimate overshoot, either it's partially visible or hidden in the tree
                    for (; lastVisibleChild < last; lastVisibleChild++)
                    {
                        var child = children[lastVisibleChild];
                        if (!child.Visible)
                            continue;
                        if (GetChildGlobalRectangle(child, ref globalTransform).Top > globalClipping.Bottom)
                            break;
                    }
                }

                for (int i = firstVisibleChild; i <= lastVisibleChild; i++)
                {
                    var child = children[i];
                    if (!child.Visible)
                        continue;
                    Render2D.PushTransform(ref child._cachedTransform);
                    child.Draw();
                    Render2D.PopTransform();
                }

                static Rectangle GetChildGlobalRectangle(Control control, ref Matrix3x3 globalTransform)
                {
                    Matrix3x3.Multiply(ref control._cachedTransform, ref globalTransform, out var globalChildTransform);
                    return new Rectangle(globalChildTransform.M31, globalChildTransform.M32, control.Width * globalChildTransform.M11, control.Height * globalChildTransform.M22);
                }
            }
            else
            {
                for (int i = 0; i <= last; i++)
                {
                    var child = children[i];
                    if (child.Visible)
                    {
                        Render2D.PushTransform(ref child._cachedTransform);
                        child.Draw();
                        Render2D.PopTransform();
                    }
                }
            }
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            UpdateMouseOverFlags(location);

            // Check if mouse hits the header
            if (_mouseOverHeader)
            {
                // Check if left button goes down
                if (button == MouseButton.Left)
                {
                    _isMouseDown = true;
                    _mouseDownOverArrow = _mouseOverArrow;
                    _mouseDownRecursiveToggle = HasAnyVisibleChild && IsRecursiveToggleModifierDown();
                    _mouseDownPos = location;
                    _mouseDownTime = Time.UnscaledGameTime;
                    if (_mouseDownOverArrow && HasAnyVisibleChild && !(_mouseDownRecursiveToggle && IsVisibleRootNode))
                    {
                        BeginArrowMouseCapture();
                    }
                }

                // Handled
                Focus();
                return true;
            }

            // Base
            if (_opened)
                return base.OnMouseDown(location, button);

            // Handled
            Focus();
            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            UpdateMouseOverFlags(location);

            // A platform mouse-up follows the double-click notification. Without this guard
            // disclosure arrows toggle open in the double-click handler and immediately
            // toggle closed again here; selection can also mutate after an editor opens.
            if (button == MouseButton.Left && Time.UnscaledGameTime <= _suppressLeftMouseUpUntil)
            {
                _suppressLeftMouseUpUntil = -1.0f;
                _isMouseDown = false;
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1.0f;
                EndArrowMouseCapture();
                return true;
            }

            if (button == MouseButton.Left && (_arrowMouseCaptureActive || _arrowMouseCaptureSuppressActions))
            {
                if (_arrowMouseCaptureActive)
                    UpdateArrowMouseCapture(location);
                _isMouseDown = false;
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1.0f;
                EndArrowMouseCapture();
                Focus();
                return true;
            }

            // A primary click is valid only when this node received its matching press.
            // Keep the press target so releasing over another part of the row cannot turn
            // a disclosure-arrow press into selection (or the inverse).
            bool completedLeftClick = button == MouseButton.Left && _isMouseDown;
            bool pressedArrow = _mouseDownOverArrow;
            bool recursiveToggle = _mouseDownRecursiveToggle;
            if (button == MouseButton.Left)
            {
                _isMouseDown = false;
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1;
            }

            // Check if mouse hits the header
            if (_mouseOverHeader)
            {
                // Skip mouse up event right after drag drop ends
                if (button == MouseButton.Left && Engine.FrameCount - _dragEndFrame < 10)
                    return true;

                // Prevent from selecting node when user is just clicking at an arrow
                if (!_mouseOverArrow)
                {
                    if (button == MouseButton.Left && completedLeftClick && recursiveToggle && HasAnyVisibleChild)
                    {
                        ToggleRecursive();
                        Focus();
                        return true;
                    }

                    // Ignore primary releases that did not start on this row, or that
                    // started on its disclosure arrow and ended over the label.
                    if (button == MouseButton.Left && (!completedLeftClick || pressedArrow))
                        return true;

                    if (button == MouseButton.Left)
                    {
                        // Check if user is pressing control key
                        var tree = ParentTree;
                        var window = tree.Root;
                        bool wasSoleSelectedNode = tree.Selection.Count == 1 && tree.SelectedNode == this;
                        bool hasSelectionModifier = window.GetKey(KeyboardKeys.Shift) || window.GetKey(KeyboardKeys.Control);
                        if (window.GetKey(KeyboardKeys.Shift))
                        {
                            // Select range
                            tree.SelectRange(this);
                        }
                        else if (window.GetKey(KeyboardKeys.Control))
                        {
                            // Add/Remove
                            tree.AddOrRemoveSelection(this);
                        }
                        else
                        {
                            // Select
                            tree.Select(this);
                        }
                        if (wasSoleSelectedNode && !hasSelectionModifier)
                            OnSelectedClickHeader();
                    }
                }

                // Check if mouse hits arrow
                if (button == MouseButton.Left && completedLeftClick && pressedArrow && _mouseOverArrow && HasAnyVisibleChild)
                {
                    if (recursiveToggle)
                        ToggleRecursive();
                    else
                    {
                        if (_opened)
                            Collapse();
                        else
                            Expand();
                    }
                }

                // Check if mouse hits bar
                if (button == MouseButton.Right && TestHeaderHit(ref location))
                {
                    ParentTree.OnRightClickInternal(this, ref location);
                }

                // Handled
                Focus();
                return true;
            }

            // Check if mouse hits bar
            if (button == MouseButton.Right && TestHeaderHit(ref location))
            {
                ParentTree.OnRightClickInternal(this, ref location);
            }

            // Base
            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && (_arrowMouseCaptureActive || _arrowMouseCaptureSuppressActions))
            {
                _isMouseDown = false;
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1.0f;
                _suppressLeftMouseUpUntil = Time.UnscaledGameTime + 0.15f;
                EndArrowMouseCapture();
                return true;
            }

            // Check if mouse hits bar
            if (TestHeaderHit(ref location))
            {
                if (button == MouseButton.Left)
                {
                    _isMouseDown = false;
                    _mouseDownOverArrow = false;
                    _mouseDownRecursiveToggle = false;
                    _mouseDownTime = -1.0f;
                    _suppressLeftMouseUpUntil = Time.UnscaledGameTime + 0.15f;
                }
                return OnMouseDoubleClickHeader(ref location, button);
            }

            // Check if animation has been finished
            if (_animationProgress >= 1.0f)
            {
                // Base
                return base.OnMouseDoubleClick(location, button);
            }

            return false;
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            UpdateMouseOverFlags(location);

            if (_arrowMouseCaptureActive || _arrowMouseCaptureSuppressActions)
            {
                if (_arrowMouseCaptureActive)
                    UpdateArrowMouseCapture(location);
                return;
            }

            // Check if start drag and drop
            if (_isMouseDown && !_mouseDownOverArrow && Float2.Distance(_mouseDownPos, location) > 10.0f)
            {
                // Clear flag
                _isMouseDown = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1;

                // Start
                DoDragDrop();
                return;
            }

            // Check if animation has been finished
            if (_animationProgress >= 1.0f)
            {
                // Base
                if (_opened)
                    base.OnMouseMove(location);
            }
        }

        private void UpdateMouseOverFlags(Vector2 location)
        {
            if (!_showHeader)
            {
                _mouseOverArrow = false;
                _mouseOverHeader = false;
                return;
            }

            // Cache flags
            _mouseOverArrow = HasAnyVisibleChild && ArrowRect.Contains(location);
            _mouseOverHeader = new Rectangle(0, 0, Width, _headerHeight - 1).Contains(location);
            if (_mouseOverHeader)
            {
                // Allow non-scrollable controls to stay on top of the header and override the mouse behaviour
                for (int i = 0; i < Children.Count; i++)
                {
                    if (!Children[i].IsScrollable && IntersectsChildContent(Children[i], location, out _))
                    {
                        _mouseOverHeader = false;
                        break;
                    }
                }
            }
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            // Clear flags
            _mouseOverArrow = false;
            _mouseOverHeader = false;

            if (_arrowMouseCaptureActive || _arrowMouseCaptureSuppressActions)
            {
                base.OnMouseLeave();
                return;
            }

            // Check if start drag and drop
            if (_isMouseDown)
            {
                // Clear flag
                _isMouseDown = false;
                _mouseDownTime = -1;

                if (!_mouseDownOverArrow)
                {
                    // Start
                    DoDragDrop();
                }
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
            }

            // Base
            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            if (_arrowMouseCaptureActive || _arrowMouseCaptureSuppressActions)
            {
                ClearArrowMouseCapture();
                _isMouseDown = false;
                _mouseDownOverArrow = false;
                _mouseDownRecursiveToggle = false;
                _mouseDownTime = -1.0f;
            }

            base.OnEndMouseCapture();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            // Base
            if (_opened)
                return base.OnKeyDown(key);
            return false;
        }

        /// <inheritdoc />
        public override void OnKeyUp(KeyboardKeys key)
        {
            // Base
            if (_opened)
                base.OnKeyUp(key);
        }

        /// <inheritdoc />
        public override void OnChildResized(Control control)
        {
            // Optimize if child is tree node that is not visible
            if (!_opened && control is TreeNode)
                return;

            PerformLayout();

            base.OnChildResized(control);
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            var result = base.OnDragEnter(ref location, data);
            if (!_showHeader)
                return result;

            // Check if no children handled that event
            _dragOverMode = DragItemPositioning.None;
            if (result == DragDropEffect.None)
            {
                UpdateDragPositioning(ref location);

                // Check if mouse is over header
                _isDragOverHeader = TestHeaderHit(ref location);
                if (_isDragOverHeader)
                {
                    if (ParentTree != null)
                        ParentTree.DraggedOverNode = this;

                    // Expand node if mouse goes over arrow
                    if (ArrowRect.Contains(location) && HasAnyVisibleChild && IsCollapsed)
                    {
                        Expand(true);
                        ParentTree?.FlushPendingPerformLayout();
                    }

                    result = OnDragEnterHeader(data);
                }

                if (result == DragDropEffect.None)
                    _dragOverMode = DragItemPositioning.None;
            }

            return result;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            var result = base.OnDragMove(ref location, data);
            if (!_showHeader)
                return result;

            // Check if no children handled that event
            ClearDragPositioning();
            if (result == DragDropEffect.None)
            {
                UpdateDragPositioning(ref location);

                // Check if mouse is over header
                bool isDragOverHeader = TestHeaderHit(ref location);
                if (isDragOverHeader)
                {
                    if (ParentTree != null)
                        ParentTree.DraggedOverNode = this;

                    // Expand node if mouse goes over arrow
                    if (ArrowRect.Contains(location) && HasAnyVisibleChild && IsCollapsed)
                    {
                        Expand(true);
                        ParentTree?.FlushPendingPerformLayout();
                    }

                    if (!_isDragOverHeader)
                        result = OnDragEnterHeader(data);
                    else
                        result = OnDragMoveHeader(data);
                }
                else if (_isDragOverHeader)
                {
                    OnDragLeaveHeader();
                }
                _isDragOverHeader = isDragOverHeader;

                if (result == DragDropEffect.None)
                    _dragOverMode = DragItemPositioning.None;
            }

            return result;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragDrop(ref Float2 location, DragData data)
        {
            var result = base.OnDragDrop(ref location, data);
            if (!_showHeader)
                return result;

            // Check if no children handled that event
            if (result == DragDropEffect.None)
            {
                UpdateDragPositioning(ref location);
                _dragEndFrame = Engine.FrameCount;

                // Check if mouse is over header
                if (TestHeaderHit(ref location))
                {
                    result = OnDragDropHeader(data);
                }
            }

            // Clear cache
            _isDragOverHeader = false;
            ClearDragPositioning();

            return result;
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            // Clear cache
            if (_isDragOverHeader)
            {
                _isDragOverHeader = false;
                OnDragLeaveHeader();
            }
            ClearDragPositioning();

            base.OnDragLeave();
        }

        /// <inheritdoc />
        public override bool OnTestTooltipOverControl(ref Float2 location)
        {
            return TestHeaderHit(ref location) && ShowTooltip;
        }

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            text = TooltipText;
            location = _headerRect.Size * new Float2(0.5f, 1.0f);
            area = new Rectangle(Float2.Zero, _headerRect.Size);
            return ShowTooltip;
        }

        /// <inheritdoc />
        protected override void OnSizeChanged()
        {
            base.OnSizeChanged();

            _headerRect = new Rectangle(0, 0, Width, LayoutHeaderHeight);
        }

        /// <inheritdoc />
        public override void PerformLayout(bool force = false)
        {
            if (_isLayoutLocked && !force)
                return;

            bool wasLocked = _isLayoutLocked;
            if (!wasLocked)
                LockChildrenRecursive();

            // Auto-size tree nodes to match the parent size
            var parent = Parent;
            var width = parent is TreeNode ? parent.Width : Width;

            // Optimize layout logic if node is collapsed
            if (_opened || _animationProgress < 1.0f)
            {
                Width = width;
                PerformLayoutBeforeChildren();
                for (int i = 0; i < _children.Count; i++)
                    _children[i].PerformLayout(true);
                PerformLayoutAfterChildren();
            }
            else
            {
                // TODO: perform layout for any non-TreeNode controls
                var headerHeight = LayoutHeaderHeight;
                _cachedHeight = headerHeight;
                Size = new Float2(width, headerHeight);
            }

            if (!wasLocked)
                UnlockChildrenRecursive();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            if (_opened)
            {
                // Update the nodes nesting level before the actual positioning
                float xOffset = _xOffset + ChildrenIndent;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node)
                        node._xOffset = xOffset;
                }
            }

            base.PerformLayoutBeforeChildren();
        }

        /// <inheritdoc />
        protected override void PerformLayoutAfterChildren()
        {
            var headerHeight = LayoutHeaderHeight;
            float y = headerHeight;
            float height = headerHeight;
            float xOffset = _xOffset + ChildrenIndent;

            // Skip full layout if it's fully collapsed
            if (_opened || _animationProgress < 1.0f)
            {
                y -= _cachedHeight * (_opened ? 1.0f - _animationProgress : _animationProgress);
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is TreeNode node && node.Visible)
                    {
                        node._xOffset = xOffset;
                        node.Location = new Float2(0, y);
                        float nodeHeight = node.Height + DefaultNodeOffsetY;
                        y += nodeHeight;
                        height += nodeHeight;
                    }
                }
            }

            _cachedHeight = height;
            Height = Mathf.Max(headerHeight, y);
        }

        /// <inheritdoc />
        protected override bool CanNavigateChild(Control child)
        {
            // Closed tree node skips navigation for hidden children
            if (IsCollapsed && child is TreeNode)
                return false;
            return base.CanNavigateChild(child);
        }

        /// <inheritdoc />
        protected override void OnParentChangedInternal()
        {
            _tree = null;

            base.OnParentChangedInternal();
        }

        /// <inheritdoc />
        public override int Compare(Control other)
        {
            if (other is TreeNode node)
            {
                return string.Compare(Text, node.Text, StringComparison.InvariantCulture);
            }
            return base.Compare(other);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (_lastHighlightedNode == this)
                _lastHighlightedNode = null;
            ParentTree?.Selection.Remove(this);

            base.OnDestroy();
        }
    }
}
