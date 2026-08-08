// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Tree;
using FlaxEditor.GUI.Drag;
using FlaxEditor.GUI.Input;
using FlaxEditor.Options;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.GUI;
using FlaxEditor.Scripting;
using FlaxEditor.States;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Windows used to present loaded scenes collection and whole scene graph.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.SceneEditorWindow" />
    public partial class SceneTreeWindow : SceneEditorWindow
    {
        private TextBox _searchBox;
        private Button _newButton;
        private Button _viewButton;
        private Tree _tree;
        private Panel _sceneTreePanel;
        private bool _isUpdatingSelection;
        private bool _blockSceneTreeScroll = false;
        private bool _isSearchFilterUpdatePending;

        private DragAssets _dragAssets;
        private DragActorType _dragActorType;
        private DragControlType _dragControlType;
        private DragScriptItems _dragScriptItems;
        private DragHandlers _dragHandlers;
        private bool _isDropping = false;
        private bool _forceScrollNodeToView = false;
        private SelectionOverflowDirection _selectionOverflowHoverDirection;
        private SelectionOverflowDirection _selectionOverflowMouseDownDirection;
        private ActorTreeNode _viewportHoveredTreeNode;

        private const float SelectionOverflowToastMargin = 6.0f;
        private const float SelectionOverflowToastHeight = 22.0f;
        private const float SelectionOverflowToastHorizontalPadding = 10.0f;
        private enum SelectionOverflowDirection
        {
            None,
            Above,
            Below,
        }

        private struct SelectionOverflowState
        {
            public int AboveCount;
            public int BelowCount;
            public TreeNode AboveTarget;
            public TreeNode BelowTarget;
            public Rectangle AboveToastBounds;
            public Rectangle BelowToastBounds;
        }

        /// <summary>
        /// Scene tree panel.
        /// </summary>
        public Panel SceneTreePanel => _sceneTreePanel;

        /// <summary>
        /// Initializes a new instance of the <see cref="SceneTreeWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public SceneTreeWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Scene";
            Icon = editor.Icons.Globe32;
            var controlHeight = Style.Current.ControlHeight > 0.0f ? Style.Current.ControlHeight : 18.0f;
            const float headerGap = 4.0f;
            var headerPadding = ToolStrip.CompactHeaderPadding;
            var headerButtonSize = controlHeight;

            // Compact creation, search, and view toolbar shared with content-oriented panels.
            var headerPanel = new ContainerControl
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                BackgroundColor = Style.Current.Background,
                IsScrollable = false,
                Offsets = new Margin(0, 0, 0, ToolStrip.GetCompactHeaderHeight(controlHeight)),
            };
            _newButton = new Button
            {
                Parent = headerPanel,
                Bounds = new Rectangle(headerPadding, headerPadding, headerButtonSize, headerButtonSize),
                Text = "+",
                TooltipText = "New actor or control",
            };
            ApplyHeaderButtonStyle(_newButton);
            _newButton.Clicked += ShowNewMenu;
            var searchLeft = headerPadding + headerButtonSize + headerGap;
            var searchRightPadding = headerPadding + headerButtonSize + headerGap;
            _searchBox = new SearchBox
            {
                AnchorPreset = AnchorPresets.HorizontalStretchMiddle,
                Parent = headerPanel,
                Bounds = new Rectangle(searchLeft, headerPadding, headerPanel.Width - searchLeft - searchRightPadding, controlHeight),
                TooltipText = "Search the scene tree.\n\nt: or a: Actor type\ns: Script type\nc: Control type",
            };
            _searchBox.TextChanged += OnSearchBoxTextChanged;
            ScriptsBuilder.ScriptsReloadEnd += OnSearchBoxTextChanged;
            _viewButton = new Button
            {
                Parent = headerPanel,
                AnchorPreset = AnchorPresets.MiddleRight,
                Bounds = new Rectangle(headerPanel.Width - headerPadding - headerButtonSize, headerPadding, headerButtonSize, headerButtonSize),
                Text = "•••",
                TooltipText = "Scene view options",
            };
            ApplyHeaderButtonStyle(_viewButton);
            _viewButton.Clicked += ShowViewMenu;

            // Scene tree panel
            _sceneTreePanel = new Panel
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, headerPanel.Bottom, 0),
                IsScrollable = true,
                ScrollBars = ScrollBars.Both,
                Parent = this,
            };

            // Create scene structure tree
            var root = editor.Scene.Root;
            root.TreeNode.ChildrenIndent = 0;
            root.TreeNode.Expand();
            _tree = new Tree(true)
            {
                Margin = new Margin(0.0f, 0.0f, 0.0f, _sceneTreePanel.ScrollBarsSize),
                IsScrollable = true,
            };
            _tree.AddChild(root.TreeNode);
            _tree.SelectedChanged += Tree_OnSelectedChanged;
            _tree.RightClick += OnTreeRightClick;
            _tree.Parent = _sceneTreePanel;
            _tree.AfterDeferredLayout += () =>
            {
                if (_forceScrollNodeToView)
                {
                    _forceScrollNodeToView = false;
                    ScrollToSelectedNode();
                }
            };

            headerPanel.Parent = this;
            Editor.Options.OptionsChanged += OnOptionsChanged;
            ApplySceneTreeStyle();

            // Setup input actions
            InputActions.Add(options => options.SelectMode, () => Editor.MainTransformGizmo.ActiveMode = TransformGizmoBase.Mode.Select);
            InputActions.Add(options => options.TranslateMode, () => Editor.MainTransformGizmo.ActiveMode = TransformGizmoBase.Mode.Translate);
            InputActions.Add(options => options.RotateMode, () => Editor.MainTransformGizmo.ActiveMode = TransformGizmoBase.Mode.Rotate);
            InputActions.Add(options => options.ScaleMode, () => Editor.MainTransformGizmo.ActiveMode = TransformGizmoBase.Mode.Scale);
            InputActions.Add(options => options.FocusSelection, () => Editor.Windows.EditWin.Viewport.FocusSelection());
            InputActions.Add(options => options.LockFocusSelection, () => Editor.Windows.EditWin.Viewport.LockFocusSelection());
            InputActions.Add(options => options.Rename, RenameSelection);
        }

        private static void ApplyHeaderButtonStyle(Button button)
        {
            var style = Style.Current;
            button.BackgroundColor = Color.Transparent;
            button.BackgroundColorHighlighted = style.BackgroundHighlighted.AlphaMultiplied(0.82f);
            button.BackgroundColorSelected = style.BorderSelected;
            button.BorderColor = Color.Transparent;
            button.BorderColorHighlighted = Color.Transparent;
            button.BorderColorSelected = Color.Transparent;
            button.HasBorder = false;
            button.TextColor = style.Foreground;
            button.TextColorHighlighted = style.Foreground;
        }

        /// <inheritdoc />
        public override void OnPlayBeginning()
        {
            base.OnPlayBeginning();
            _blockSceneTreeScroll = true;
        }

        /// <inheritdoc />
        public override void OnPlayBegin()
        {
            base.OnPlayBegin();
            _blockSceneTreeScroll = false;
            OnSearchBoxTextChanged();
        }

        /// <inheritdoc />
        public override void OnPlayEnding()
        {
            base.OnPlayEnding();
            _blockSceneTreeScroll = true;
        }

        /// <inheritdoc />
        public override void OnPlayEnd()
        {
            base.OnPlayEnd();
            _blockSceneTreeScroll = true;
            OnSearchBoxTextChanged();
        }

        /// <summary>
        /// Enables or disables vertical and horizontal scrolling on the scene tree panel.
        /// </summary>
        /// <param name="enabled">The state to set scrolling to</param>
        public void ScrollingOnSceneTreeView(bool enabled)
        {
            if (_sceneTreePanel.VScrollBar != null)
                _sceneTreePanel.VScrollBar.ThumbEnabled = enabled;
            if (_sceneTreePanel.HScrollBar != null)
                _sceneTreePanel.HScrollBar.ThumbEnabled = enabled;
        }

        /// <summary>
        /// Scrolls to the selected node in the scene tree.
        /// </summary>
        public void ScrollToSelectedNode()
        {
            // Scroll to node
            var nodeSelection = _tree.Selection;
            if (nodeSelection.Count != 0)
            {
                var scrollControl = nodeSelection[nodeSelection.Count - 1];
                _sceneTreePanel.ScrollViewTo(scrollControl);
            }
        }

        /// <summary>
        /// Sets the actor highlighted in the scene tree when hovering the editor viewport.
        /// </summary>
        /// <param name="actorNode">The hovered actor node or null to clear the highlight.</param>
        public void SetViewportHoveredActor(ActorNode actorNode)
        {
            var treeNode = actorNode?.TreeNode;
            if (_viewportHoveredTreeNode == treeNode)
                return;

            if (_viewportHoveredTreeNode != null)
                _viewportHoveredTreeNode.IsViewportHovered = false;

            _viewportHoveredTreeNode = treeNode;

            if (_viewportHoveredTreeNode != null)
                _viewportHoveredTreeNode.IsViewportHovered = true;
        }

        private static bool IsTreeNodeHeaderVisible(TreeNode node)
        {
            if (node == null || !node.VisibleInHierarchy || node.HeaderRect.Height <= 0.0f)
                return false;

            return !(node.Parent is TreeNode parentNode) || !parentNode.IsCollapsedInHierarchy;
        }

        private Rectangle GetTreeNodeHeaderBounds(TreeNode node)
        {
            var headerRect = node.HeaderRect;
            headerRect.Location = node.PointToParent(_sceneTreePanel, headerRect.Location);
            return headerRect;
        }

        private static string GetSelectionOverflowText(int count, string direction)
        {
            return count == 1 ? $"1 selected item {direction}" : $"{count} selected items {direction}";
        }

        private static Rectangle GetSelectionOverflowToastBounds(Rectangle area, string text, bool top, float toastHeight, Font font)
        {
            if (!font || area.Width <= SelectionOverflowToastMargin * 2.0f || area.Height <= SelectionOverflowToastMargin * 2.0f)
                return Rectangle.Empty;

            var maxWidth = Mathf.Max(1.0f, area.Width - SelectionOverflowToastMargin * 2.0f);
            var maxTextWidth = Mathf.Max(1.0f, maxWidth - SelectionOverflowToastHorizontalPadding * 2.0f);
            var maxTextHeight = Mathf.Max(1.0f, toastHeight - 4.0f);
            var textSize = font.MeasureText(text);
            var textScale = 1.0f;
            if (textSize.X > maxTextWidth && textSize.X > 0.0f)
                textScale = Mathf.Min(textScale, maxTextWidth / textSize.X);
            if (textSize.Y > maxTextHeight && textSize.Y > 0.0f)
                textScale = Mathf.Min(textScale, maxTextHeight / textSize.Y);
            var toastWidth = Mathf.Min(maxWidth, textSize.X * textScale + SelectionOverflowToastHorizontalPadding * 2.0f);
            var toastX = area.X + (area.Width - toastWidth) * 0.5f;
            var toastY = top ? area.Y + SelectionOverflowToastMargin : area.Bottom - toastHeight - SelectionOverflowToastMargin;
            return new Rectangle(toastX, toastY, toastWidth, toastHeight);
        }

        private SelectionOverflowState GetSelectionOverflowState()
        {
            var result = new SelectionOverflowState
            {
                AboveToastBounds = Rectangle.Empty,
                BelowToastBounds = Rectangle.Empty,
            };

            if (_sceneTreePanel.VScrollBar == null || !_sceneTreePanel.VScrollBar.Enabled || _tree.Selection.Count == 0)
                return result;

            var clientArea = _sceneTreePanel.GetClientArea();
            var viewTop = -_sceneTreePanel.ViewOffset.Y + clientArea.Top;
            var viewBottom = viewTop + clientArea.Height;
            var nearestAboveBottom = float.MinValue;
            var nearestBelowTop = float.MaxValue;

            for (int i = 0; i < _tree.Selection.Count; i++)
            {
                var node = _tree.Selection[i];
                if (!IsTreeNodeHeaderVisible(node))
                    continue;

                var bounds = GetTreeNodeHeaderBounds(node);
                if (bounds.Bottom <= viewTop + Mathf.Epsilon)
                {
                    result.AboveCount++;
                    if (bounds.Bottom > nearestAboveBottom)
                    {
                        nearestAboveBottom = bounds.Bottom;
                        result.AboveTarget = node;
                    }
                }
                else if (bounds.Top >= viewBottom - Mathf.Epsilon)
                {
                    result.BelowCount++;
                    if (bounds.Top < nearestBelowTop)
                    {
                        nearestBelowTop = bounds.Top;
                        result.BelowTarget = node;
                    }
                }
            }

            if (result.AboveCount == 0 && result.BelowCount == 0)
                return result;

            var sceneTreeArea = new Rectangle(_sceneTreePanel.X + clientArea.X, _sceneTreePanel.Y + clientArea.Y, clientArea.Width, clientArea.Height);
            var toastHeight = Mathf.Min(SelectionOverflowToastHeight, sceneTreeArea.Height - SelectionOverflowToastMargin * 2.0f);
            if (result.AboveCount > 0 && result.BelowCount > 0)
                toastHeight = Mathf.Min(toastHeight, (sceneTreeArea.Height - SelectionOverflowToastMargin * 3.0f) * 0.5f);
            if (toastHeight <= 1.0f)
                return result;

            var font = Style.Current.FontSmall;
            if (result.AboveCount > 0)
                result.AboveToastBounds = GetSelectionOverflowToastBounds(sceneTreeArea, GetSelectionOverflowText(result.AboveCount, "above"), true, toastHeight, font);
            if (result.BelowCount > 0)
                result.BelowToastBounds = GetSelectionOverflowToastBounds(sceneTreeArea, GetSelectionOverflowText(result.BelowCount, "below"), false, toastHeight, font);

            return result;
        }

        private static void DrawSelectionOverflowToast(Rectangle toastRect, string text, bool isHovered, bool isPressed, Style style)
        {
            var font = style.FontSmall;
            if (!font || toastRect.Width <= 1.0f || toastRect.Height <= 1.0f)
                return;

            var maxTextWidth = Mathf.Max(1.0f, toastRect.Width - SelectionOverflowToastHorizontalPadding * 2.0f);
            var maxTextHeight = Mathf.Max(1.0f, toastRect.Height - 4.0f);
            var textSize = font.MeasureText(text);
            var textScale = 1.0f;
            if (textSize.X > maxTextWidth && textSize.X > 0.0f)
                textScale = Mathf.Min(textScale, maxTextWidth / textSize.X);
            if (textSize.Y > maxTextHeight && textSize.Y > 0.0f)
                textScale = Mathf.Min(textScale, maxTextHeight / textSize.Y);

            var fillColor = isPressed
                ? Color.Lerp(style.Background, style.BackgroundSelected, 0.62f)
                : isHovered
                    ? Color.Lerp(style.Background, style.BackgroundSelected, 0.46f)
                    : Color.Lerp(style.Background, style.BackgroundSelected, 0.30f);
            var borderColor = isHovered || isPressed ? style.BorderSelected : style.BorderSelected.AlphaMultiplied(0.75f);
            StyleRendering.DrawRoundedRectangle(toastRect, fillColor.AlphaMultiplied(0.96f), borderColor, 1.0f, style.GetPopupCornerRadius());
            if (isHovered || isPressed)
                Render2D.FillRectangle(new Rectangle(toastRect.X + 3.0f, toastRect.Y + 2.0f, 2.0f, toastRect.Height - 4.0f), style.BorderSelected);
            Render2D.DrawText(font, text, toastRect, style.Foreground, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, textScale);
        }

        private void DrawSelectionOverflowToasts(Style style)
        {
            var state = GetSelectionOverflowState();

            if (state.AboveCount > 0)
            {
                var isHovered = _selectionOverflowHoverDirection == SelectionOverflowDirection.Above;
                var isPressed = _selectionOverflowMouseDownDirection == SelectionOverflowDirection.Above && isHovered;
                DrawSelectionOverflowToast(state.AboveToastBounds, GetSelectionOverflowText(state.AboveCount, "above"), isHovered, isPressed, style);
            }
            if (state.BelowCount > 0)
            {
                var isHovered = _selectionOverflowHoverDirection == SelectionOverflowDirection.Below;
                var isPressed = _selectionOverflowMouseDownDirection == SelectionOverflowDirection.Below && isHovered;
                DrawSelectionOverflowToast(state.BelowToastBounds, GetSelectionOverflowText(state.BelowCount, "below"), isHovered, isPressed, style);
            }
        }

        private bool TryGetSelectionOverflowTarget(Float2 location, out SelectionOverflowDirection direction, out TreeNode target)
        {
            direction = SelectionOverflowDirection.None;
            target = null;

            var state = GetSelectionOverflowState();
            if (state.AboveTarget != null && state.AboveToastBounds.Width > 1.0f && state.AboveToastBounds.Height > 1.0f && state.AboveToastBounds.Contains(ref location))
            {
                direction = SelectionOverflowDirection.Above;
                target = state.AboveTarget;
                return true;
            }
            if (state.BelowTarget != null && state.BelowToastBounds.Width > 1.0f && state.BelowToastBounds.Height > 1.0f && state.BelowToastBounds.Contains(ref location))
            {
                direction = SelectionOverflowDirection.Below;
                target = state.BelowTarget;
                return true;
            }

            return false;
        }

        private bool IsSelectionOverflowToastHit(Float2 location)
        {
            return TryGetSelectionOverflowTarget(location, out _, out _);
        }

        private void UpdateSelectionOverflowHover(Float2 location)
        {
            if (TryGetSelectionOverflowTarget(location, out var direction, out _))
            {
                _selectionOverflowHoverDirection = direction;
                Cursor = CursorType.Hand;
            }
            else
            {
                _selectionOverflowHoverDirection = SelectionOverflowDirection.None;
                Cursor = CursorType.Default;
            }
        }

        private ActorNode FindHoveredSceneTreeActorNode(TreeNode node, ref Float2 location)
        {
            if (node == null || !node.VisibleInHierarchy)
                return null;

            if (IsTreeNodeHeaderVisible(node))
            {
                var bounds = GetTreeNodeHeaderBounds(node);
                if (bounds.Contains(ref location))
                    return node is ActorTreeNode actorTreeNode && actorTreeNode.Actor ? actorTreeNode.ActorNode : null;
            }
            if (node.IsCollapsed)
                return null;

            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is TreeNode childNode)
                {
                    var result = FindHoveredSceneTreeActorNode(childNode, ref location);
                    if (result != null)
                        return result;
                }
            }

            return null;
        }

        private void UpdateSceneTreeHoverOutline(Float2 location)
        {
            ActorNode actorNode = null;
            if (Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport && _sceneTreePanel != null && _tree != null)
            {
                var panelLocation = _sceneTreePanel.PointFromParent(this, location);
                for (int i = 0; i < _tree.ChildrenCount; i++)
                {
                    if (_tree.GetChild(i) is TreeNode node)
                    {
                        actorNode = FindHoveredSceneTreeActorNode(node, ref panelLocation);
                        if (actorNode != null)
                            break;
                    }
                }
            }
            Editor.Windows.EditWin.Viewport.SetSceneTreeHoveredActor(actorNode);
        }

        private void ClearSceneTreeHoverOutline()
        {
            Editor.Windows.EditWin.Viewport.SetSceneTreeHoveredActor(null);
        }

        private void ClearSelectionOverflowInputState()
        {
            _selectionOverflowHoverDirection = SelectionOverflowDirection.None;
            _selectionOverflowMouseDownDirection = SelectionOverflowDirection.None;
            Cursor = CursorType.Default;
        }

        private void ScrollToSelectionOverflowTarget(TreeNode target, SelectionOverflowDirection direction)
        {
            if (target == null)
                return;

            target.ExpandAllParents(true);
            if (direction != SelectionOverflowDirection.Below || _sceneTreePanel.VScrollBar == null || !_sceneTreePanel.VScrollBar.Enabled)
            {
                _sceneTreePanel.ScrollViewTo(target);
                return;
            }

            _tree.FlushPendingPerformLayout();
            _sceneTreePanel.PerformLayout();

            var bounds = GetTreeNodeHeaderBounds(target);
            if (_sceneTreePanel.HScrollBar != null && _sceneTreePanel.HScrollBar.Enabled)
                _sceneTreePanel.HScrollBar.ScrollViewTo(bounds.Left, bounds.Right);

            var clientArea = _sceneTreePanel.GetClientArea();
            var scrollValue = bounds.Bottom - clientArea.Top - clientArea.Height;
            _sceneTreePanel.VScrollBar.Value = scrollValue;
        }

        private void OnSearchBoxTextChanged()
        {
            // Skip events during setup or init stuff
            if (IsLayoutLocked || _tree == null || _searchBox == null)
            {
                _isSearchFilterUpdatePending = true;
                return;
            }

            _isSearchFilterUpdatePending = false;
            ClearSelectionOverflowInputState();
            PerformLayout();
            _tree.LockChildrenRecursive();

            // Update tree
            var query = _searchBox.Text;
            var root = Editor.Scene.Root;
            root.TreeNode.UpdateFilter(query);

            _tree.UnlockChildrenRecursive();

            // When keep the selected nodes in a view
            var nodeSelection = _tree.Selection;
            if (nodeSelection.Count != 0)
            {
                var node = nodeSelection[nodeSelection.Count - 1];
                node.Expand(true);
                _forceScrollNodeToView = true;
            }

            PerformLayout();
            PerformLayout();
        }

        private void ShowNewMenu()
        {
            var menu = new ActorCreationContextMenu(Editor, Spawn);
            menu.Show(_newButton.Parent, _newButton.BottomLeft);
        }

        private void ShowViewMenu()
        {
            var menu = new ContextMenu();
            var alternating = menu.AddButton("Alternating rows", () =>
            {
                Editor.Options.Options.Interface.AlternatingTreeRows = !Editor.Options.Options.Interface.AlternatingTreeRows;
                Editor.Options.Apply(Editor.Options.Options);
            });
            alternating.Checked = Editor.Options.Options.Interface.AlternatingTreeRows;

            ContextMenuButton highlightInViewport = null;
            highlightInViewport = menu.AddButton("Highlight in Editor", () =>
            {
                Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport = !Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport;
                if (!Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport)
                    ClearSceneTreeHoverOutline();
                highlightInViewport.Checked = Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport;
                Editor.Options.SaveOptions();
            });
            highlightInViewport.CloseMenuOnClick = false;
            highlightInViewport.TooltipText = "When hovered Scene items will highlight items in Editor.";
            highlightInViewport.Checked = Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport;

            ContextMenuButton highlightEditorHoverInScene = null;
            highlightEditorHoverInScene = menu.AddButton("Highlight Scene Items from Editor", () =>
            {
                Editor.Options.Options.Interface.HighlightViewportObjectHover = !Editor.Options.Options.Interface.HighlightViewportObjectHover;
                if (!Editor.Options.Options.Interface.HighlightViewportObjectHover)
                    SetViewportHoveredActor(null);
                highlightEditorHoverInScene.Checked = Editor.Options.Options.Interface.HighlightViewportObjectHover;
                Editor.Options.SaveOptions();
            });
            highlightEditorHoverInScene.CloseMenuOnClick = false;
            highlightEditorHoverInScene.TooltipText = "When hovered Editor items will highlight items in Scene.";
            highlightEditorHoverInScene.Checked = Editor.Options.Options.Interface.HighlightViewportObjectHover;

            var treeRowHeight = menu.AddButton("Tree Row Height");
            treeRowHeight.CloseMenuOnClick = false;
            var treeRowHeightValue = new FloatValueBox(Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 16.0f, 135, 2, 55.0f, 12.0f, 96.0f, 1.0f)
            {
                Parent = treeRowHeight
            };
            treeRowHeightValue.ValueChanged += () =>
            {
                var value = Mathf.Clamp(treeRowHeightValue.Value, 12.0f, 96.0f);
                Editor.Options.Options.Interface.TreeRowHeight = value;
                Style.Current.TreeRowHeight = value;
                Editor.Options.SaveOptions();
                ApplySceneTreeStyle();
            };

            var sceneIconSize = menu.AddButton("Scene Icon Size");
            sceneIconSize.CloseMenuOnClick = false;
            var sceneIconSizeValue = new FloatValueBox(Style.Current.GetSceneTreeIconSize(), 135, 2, 55.0f, 0.0f, 96.0f, 1.0f)
            {
                Parent = sceneIconSize
            };
            sceneIconSizeValue.ValueChanged += () =>
            {
                var value = Mathf.Clamp(sceneIconSizeValue.Value, 0.0f, 96.0f);
                Editor.Options.Options.Interface.SceneTreeIconSize = value;
                Style.Current.SceneTreeIconSize = value;
                Editor.Options.SaveOptions();
                ApplySceneTreeStyle();
            };

            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                highlightInViewport.Checked = Editor.Options.Options.Interface.HighlightSceneTreeHoverInViewport;
                highlightEditorHoverInScene.Checked = Editor.Options.Options.Interface.HighlightViewportObjectHover;
                treeRowHeightValue.Value = Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 16.0f;
                sceneIconSizeValue.Value = Style.Current.GetSceneTreeIconSize();
            };
            menu.Show(_viewButton.Parent, _viewButton.BottomLeft);
        }

        private void OnOptionsChanged(EditorOptions options)
        {
            if (!options.Interface.HighlightSceneTreeHoverInViewport)
                ClearSceneTreeHoverOutline();
            if (!options.Interface.HighlightViewportObjectHover)
                SetViewportHoveredActor(null);
            ApplySceneTreeStyle();
        }

        private void ApplySceneTreeStyle()
        {
            if (_tree == null)
                return;

            var headerHeight = Mathf.Clamp(Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 16.0f, 12.0f, 96.0f);
            ApplySceneTreeNodeStyle(_tree, headerHeight);
            _tree.PerformLayout(true);
            _sceneTreePanel?.PerformLayout();
        }

        private static void ApplySceneTreeNodeStyle(ContainerControl control, float headerHeight)
        {
            if (control == null)
                return;

            for (int i = 0; i < control.ChildrenCount; i++)
            {
                if (control.GetChild(i) is TreeNode node)
                {
                    node.HeaderHeight = headerHeight;
                    ApplySceneTreeNodeStyle(node, headerHeight);
                }
            }
        }

        private void Spawn(ActorCreationContextMenu.Entry entry)
        {
            Actor actor;
            switch (entry.Kind)
            {
            case ActorCreationContextMenu.EntryKind.Control:
                var control = entry.ScriptType.CreateInstance() as Control;
                if (control == null)
                {
                    Editor.LogWarning("Failed to create UI control of type " + entry.ScriptType.TypeName);
                    return;
                }
                actor = new UIControl { Control = control };
                break;
            case ActorCreationContextMenu.EntryKind.Primitive:
                actor = new StaticModel
                {
                    Model = FlaxEngine.Content.LoadAsync<Model>(StringUtils.CombinePaths(Globals.EngineContentFolder, "Editor/" + entry.AssetPath)),
                };
                break;
            default:
                actor = entry.ScriptType.CreateInstance() as Actor;
                break;
            }
            if (actor == null)
                return;

            Spawn(actor, entry.Name);
        }

        private void Spawn(Type type)
        {
            Spawn((Actor)FlaxEngine.Object.New(type), type.Name);
        }

        private void Spawn(Actor actor, string displayName)
        {
            if (actor == null)
                return;

            Actor parentActor = null;
            if (Editor.SceneEditing.HasSthSelected && Editor.SceneEditing.Selection[0] is ActorNode actorNode)
            {
                parentActor = actorNode.Actor;
                actorNode.TreeNode.Expand();
            }
            if (parentActor == null)
            {
                var scenes = Level.Scenes;
                if (scenes.Length > 0)
                    parentActor = scenes[scenes.Length - 1];
            }
            if (parentActor != null)
            {
                // Use the same location
                actor.Transform = parentActor.Transform;

                // Rename actor to identify it easily
                actor.Name = Utilities.Utils.IncrementNameNumber(displayName, x => parentActor.GetChild(x) == null);
            }

            // Spawn it
            Editor.SceneEditing.Spawn(actor, parentActor);

            Editor.SceneEditing.Select(actor);
            RenameSelection();
        }

        /// <summary>
        /// Focuses search box.
        /// </summary>
        public void Search()
        {
            _searchBox.Focus();
        }

        private void Tree_OnSelectedChanged(List<TreeNode> before, List<TreeNode> after)
        {
            // Check if lock events
            if (_isUpdatingSelection)
                return;

            if (after.Count > 0)
            {
                // Get actors from nodes
                var actors = new List<SceneGraphNode>(after.Count);
                for (int i = 0; i < after.Count; i++)
                {
                    if (after[i] is ActorTreeNode node && node.Actor)
                        actors.Add(node.ActorNode);
                }

                // Select
                Editor.SceneEditing.Select(actors);
            }
            else
            {
                // Deselect
                Editor.SceneEditing.Deselect();
            }
        }

        private void OnTreeRightClick(TreeNode node, Float2 location)
        {
            if (!Editor.StateMachine.CurrentState.CanEditScene)
                return;
            if (node is ActorTreeNode && !_tree.Selection.Contains(node))
                _tree.Select(node);
            ShowContextMenu(node, location);
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            Editor.SceneEditing.SelectionChanged += OnSelectionChanged;
            Editor.Scene.SceneGraphChanged += OnSceneGraphChanged;
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            base.OnUpdate();

            if (_isSearchFilterUpdatePending && !IsLayoutLocked)
                OnSearchBoxTextChanged();
        }

        private void OnSceneGraphChanged()
        {
            _isSearchFilterUpdatePending = true;
        }

        private void OnSelectionChanged()
        {
            ClearSelectionOverflowInputState();
            _isUpdatingSelection = true;

            var selection = Editor.SceneEditing.Selection;
            if (selection.Count == 0)
            {
                _tree.Deselect();
            }
            else
            {
                // Find nodes to select
                var nodes = new List<TreeNode>(selection.Count);
                for (int i = 0; i < selection.Count; i++)
                {
                    if (selection[i] is ActorNode node)
                    {
                        nodes.Add(node.TreeNode);
                    }
                }

                // Select nodes
                _tree.Select(nodes);

                // For single node selected scroll view so user can see it
                if (nodes.Count == 1 && !_blockSceneTreeScroll)
                {
                    nodes[0].ExpandAllParents(true);
                    _sceneTreePanel.ScrollViewTo(nodes[0]);
                }
            }

            _isUpdatingSelection = false;
        }

        /// <inheritdoc />
        public override void OnEditorStateChanged()
        {
            _blockSceneTreeScroll = Editor.StateMachine.ReloadingScriptsState.IsActive;
        }

        private bool ValidateDragAsset(AssetItem assetItem)
        {
            if (assetItem.IsOfType<SceneAsset>())
                return true;
            return assetItem.OnEditorDrag(this) && Level.IsAnySceneLoaded;
        }

        private static bool ValidateDragActorType(ScriptType actorType)
        {
            return Editor.Instance.CodeEditing.Actors.Get().Contains(actorType) && Level.IsAnySceneLoaded;
        }

        private static bool ValidateDragControlType(ScriptType controlType)
        {
            return Editor.Instance.CodeEditing.Controls.Get().Contains(controlType) && Level.IsAnySceneLoaded;
        }

        private static bool ValidateDragScriptItem(ScriptItem script)
        {
            return Editor.Instance.CodeEditing.Actors.Get(script) != ScriptType.Null && Level.IsAnySceneLoaded;
        }

        /// <inheritdoc />
        public override void Draw()
        {
            var style = Style.Current;

            // Draw overlay
            string overlayText = null;
            var state = Editor.StateMachine.CurrentState;
            var textWrap = TextWrapping.NoWrap;
            if (state is LoadingState)
            {
                overlayText = "Loading...";
            }
            else if (state is ChangingScenesState)
            {
                overlayText = "Loading scene...";
            }
            else if (((ContainerControl)_tree.GetChild(0)).ChildrenCount == 0)
            {
                overlayText = "No scene\nOpen one from the content window";
                textWrap = TextWrapping.WrapWords;
            }
            if (overlayText != null)
            {
                Render2D.DrawText(style.FontLarge, overlayText, GetClientArea(), style.ForegroundDisabled, TextAlignment.Center, TextAlignment.Center, textWrap);
            }

            base.Draw();

            if (overlayText == null)
                DrawSelectionOverflowToasts(style);
        }

        /// <inheritdoc />
        public override bool IntersectsChildContent(Control child, Float2 location, out Float2 childSpaceLocation)
        {
            if (child == _sceneTreePanel && IsSelectionOverflowToastHit(location))
            {
                childSpaceLocation = Float2.Zero;
                return false;
            }

            return base.IntersectsChildContent(child, location, out childSpaceLocation);
        }

        /// <inheritdoc />
        public override void OnMouseEnter(Float2 location)
        {
            UpdateSelectionOverflowHover(location);
            UpdateSceneTreeHoverOutline(location);

            base.OnMouseEnter(location);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            UpdateSelectionOverflowHover(location);
            UpdateSceneTreeHoverOutline(location);

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            ClearSelectionOverflowInputState();
            ClearSceneTreeHoverOutline();

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton buttons)
        {
            if (IsSelectionOverflowToastHit(location))
            {
                if (buttons == MouseButton.Left && TryGetSelectionOverflowTarget(location, out var direction, out _))
                {
                    _selectionOverflowHoverDirection = direction;
                    _selectionOverflowMouseDownDirection = direction;
                    Cursor = CursorType.Hand;
                }
                return true;
            }
            if (buttons == MouseButton.Left)
                _selectionOverflowMouseDownDirection = SelectionOverflowDirection.None;

            if (base.OnMouseDown(location, buttons))
                return true;

            if (buttons == MouseButton.Right)
                return true;

            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            if (IsSelectionOverflowToastHit(location))
                return true;

            return base.OnMouseWheel(location, delta);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton buttons)
        {
            if (buttons == MouseButton.Left && _selectionOverflowMouseDownDirection != SelectionOverflowDirection.None)
            {
                var mouseDownDirection = _selectionOverflowMouseDownDirection;
                _selectionOverflowMouseDownDirection = SelectionOverflowDirection.None;
                if (TryGetSelectionOverflowTarget(location, out var direction, out var target) && direction == mouseDownDirection)
                    ScrollToSelectionOverflowTarget(target, direction);
                UpdateSelectionOverflowHover(location);
                return true;
            }
            if (IsSelectionOverflowToastHit(location))
            {
                _selectionOverflowMouseDownDirection = SelectionOverflowDirection.None;
                return true;
            }

            if (base.OnMouseUp(location, buttons))
                return true;

            if (buttons == MouseButton.Right)
            {
                if (Editor.StateMachine.CurrentState.CanEditScene)
                {
                    // Show context menu
                    ShowContextMenu(Parent, location + _searchBox.BottomLeft);
                }

                return true;
            }

            if (buttons == MouseButton.Left)
            {
                if (Editor.StateMachine.CurrentState.CanEditScene && !_isDropping)
                {
                    Editor.SceneEditing.Deselect();
                }
                if (_isDropping)
                    _isDropping = false;
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton buttons)
        {
            if (IsSelectionOverflowToastHit(location))
                return true;

            return base.OnMouseDoubleClick(location, buttons);
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            var result = base.OnDragEnter(ref location, data);
            if (Editor.StateMachine.CurrentState.CanEditScene)
            {
                if (_dragHandlers == null)
                    _dragHandlers = new DragHandlers();
                if (_dragAssets == null)
                {
                    _dragAssets = new DragAssets(ValidateDragAsset);
                    _dragHandlers.Add(_dragAssets);
                }
                if (_dragAssets.OnDragEnter(data) && result == DragDropEffect.None)
                    return _dragAssets.Effect;
                if (_dragActorType == null)
                {
                    _dragActorType = new DragActorType(ValidateDragActorType);
                    _dragHandlers.Add(_dragActorType);
                }
                if (_dragActorType.OnDragEnter(data) && result == DragDropEffect.None)
                    return _dragActorType.Effect;
                if (_dragControlType == null)
                {
                    _dragControlType = new DragControlType(ValidateDragControlType);
                    _dragHandlers.Add(_dragControlType);
                }
                if (_dragControlType.OnDragEnter(data) && result == DragDropEffect.None)
                    return _dragControlType.Effect;
                if (_dragScriptItems == null)
                {
                    _dragScriptItems = new DragScriptItems(ValidateDragScriptItem);
                    _dragHandlers.Add(_dragScriptItems);
                }
                if (_dragScriptItems.OnDragEnter(data) && result == DragDropEffect.None)
                    return _dragScriptItems.Effect;
            }
            return result;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            var result = base.OnDragMove(ref location, data);
            if (result == DragDropEffect.None && Editor.StateMachine.CurrentState.CanEditScene && _dragHandlers != null)
            {
                result = _dragHandlers.Effect;
            }
            return result;
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            base.OnDragLeave();

            _dragHandlers?.OnDragLeave();
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragDrop(ref Float2 location, DragData data)
        {
            var result = base.OnDragDrop(ref location, data);
            if (result == DragDropEffect.None)
            {
                _isDropping = true;

                // Drag assets
                if (_dragAssets != null && _dragAssets.HasValidDrag)
                {
                    List<SceneGraphNode> graphNodes = new List<SceneGraphNode>();
                    for (int i = 0; i < _dragAssets.Objects.Count; i++)
                    {
                        var item = _dragAssets.Objects[i];
                        if (item.IsOfType<SceneAsset>())
                        {
                            Editor.Instance.Scene.OpenScene(item.ID, true);
                            continue;
                        }
                        var actor = item.OnEditorDrop(this);
                        actor.Name = item.ShortName;
                        Editor.SceneEditing.Spawn(actor);
                        var graphNode = Editor.Scene.GetActorNode(actor.ID);
                        if (graphNode != null)
                            graphNodes.Add(graphNode);
                        Editor.Scene.MarkSceneEdited(actor.Scene);
                    }
                    if (graphNodes.Count > 0)
                        Editor.SceneEditing.Select(graphNodes);
                    result = DragDropEffect.Move;
                }
                // Drag actor type
                else if (_dragActorType != null && _dragActorType.HasValidDrag)
                {
                    for (int i = 0; i < _dragActorType.Objects.Count; i++)
                    {
                        var item = _dragActorType.Objects[i];
                        var actor = item.CreateInstance() as Actor;
                        if (actor == null)
                        {
                            Editor.LogWarning("Failed to spawn actor of type " + item.TypeName);
                            continue;
                        }
                        actor.Name = item.Name;
                        Editor.SceneEditing.Spawn(actor);
                        Editor.Scene.MarkSceneEdited(actor.Scene);
                    }
                    result = DragDropEffect.Move;
                }
                // Drag control type
                else if (_dragControlType != null && _dragControlType.HasValidDrag)
                {
                    for (int i = 0; i < _dragControlType.Objects.Count; i++)
                    {
                        var item = _dragControlType.Objects[i];
                        var control = item.CreateInstance() as Control;
                        if (control == null)
                        {
                            Editor.LogWarning("Failed to spawn UIControl with control type " + item.TypeName);
                            continue;
                        }
                        var uiControl = new UIControl
                        {
                            Control = control,
                            Name = item.Name,
                        };
                        Editor.SceneEditing.Spawn(uiControl);
                        Editor.Scene.MarkSceneEdited(uiControl.Scene);
                    }
                    result = DragDropEffect.Move;
                }
                // Drag script item
                else if (_dragScriptItems != null && _dragScriptItems.HasValidDrag)
                {
                    List<SceneGraphNode> graphNodes = new List<SceneGraphNode>();
                    for (int i = 0; i < _dragScriptItems.Objects.Count; i++)
                    {
                        var item = _dragScriptItems.Objects[i];
                        var actorType = Editor.Instance.CodeEditing.Actors.Get(item);
                        if (actorType != ScriptType.Null)
                        {
                            var actor = actorType.CreateInstance() as Actor;
                            if (actor == null)
                            {
                                Editor.LogWarning("Failed to spawn actor of type " + actorType.TypeName);
                                continue;
                            }
                            actor.Name = actorType.Name;
                            Editor.SceneEditing.Spawn(actor);
                            var graphNode = Editor.Scene.GetActorNode(actor.ID);
                            if (graphNode != null)
                                graphNodes.Add(graphNode);
                            Editor.Scene.MarkSceneEdited(actor.Scene);
                        }
                    }
                    if (graphNodes.Count > 0)
                        Editor.SceneEditing.Select(graphNodes);
                    result = DragDropEffect.Move;
                }

                _dragHandlers.OnDragDrop(null);
            }
            return result;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            SetViewportHoveredActor(null);
            _dragAssets = null;
            _dragActorType = null;
            _dragControlType = null;
            _dragScriptItems = null;
            _dragHandlers?.Clear();
            _dragHandlers = null;
            _tree = null;
            _searchBox = null;
            Editor.Options.OptionsChanged -= OnOptionsChanged;
            Editor.SceneEditing.SelectionChanged -= OnSelectionChanged;
            Editor.Scene.SceneGraphChanged -= OnSceneGraphChanged;
            ScriptsBuilder.ScriptsReloadEnd -= OnSearchBoxTextChanged;

            base.OnDestroy();
        }
    }
}
