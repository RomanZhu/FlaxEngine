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

                // Check if docking as tabs at a specific insertion index
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
                    _toMove.GetTab(0).Show(_toSet, _toDock);
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

        private void AddDockHints()
        {
            if (_toDock == null)
                return;

            if (_toDock.RootWindow.Window != _dragSourceWindow)
                _toDock.RootWindow.Window.MouseUp += OnMouseUp;

            _dockHintDown = AddHintControl(new Float2(0.5f, 1));
            _dockHintUp = AddHintControl(new Float2(0.5f, 0));
            _dockHintLeft = AddHintControl(new Float2(0, 0.5f));
            _dockHintRight = AddHintControl(new Float2(1, 0.5f));

            Control AddHintControl(Float2 pivot)
            {
                DragVisuals hintControl = _toDock.AddChild<DragVisuals>();
                hintControl.Size = new Float2(HintControlSize);
                hintControl.BackgroundColor = Style.Current.Selection.AlphaMultiplied(0.6f);
                hintControl.Pivot = pivot;
                hintControl.PivotRelative = true;
                hintControl.Visible = false;
                return hintControl;
            }
        }
        
        private void RemoveDockHints()
        {
            if (_toDock == null)
                return;

            _toDock.TabsProxy?.ClearTabInsertionFeedback();
            var window = _toDock.RootWindow?.Window;
            if (window != null && window != _dragSourceWindow)
                window.MouseUp -= OnMouseUp;

            _dockHintDown?.Parent.RemoveChild(_dockHintDown);
            _dockHintUp?.Parent.RemoveChild(_dockHintUp);
            _dockHintLeft?.Parent.RemoveChild(_dockHintLeft);
            _dockHintRight?.Parent.RemoveChild(_dockHintRight);
            _dockHintDown = _dockHintUp = _dockHintLeft = _dockHintRight = null;
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
                var hintTestPoint = _toDock.PointFromScreen(_mouse);
                if (showBorderHints)
                {
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
            }

            // Update sizes and opacity of hint controls
            if (_toDock != null)
            {
                var mainColor = Style.Current.Selection;
                if (hoveredHintControl != _dockHintDown)
                {
                    _dockHintDown.Size = new Float2(HintControlSize);
                    _dockHintDown.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (hoveredHintControl != _dockHintLeft)
                {
                    _dockHintLeft.Size = new Float2(HintControlSize);
                    _dockHintLeft.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (hoveredHintControl != _dockHintRight)
                {
                    _dockHintRight.Size = new Float2(HintControlSize);
                    _dockHintRight.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }
                if (hoveredHintControl != _dockHintUp)
                {
                    _dockHintUp.Size = new Float2(HintControlSize);
                    _dockHintUp.BackgroundColor = mainColor.AlphaMultiplied(0.6f);
                }

                if (_toSet != DockState.Float)
                {
                    if (hoveredHintControl != null)
                    {
                        hoveredHintControl.BackgroundColor = mainColor;
                        hoveredHintControl.Size = hoveredSizeOverride - hoveredMargin;
                        hoveredHintControl.Location = hoveredPreviewLocation;
                    }
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
