// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Docking
{
    /// <summary>
    /// Proxy control used for docking <see cref="DockWindow"/> inside <see cref="DockPanel"/>.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    public class DockPanelProxy : ContainerControl
    {
        private DockPanel _panel;
        private InterfaceOptions.TabCloseButtonVisibility _closeButtonVisibility;
        private double _dragEnterTime = -1;
        private float _tabHeight, _minimumTabWidth;
        private bool _useMinimumTabWidth;
        private DockPanel _tabInsertionFeedbackPanel;
        private int _tabInsertionFeedbackIndex = -1;

        /// <summary>
        /// The is mouse down flag (left button).
        /// </summary>
        public bool IsMouseLeftButtonDown;

        /// <summary>
        /// The is mouse down flag (right button).
        /// </summary>
        public bool IsMouseRightButtonDown;

        /// <summary>
        /// The is mouse down flag (middle button).
        /// </summary>
        public bool IsMouseMiddleButtonDown;

        /// <summary>
        /// The is mouse down over cross button flag.
        /// </summary>
        public bool IsMouseDownOverCross;

        /// <summary>
        /// The mouse cursor over window.
        /// </summary>
        public DockWindow MouseOverWindow;

        /// <summary>
        /// The mouse down window.
        /// </summary>
        public DockWindow MouseDownWindow;

        /// <summary>
        /// The mouse position.
        /// </summary>
        public Float2 MousePosition = Float2.Minimum;

        /// <summary>
        /// The mouse position.
        /// </summary>
        public Float2 MouseStartPosition = Float2.Minimum;

        /// <summary>
        /// The start drag asynchronous window.
        /// </summary>
        public DockWindow StartDragAsyncWindow;

        internal bool IsUsingDockPanel => MouseOverWindow != null || StartDragAsyncWindow != null || (_panel is FloatWindowDockPanel floatPanel && floatPanel.IsDragging);
        private Rectangle HeaderRectangle => new Rectangle(0, 0, Width, _tabHeight);

        /// <summary>
        /// Initializes a new instance of the <see cref="DockPanelProxy"/> class.
        /// </summary>
        /// <param name="panel">The panel.</param>
        internal DockPanelProxy(DockPanel panel)
        : base(0, 0, 64, 64)
        {
            _panel = panel;
            AnchorPreset = AnchorPresets.StretchAll;
            Offsets = Margin.Zero;

            var options = Editor.Instance.Options;
            var interfaceOptions = options.Options.Interface;
            _tabHeight = interfaceOptions.TabHeight;
            _useMinimumTabWidth = interfaceOptions.UseMinimumTabWidth;
            _minimumTabWidth = interfaceOptions.MinimumTabWidth;

            options.OptionsChanged += OnEditorOptionsChanged;
            OnEditorOptionsChanged(options.Options);
        }

        private void OnEditorOptionsChanged(EditorOptions options)
        {
            _closeButtonVisibility = options.Interface.ShowTabCloseButton;
        }

        private DockWindow GetTabAtPos(Float2 position, out bool closeButton)
        {
            DockWindow result = null;
            closeButton = false;

            var tabsCount = _panel.TabsCount;
            if (tabsCount == 1)
            {
                var crossRect = new Rectangle(Width - DockPanel.DefaultButtonsSize - DockPanel.DefaultButtonsMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize);
                if (HeaderRectangle.Contains(position))
                {
                    result = _panel.GetTab(0);
                    closeButton = crossRect.Contains(position) && IsCloseButtonVisible(result, _closeButtonVisibility);
                }
            }
            else
            {
                float x = 0;
                for (int i = 0; i < tabsCount; i++)
                {
                    var tab = _panel.GetTab(i);
                    float width = GetTabWidth(tab);

                    var tabRect = new Rectangle(x, 0, width, HeaderRectangle.Height);
                    var isMouseOver = tabRect.Contains(position);
                    if (isMouseOver)
                    {
                        var crossRect = new Rectangle(x + width - DockPanel.DefaultButtonsSize - DockPanel.DefaultButtonsMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize);
                        closeButton = crossRect.Contains(position) && IsCloseButtonVisible(tab, _closeButtonVisibility);
                        result = tab;
                        break;
                    }
                    x += width;
                }
            }

            return result;
        }

        internal bool TryGetTabInsertionIndex(Float2 position, out int tabIndex)
        {
            tabIndex = -1;
            if (!HeaderRectangle.Contains(position))
                return false;

            var tabsCount = _panel.TabsCount;
            if (tabsCount == 0)
            {
                tabIndex = 0;
                return true;
            }
            if (tabsCount == 1)
            {
                tabIndex = position.X < HeaderRectangle.Width * 0.5f ? 0 : 1;
                return true;
            }

            float x = 0;
            for (int i = 0; i < tabsCount; i++)
            {
                var width = GetTabWidth(_panel.GetTab(i));
                if (position.X < x + width * 0.5f)
                {
                    tabIndex = i;
                    return true;
                }
                x += width;
            }

            tabIndex = tabsCount;
            return true;
        }

        internal void SetTabInsertionFeedback(DockPanel panel, int tabIndex)
        {
            _tabInsertionFeedbackPanel = panel;
            _tabInsertionFeedbackIndex = tabIndex;
        }

        internal void ClearTabInsertionFeedback()
        {
            _tabInsertionFeedbackPanel = null;
            _tabInsertionFeedbackIndex = -1;
        }

        private bool IsCloseButtonVisible(DockWindow win, InterfaceOptions.TabCloseButtonVisibility visibilityMode)
        {
            return IsCloseButtonVisible(win, visibilityMode, _panel);
        }

        private bool IsCloseButtonVisible(DockWindow win, InterfaceOptions.TabCloseButtonVisibility visibilityMode, DockPanel panel)
        {
            return visibilityMode != InterfaceOptions.TabCloseButtonVisibility.Never &&
                (visibilityMode == InterfaceOptions.TabCloseButtonVisibility.Always ||
                (visibilityMode == InterfaceOptions.TabCloseButtonVisibility.SelectedTab && panel.SelectedTab == win));
        }

        private float CalculateTabWidth(DockWindow win, InterfaceOptions.TabCloseButtonVisibility visibilityMode)
        {
            return CalculateTabWidth(win, visibilityMode, _panel);
        }

        private float CalculateTabWidth(DockWindow win, InterfaceOptions.TabCloseButtonVisibility visibilityMode, DockPanel panel)
        {
            var iconWidth = win.Icon.IsValid ? DockPanel.DefaultButtonsSize + DockPanel.DefaultLeftTextMargin : 0;
            var width = win.TitleSize.X + DockPanel.DefaultLeftTextMargin + DockPanel.DefaultRightTextMargin + iconWidth;

            if (IsCloseButtonVisible(win, visibilityMode, panel))
                width += 2 * DockPanel.DefaultButtonsMargin + DockPanel.DefaultButtonsSize;

            return width;
        }

        private float GetTabWidth(DockWindow win)
        {
            return GetTabWidth(win, _panel);
        }

        private float GetTabWidth(DockWindow win, DockPanel panel)
        {
            var width = CalculateTabWidth(win, _closeButtonVisibility, panel);
            if (_useMinimumTabWidth && width < _minimumTabWidth)
                width = _minimumTabWidth;
            return width;
        }

        private void GetTabRect(DockWindow win, out Rectangle bounds)
        {
            FlaxEngine.Assertions.Assert.IsTrue(_panel.ContainsTab(win));

            var tabsCount = _panel.TabsCount;
            if (tabsCount == 1)
            {
                bounds = HeaderRectangle;
                return;
            }
            else
            {
                float x = 0;
                for (int i = 0; i < tabsCount; i++)
                {
                    var tab = _panel.GetTab(i);
                    var titleSize = tab.TitleSize;
                    float width = GetTabWidth(tab);
                    if (tab == win)
                    {
                        bounds = new Rectangle(x, 0, width, HeaderRectangle.Height);
                        return;
                    }
                    x += width;
                }
            }

            bounds = Rectangle.Empty;
        }

        private void StartDrag(DockWindow win)
        {
            // Clear cache
            MouseOverWindow = null;
            MouseDownWindow = null;
            StartDragAsyncWindow = win;

            // Register for late update in an async manner (to prevent from crash due to changing UI structure on window undock)
            FlaxEngine.Scripting.LateUpdate += StartDragAsync;
        }

        private void StartDragAsync()
        {
            FlaxEngine.Scripting.LateUpdate -= StartDragAsync;

            if (StartDragAsyncWindow != null)
            {
                var win = StartDragAsyncWindow;
                StartDragAsyncWindow = null;

                // Check if has only one window docked and is floating
                if (_panel.ChildPanelsCount == 0 && _panel.TabsCount == 1 && _panel.IsFloating)
                {
                    // Create docking hint window but in an async manner
                    WindowDragHelper.StartDragging(_panel as FloatWindowDockPanel, win.RootWindow.Window);
                }
                else
                {
                    // Select another tab
                    int index = _panel.GetTabIndex(win);
                    if (index == 0)
                        index = _panel.TabsCount;
                    _panel.SelectTab(index - 1);

                    // Create docking hint window
                    WindowDragHelper.StartDragging(win, _panel.RootWindow.Window);
                }
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            // Cache data
            var style = Style.Current;
            var window = Root;
            bool containsFocus = ContainsFocus && ((WindowRootControl)window).Window.IsFocused;
            var headerRect = HeaderRectangle;
            var tabsCount = _panel.TabsCount;

            var sourcePanel = _tabInsertionFeedbackPanel;
            var feedbackIndex = _tabInsertionFeedbackIndex;
            var hasFeedback = sourcePanel != null && feedbackIndex >= 0 && sourcePanel.TabsCount > 0;
            if (hasFeedback)
                feedbackIndex = Math.Min(feedbackIndex, tabsCount);

            // Check if has only one window docked
            if (tabsCount == 1)
            {
                var tab = _panel.GetTab(0);
                if (!hasFeedback)
                {
                    // Draw header
                    bool isMouseOver = headerRect.Contains(MousePosition);
                    Render2D.FillRectangle(headerRect, containsFocus ? style.BackgroundSelected : isMouseOver ? style.BackgroundHighlighted : style.LightBackground);

                    float iconWidth = tab.Icon.IsValid ? DockPanel.DefaultButtonsSize + DockPanel.DefaultLeftTextMargin : 0;

                    if (tab.Icon.IsValid)
                    {
                        Render2D.DrawSprite(
                            tab.Icon,
                            new Rectangle(DockPanel.DefaultLeftTextMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize),
                            style.Foreground);
                    }

                    // Draw text
                    Render2D.DrawText(
                        style.FontMedium,
                        tab.Title,
                        new Rectangle(DockPanel.DefaultLeftTextMargin + iconWidth, 0, Width - DockPanel.DefaultLeftTextMargin - DockPanel.DefaultButtonsSize - 2 * DockPanel.DefaultButtonsMargin, HeaderRectangle.Height),
                        style.Foreground,
                        TextAlignment.Near,
                        TextAlignment.Center);

                    if (IsCloseButtonVisible(tab, _closeButtonVisibility))
                    {
                        // Draw cross
                        var crossRect = new Rectangle(Width - DockPanel.DefaultButtonsSize - DockPanel.DefaultButtonsMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize);
                        bool isMouseOverCross = isMouseOver && crossRect.Contains(MousePosition);
                        if (isMouseOverCross)
                            Render2D.FillRectangle(crossRect, (containsFocus ? style.BackgroundSelected : style.LightBackground) * 1.3f);
                        Render2D.DrawSprite(style.Cross, crossRect, isMouseOverCross ? style.Foreground : style.ForegroundGrey);
                    }
                }
                else
                {
                    Render2D.FillRectangle(headerRect, style.LightBackground);
                    var ghostWidth = GetTabInsertionFeedbackWidth(sourcePanel);
                    if (feedbackIndex == 0)
                    {
                        float insertionX = 0;
                        DrawTabInsertionFeedback(style, sourcePanel, ref insertionX);
                        DrawTab(style, tab, insertionX, Math.Max(0, Width - insertionX), containsFocus);
                        DrawTabInsertionMarker(style, 0);
                    }
                    else
                    {
                        var insertionX = Math.Max(0, Width - ghostWidth);
                        DrawTab(style, tab, 0, insertionX, containsFocus);
                        var ghostX = insertionX;
                        DrawTabInsertionFeedback(style, sourcePanel, ref ghostX);
                        DrawTabInsertionMarker(style, insertionX);
                    }
                }
            }
            else
            {
                // Draw background
                Render2D.FillRectangle(headerRect, style.LightBackground);

                // Render all tabs and insert the ghost tabs into the same flow.
                float x = 0;
                for (int i = 0; i < tabsCount; i++)
                {
                    if (hasFeedback && i == feedbackIndex)
                    {
                        var insertionX = x;
                        DrawTabInsertionFeedback(style, sourcePanel, ref x);
                        DrawTabInsertionMarker(style, insertionX);
                    }

                    var tab = _panel.GetTab(i);
                    var width = GetTabWidth(tab);
                    DrawTab(style, tab, x, width, containsFocus);
                    x += width;
                }
                if (hasFeedback && feedbackIndex >= tabsCount)
                {
                    var insertionX = x;
                    DrawTabInsertionFeedback(style, sourcePanel, ref x);
                    DrawTabInsertionMarker(style, insertionX);
                }

                // Draw selected tab strip
                Render2D.FillRectangle(new Rectangle(0, HeaderRectangle.Height - 2, Width, 2), containsFocus ? style.BackgroundSelected : style.BackgroundNormal);
            }
        }

        private void DrawTab(Style style, DockWindow tab, float x, float width, bool containsFocus)
        {
            var tabColor = Color.Black;
            var iconWidth = tab.Icon.IsValid ? DockPanel.DefaultButtonsSize + DockPanel.DefaultLeftTextMargin : 0;
            var tabRect = new Rectangle(x, 0, width, HeaderRectangle.Height);
            var isMouseOver = tabRect.Contains(MousePosition);
            var isSelected = _panel.SelectedTab == tab;

            if (isSelected)
            {
                tabColor = containsFocus ? style.BackgroundSelected : style.BackgroundNormal;
                Render2D.FillRectangle(tabRect, tabColor);
            }
            else if (isMouseOver)
            {
                tabColor = style.BackgroundHighlighted;
                Render2D.FillRectangle(tabRect, tabColor);
            }
            else
            {
                tabColor = style.BackgroundHighlighted;
                Render2D.DrawLine(tabRect.BottomLeft - new Float2(0, 1), tabRect.UpperLeft, tabColor);
                Render2D.DrawLine(tabRect.BottomRight - new Float2(0, 1), tabRect.UpperRight, tabColor);
            }

            if (tab.Icon.IsValid)
            {
                Render2D.DrawSprite(
                    tab.Icon,
                    new Rectangle(x + DockPanel.DefaultLeftTextMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize),
                    style.Foreground);
            }

            Render2D.DrawText(
                style.FontMedium,
                tab.Title,
                new Rectangle(x + DockPanel.DefaultLeftTextMargin + iconWidth, 0, 10000, HeaderRectangle.Height),
                style.Foreground,
                TextAlignment.Near,
                TextAlignment.Center);

            if (IsCloseButtonVisible(tab, _closeButtonVisibility))
            {
                var crossRect = new Rectangle(x + width - DockPanel.DefaultButtonsSize - DockPanel.DefaultButtonsMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize);
                bool isMouseOverCross = isMouseOver && crossRect.Contains(MousePosition);
                if (isMouseOverCross)
                    Render2D.FillRectangle(crossRect, tabColor * 1.3f);
                Render2D.DrawSprite(style.Cross, crossRect, isMouseOverCross ? style.Foreground : style.ForegroundGrey);
            }
        }

        private float GetTabInsertionFeedbackWidth(DockPanel sourcePanel)
        {
            float width = 0;
            for (int i = 0; i < sourcePanel.TabsCount; i++)
                width += GetTabWidth(sourcePanel.GetTab(i), sourcePanel);
            return width;
        }

        private void DrawTabInsertionFeedback(Style style, DockPanel sourcePanel, ref float x)
        {
            var ghostColor = style.Selection.AlphaMultiplied(0.35f);
            var ghostBorderColor = style.SelectionBorder.AlphaMultiplied(0.85f);
            var ghostTextColor = style.Foreground.AlphaMultiplied(0.85f);
            for (int i = 0; i < sourcePanel.TabsCount; i++)
            {
                var tab = sourcePanel.GetTab(i);
                var width = GetTabWidth(tab, sourcePanel);
                var tabRect = new Rectangle(x, 0, width, HeaderRectangle.Height);
                var iconWidth = tab.Icon.IsValid ? DockPanel.DefaultButtonsSize + DockPanel.DefaultLeftTextMargin : 0;

                Render2D.FillRectangle(tabRect, ghostColor);
                Render2D.DrawRectangle(tabRect, ghostBorderColor);
                if (tab.Icon.IsValid)
                {
                    Render2D.DrawSprite(
                        tab.Icon,
                        new Rectangle(x + DockPanel.DefaultLeftTextMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize),
                        ghostTextColor);
                }

                Render2D.DrawText(
                    style.FontMedium,
                    tab.Title,
                    new Rectangle(x + DockPanel.DefaultLeftTextMargin + iconWidth, 0, Math.Max(0, width - DockPanel.DefaultLeftTextMargin - DockPanel.DefaultRightTextMargin - iconWidth), HeaderRectangle.Height),
                    ghostTextColor,
                    TextAlignment.Near,
                    TextAlignment.Center);

                if (IsCloseButtonVisible(tab, _closeButtonVisibility, sourcePanel))
                {
                    var crossRect = new Rectangle(x + width - DockPanel.DefaultButtonsSize - DockPanel.DefaultButtonsMargin, (HeaderRectangle.Height - DockPanel.DefaultButtonsSize) / 2, DockPanel.DefaultButtonsSize, DockPanel.DefaultButtonsSize);
                    Render2D.DrawSprite(style.Cross, crossRect, ghostTextColor);
                }

                x += width;
            }
        }

        private void DrawTabInsertionMarker(Style style, float x)
        {
            Render2D.FillRectangle(new Rectangle(x - 1, 0, 2, HeaderRectangle.Height), style.SelectionBorder);
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            IsMouseLeftButtonDown = false;
            IsMouseRightButtonDown = false;
            IsMouseMiddleButtonDown = false;
            MouseDownWindow = null;
            MouseOverWindow = null;
            MousePosition = Float2.Minimum;

            base.OnLostFocus();
        }

        /// <inheritdoc />
        public override void OnMouseEnter(Float2 location)
        {
            MousePosition = location;

            base.OnMouseEnter(location);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {

            // Maximize/restore on double click
            var tab = GetTabAtPos(location, out _);
            var rootWindow = tab?.RootWindow;
            if (rootWindow != null && button == MouseButton.Left)
            {
                if (rootWindow.IsMaximized)
                    rootWindow.Restore();
                else
                    rootWindow.Maximize();
                return true;
            }

            return base.OnMouseDoubleClick(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            MouseOverWindow = GetTabAtPos(location, out IsMouseDownOverCross);
            MouseDownWindow = MouseOverWindow;
            MouseStartPosition = location;

            // Check buttons
            if (button == MouseButton.Left)
            {
                // Cache data
                IsMouseLeftButtonDown = true;
                if (!IsMouseDownOverCross && MouseDownWindow != null)
                    _panel.SelectTab(MouseDownWindow);
            }
            else if (button == MouseButton.Right)
            {
                // Cache data
                IsMouseRightButtonDown = true;
                if (MouseDownWindow != null)
                    _panel.SelectTab(MouseDownWindow, false);
            }
            else if (button == MouseButton.Middle)
            {
                // Cache data
                IsMouseMiddleButtonDown = true;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {

            // Check tabs under mouse position at the beginning and at the end
            MouseOverWindow = GetTabAtPos(location, out var overCross);

            // Check buttons
            if (button == MouseButton.Left && IsMouseLeftButtonDown)
            {
                // Clear flag
                IsMouseLeftButtonDown = false;

                // Check if tabs are the same and cross was pressed
                if (MouseOverWindow != null && MouseOverWindow == MouseDownWindow && IsMouseDownOverCross && overCross)
                    MouseOverWindow.Close(ClosingReason.User);
                MouseDownWindow = null;
            }
            else if (button == MouseButton.Right && IsMouseRightButtonDown)
            {
                // Clear flag
                IsMouseRightButtonDown = false;

                if (MouseOverWindow != null)
                    ShowContextMenu(MouseOverWindow, ref location);
            }
            else if (button == MouseButton.Middle && IsMouseMiddleButtonDown)
            {
                // Clear flag
                IsMouseMiddleButtonDown = false;

                if (MouseOverWindow != null)
                    MouseOverWindow.Close(ClosingReason.User);
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            MousePosition = location;
            MouseOverWindow = GetTabAtPos(location, out _);
            if (IsMouseLeftButtonDown)
            {
                // Check if mouse is outside the header
                if (!HeaderRectangle.Contains(location))
                {
                    // Clear flag
                    IsMouseLeftButtonDown = false;

                    // Check tab under the mouse
                    if (!IsMouseDownOverCross && MouseDownWindow != null)
                        StartDrag(MouseDownWindow);
                    MouseDownWindow = null;
                }
                // Check if single tab is tried to be moved
                else if (MouseDownWindow != null && _panel.TabsCount <= 1)
                {
                    if ((MousePosition - MouseStartPosition).Length > 3)
                    {
                        // Clear flag
                        IsMouseLeftButtonDown = false;

                        // Check tab under the mouse
                        if (!IsMouseDownOverCross && MouseDownWindow != null)
                            StartDrag(MouseDownWindow);
                        MouseDownWindow = null;
                    }
                }
                // Check if has more than one tab to change order
                else if (MouseDownWindow != null && _panel.TabsCount > 1)
                {
                    // Check if mouse left current tab rect
                    GetTabRect(MouseDownWindow, out Rectangle currWinRect);
                    if (!currWinRect.Contains(location))
                    {
                        int index = _panel.GetTabIndex(MouseDownWindow);

                        // Check if move right or left
                        if (location.X < currWinRect.X)
                        {
                            // Move left
                            _panel.MoveTabLeft(index);
                        }
                        else if (_panel.LastTab != MouseDownWindow)
                        {
                            // Move right
                            _panel.MoveTabRight(index);
                        }

                        // Update
                        _panel.PerformLayout();
                    }
                }
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            if (IsMouseLeftButtonDown)
            {
                IsMouseLeftButtonDown = false;

                // Check tabs under mouse position
                if (!IsMouseDownOverCross && MouseDownWindow != null)
                    StartDrag(MouseDownWindow);
            }
            MouseDownWindow = null;
            MouseOverWindow = null;
            IsMouseRightButtonDown = false;
            IsMouseMiddleButtonDown = false;
            MousePosition = Float2.Minimum;

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            var result = base.OnDragEnter(ref location, data);
            if (result != DragDropEffect.None)
                return result;
            return TrySelectTabUnderLocation(ref location);
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            var result = base.OnDragMove(ref location, data);
            if (result != DragDropEffect.None)
                return result;
            return TrySelectTabUnderLocation(ref location);
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            _dragEnterTime = -1;

            base.OnDragLeave();
        }

        /// <inheritdoc />
        public override void GetDesireClientArea(out Rectangle rect)
        {
            rect = new Rectangle(0, HeaderRectangle.Height, Width, Height - HeaderRectangle.Height);
        }

        private DragDropEffect TrySelectTabUnderLocation(ref Float2 location)
        {
            var tab = GetTabAtPos(location, out _);
            if (tab != null)
            {
                // Auto-select tab only if drag takes some time
                var time = Platform.TimeSeconds;
                if (_dragEnterTime < 0)
                    _dragEnterTime = time;
                if (time - _dragEnterTime < 0.3f)
                    return DragDropEffect.Link;
                _dragEnterTime = -1;

                _panel.SelectTab(tab);
                Update(0); // Fake update
                return DragDropEffect.Move;
            }
            _dragEnterTime = -1;
            return DragDropEffect.None;
        }

        private void ShowContextMenu(DockWindow tab, ref Float2 location)
        {
            var menu = new ContextMenu.ContextMenu
            {
                Tag = tab
            };
            tab.OnShowContextMenu(menu);
            menu.AddButton("Close", OnTabMenuCloseClicked);
            menu.AddButton("Close All", OnTabMenuCloseAllClicked);
            menu.AddButton("Close All But This", OnTabMenuCloseAllButThisClicked);
            if (_panel.Tabs.IndexOf(tab) + 1 < _panel.TabsCount)
            {
                menu.AddButton("Close All To The Right", OnTabMenuCloseAllToTheRightClicked);
            }
            if (!_panel.IsFloating)
            {
                menu.AddSeparator();
                menu.AddButton("Undock", OnTabMenuUndockClicked);
            }
            else if (!tab.RootWindow?.IsMaximized ?? false)
            {
                menu.AddSeparator();
                menu.AddButton("Maximize", OnTabMenuMaximizeClicked);
            }
            else if (tab.RootWindow?.IsMaximized ?? false)
            {
                menu.AddSeparator();
                menu.AddButton("Restore", OnTabMenuRestoreClicked);
            }
            menu.Show(this, location);
        }

        private void OnTabMenuCloseClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            tab.Close(ClosingReason.User);
            if (tab == MouseDownWindow)
                MouseDownWindow = null;
        }

        private void OnTabMenuCloseAllClicked(ContextMenuButton button)
        {
            _panel.CloseAll();
        }

        private void OnTabMenuCloseAllButThisClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            var windows = _panel.Tabs.ToArray();
            for (int i = 0; i < windows.Length; i++)
            {
                if (windows[i] != tab)
                    windows[i].Close();
            }
        }

        private void OnTabMenuCloseAllToTheRightClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            var windows = _panel.Tabs.ToArray();
            for (int i = _panel.Tabs.IndexOf(tab) + 1; i < windows.Length; i++)
            {
                windows[i].Close();
            }
        }

        private void OnTabMenuUndockClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            tab.ShowFloating();
        }

        private void OnTabMenuMaximizeClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            tab.RootWindow.Maximize();
        }

        private void OnTabMenuRestoreClicked(ContextMenuButton button)
        {
            var tab = (DockWindow)button.ParentContextMenu.Tag;
            tab.RootWindow.Restore();
        }
    }
}
