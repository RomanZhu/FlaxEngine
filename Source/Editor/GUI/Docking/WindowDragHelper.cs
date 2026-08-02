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
        private FloatWindowDockPanel _toMove;

        private Float2 _dragOffset;
        private Rectangle _rectDock;
        private Float2 _mouse;
        private DockState _toSet;
        private DockPanel _toDock;
        private DockPanel _aggregateDock;
        private bool _aggregateCandidate;
        private Rectangle _rAggregateLeft, _rAggregateRight, _rAggregateBottom, _rAggregateUpper;
        private Control _aggregateDockHintDown, _aggregateDockHintUp, _aggregateDockHintLeft, _aggregateDockHintRight;

        private int _tabInsertionIndex = -1;
        private Window _dragSourceWindow;

        private Rectangle _rLeft, _rRight, _rBottom, _rUpper;
        private Control _dockHintDown, _dockHintUp, _dockHintLeft, _dockHintRight;

        /// <summary>
        /// The hint control size.
        /// </summary>
        public const float HintControlSize = 48.0f;

        /// <summary>
        /// The opacity of the dragged window when hint controls are shown.
        /// </summary>
        public const float DragWindowOpacity = 0.4f;
        
        /// <summary>
        /// Returns true if any windows are being dragged.
        /// </summary>
        public static bool IsDragActive { get; private set; }

        private WindowDragHelper(FloatWindowDockPanel toMove, Window dragSourceWindow)
        {
            IsDragActive = true;
            toMove.IsDragging = true;
            _toMove = toMove;
            _toSet = DockState.Float;
            var window = toMove.Window.Window;
            var mousePos = Platform.MousePosition;

            // Check if window is maximized and restore window for correct dragging
            if (window.IsMaximized)
            {
                var windowMousePos = mousePos - window.Position;
                var previousSize = window.Size;
                window.Restore();
                window.Position = mousePos - windowMousePos * window.Size / previousSize;
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
                var dragSourceWindowWayland = toMove.MasterPanel?.RootWindow.Window ?? Editor.Instance.Windows.MainWindow;
                window.DoDragDrop(window.Title, _dragOffset, dragSourceWindowWayland);
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

            return new WindowDragHelper(toMove, dragSourceWindow);
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
            toMove.CreateFloating();

            // Get floating panel
            var window = (WindowRootControl)toMove.Root;
            var floatingPanelToMove = window.GetChild(0) as FloatWindowDockPanel;

            return new WindowDragHelper(floatingPanelToMove, dragSourceWindow);
        }

        private sealed class DragVisuals : Control
        {
            public DragVisuals()
            {
                AnchorPreset = AnchorPresets.StretchAll;
                Offsets = Margin.Zero;
            }

            public override void Draw()
            {
                base.Draw();
                Render2D.DrawRectangle(new Rectangle(Float2.Zero, Size), Style.Current.SelectionBorder);
            }
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

        private Control AddHintControl(ContainerControl panel, Float2 pivot)
        {
            if (panel == null)
                return null;

            var hintControl = panel.AddChild<DragVisuals>();
            hintControl.Size = new Float2(HintControlSize);
            hintControl.BackgroundColor = Style.Current.Selection.AlphaMultiplied(0.6f);
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
            Control hoveredHintControl = null;
            Float2 hoveredPreviewLocation = Float2.Zero;
            Float2 hoveredSizeOverride = Float2.Zero;
            var tabInsertionIndex = -1;
            float hoveredMargin = 1.0f;
            if (showProxyHints)
            {
                // Disable docking windows with one or more dock panels inside
                if (_toMove.ChildPanelsCount > 0)
                    showBorderHints = false;

                // Get dock area
                _rectDock = _toDock.DockAreaBounds;

                // Cache dock rectangles
                var size = _rectDock.Size / Platform.DpiScale;
#if PLATFORM_MAC && !PLATFORM_SDL
                size *= (float)Platform.Dpi / 96.0f; // TODO: refactor DPI support on macOS to skip such hacks
#endif
                var offset = _toDock.PointFromScreen(_rectDock.Location);
                var edgeWidth = Math.Min(size.X, Math.Max(HintControlSize, size.X / 3.0f));
                var edgeHeight = Math.Min(size.Y, Math.Max(HintControlSize, size.Y / 3.0f));
                _rUpper = new Rectangle(0, 0, size.X, edgeHeight) + offset;
                _rBottom = new Rectangle(0, size.Y - edgeHeight, size.X, edgeHeight) + offset;
                _rLeft = new Rectangle(0, 0, edgeWidth, size.Y) + offset;
                _rRight = new Rectangle(size.X - edgeWidth, 0, edgeWidth, size.Y) + offset;

                // Hit test, and calculate the approximation for filled area when hovered over the edge socket
                var toSet = DockState.Float;
                _aggregateCandidate = false;

                // Aggregate hints target the internal junctions where the hovered leaf meets the root edge.
                if (showBorderHints && _aggregateDock != null)
                {
                    var aggregateRect = _aggregateDock.RootDockAreaBounds;
                    var leafRect = _toDock.DockAreaBounds;
                    var aggregateSize = aggregateRect.Size / Platform.DpiScale;
                    var leafSize = leafRect.Size / Platform.DpiScale;
#if PLATFORM_MAC && !PLATFORM_SDL
                    var dpiScale = (float)Platform.Dpi / 96.0f;
                    aggregateSize *= dpiScale;
                    leafSize *= dpiScale;
#endif
                    var aggregateHintParent = GetAggregateHintParent(_aggregateDock);
                    var aggregateOffset = aggregateHintParent.PointFromScreen(aggregateRect.Location);
                    var leafOffset = aggregateHintParent.PointFromScreen(leafRect.Location);
                    var aggregateBounds = new Rectangle(aggregateOffset, aggregateSize);
                    var leafBounds = new Rectangle(leafOffset, leafSize);
                    var aggregatePoint = aggregateHintParent.PointFromScreen(_mouse);

                    _rAggregateUpper = Rectangle.Empty;
                    _rAggregateBottom = Rectangle.Empty;
                    _rAggregateLeft = Rectangle.Empty;
                    _rAggregateRight = Rectangle.Empty;

                    var aggregateTopBoundaryDistance = float.MaxValue;
                    var aggregateBottomBoundaryDistance = float.MaxValue;
                    var aggregateLeftBoundaryDistance = float.MaxValue;
                    var aggregateRightBoundaryDistance = float.MaxValue;
                    var nearestEdge = DockState.Unknown;
                    var nearestEdgeDistance = float.MaxValue;
                    var nearestBoundaryDistance = float.MaxValue;

                    if (leafBounds.Top > aggregateBounds.Top && leafBounds.Top < aggregateBounds.Bottom)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.Y - leafBounds.Top);
                        var candidate = new Rectangle(aggregateBounds.Left, leafBounds.Top - HintControlSize, HintControlSize, HintControlSize * 2.0f);
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

                        candidate = new Rectangle(aggregateBounds.Right - HintControlSize, leafBounds.Top - HintControlSize, HintControlSize, HintControlSize * 2.0f);
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

                    if (leafBounds.Bottom > aggregateBounds.Top && leafBounds.Bottom < aggregateBounds.Bottom)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.Y - leafBounds.Bottom);
                        var candidate = new Rectangle(aggregateBounds.Left, leafBounds.Bottom - HintControlSize, HintControlSize, HintControlSize * 2.0f);
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

                        candidate = new Rectangle(aggregateBounds.Right - HintControlSize, leafBounds.Bottom - HintControlSize, HintControlSize, HintControlSize * 2.0f);
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

                    if (leafBounds.Left > aggregateBounds.Left && leafBounds.Left < aggregateBounds.Right)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.X - leafBounds.Left);
                        var candidate = new Rectangle(leafBounds.Left - HintControlSize, aggregateBounds.Top, HintControlSize * 2.0f, HintControlSize);
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

                        candidate = new Rectangle(leafBounds.Left - HintControlSize, aggregateBounds.Bottom - HintControlSize, HintControlSize * 2.0f, HintControlSize);
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

                    if (leafBounds.Right > aggregateBounds.Left && leafBounds.Right < aggregateBounds.Right)
                    {
                        var boundaryDistance = Mathf.Abs(aggregatePoint.X - leafBounds.Right);
                        var candidate = new Rectangle(leafBounds.Right - HintControlSize, aggregateBounds.Top, HintControlSize * 2.0f, HintControlSize);
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

                        candidate = new Rectangle(leafBounds.Right - HintControlSize, aggregateBounds.Bottom - HintControlSize, HintControlSize * 2.0f, HintControlSize);
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
                        hoveredSizeOverride = new Float2(aggregateSize.X, aggregateSize.Y * DockPanel.DefaultSplitterValue);
                        hoveredPreviewLocation = new Float2(aggregateOffset.X, aggregateOffset.Y);
                        break;
                    case DockState.DockBottom:
                        _aggregateCandidate = true;
                        toSet = DockState.DockBottom;
                        hoveredHintControl = _aggregateDockHintDown;
                        hoveredSizeOverride = new Float2(aggregateSize.X, aggregateSize.Y * DockPanel.DefaultSplitterValue);
                        hoveredPreviewLocation = new Float2(aggregateOffset.X, aggregateOffset.Y + aggregateSize.Y - hoveredSizeOverride.Y);
                        break;
                    case DockState.DockLeft:
                        _aggregateCandidate = true;
                        toSet = DockState.DockLeft;
                        hoveredHintControl = _aggregateDockHintLeft;
                        hoveredSizeOverride = new Float2(aggregateSize.X * DockPanel.DefaultSplitterValue, aggregateSize.Y);
                        hoveredPreviewLocation = new Float2(aggregateOffset.X, aggregateOffset.Y);
                        break;
                    case DockState.DockRight:
                        _aggregateCandidate = true;
                        toSet = DockState.DockRight;
                        hoveredHintControl = _aggregateDockHintRight;
                        hoveredSizeOverride = new Float2(aggregateSize.X * DockPanel.DefaultSplitterValue, aggregateSize.Y);
                        hoveredPreviewLocation = new Float2(aggregateOffset.X + aggregateSize.X - hoveredSizeOverride.X, aggregateOffset.Y);
                        break;
                    }
                }

                if (!_aggregateCandidate && showBorderHints)
                {
                    var hintTestPoint = _toDock.PointFromScreen(_mouse);
                    var nearestEdge = DockState.Unknown;
                    var nearestEdgeDistance = float.MaxValue;
                    if (_rUpper.Contains(ref hintTestPoint))
                    {
                        var distance = hintTestPoint.Y - offset.Y;
                        if (distance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockTop;
                            nearestEdgeDistance = distance;
                        }
                    }
                    if (_rBottom.Contains(ref hintTestPoint))
                    {
                        var distance = offset.Y + size.Y - hintTestPoint.Y;
                        if (distance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockBottom;
                            nearestEdgeDistance = distance;
                        }
                    }
                    if (_rLeft.Contains(ref hintTestPoint))
                    {
                        var distance = hintTestPoint.X - offset.X;
                        if (distance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockLeft;
                            nearestEdgeDistance = distance;
                        }
                    }
                    if (_rRight.Contains(ref hintTestPoint))
                    {
                        var distance = offset.X + size.X - hintTestPoint.X;
                        if (distance < nearestEdgeDistance)
                        {
                            nearestEdge = DockState.DockRight;
                            nearestEdgeDistance = distance;
                        }
                    }

                    switch (nearestEdge)
                    {
                    case DockState.DockTop:
                        toSet = DockState.DockTop;
                        hoveredHintControl = _dockHintUp;
                        hoveredSizeOverride = new Float2(size.X, size.Y * DockPanel.DefaultSplitterValue);
                        hoveredPreviewLocation = new Float2(offset.X, offset.Y);
                        break;
                    case DockState.DockBottom:
                        toSet = DockState.DockBottom;
                        hoveredHintControl = _dockHintDown;
                        hoveredSizeOverride = new Float2(size.X, size.Y * DockPanel.DefaultSplitterValue);
                        hoveredPreviewLocation = new Float2(offset.X, offset.Y + size.Y - hoveredSizeOverride.Y);
                        break;
                    case DockState.DockLeft:
                        toSet = DockState.DockLeft;
                        hoveredHintControl = _dockHintLeft;
                        hoveredSizeOverride = new Float2(size.X * DockPanel.DefaultSplitterValue, size.Y);
                        hoveredPreviewLocation = new Float2(offset.X, offset.Y);
                        break;
                    case DockState.DockRight:
                        toSet = DockState.DockRight;
                        hoveredHintControl = _dockHintRight;
                        hoveredSizeOverride = new Float2(size.X * DockPanel.DefaultSplitterValue, size.Y);
                        hoveredPreviewLocation = new Float2(offset.X + size.X - hoveredSizeOverride.X, offset.Y);
                        break;
                    }
                }

                var tabsProxy = _toDock.TabsProxy;
                tabsProxy?.ClearTabInsertionFeedback();
                if (_toMove.ChildPanelsCount == 0 && tabsProxy != null)
                {
                    var tabPosition = tabsProxy.PointFromScreen(_mouse);
                    if (tabsProxy.TryGetTabInsertionIndex(tabPosition, out tabInsertionIndex))
                    {
                        toSet = DockState.DockFill;
                        hoveredHintControl = null;
                        hoveredSizeOverride = Float2.Zero;
                        _aggregateCandidate = false;
                        tabsProxy.SetTabInsertionFeedback(_toMove, tabInsertionIndex);
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

            // Update sizes and opacity of hint controls
            if (_toDock != null)
            {
                var mainColor = Style.Current.Selection;
                if (_dockHintDown != null && hoveredHintControl != _dockHintDown)
                {
                    _dockHintDown.Size = new Float2(HintControlSize);
                    _dockHintDown.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_dockHintLeft != null && hoveredHintControl != _dockHintLeft)
                {
                    _dockHintLeft.Size = new Float2(HintControlSize);
                    _dockHintLeft.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_dockHintRight != null && hoveredHintControl != _dockHintRight)
                {
                    _dockHintRight.Size = new Float2(HintControlSize);
                    _dockHintRight.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_dockHintUp != null && hoveredHintControl != _dockHintUp)
                {
                    _dockHintUp.Size = new Float2(HintControlSize);
                    _dockHintUp.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_aggregateDockHintDown != null && hoveredHintControl != _aggregateDockHintDown)
                {
                    _aggregateDockHintDown.Size = new Float2(HintControlSize);
                    _aggregateDockHintDown.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_aggregateDockHintLeft != null && hoveredHintControl != _aggregateDockHintLeft)
                {
                    _aggregateDockHintLeft.Size = new Float2(HintControlSize);
                    _aggregateDockHintLeft.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_aggregateDockHintRight != null && hoveredHintControl != _aggregateDockHintRight)
                {
                    _aggregateDockHintRight.Size = new Float2(HintControlSize);
                    _aggregateDockHintRight.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (_aggregateDockHintUp != null && hoveredHintControl != _aggregateDockHintUp)
                {
                    _aggregateDockHintUp.Size = new Float2(HintControlSize);
                    _aggregateDockHintUp.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }

                if (_toSet != DockState.Float && hoveredHintControl != null)
                {
                    hoveredHintControl.BackgroundColor = mainColor;
                    hoveredHintControl.Size = hoveredSizeOverride - hoveredMargin;
                    hoveredHintControl.Location = hoveredPreviewLocation;
                }
            }

            // Update hint controls visibility and location
            if (showProxyHints)
            {
                if (hoveredHintControl != _dockHintDown)
                    _dockHintDown.Location = _rBottom.Location;
                if (hoveredHintControl != _dockHintLeft)
                    _dockHintLeft.Location = _rLeft.Location;
                if (hoveredHintControl != _dockHintRight)
                    _dockHintRight.Location = _rRight.Location;
                if (hoveredHintControl != _dockHintUp)
                    _dockHintUp.Location = _rUpper.Location;

                _dockHintDown.Visible = hoveredHintControl == _dockHintDown;
                _dockHintLeft.Visible = hoveredHintControl == _dockHintLeft;
                _dockHintRight.Visible = hoveredHintControl == _dockHintRight;
                _dockHintUp.Visible = hoveredHintControl == _dockHintUp;

                if (_aggregateDock != null)
                {
                    if (hoveredHintControl != _aggregateDockHintDown)
                        _aggregateDockHintDown.Location = _rAggregateBottom.Location;
                    if (hoveredHintControl != _aggregateDockHintLeft)
                        _aggregateDockHintLeft.Location = _rAggregateLeft.Location;
                    if (hoveredHintControl != _aggregateDockHintRight)
                        _aggregateDockHintRight.Location = _rAggregateRight.Location;
                    if (hoveredHintControl != _aggregateDockHintUp)
                        _aggregateDockHintUp.Location = _rAggregateUpper.Location;

                    _aggregateDockHintDown.Visible = hoveredHintControl == _aggregateDockHintDown;
                    _aggregateDockHintLeft.Visible = hoveredHintControl == _aggregateDockHintLeft;
                    _aggregateDockHintRight.Visible = hoveredHintControl == _aggregateDockHintRight;
                    _aggregateDockHintUp.Visible = hoveredHintControl == _aggregateDockHintUp;
                }
            }
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

                UpdateRects(mousePos);
            }
        }
    }
}
