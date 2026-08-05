// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Docking
{
    /// <summary>
    /// Helper class used to handle docking windows dragging and docking.
    /// </summary>
    public class WindowDragHelper
    {
        private enum DragKind
        {
            FloatingWindow,
            Tab,
        }

        private FloatWindowDockPanel _toMove;

        private Float2 _dragOffset;
        private Float2 _restoreWindowSize;
        private Rectangle _rectDock;
        private Float2 _mouse;
        private DockState _toSet;
        private DockPanel _toDock;
        private DockPanel _aggregateDock;
        private bool _aggregateCandidate;
        private bool _wasCollapsedToTabPill;
        private readonly DragKind _dragKind;
        private Rectangle _rAggregateLeft, _rAggregateRight, _rAggregateBottom, _rAggregateUpper;
        private DockHintControl _aggregateDockHintDown, _aggregateDockHintUp, _aggregateDockHintLeft, _aggregateDockHintRight;

        private int _tabInsertionIndex = -1;
        private Window _dragSourceWindow;

        private Rectangle _rLeft, _rRight, _rBottom, _rUpper;
        private DockHintControl _dockHintDown, _dockHintUp, _dockHintLeft, _dockHintRight;

        /// <summary>
        /// The hint control size.
        /// </summary>
        public const float HintControlSize = 36.0f;

        private const float AggregateHintControlSize = 64.0f;

        /// <summary>
        /// Size of the temporary floating tab preview used while dragging a tab.
        /// </summary>
        public static readonly Float2 TabDragPillMinSize = new Float2(132.0f, 34.0f);

        /// <summary>
        /// Maximum width of the temporary floating tab preview used while dragging a tab.
        /// </summary>
        public const float TabDragPillMaxWidth = 260.0f;

        /// <summary>
        /// The opacity of the dragged window when hint controls are shown.
        /// </summary>
        public const float DragWindowOpacity = 0.72f;
        
        /// <summary>
        /// Returns true if any windows are being dragged.
        /// </summary>
        public static bool IsDragActive { get; private set; }

        private WindowDragHelper(FloatWindowDockPanel toMove, Window dragSourceWindow, DragKind dragKind, Float2 restoreWindowSize = default)
        {
            _dragKind = dragKind;
            IsDragActive = true;
            toMove.IsDragging = true;
            _toMove = toMove;
            _toSet = DockState.Float;
            var window = toMove.Window.Window;
            var mousePos = Platform.MousePosition;
            _restoreWindowSize = restoreWindowSize.LengthSquared > 4.0f ? restoreWindowSize : GetWindowClientSizeInUi(window);

            // Check if window is maximized and restore window for correct dragging
            if (window.IsMaximized)
            {
                var windowMousePos = mousePos - window.Position;
                var previousSize = window.Size;
                window.Restore();
                window.Position = mousePos - windowMousePos * window.Size / previousSize;
            }

            if (_dragKind == DragKind.Tab)
            {
                var pillSize = GetTabDragPillSize(toMove);
                if (pillSize.LengthSquared > 4.0f)
                {
                    window.ClientSize = pillSize * window.DpiScale;
                    toMove.Window.PerformLayout();
                    _wasCollapsedToTabPill = true;
                }
            }

            // When drag starts from a tabs the window might not be shown yet
            if (!window.IsVisible)
            {
                window.Show();
                window.Position = mousePos - new Float2(40, 10);
            }

            // Bind events
            FlaxEngine.Scripting.Update += OnUpdate;
            window.MouseUp += OnMouseUp;
#if !PLATFORM_SDL
            window.StartTrackingMouse(false);
#endif

            // Update rectangles
            UpdateRects(mousePos);
            
            // Ensure the dragged window stays on top of every other window
            window.IsAlwaysOnTop = true;

            _dragSourceWindow = dragSourceWindow;
            if (_dragSourceWindow != null) // Detaching a tab from existing window
            {
#if PLATFORM_SDL
                _dragOffset = new Float2(window.Size.X / 2, 10.0f);
#else
                _dragOffset = mousePos - window.Position;
#endif

                // The mouse up event is sent to the source window on Windows
                _dragSourceWindow.MouseUp += OnMouseUp;

                // TODO: when detaching tab in floating window (not main window), the drag source window is still main window?
                var dragDropSourceWindow = _dragKind == DragKind.Tab ? toMove.MasterPanel?.RootWindow.Window ?? Editor.Instance.Windows.MainWindow : _dragSourceWindow;
                window.DoDragDrop(window.Title, _dragOffset, dragDropSourceWindow);
#if !PLATFORM_SDL
                _dragSourceWindow.BringToFront();
#endif
            }
            else
            {
                _dragOffset = window.MousePosition;
                window.DoDragDrop(window.Title, _dragOffset, window);
            }
        }

        /// <summary>
        /// Releases unmanaged and - optionally - managed resources.
        /// </summary>
        public void Dispose()
        {
            IsDragActive = false;
            if (_toMove != null)
                _toMove.IsDragging = false;
            var window = _toMove?.Window?.Window;

            // Unbind events
            FlaxEngine.Scripting.Update -= OnUpdate;
            if (window != null)
            {
                window.MouseUp -= OnMouseUp;
#if !PLATFORM_SDL
                window.EndTrackingMouse();
#endif
            }
            if (_dragSourceWindow != null)
                _dragSourceWindow.MouseUp -= OnMouseUp;

            var aggregateDock = _aggregateDock;
            var aggregateCandidate = _aggregateCandidate;
            RemoveDockHints();
            var tabInsertionIndex = _tabInsertionIndex;
            _tabInsertionIndex = -1;

            if (_toMove == null)
                return;
            if (window != null)
            {
                window.Opacity = 1.0f;
                window.IsAlwaysOnTop = false;
                window.BringToFront();
            }

            // Check if window won't be docked
            if (_toSet == DockState.Float)
            {
                if (window == null)
                    return;

                if (_wasCollapsedToTabPill)
                    RestoreFloatingTabWindow(window);

                // Show base window
                window.Show();
            }
            else
            {
                bool hasNoChildPanels = _toMove.ChildPanelsCount == 0;

                if (hasNoChildPanels && _toSet == DockState.DockFill && tabInsertionIndex >= 0)
                {
                    var insertionIndex = tabInsertionIndex;
                    while (_toMove.TabsCount > 0)
                    {
                        _toDock.DockWindowAt(_toMove.GetTab(0), insertionIndex++);
                    }
                }
                // Check if window has only single tab
                else if (hasNoChildPanels && _toMove.TabsCount == 1)
                {
                    // Dock window
                    var tab = _toMove.GetTab(0);
                    if (aggregateCandidate && aggregateDock != null)
                        tab.ShowAggregate(_toSet, aggregateDock);
                    else
                        tab.Show(_toSet, _toDock);
                }
                // Check if dock as tab and has no child panels
                else if (hasNoChildPanels && _toSet == DockState.DockFill)
                {
                    // Dock all tabs
                    while (_toMove.TabsCount > 0)
                    {
                        _toMove.GetTab(0).Show(DockState.DockFill, _toDock);
                    }
                }
                else
                {
                    var selectedTab = _toMove.SelectedTab;

                    // Dock the first tab into the target location
                    if (_toMove.TabsCount > 0)
                    {
                        var firstTab = _toMove.GetTab(0);
                        if (aggregateCandidate && aggregateDock != null)
                            firstTab.ShowAggregate(_toSet, aggregateDock);
                        else
                            firstTab.Show(_toSet, _toDock);

                        // Dock rest of the tabs
                        while (_toMove.TabsCount > 0)
                        {
                            _toMove.GetTab(0).Show(DockState.DockFill, firstTab);
                        }
                    }

                    // Keep selected tab being selected
                    selectedTab?.SelectTab();
                }
                // Focus target window
                _toDock.Root.Focus();
            }

            _aggregateDock = null;
            _aggregateCandidate = false;
            _toMove = null;
        }

        /// <summary>
        /// Start dragging a floating dock panel.
        /// </summary>
        /// <param name="toMove">Floating dock panel to move.</param>
        /// <param name="dragSourceWindow">The window where dragging started from.</param>
        /// <returns>The window drag helper object.</returns>
        public static WindowDragHelper StartDragging(FloatWindowDockPanel toMove, Window dragSourceWindow = null)
        {
            if (toMove == null)
                throw new ArgumentNullException();

            return new WindowDragHelper(toMove, dragSourceWindow, DragKind.FloatingWindow);
        }

        internal static WindowDragHelper StartDraggingTab(FloatWindowDockPanel toMove, Window dragSourceWindow)
        {
            if (toMove == null)
                throw new ArgumentNullException();

            return new WindowDragHelper(toMove, dragSourceWindow, DragKind.Tab);
        }

        /// <summary>
        /// Start dragging a docked panel into a floating window.
        /// </summary>
        /// <param name="toMove">Dock window to move.</param>
        /// <param name="dragSourceWindow">The window where dragging started from.</param>
        /// <returns>The window drag helper object.</returns>
        public static WindowDragHelper StartDragging(DockWindow toMove, Window dragSourceWindow)
        {
            if (toMove == null)
                throw new ArgumentNullException();

            // Create floating window
            var restoreSize = toMove.DefaultSize;
            toMove.CreateFloating(Float2.Zero, GetTabDragPillSize(toMove), WindowStartPosition.CenterParent);

            // Get floating panel
            var window = (WindowRootControl)toMove.Root;
            var floatingPanelToMove = window.GetChild(0) as FloatWindowDockPanel;

            return new WindowDragHelper(floatingPanelToMove, dragSourceWindow, DragKind.Tab, restoreSize);
        }

        private sealed class DockHintControl : Control
        {
            private const float AnimationDuration = 0.24f;

            private bool _hasAnimationBounds;
            private bool _targetHovered;
            private float _opacity;
            private float _animationStartOpacity;
            private float _targetOpacity;
            private double _animationStartTime;
            private Rectangle _animationStartBounds;
            private Rectangle _targetBounds;

            public bool IsHovered;

            public DockHintControl()
            {
                AnchorPreset = AnchorPresets.StretchAll;
                Offsets = Margin.Zero;
            }

            public void ResetTargetBounds()
            {
                _hasAnimationBounds = false;
                _targetHovered = false;
                _opacity = 0.0f;
                _animationStartOpacity = 0.0f;
                _targetOpacity = 0.0f;
            }

            public bool SetTargetBounds(Rectangle compactBounds, Rectangle previewBounds, bool isHovered)
            {
                IsHovered = isHovered;
                var targetBounds = isHovered && IsHintAreaValid(previewBounds) ? previewBounds : compactBounds;
                if (!IsHintAreaValid(targetBounds))
                {
                    ResetTargetBounds();
                    return false;
                }

                if (!_hasAnimationBounds)
                {
                    if (!isHovered)
                        return false;

                    _hasAnimationBounds = true;
                    _targetHovered = isHovered;
                    _opacity = 1.0f;
                    _animationStartOpacity = 1.0f;
                    _targetOpacity = 1.0f;
                    _animationStartTime = Platform.TimeSeconds;
                    _animationStartBounds = IsHintAreaValid(compactBounds) ? compactBounds : targetBounds;
                    _targetBounds = targetBounds;
                    Bounds = _animationStartBounds;
                    return true;
                }

                var targetOpacity = isHovered ? 1.0f : 0.0f;
                if (_targetHovered != isHovered || !AreDockBoundsEquivalent(_targetBounds, targetBounds))
                {
                    _targetHovered = isHovered;
                    _animationStartOpacity = _opacity;
                    _targetOpacity = targetOpacity;
                    _animationStartTime = Platform.TimeSeconds;
                    _animationStartBounds = Bounds;
                    _targetBounds = targetBounds;
                }

                var progress = Mathf.Saturate((float)(Platform.TimeSeconds - _animationStartTime) / AnimationDuration);
                var easedProgress = Mathf.InterpEaseInOut(0.0f, 1.0f, progress, 2.0f);
                Bounds = LerpBounds(_animationStartBounds, _targetBounds, easedProgress);
                _opacity = Mathf.Lerp(_animationStartOpacity, _targetOpacity, easedProgress);
                if (!isHovered && progress >= 1.0f)
                {
                    ResetTargetBounds();
                    return false;
                }

                return true;
            }

            public override void Draw()
            {
                var style = Style.Current;
                var opacity = Mathf.Saturate(_opacity);
                var fillColor = style.ForegroundDisabled.AlphaMultiplied((IsHovered ? 0.24f : 0.10f) * opacity);
                var borderColor = style.BorderNormal.AlphaMultiplied((IsHovered ? 0.88f : 0.64f) * opacity);
                var bounds = new Rectangle(Float2.Zero, Size);
                StyleRendering.DrawRoundedRectangle(
                    bounds,
                    fillColor,
                    borderColor,
                    1.0f,
                    style.GetPopupCornerRadius());
            }
        }

        private static Float2 GetTabDragPillSize(DockWindow tab)
        {
            if (tab == null)
                return TabDragPillMinSize;

            var iconWidth = tab.Icon.IsValid ? DockPanel.DefaultButtonsSize + DockPanel.DefaultLeftTextMargin : 0.0f;
            var width = tab.TitleSize.X + DockPanel.DefaultLeftTextMargin + DockPanel.DefaultRightTextMargin + iconWidth + 20.0f;
            return new Float2(Mathf.Clamp(width, TabDragPillMinSize.X, TabDragPillMaxWidth), TabDragPillMinSize.Y);
        }

        private static Float2 GetTabDragPillSize(DockPanel panel)
        {
            return GetTabDragPillSize(panel?.SelectedTab ?? panel?.FirstTab);
        }

        private static Float2 GetWindowClientSizeInUi(Window window)
        {
            var result = window.ClientSize / window.DpiScale;
            return result.LengthSquared > 4.0f ? result : window.Size / window.DpiScale;
        }

        private void RestoreFloatingTabWindow(Window window)
        {
            var restoreSize = _restoreWindowSize.LengthSquared > 4.0f ? _restoreWindowSize : _toMove?.SelectedTab?.DefaultSize ?? Float2.Zero;
            if (restoreSize.LengthSquared <= 4.0f)
                return;

            var restoreScreenSize = restoreSize * window.DpiScale;
            window.ClientSize = restoreSize * window.DpiScale;
            window.Position = _mouse - new Float2(Mathf.Min(_dragOffset.X, restoreScreenSize.X - 24.0f), Mathf.Min(_dragOffset.Y, restoreScreenSize.Y - 24.0f));
            _toMove?.Window.PerformLayout();
        }

        private static DockPanel GetAggregateDockPanel(DockPanel dockPanel)
        {
            var rootWindow = dockPanel?.RootWindow;
            var floatingPanel = rootWindow?.GetChild<FloatWindowDockPanel>();
            return floatingPanel != null ? (DockPanel)floatingPanel : Editor.Instance.UI.MasterPanel;
        }

        private static bool AreDockBoundsEquivalent(Rectangle a, Rectangle b)
        {
            const float epsilon = 1.0f;
            return Mathf.Abs(a.X - b.X) <= epsilon
                && Mathf.Abs(a.Y - b.Y) <= epsilon
                && Mathf.Abs(a.Width - b.Width) <= epsilon
                && Mathf.Abs(a.Height - b.Height) <= epsilon;
        }

        private static ContainerControl GetAggregateHintParent(DockPanel dockPanel)
        {
            return dockPanel is FloatWindowDockPanel ? (ContainerControl)dockPanel.RootWindow : dockPanel;
        }

        private DockHintControl AddHintControl(ContainerControl panel, Float2 pivot)
        {
            if (panel == null)
                return null;

            var hintControl = panel.AddChild<DockHintControl>();
            hintControl.Size = new Float2(HintControlSize);
            hintControl.BackgroundColor = Color.Transparent;
            hintControl.Pivot = pivot;
            hintControl.PivotRelative = true;
            hintControl.Visible = false;
            return hintControl;
        }

        private void AddDockHints()
        {
            if (_toDock == null)
                return;

            if (_toDock.RootWindow.Window != _dragSourceWindow)
                _toDock.RootWindow.Window.MouseUp += OnMouseUp;

            _dockHintDown = AddHintControl(_toDock, new Float2(0.5f, 1));
            _dockHintUp = AddHintControl(_toDock, new Float2(0.5f, 0));
            _dockHintLeft = AddHintControl(_toDock, new Float2(0, 0.5f));
            _dockHintRight = AddHintControl(_toDock, new Float2(1, 0.5f));

            _aggregateDock = GetAggregateDockPanel(_toDock);
            if (_aggregateDock == null || _aggregateDock == _toDock || AreDockBoundsEquivalent(_toDock.DockAreaBounds, _aggregateDock.RootDockAreaBounds))
            {
                _aggregateDock = null;
            }
            else
            {
                var aggregateHintParent = GetAggregateHintParent(_aggregateDock);
                _aggregateDockHintDown = AddHintControl(aggregateHintParent, new Float2(0.5f, 1));
                _aggregateDockHintUp = AddHintControl(aggregateHintParent, new Float2(0.5f, 0));
                _aggregateDockHintLeft = AddHintControl(aggregateHintParent, new Float2(0, 0.5f));
                _aggregateDockHintRight = AddHintControl(aggregateHintParent, new Float2(1, 0.5f));
            }
        }

        private static void RemoveHintControl(Control hintControl)
        {
            if (hintControl?.Parent != null)
                hintControl.Parent.RemoveChild(hintControl);
        }

        private void RemoveDockHints()
        {
            if (_toDock != null)
                _toDock.TabsProxy?.ClearTabInsertionFeedback();

            var window = _toDock?.RootWindow?.Window;
            if (window != null && window != _dragSourceWindow)
                window.MouseUp -= OnMouseUp;

            RemoveHintControl(_dockHintDown);
            RemoveHintControl(_dockHintUp);
            RemoveHintControl(_dockHintLeft);
            RemoveHintControl(_dockHintRight);
            _dockHintDown = _dockHintUp = _dockHintLeft = _dockHintRight = null;

            RemoveHintControl(_aggregateDockHintDown);
            RemoveHintControl(_aggregateDockHintUp);
            RemoveHintControl(_aggregateDockHintLeft);
            RemoveHintControl(_aggregateDockHintRight);
            _aggregateDockHintDown = _aggregateDockHintUp = _aggregateDockHintLeft = _aggregateDockHintRight = null;
            _aggregateDock = null;
            _aggregateCandidate = false;
        }

        private void UpdateRects(Float2 mousePos)
        {
            // Cache mouse position
            _mouse = mousePos;

            if (_dragKind != DragKind.Tab)
            {
                RemoveDockHints();
                _toDock = null;
                _toSet = DockState.Float;
                _tabInsertionIndex = -1;
                _aggregateCandidate = false;
                return;
            }

            // Check intersection with any dock panel
            DockPanel dockPanel = null;
            if (_toMove.MasterPanel.HitTest(ref _mouse, _toMove, out var hitResults))
            {
                dockPanel = hitResults[0];

                // Prefer panel which currently has focus
                foreach (var hit in hitResults)
                {
                    if (hit.RootWindow.Window.IsFocused)
                    {
                        dockPanel = hit;
                        break;
                    }
                }

                // Prefer panel in the same window we hit earlier
                // TODO: this doesn't allow docking window into another floating window over the main window
                /*if (dockPanel?.RootWindow != _toDock?.RootWindow)
                {
                    foreach (var hit in hitResults)
                    {
                        if (hit.RootWindow == _toDock?.RootWindow)
                        {
                            dockPanel = _toDock;
                            break;
                        }
                    }
                }*/
            }

            if (dockPanel != _toDock)
            {
                RemoveDockHints();
                _toDock = dockPanel;
                AddDockHints();

                // Make sure the all the dock hint areas are not under other windows
                if (_toDock != Editor.Instance.UI.MasterPanel)
                    _toDock?.RootWindow.Window.BringToFront();
                //_toDock?.RootWindow.Window.Focus();

                // Make the dragged window transparent when dock hints are visible
                _toMove.Window.Window.Opacity = _toDock == null ? 1.0f : DragWindowOpacity;

#if !PLATFORM_SDL
                // Bring the drop source always to the top
                if (_dragSourceWindow != null)
                    _dragSourceWindow.BringToFront();
#endif
            }

            // Check dock state to use
            bool showProxyHints = _toDock != null;
            bool showBorderHints = showProxyHints;
            DockHintControl hoveredHintControl = null;
            var dockPreviewUpper = Rectangle.Empty;
            var dockPreviewBottom = Rectangle.Empty;
            var dockPreviewLeft = Rectangle.Empty;
            var dockPreviewRight = Rectangle.Empty;
            var aggregatePreviewUpper = Rectangle.Empty;
            var aggregatePreviewBottom = Rectangle.Empty;
            var aggregatePreviewLeft = Rectangle.Empty;
            var aggregatePreviewRight = Rectangle.Empty;
            var tabInsertionIndex = -1;
            if (showProxyHints)
            {
                // Disable docking windows with one or more dock panels inside
                if (_toMove.ChildPanelsCount > 0)
                    showBorderHints = false;

                // Get dock area
                _rectDock = _toDock.DockAreaBounds;

                // Cache dock rectangles
                var size = _rectDock.Size / _toDock.DpiScale;
                var offset = _toDock.PointFromScreen(_rectDock.Location);
                var dockBounds = new Rectangle(offset, size);
                var edgeWidth = Math.Min(size.X * 0.25f, 52.0f);
                var edgeHeight = Math.Min(size.Y * 0.25f, 52.0f);
                _rUpper = new Rectangle(dockBounds.Left, dockBounds.Top, dockBounds.Width, edgeHeight);
                _rBottom = new Rectangle(dockBounds.Left, dockBounds.Bottom - edgeHeight, dockBounds.Width, edgeHeight);
                _rLeft = new Rectangle(dockBounds.Left, dockBounds.Top, edgeWidth, dockBounds.Height);
                _rRight = new Rectangle(dockBounds.Right - edgeWidth, dockBounds.Top, edgeWidth, dockBounds.Height);
                dockPreviewUpper = GetDockPreviewBounds(dockBounds, DockState.DockTop);
                dockPreviewBottom = GetDockPreviewBounds(dockBounds, DockState.DockBottom);
                dockPreviewLeft = GetDockPreviewBounds(dockBounds, DockState.DockLeft);
                dockPreviewRight = GetDockPreviewBounds(dockBounds, DockState.DockRight);

                // Hit test, and calculate the approximation for filled area when hovered over the edge socket
                var toSet = DockState.Float;
                _aggregateCandidate = false;

                var tabsProxy = _toDock.TabsProxy;
                var dockAsTab = false;
                tabsProxy?.ClearTabInsertionFeedback();
                if (_toMove.ChildPanelsCount == 0 && tabsProxy != null)
                {
                    var tabPosition = tabsProxy.PointFromScreen(_mouse);
                    if (tabsProxy.TryGetTabInsertionIndex(tabPosition, out tabInsertionIndex))
                    {
                        toSet = DockState.DockFill;
                        dockAsTab = true;
                        tabsProxy.SetTabInsertionFeedback(_toMove, tabInsertionIndex);
                    }
                }

                // Aggregate hints target the internal junctions where the hovered leaf meets the root edge.
                if (!dockAsTab && showBorderHints && _aggregateDock != null)
                {
                    var aggregateRect = _aggregateDock.RootDockAreaBounds;
                    var leafRect = _toDock.DockAreaBounds;
                    var aggregateHintParent = GetAggregateHintParent(_aggregateDock);
                    var aggregateHintParentDpiScale = aggregateHintParent.DpiScale;
                    var aggregateSize = aggregateRect.Size / aggregateHintParentDpiScale;
                    var leafSize = leafRect.Size / aggregateHintParentDpiScale;
                    var aggregateOffset = aggregateHintParent.PointFromScreen(aggregateRect.Location);
                    var leafOffset = aggregateHintParent.PointFromScreen(leafRect.Location);
                    var aggregateBounds = new Rectangle(aggregateOffset, aggregateSize);
                    var leafBounds = new Rectangle(leafOffset, leafSize);
                    var aggregatePoint = aggregateHintParent.PointFromScreen(_mouse);
                    var canUseAggregateHints = IsDockPreviewBoundsValid(aggregateBounds) && IsDockPreviewBoundsValid(leafBounds);

                    _rAggregateUpper = Rectangle.Empty;
                    _rAggregateBottom = Rectangle.Empty;
                    _rAggregateLeft = Rectangle.Empty;
                    _rAggregateRight = Rectangle.Empty;
                    aggregatePreviewUpper = GetDockPreviewBounds(aggregateBounds, DockState.DockTop);
                    aggregatePreviewBottom = GetDockPreviewBounds(aggregateBounds, DockState.DockBottom);
                    aggregatePreviewLeft = GetDockPreviewBounds(aggregateBounds, DockState.DockLeft);
                    aggregatePreviewRight = GetDockPreviewBounds(aggregateBounds, DockState.DockRight);

                    var aggregateTopBoundaryDistance = float.MaxValue;
                    var aggregateBottomBoundaryDistance = float.MaxValue;
                    var aggregateLeftBoundaryDistance = float.MaxValue;
                    var aggregateRightBoundaryDistance = float.MaxValue;
                    var nearestEdge = DockState.Unknown;
                    var nearestEdgeDistance = float.MaxValue;
                    var nearestBoundaryDistance = float.MaxValue;

                    if (canUseAggregateHints && leafBounds.Top > aggregateBounds.Top && leafBounds.Top < aggregateBounds.Bottom)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.Y - leafBounds.Top);
                        var candidate = new Rectangle(aggregateBounds.Left, leafBounds.Top - AggregateHintControlSize, AggregateHintControlSize, AggregateHintControlSize * 2.0f);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateLeftBoundaryDistance)
                            {
                                aggregateLeftBoundaryDistance = boundaryDistance;
                                _rAggregateLeft = candidate;
                            }
                            var distance = aggregatePoint.X - aggregateBounds.Left;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockLeft;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }

                        candidate = new Rectangle(aggregateBounds.Right - AggregateHintControlSize, leafBounds.Top - AggregateHintControlSize, AggregateHintControlSize, AggregateHintControlSize * 2.0f);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateRightBoundaryDistance)
                            {
                                aggregateRightBoundaryDistance = boundaryDistance;
                                _rAggregateRight = candidate;
                            }
                            var distance = aggregateBounds.Right - aggregatePoint.X;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockRight;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }
                    }

                    if (canUseAggregateHints && leafBounds.Bottom > aggregateBounds.Top && leafBounds.Bottom < aggregateBounds.Bottom)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.Y - leafBounds.Bottom);
                        var candidate = new Rectangle(aggregateBounds.Left, leafBounds.Bottom - AggregateHintControlSize, AggregateHintControlSize, AggregateHintControlSize * 2.0f);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateLeftBoundaryDistance)
                            {
                                aggregateLeftBoundaryDistance = boundaryDistance;
                                _rAggregateLeft = candidate;
                            }
                            var distance = aggregatePoint.X - aggregateBounds.Left;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockLeft;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }

                        candidate = new Rectangle(aggregateBounds.Right - AggregateHintControlSize, leafBounds.Bottom - AggregateHintControlSize, AggregateHintControlSize, AggregateHintControlSize * 2.0f);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateRightBoundaryDistance)
                            {
                                aggregateRightBoundaryDistance = boundaryDistance;
                                _rAggregateRight = candidate;
                            }
                            var distance = aggregateBounds.Right - aggregatePoint.X;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockRight;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }
                    }

                    if (canUseAggregateHints && leafBounds.Left > aggregateBounds.Left && leafBounds.Left < aggregateBounds.Right)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.X - leafBounds.Left);
                        var candidate = new Rectangle(leafBounds.Left - AggregateHintControlSize, aggregateBounds.Top, AggregateHintControlSize * 2.0f, AggregateHintControlSize);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateTopBoundaryDistance)
                            {
                                aggregateTopBoundaryDistance = boundaryDistance;
                                _rAggregateUpper = candidate;
                            }
                            var distance = aggregatePoint.Y - aggregateBounds.Top;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockTop;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }

                        candidate = new Rectangle(leafBounds.Left - AggregateHintControlSize, aggregateBounds.Bottom - AggregateHintControlSize, AggregateHintControlSize * 2.0f, AggregateHintControlSize);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateBottomBoundaryDistance)
                            {
                                aggregateBottomBoundaryDistance = boundaryDistance;
                                _rAggregateBottom = candidate;
                            }
                            var distance = aggregateBounds.Bottom - aggregatePoint.Y;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockBottom;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }
                    }

                    if (canUseAggregateHints && leafBounds.Right > aggregateBounds.Left && leafBounds.Right < aggregateBounds.Right)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.X - leafBounds.Right);
                        var candidate = new Rectangle(leafBounds.Right - AggregateHintControlSize, aggregateBounds.Top, AggregateHintControlSize * 2.0f, AggregateHintControlSize);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateTopBoundaryDistance)
                            {
                                aggregateTopBoundaryDistance = boundaryDistance;
                                _rAggregateUpper = candidate;
                            }
                            var distance = aggregatePoint.Y - aggregateBounds.Top;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockTop;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }

                        candidate = new Rectangle(leafBounds.Right - AggregateHintControlSize, aggregateBounds.Bottom - AggregateHintControlSize, AggregateHintControlSize * 2.0f, AggregateHintControlSize);
                        if (candidate.Contains(ref aggregatePoint))
                        {
                            if (boundaryDistance < aggregateBottomBoundaryDistance)
                            {
                                aggregateBottomBoundaryDistance = boundaryDistance;
                                _rAggregateBottom = candidate;
                            }
                            var distance = aggregateBounds.Bottom - aggregatePoint.Y;
                            if (distance < nearestEdgeDistance || (distance == nearestEdgeDistance && boundaryDistance < nearestBoundaryDistance))
                            {
                                nearestEdge = DockState.DockBottom;
                                nearestEdgeDistance = distance;
                                nearestBoundaryDistance = boundaryDistance;
                            }
                        }
                    }

                    switch (nearestEdge)
                    {
                    case DockState.DockTop:
                        _aggregateCandidate = true;
                        toSet = DockState.DockTop;
                        hoveredHintControl = _aggregateDockHintUp;
                        break;
                    case DockState.DockBottom:
                        _aggregateCandidate = true;
                        toSet = DockState.DockBottom;
                        hoveredHintControl = _aggregateDockHintDown;
                        break;
                    case DockState.DockLeft:
                        _aggregateCandidate = true;
                        toSet = DockState.DockLeft;
                        hoveredHintControl = _aggregateDockHintLeft;
                        break;
                    case DockState.DockRight:
                        _aggregateCandidate = true;
                        toSet = DockState.DockRight;
                        hoveredHintControl = _aggregateDockHintRight;
                        break;
                    }
                }

                if (!dockAsTab && !_aggregateCandidate && showBorderHints)
                {
                    var hintTestPoint = _toDock.PointFromScreen(_mouse);
                    var nearestEdge = DockState.Unknown;
                    var nearestEdgeDistance = float.MaxValue;
                    if (dockBounds.Contains(ref hintTestPoint))
                    {
                        var topDistance = hintTestPoint.Y - dockBounds.Top;
                        if (topDistance <= edgeHeight && topDistance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockTop;
                            nearestEdgeDistance = topDistance;
                        }

                        var bottomDistance = dockBounds.Bottom - hintTestPoint.Y;
                        if (bottomDistance <= edgeHeight && bottomDistance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockBottom;
                            nearestEdgeDistance = bottomDistance;
                        }

                        var leftDistance = hintTestPoint.X - dockBounds.Left;
                        if (leftDistance <= edgeWidth && leftDistance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockLeft;
                            nearestEdgeDistance = leftDistance;
                        }

                        var rightDistance = dockBounds.Right - hintTestPoint.X;
                        if (rightDistance <= edgeWidth && rightDistance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockRight;
                            nearestEdgeDistance = rightDistance;
                        }
                    }

                    switch (nearestEdge)
                    {
                    case DockState.DockTop:
                        toSet = DockState.DockTop;
                        hoveredHintControl = _dockHintUp;
                        break;
                    case DockState.DockBottom:
                        toSet = DockState.DockBottom;
                        hoveredHintControl = _dockHintDown;
                        break;
                    case DockState.DockLeft:
                        toSet = DockState.DockLeft;
                        hoveredHintControl = _dockHintLeft;
                        break;
                    case DockState.DockRight:
                        toSet = DockState.DockRight;
                        hoveredHintControl = _dockHintRight;
                        break;
                    }
                }

                _toSet = toSet;
                _tabInsertionIndex = tabInsertionIndex;
            }
            else
            {
                _toSet = DockState.Float;
                _tabInsertionIndex = -1;
                _aggregateCandidate = false;
            }

            // Update hint controls visibility and location
            if (showProxyHints)
            {
                SetHintLayout(_dockHintDown, _rBottom, dockPreviewBottom, hoveredHintControl == _dockHintDown, showBorderHints);
                SetHintLayout(_dockHintLeft, _rLeft, dockPreviewLeft, hoveredHintControl == _dockHintLeft, showBorderHints);
                SetHintLayout(_dockHintRight, _rRight, dockPreviewRight, hoveredHintControl == _dockHintRight, showBorderHints);
                SetHintLayout(_dockHintUp, _rUpper, dockPreviewUpper, hoveredHintControl == _dockHintUp, showBorderHints);

                if (_aggregateDock != null)
                {
                    SetHintLayout(_aggregateDockHintDown, _rAggregateBottom, aggregatePreviewBottom, hoveredHintControl == _aggregateDockHintDown, showBorderHints && IsHintAreaValid(_rAggregateBottom));
                    SetHintLayout(_aggregateDockHintLeft, _rAggregateLeft, aggregatePreviewLeft, hoveredHintControl == _aggregateDockHintLeft, showBorderHints && IsHintAreaValid(_rAggregateLeft));
                    SetHintLayout(_aggregateDockHintRight, _rAggregateRight, aggregatePreviewRight, hoveredHintControl == _aggregateDockHintRight, showBorderHints && IsHintAreaValid(_rAggregateRight));
                    SetHintLayout(_aggregateDockHintUp, _rAggregateUpper, aggregatePreviewUpper, hoveredHintControl == _aggregateDockHintUp, showBorderHints && IsHintAreaValid(_rAggregateUpper));
                }
            }
        }

        private static Rectangle LerpBounds(Rectangle start, Rectangle end, float amount)
        {
            return new Rectangle(
                Float2.Lerp(start.Location, end.Location, amount),
                Float2.Lerp(start.Size, end.Size, amount));
        }

        private static Rectangle GetDockPreviewBounds(Rectangle bounds, DockState state)
        {
            var splitterSize = DockPanel.DefaultSplitterValue;
            switch (state)
            {
            case DockState.DockTop:
                bounds.Height *= splitterSize;
                break;
            case DockState.DockBottom:
                var bottomHeight = bounds.Height * splitterSize;
                bounds.Y = bounds.Bottom - bottomHeight;
                bounds.Height = bottomHeight;
                break;
            case DockState.DockLeft:
                bounds.Width *= splitterSize;
                break;
            case DockState.DockRight:
                var rightWidth = bounds.Width * splitterSize;
                bounds.X = bounds.Right - rightWidth;
                bounds.Width = rightWidth;
                break;
            }

            return bounds.Width > 16.0f && bounds.Height > 16.0f ? bounds.MakeExpanded(-10.0f) : bounds;
        }

        private static bool IsDockPreviewBoundsValid(Rectangle bounds)
        {
            return bounds.Width > HintControlSize && bounds.Height > HintControlSize;
        }

        private static bool IsHintAreaValid(Rectangle bounds)
        {
            return bounds.Width > 0.0f && bounds.Height > 0.0f;
        }

        private static Rectangle GetCompactHintBounds(Rectangle bounds)
        {
            if (!IsHintAreaValid(bounds))
                return Rectangle.Empty;

            var size = new Float2(HintControlSize);
            return new Rectangle(bounds.Center - size * 0.5f, size);
        }

        private static void SetHintLayout(DockHintControl hintControl, Rectangle compactArea, Rectangle previewBounds, bool isHovered, bool visible)
        {
            if (hintControl == null)
                return;

            hintControl.Visible = hintControl.SetTargetBounds(GetCompactHintBounds(compactArea), previewBounds, visible && isHovered);
        }

        private void OnMouseUp(ref Float2 location, MouseButton button, ref bool handled)
        {
            if (button == MouseButton.Left)
                Dispose();
        }

        private void OnUpdate()
        {
            // If the engine lost focus during dragging, end the action
            if (!Engine.HasFocus)
            {
                Dispose();
                return;
            }

            var mousePos = Platform.MousePosition;
            if (_mouse != mousePos)
            {
                if (_dragSourceWindow != null)
                    _toMove.Window.Window.Position = mousePos - _dragOffset;
            }

            UpdateRects(mousePos);
        }
    }
}
