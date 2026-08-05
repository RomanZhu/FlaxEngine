#if PLATFORM_WINDOWS || PLATFORM_SDL || PLATFORM_MAC
#define USE_IS_FOREGROUND
#else
#endif
#if PLATFORM_SDL || PLATFORM_MAC
#define USE_SDL_WORKAROUNDS
#endif
// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEngine;
using FlaxEngine.Assertions;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.ContextMenu
{
    /// <summary>
    /// Context menu popup directions.
    /// </summary>
    [HideInEditor]
    public enum ContextMenuDirection
    {
        /// <summary>
        /// The right down.
        /// </summary>
        RightDown,

        /// <summary>
        /// The right up.
        /// </summary>
        RightUp,

        /// <summary>
        /// The left down.
        /// </summary>
        LeftDown,

        /// <summary>
        /// The left up.
        /// </summary>
        LeftUp,
    }

    /// <summary>
    /// Base class for all context menu controls.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    [HideInEditor]
    public class ContextMenuBase : ContainerControl
    {
        private const float PopupAnimationDuration = 0.02f;
        private const float PopupShadowOffset = 0.0f;
        private const float PopupShadowOpacity = 0.0f;
        private const float PopupInnerBorderInset = 1.5f;
        private const float SubmenuAimVerticalPadding = 18.0f;
        private const float ForegroundCloseGraceDuration = 0.18f;
        private const bool LogVisibilityReasons = false;
        private static readonly List<ContextMenuBase> OpenMenus = new List<ContextMenuBase>();
        private static readonly Color PopupInnerBorderColor = new Color(1.0f, 1.0f, 1.0f, 0.05f);
        private static readonly Color PopupSurfaceColor = Color.FromBgra(0xFF303033);
        private static readonly Color PopupBorderColor = Color.FromBgra(0xFF434347);

        private ContextMenuDirection _direction;
        private ContextMenuBase _parentCM;
        private bool _isSubMenu;
        private ContextMenuBase _childCM;
        private Window _window;
        private Window _ownerWindow;
        private Control _previouslyFocused;
        private Float2 _submenuAimOriginScreen;
        private float _visibilityAlpha = 1.0f;
        private float _ignoreForegroundCloseUntil;
        private bool _isHiding;
        private bool _loggedForegroundCloseGrace;
        private string _nextHideReason;
        private string _hideReason;

        /// <summary>
        /// True if any context menu popup is currently open or fading out.
        /// </summary>
        public static bool HasOpenMenu => OpenMenus.Count != 0;

        /// <summary>
        /// Gets a value indicating whether use automatic popup direction fix based on the screen dimensions.
        /// </summary>
        protected virtual bool UseAutomaticDirectionFix => true;

        private Float2 PopupWindowSize => Size + new Float2(PopupShadowOffset);

        /// <summary>
        /// Returns true if context menu is opened
        /// </summary>
        public bool IsOpened => Parent != null;

        /// <summary>
        /// Gets the popup direction.
        /// </summary>
        public ContextMenuDirection Direction => _direction;

        /// <summary>
        /// Gets a value indicating whether any child context menu has been opened.
        /// </summary>
        public bool HasChildCMOpened => _childCM != null;

        /// <summary>
        /// Gets the parent context menu (if exists).
        /// </summary>
        public ContextMenuBase ParentCM => _parentCM;

        /// <summary>
        /// Gets the topmost context menu.
        /// </summary>
        public ContextMenuBase TopmostCM
        {
            get
            {
                var cm = this;
                while (cm._parentCM != null && cm._isSubMenu)
                    cm = cm._parentCM;
                return cm;
            }
        }

        /// <summary>
        /// Gets a value indicating whether this context menu is a sub-menu. Sub menus are treated like child context menus of the other menu (eg. hierarchy).
        /// </summary>
        public bool IsSubMenu => _isSubMenu;

        /// <summary>
        /// External dialog popups opened within the context window (eg. color picker) that should preserve context menu visibility (prevent from closing context menu).
        /// </summary>
        public List<Window> ExternalPopups = new List<Window>();

        /// <summary>
        /// Optional flag that can disable popup visibility based on window focus and use external control via Hide.
        /// </summary>
        public bool UseVisibilityControl = true;

        /// <summary>
        /// Optional flag that can disable popup input capturing. Useful for transparent or visual-only popups.
        /// </summary>
        public bool UseInput = true;

        /// <summary>
        /// Optional flag that can disable UI navigation (tab/enter).
        /// </summary>
        public bool UseNavigation = true;

        /// <summary>
        /// Optional popup background override. Transparent color uses the current style popup background.
        /// </summary>
        public Color PopupBackgroundColor = Color.Transparent;

        /// <summary>
        /// Initializes a new instance of the <see cref="ContextMenuBase"/> class.
        /// </summary>
        public ContextMenuBase()
        : base(0, 0, 120, 32)
        {
            Visible = false;
            AutoFocus = true;

            _direction = ContextMenuDirection.RightDown;
            _isSubMenu = true;
        }

        internal void HideWithReason(string reason)
        {
            _nextHideReason = reason;
            try
            {
                Hide();
            }
            finally
            {
                if (_nextHideReason == reason)
                    _nextHideReason = null;
            }
        }

        private string GetLogName()
        {
            return GetType().Name + "#" + GetHashCode().ToString("X8");
        }

        private string GetLogState()
        {
            var parent = _parentCM != null ? _parentCM.GetLogName() : "none";
            var child = _childCM != null ? _childCM.GetLogName() : "none";
            return $"visible={Visible}, hiding={_isHiding}, opened={IsOpened}, subMenu={_isSubMenu}, mouseOver={IsMouseOver}, parent={parent}, child={child}, mouse={FlaxEngine.Input.MouseScreenPosition}";
        }

        private void LogVisibility(string message)
        {
            if (LogVisibilityReasons)
                Editor.Log($"[ContextMenu] {GetLogName()} {message}; {GetLogState()}");
        }

        private void DelayForegroundClose(string reason)
        {
            var topmost = TopmostCM;
            topmost._ignoreForegroundCloseUntil = Mathf.Max(topmost._ignoreForegroundCloseUntil, Time.UnscaledGameTime + ForegroundCloseGraceDuration);
            topmost._loggedForegroundCloseGrace = false;
            topmost.LogVisibility("foreground close grace started: " + reason);
        }

        /// <summary>
        /// Shows the empty menu popup on a screen.
        /// </summary>
        /// <param name="control">The target control.</param>
        /// <param name="area">The target control area to cover.</param>
        /// <returns>Created popup.</returns>
        public static ContextMenuBase ShowEmptyMenu(Control control, Rectangle area)
        {
            // Calculate the control size in the window space to handle scaled controls
            var upperLeft = control.PointToWindow(area.UpperLeft);
            var bottomRight = control.PointToWindow(area.BottomRight);
            var size = bottomRight - upperLeft;

            var popup = new ContextMenuBase();
            popup.Size = size;
            popup.Show(control, area.Location + new Float2(0, (size.Y - popup.Height) * 0.5f));
            return popup;
        }

        /// <summary>
        /// Show context menu over given control.
        /// </summary>
        /// <param name="parent">Parent control to attach to it.</param>
        /// <param name="location">Popup menu origin location in parent control coordinates.</param>
        /// <param name="direction">The custom popup direction. Null to use automatic direction.</param>
        public virtual void Show(Control parent, Float2 location, ContextMenuDirection? direction = null)
        {
            Assert.IsNotNull(parent);
            bool isAlreadyVisible = Visible && _window;
            if (!isAlreadyVisible && Visible)
                HideWithReason("Show requested while visible without popup window");
            _isHiding = false;

            // Peek parent control window
            var parentWin = parent.RootWindow;
            if (parentWin == null)
            {
                if (Visible)
                    HideWithReason("Show aborted because parent has no root window");
                return;
            }
            _ownerWindow = parentWin.Window;

            // Check if show menu inside the other menu - then link as a child to prevent closing the calling menu window on lost focus
            if (_parentCM == null && parentWin.ChildrenCount == 1 && parentWin.Children[0] is ContextMenuBase parentCM)
            {
                if (Visible)
                    HideWithReason("Show linking popup into parent context menu");
                parentCM.ShowChild(this, parentCM.PointFromScreen(parent.PointToScreen(location)), false);
                return;
            }

            // Unlock and perform controls update
            Location = Float2.Zero;
            UnlockChildrenRecursive();
            PerformLayout();

            // Calculate popup direction and initial location (fit on a single monitor)
            var dpiScale = parentWin.DpiScale;
            var dpiSize = PopupWindowSize * dpiScale;
            var locationWS = parent.PointToWindow(location);
            var locationSS = parentWin.PointToScreen(locationWS);
            var monitorBounds = Platform.GetMonitorBounds(locationSS);
            var rightBottomLocationSS = locationSS + dpiSize;
            bool isUp = false, isLeft = false;
            if (UseAutomaticDirectionFix && direction == null)
            {
                var parentMenu = parent as ContextMenu;
                if (monitorBounds.Bottom < rightBottomLocationSS.Y)
                {
                    isUp = true;
                    locationSS.Y -= dpiSize.Y;
                    if (parentMenu != null && parentMenu._childCM != null)
                        locationSS.Y += 30.0f * dpiScale;
                }
                if (parentMenu == null)
                {
                    if (monitorBounds.Right < rightBottomLocationSS.X)
                    {
                        isLeft = true;
                        locationSS.X -= dpiSize.X;
                    }
                }
                else if (monitorBounds.Right < rightBottomLocationSS.X || _parentCM?.Direction == ContextMenuDirection.LeftDown || _parentCM?.Direction == ContextMenuDirection.LeftUp)
                {
                    isLeft = true;
                    if (IsSubMenu && _parentCM != null)
                        locationSS.X -= _parentCM.Width + dpiSize.X;
                    else
                        locationSS.X -= dpiSize.X;
                }
            }
            else if (direction.HasValue)
            {
                switch (direction.Value)
                {
                case ContextMenuDirection.RightUp:
                    isUp = true;
                    break;
                case ContextMenuDirection.LeftDown:
                    isLeft = true;
                    break;
                case ContextMenuDirection.LeftUp:
                    isLeft = true;
                    isUp = true;
                    break;
                }
                if (isLeft)
                    locationSS.X -= dpiSize.X;
                if (isUp)
                    locationSS.Y -= dpiSize.Y;
            }

            // Update direction flag
            if (isUp)
                _direction = isLeft ? ContextMenuDirection.LeftUp : ContextMenuDirection.RightUp;
            else
                _direction = isLeft ? ContextMenuDirection.LeftDown : ContextMenuDirection.RightDown;

            if (isAlreadyVisible)
            {
                _window.ClientBounds = new Rectangle(locationSS, dpiSize);
            }
            else
            {
                // Create window
                var desc = CreateWindowSettings.Default;
                desc.Position = locationSS;
                desc.StartPosition = WindowStartPosition.Manual;
                desc.Size = dpiSize;
                desc.Fullscreen = false;
                desc.HasBorder = false;
                desc.SupportsTransparency = true;
                desc.ShowInTaskbar = false;
                desc.ActivateWhenFirstShown = UseInput;
                desc.AllowInput = UseInput;
                desc.AllowMinimize = false;
                desc.AllowMaximize = false;
                desc.AllowDragAndDrop = false;
                desc.IsTopmost = true;
                desc.Type = WindowType.Popup;
                desc.Parent = parentWin.Window;
                desc.Title = "ContextMenu";
                desc.HasSizingFrame = false;
                OnWindowCreating(ref desc);
                _window = Platform.CreateWindow(ref desc);
                _visibilityAlpha = 0.0f;
                _window.Opacity = _visibilityAlpha;
                if (UseVisibilityControl)
                {
                    _window.GotFocus += OnWindowGotFocus;
                    _window.LostFocus += OnWindowLostFocus;
                }

#if USE_IS_FOREGROUND && USE_SDL_WORKAROUNDS
                // The focus between popup and parent windows doesn't change, force hide the popup when clicked on parent
                parentWin.Window.MouseDown += OnWindowMouseDown;
                _window.Closed += () => parentWin.Window.MouseDown -= OnWindowMouseDown;
#elif USE_IS_FOREGROUND
                if (!(parent is ContextMenuBase))
                {
                    parentWin.Window.MouseDown += OnOwnerWindowMouseDown;
                    _window.Closed += () => parentWin.Window.MouseDown -= OnOwnerWindowMouseDown;
                }
#endif

                // Attach to the window
                _parentCM = parent as ContextMenuBase;
                Parent = _window.GUI;

                // Show
                Visible = true;
                if (_window == null)
                    return;
                _window.Show();
            }
            RegisterOpenMenu(this);
            if (_window != null)
                _window.Opacity = _visibilityAlpha;
            PerformLayout();
            if (UseVisibilityControl)
            {
                _previouslyFocused = parentWin.FocusedControl;
                Focus();
                OnShow();
            }
        }

        private static void ForceDefocus(ContainerControl c)
        {
            foreach (var cc in c.Children)
            {
                if (cc.ContainsFocus)
                    cc.Defocus();
                if (cc is ContainerControl ccc)
                    ForceDefocus(ccc);
            }
        }

        private static void RegisterOpenMenu(ContextMenuBase menu)
        {
            if (!OpenMenus.Contains(menu))
                OpenMenus.Add(menu);
        }

        private static void UnregisterOpenMenu(ContextMenuBase menu)
        {
            OpenMenus.Remove(menu);
        }

        /// <summary>
        /// Hide popup menu and all child menus.
        /// </summary>
        public virtual void Hide()
        {
            var reason = _nextHideReason ?? "Hide()";
            _nextHideReason = null;
            if (!Visible || _isHiding)
            {
                LogVisibility("hide ignored: " + reason);
                return;
            }

            _hideReason = reason;
            LogVisibility("hide requested: " + reason);

            // Lock update
            IsLayoutLocked = true;

            // Close child
            HideChild("parent hiding: " + reason);

            // Force defocus
            ForceDefocus(this);

            // Unlink from parent immediately so delayed fade-out does not block replacement submenus.
            if (_parentCM != null)
            {
                if (_parentCM._childCM == this)
                    _parentCM._childCM = null;
                _parentCM = null;
            }

            if (_window != null && PopupAnimationDuration > 0.0f)
            {
                _isHiding = true;
                return;
            }

            CloseNow("hide without animation: " + reason);
        }

        private void CloseNow(string reason = null)
        {
            LogVisibility("close now: " + (reason ?? _hideReason ?? "CloseNow()"));
            IsLayoutLocked = true;

            if (_childCM != null)
            {
                var child = _childCM;
                _childCM = null;
                child.CloseNow("parent close now: " + (reason ?? _hideReason ?? "CloseNow()"));
            }

            ForceDefocus(this);

            if (_parentCM != null)
            {
                if (_parentCM._childCM == this)
                    _parentCM._childCM = null;
                _parentCM = null;
            }

            // Unlink from window
            Parent = null;

            // Close window
            if (_window != null)
            {
                var win = _window;
                _window = null;
                win.Close();
            }
            _ownerWindow = null;
            UnregisterOpenMenu(this);

            // Return focus
            if (_previouslyFocused != null)
            {
                _previouslyFocused.RootWindow?.Focus();
                _previouslyFocused?.Focus();
                _previouslyFocused = null;
            }

            // Hide
            _isHiding = false;
            _hideReason = null;
            _visibilityAlpha = 0.0f;
            Visible = false;
            OnHide();
        }

        /// <summary>
        /// Shows new child context menu.
        /// </summary>
        /// <param name="child">The child menu.</param>
        /// <param name="location">The child menu initial location.</param>
        /// <param name="isSubMenu">True if context menu is a normal sub-menu, otherwise it is a custom menu popup linked as child.</param>
        public void ShowChild(ContextMenuBase child, Float2 location, bool isSubMenu = true)
        {
            DelayForegroundClose("show child " + child.GetLogName());

            // Hide current child
            HideChild("ShowChild replacing current child with " + child.GetLogName());

            // Set child
            _childCM = child;
            _childCM._parentCM = this;
            _childCM._isSubMenu = isSubMenu;

            // Show child
            _childCM.Show(this, location);
            _submenuAimOriginScreen = FlaxEngine.Input.MouseScreenPosition;
            LogVisibility("show child: " + child.GetLogName());
        }

        internal bool IsPointerInsideSubmenuAim(Float2 pointerScreen)
        {
            if (_childCM == null || _childCM._window == null)
                return false;

            var bounds = _childCM._window.ClientBounds;
            var edgeX = _submenuAimOriginScreen.X <= bounds.Center.X ? bounds.Left : bounds.Right;
            var top = new Float2(edgeX, bounds.Top - SubmenuAimVerticalPadding);
            var bottom = new Float2(edgeX, bounds.Bottom + SubmenuAimVerticalPadding);
            return IsPointInTriangle(pointerScreen, _submenuAimOriginScreen, top, bottom);
        }

        internal bool IsPointerInsideChildMenuTreeOrAim(Float2 pointerScreen)
        {
            return _childCM != null && _childCM.IsPointerInsideMenuTreeOrAim(pointerScreen);
        }

        private bool IsPointerInsideMenuTreeOrAim(Float2 pointerScreen)
        {
            var cm = this;
            while (cm != null)
            {
                if (cm.ContainsScreenPoint(pointerScreen) || cm.IsPointerInsideSubmenuAim(pointerScreen))
                    return true;
                cm = cm._childCM;
            }
            return false;
        }

        private bool ContainsScreenPoint(Float2 pointerScreen)
        {
            if (_window == null)
                return false;

            var location = PointFromScreen(pointerScreen);
            return ContainsPoint(ref location);
        }

        private static bool IsPointInTriangle(Float2 point, Float2 a, Float2 b, Float2 c)
        {
            static float Sign(Float2 p1, Float2 p2, Float2 p3)
            {
                return (p1.X - p3.X) * (p2.Y - p3.Y) - (p2.X - p3.X) * (p1.Y - p3.Y);
            }

            var d1 = Sign(point, a, b);
            var d2 = Sign(point, b, c);
            var d3 = Sign(point, c, a);
            bool hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
            bool hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
            return !(hasNegative && hasPositive);
        }

        /// <summary>
        /// Hides child popup menu if any opened.
        /// </summary>
        public void HideChild()
        {
            HideChild("HideChild()");
        }

        internal void HideChild(string reason)
        {
            if (_childCM != null)
            {
                DelayForegroundClose("hide child " + _childCM.GetLogName() + ": " + reason);
                LogVisibility("hide child requested: " + reason + ", child=" + _childCM.GetLogName());
                _childCM.HideWithReason("parent " + GetLogName() + " hide child: " + reason);
                _childCM = null;
            }
        }

        /// <summary>
        /// Updates the size of the window to match context menu dimensions.
        /// </summary>
        protected void UpdateWindowSize()
        {
            if (_window != null)
            {
                _window.ClientSize = PopupWindowSize * _window.DpiScale;
            }
        }

        /// <summary>
        /// Called when context menu window setup is performed. Can be used to adjust the popup window options.
        /// </summary>
        /// <param name="settings">The settings.</param>
        protected virtual void OnWindowCreating(ref CreateWindowSettings settings)
        {
        }

        /// <summary>
        /// Called on context menu show.
        /// </summary>
        protected virtual void OnShow()
        {
            // Nothing to do
        }

        /// <summary>
        /// Called on context menu hide.
        /// </summary>
        protected virtual void OnHide()
        {
            // Nothing to do
        }

#if USE_IS_FOREGROUND
        /// <summary>
        /// Returns true if context menu is in foreground (eg. context window or any child window has user focus or user opened additional popup within this context).
        /// </summary>
        protected virtual bool IsForeground
        {
            get
            {
                // Any external popup is focused
                foreach (var externalPopup in ExternalPopups)
                {
                    if (externalPopup && externalPopup.IsForegroundWindow)
                        return true;
                }

                if (_ownerWindow && _ownerWindow.IsForegroundWindow)
                    return true;

                // Any context menu window is focused
                var anyForeground = false;
                var c = this;
                while (!anyForeground && c != null)
                {
                    if (c._window != null && c._window.IsForegroundWindow)
                        anyForeground = true;
                    c = c._childCM;
                }

                return anyForeground;
            }
        }

#if USE_SDL_WORKAROUNDS
        private void OnWindowGotFocus()
        {
        }
        
        private void OnWindowMouseDown(ref Float2 mousePosition, MouseButton button, ref bool handled)
        {
            // The user clicked outside the popup window
            HideWithReason("owner window mouse down outside popup (SDL workaround)");
        }
#else
        private void OnOwnerWindowMouseDown(ref Float2 mousePosition, MouseButton button, ref bool handled)
        {
            if (_parentCM == null)
                HideWithReason("owner window mouse down while root menu is open");
        }

        private void OnWindowGotFocus()
        {
        }
#endif

        private void OnWindowLostFocus()
        {
            // Skip for parent menus (child should handle lost of focus)
            if (_childCM != null)
            {
                LogVisibility("window lost focus ignored because child is open");
                return;
            }

            // Check if user stopped using that popup menu
            if (_parentCM != null)
            {
                // Focus parent if user clicked over the parent popup
                var mouse = _parentCM.PointFromScreen(FlaxEngine.Input.MouseScreenPosition);
                if (_parentCM.ContainsPoint(ref mouse))
                {
                    LogVisibility("window lost focus moved focus back to parent popup");
                    _parentCM._window.Focus();
                }
            }
        }
#else
        private void OnWindowGotFocus()
        {
        }

        private void OnWindowLostFocus()
        {
            // Skip for parent menus (child should handle lost of focus)
            if (_childCM != null)
            {
                LogVisibility("window lost focus ignored because child is open");
                return;
            }

            if (_parentCM != null)
            {
                if (IsMouseOver)
                {
                    LogVisibility("window lost focus ignored because mouse is over this popup");
                    return;
                }

                // Check if any external popup is focused
                foreach (var externalPopup in ExternalPopups)
                {
                    if (externalPopup && externalPopup.IsFocused)
                    {
                        LogVisibility("window lost focus ignored because external popup is focused");
                        return;
                    }
                }

                // Check if mouse is over any of the parents
                ContextMenuBase focusCM = null;
                var cm = _parentCM;
                while (cm != null)
                {
                    if (cm.IsMouseOver)
                        focusCM = cm;
                    cm = cm._parentCM;
                }

                if (focusCM != null)
                {
                    // Focus on the clicked parent and hide any open sub-menus
                    focusCM.HideChild("window lost focus while mouse is over ancestor " + focusCM.GetLogName());
                    focusCM._window?.Focus();
                }
                else
                {
                    // User clicked outside the context menus, hide the whole context menu tree
                    TopmostCM.HideWithReason("window lost focus outside context menu tree");
                }
            }
            else if (!IsMouseOver)
            {
                HideWithReason("root window lost focus while mouse is outside popup");
            }
        }
#endif

        /// <inheritdoc />
        public override bool IsMouseOver
        {
            get
            {
                var mouseScreen = FlaxEngine.Input.MouseScreenPosition;
                if (IsPointerInsideMenuTreeOrAim(mouseScreen))
                    return true;

                bool result = false;
                for (int i = 0; i < _children.Count; i++)
                {
                    var c = _children[i];
                    if (c.Visible && c.IsMouseOver)
                    {
                        result = true;
                        break;
                    }
                }
                return result;
            }
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);

            UpdateVisibilityAnimation(deltaTime);

#if USE_IS_FOREGROUND
            // Let root context menu to check if none of the popup windows
            if (_parentCM == null && UseVisibilityControl && !IsForeground)
            {
                if (!Platform.HasFocus)
                {
                    HideWithReason("application lost focus");
                    return;
                }

                if (IsMouseOver)
                {
                    if (!_loggedForegroundCloseGrace)
                    {
                        _loggedForegroundCloseGrace = true;
                        LogVisibility("root popup foreground loss ignored because pointer is still over menu tree");
                    }
                    return;
                }

                if (Time.UnscaledGameTime < _ignoreForegroundCloseUntil)
                {
                    if (!_loggedForegroundCloseGrace)
                    {
                        _loggedForegroundCloseGrace = true;
                        LogVisibility("root popup foreground loss ignored during submenu handoff");
                    }
                    return;
                }

                HideWithReason("root popup is no longer foreground");
            }
            else
            {
                _ignoreForegroundCloseUntil = 0.0f;
                _loggedForegroundCloseGrace = false;
            }
#endif
        }

        private void UpdateVisibilityAnimation(float deltaTime)
        {
            if (_window == null)
                return;

            float target = _isHiding ? 0.0f : 1.0f;
            if (Mathf.NearEqual(_visibilityAlpha, target))
            {
                _window.Opacity = target;
                if (_isHiding)
                    CloseNow("fade-out completed: " + _hideReason);
                return;
            }

            float step = PopupAnimationDuration > 0.0f ? deltaTime / PopupAnimationDuration : 1.0f;
            _visibilityAlpha = Mathf.MoveTowards(_visibilityAlpha, target, step);
            _window.Opacity = _visibilityAlpha;
            if (_isHiding && Mathf.NearEqual(_visibilityAlpha, 0.0f))
                CloseNow("fade-out reached zero opacity: " + _hideReason);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            // Draw background
            var style = Style.Current;
            var bounds = new Rectangle(Float2.Zero, Size);
            var cornerRadius = style.GetDropdownCornerRadius();
            StyleRendering.DrawRoundedRectangle(new Rectangle(PopupShadowOffset, PopupShadowOffset, Width, Height), Color.Black.AlphaMultiplied(PopupShadowOpacity), Color.Transparent, 0.0f, cornerRadius);
            var popup = PopupBackgroundColor.A > 0.0f ? PopupBackgroundColor : PopupSurfaceColor;
            StyleRendering.DrawRoundedRectangle(bounds, popup, PopupBorderColor, 1.0f, cornerRadius);
            var innerBorderRect = bounds.MakeExpanded(-PopupInnerBorderInset);
            if (innerBorderRect.Width > 0.0f && innerBorderRect.Height > 0.0f)
                StyleRendering.DrawRoundedRectangleBorder(innerBorderRect, PopupInnerBorderColor, 1.0f, Mathf.Max(0.0f, cornerRadius - PopupInnerBorderInset));

            base.Draw();
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            base.OnMouseDown(location, button);
            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            base.OnMouseUp(location, button);
            return true;
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (base.OnKeyDown(key))
                return true;

            var editor = Editor.Instance;
            if (editor != null)
            {
                var inputOptions = editor.Options.Options.Input;
                if (inputOptions.Undo.Process(this, key))
                {
                    editor.PerformUndo();
                    return true;
                }
                if (inputOptions.Redo.Process(this, key))
                {
                    editor.PerformRedo();
                    return true;
                }
            }

            switch (key)
            {
            case KeyboardKeys.Escape:
                HideWithReason("Escape key on context menu");
                return true;
            case KeyboardKeys.Return:
                if (UseNavigation && Root?.FocusedControl != null)
                {
                    Root.SubmitFocused();
                    return true;
                }
                break;
            case KeyboardKeys.Tab:
                if (UseNavigation && Root != null)
                {
                    bool shiftDown = Root.GetKey(KeyboardKeys.Shift);
                    Root.Navigate(shiftDown ? NavDirection.Previous : NavDirection.Next);
                    return true;
                }
                break;
            }
            return false;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            // Ensure to be hidden
            if (Visible)
                CloseNow("OnDestroy while still visible");

            base.OnDestroy();
        }
    }
}
