// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Viewport.Overlays
{
    /// <summary>Implemented by overlay content that computes its height from the available width.</summary>
    public interface IViewportOverlayResponsiveContent
    {
        /// <summary>Gets the desired content height for the given width.</summary>
        float GetDesiredHeight(float width);
    }

    /// <summary>
    /// Docking location for a viewport overlay.
    /// </summary>
    public enum ViewportOverlayDock
    {
        /// <summary>The overlay freely floats over the viewport.</summary>
        Floating = 0,
        /// <summary>The overlay is docked to the top edge.</summary>
        Top = 1,
        /// <summary>The overlay is docked to the bottom edge.</summary>
        Bottom = 2,
        /// <summary>The overlay is docked to the left edge.</summary>
        Left = 3,
        /// <summary>The overlay is docked to the right edge.</summary>
        Right = 4,
        /// <summary>The overlay is docked to the upper-left corner.</summary>
        TopLeft = 5,
        /// <summary>The overlay is docked to the upper-right corner.</summary>
        TopRight = 6,
        /// <summary>The overlay is docked to the lower-left corner.</summary>
        BottomLeft = 7,
        /// <summary>The overlay is docked to the lower-right corner.</summary>
        BottomRight = 8,
        /// <summary>The overlay is attached to the viewport's primary toolstrip.</summary>
        Toolbar = 9,
    }

    /// <summary>
    /// Presentation mode for a viewport overlay.
    /// </summary>
    public enum ViewportOverlayLayoutMode
    {
        /// <summary>Header and full panel content.</summary>
        Panel,
        /// <summary>Compact horizontal tool content.</summary>
        Horizontal,
        /// <summary>Compact vertical tool content.</summary>
        Vertical,
        /// <summary>Header-only collapsed presentation.</summary>
        Collapsed,
    }

    /// <summary>
    /// Persistent, movable viewport overlay container.
    /// </summary>
    [HideInEditor]
    public sealed class ViewportOverlayContainer : ContainerControl
    {
        /// <summary>Standard overlay header height.</summary>
        public const float HeaderHeight = 24.0f;

        private sealed class Header : Control
        {
            private readonly ViewportOverlayContainer _owner;
            private bool _pressed;
            private bool _dragging;
            private Float2 _pressPointer;

            public Header(ViewportOverlayContainer owner)
            : base(0, 0, 100, HeaderHeight)
            {
                _owner = owner;
                AutoFocus = false;
            }

            /// <inheritdoc />
            public override void Draw()
            {
                var style = Style.Current;
                var bounds = new Rectangle(Float2.Zero, Size);
                if (_owner.UsesDockedChrome)
                {
                    if (IsMouseOver)
                        Render2D.FillRectangle(bounds, style.BackgroundSelected.AlphaMultiplied(0.35f));
                    var dockedGrip = style.ForegroundViewport.AlphaMultiplied(0.6f);
                    float startX = (Width - 6.0f) * 0.5f;
                    float startY = (Height - 6.0f) * 0.5f;
                    for (int y = 0; y < 2; y++)
                    {
                        for (int x = 0; x < 2; x++)
                            StyleRendering.FillRoundedRectangle(new Rectangle(startX + x * 4, startY + y * 4, 2, 2), dockedGrip, 1.0f);
                    }
                    return;
                }
                StyleRendering.FillRoundedRectangle(bounds, style.SecondaryBackground.AlphaMultiplied(IsMouseOver ? 0.96f : 0.9f), 4.0f);
                Render2D.DrawLine(new Float2(0, Height - 1), new Float2(Width, Height - 1), style.BorderNormal.AlphaMultiplied(0.65f));

                var grip = style.ForegroundViewport.AlphaMultiplied(0.55f);
                for (int y = 0; y < 2; y++)
                {
                    for (int x = 0; x < 3; x++)
                        StyleRendering.FillRoundedRectangle(new Rectangle(7 + x * 4, 8 + y * 4, 2, 2), grip, 1.0f);
                }

                var titleBounds = new Rectangle(23, 0, Mathf.Max(0, Width - 49), Height);
                Render2D.DrawText(style.FontMedium, _owner.Title, titleBounds, style.ForegroundViewport, TextAlignment.Near, TextAlignment.Center);
                Render2D.DrawText(style.FontMedium, "...", new Rectangle(Width - 26, -2, 22, Height), style.ForegroundViewport.AlphaMultiplied(0.8f), TextAlignment.Center, TextAlignment.Center);
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Right || (!_owner.UsesDockedChrome && button == MouseButton.Left && location.X >= Width - 30.0f))
                {
                    _owner.ShowSettings(location);
                    return true;
                }
                if (button == MouseButton.Left)
                {
                    _pressed = true;
                    _dragging = false;
                    _pressPointer = _owner.Host.PointFromWindow(PointToWindow(location));
                    StartMouseCapture();
                    return true;
                }
                return base.OnMouseDown(location, button);
            }

            /// <inheritdoc />
            public override void OnMouseMove(Float2 location)
            {
                if (_pressed)
                {
                    var pointer = _owner.Host.PointFromWindow(PointToWindow(location));
                    if (!_dragging && Float2.DistanceSquared(pointer, _pressPointer) >= 16.0f)
                    {
                        _dragging = true;
                        _owner.Host.BeginDrag(_owner, _pressPointer);
                    }
                    if (_dragging)
                        _owner.Host.UpdateDrag(_owner, pointer);
                }
                base.OnMouseMove(location);
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left && _pressed)
                {
                    _pressed = false;
                    if (_dragging)
                        _owner.Host.EndDrag(_owner, true);
                    _dragging = false;
                    EndMouseCapture();
                    return true;
                }
                return base.OnMouseUp(location, button);
            }

            /// <inheritdoc />
            public override void OnEndMouseCapture()
            {
                if (_dragging)
                {
                    _dragging = false;
                    _owner.Host.EndDrag(_owner, false);
                }
                _pressed = false;
                base.OnEndMouseCapture();
            }
        }

        private sealed class ResizeHandle : Control
        {
            private readonly ViewportOverlayContainer _owner;
            private bool _resizing;
            private Float2 _startPointer;
            private Float2 _startContentSize;

            public ResizeHandle(ViewportOverlayContainer owner)
            : base(0, 0, 14, 14)
            {
                _owner = owner;
                AutoFocus = false;
                Cursor = CursorType.SizeNWSE;
            }

            /// <inheritdoc />
            public override void Draw()
            {
                var color = Style.Current.ForegroundViewport.AlphaMultiplied(IsMouseOver ? 0.75f : 0.45f);
                for (int i = 0; i < 3; i++)
                {
                    float offset = 3.0f + i * 3.0f;
                    Render2D.DrawLine(new Float2(Width - offset, Height - 2.0f), new Float2(Width - 2.0f, Height - offset), color);
                }
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button != MouseButton.Left)
                    return base.OnMouseDown(location, button);
                _resizing = true;
                _startPointer = _owner.Host.PointFromWindow(PointToWindow(location));
                _startContentSize = _owner._preferredContentSize;
                StartMouseCapture();
                return true;
            }

            /// <inheritdoc />
            public override void OnMouseMove(Float2 location)
            {
                if (_resizing)
                {
                    var pointer = _owner.Host.PointFromWindow(PointToWindow(location));
                    _owner.ResizeFloating(_startContentSize, pointer - _startPointer);
                    return;
                }
                base.OnMouseMove(location);
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left && _resizing)
                {
                    EndMouseCapture();
                    return true;
                }
                return base.OnMouseUp(location, button);
            }

            /// <inheritdoc />
            public override void OnEndMouseCapture()
            {
                bool wasResizing = _resizing;
                _resizing = false;
                if (wasResizing)
                    _owner.Host.NotifyLayoutChanged();
                base.OnEndMouseCapture();
            }
        }

        private readonly Header _header;
        private readonly ResizeHandle _resizeHandle;
        private Control _content;
        private bool _userVisible = true;
        private bool _contextVisible = true;
        private ViewportOverlayDock _dock;
        private ViewportOverlayLayoutMode _layoutMode;
        private Float2 _preferredContentSize;

        private bool UsesDockedChrome => Dock != ViewportOverlayDock.Floating;

        // Corners and the primary toolstrip use an inline grip. Full-edge docks use a separate
        // grip row above their content and are centered along the corresponding viewport edge.
        private bool UsesHorizontalDockedChrome => Dock is ViewportOverlayDock.TopLeft or ViewportOverlayDock.TopRight or
            ViewportOverlayDock.BottomLeft or ViewportOverlayDock.BottomRight or ViewportOverlayDock.Toolbar;

        /// <summary>Stable identifier used by saved layouts.</summary>
        public string Id { get; }

        /// <summary>User-facing overlay title.</summary>
        public string Title { get; }

        /// <summary>Owning overlay host.</summary>
        public ViewportOverlayHost Host { get; }

        /// <summary>Current docking location.</summary>
        public ViewportOverlayDock Dock
        {
            get => _dock;
            set
            {
                if (_dock == value)
                    return;
                _dock = value;
                ApplyPresentationSize();
                PerformLayout();
                Host.PerformLayout();
            }
        }

        /// <summary>Current presentation mode.</summary>
        public ViewportOverlayLayoutMode LayoutMode
        {
            get => _layoutMode;
            set
            {
                if (_layoutMode == value)
                    return;
                _layoutMode = value;
                ApplyPresentationSize();
                PerformLayout();
                Host.PerformLayout();
            }
        }

        /// <summary>Whether the user has enabled this overlay.</summary>
        public bool UserVisible
        {
            get => _userVisible;
            set => SetUserVisible(value, true);
        }

        /// <summary>Overlay content control.</summary>
        public Control Content => _content;

        /// <summary>Current preferred content size, including user resizing.</summary>
        internal Float2 PreferredContentSize => _preferredContentSize;

        internal Float2 FloatingLocation;

        /// <summary>
        /// Initializes a new viewport overlay.
        /// </summary>
        internal ViewportOverlayContainer(ViewportOverlayHost host, string id, string title, Control content, Float2 contentSize,
                                          ViewportOverlayDock dock, ViewportOverlayLayoutMode layoutMode, Float2 floatingLocation)
        : base(floatingLocation.X, floatingLocation.Y, Mathf.Max(110.0f, contentSize.X), HeaderHeight + contentSize.Y)
        {
            Host = host ?? throw new ArgumentNullException(nameof(host));
            Id = string.IsNullOrWhiteSpace(id) ? throw new ArgumentException("Overlay id is required.", nameof(id)) : id;
            Title = string.IsNullOrWhiteSpace(title) ? id : title;
            _dock = dock;
            _layoutMode = layoutMode;
            FloatingLocation = floatingLocation;
            AutoFocus = false;
            ClipChildren = false;
            _header = new Header(this) { Parent = this };
            _resizeHandle = new ResizeHandle(this) { Parent = this };
            SetContent(content, contentSize);
        }

        /// <summary>Sets whether the current editor context wants the overlay displayed.</summary>
        public void SetContextVisible(bool visible)
        {
            if (_contextVisible == visible)
                return;
            _contextVisible = visible;
            UpdateEffectiveVisibility();
            Host.PerformLayout();
        }

        internal void SetUserVisible(bool visible, bool notify)
        {
            if (_userVisible == visible)
                return;
            _userVisible = visible;
            UpdateEffectiveVisibility();
            if (notify)
                Host.NotifyLayoutChanged();
        }

        /// <summary>Replaces the overlay content and its desired size.</summary>
        public void SetContent(Control content, Float2 contentSize)
        {
            if (_content != null && _content != content)
                _content.Parent = null;
            _content = content;
            _preferredContentSize = contentSize;
            if (_content != null)
            {
                _content.Parent = this;
                _content.AnchorPreset = AnchorPresets.Custom;
                _content.Size = contentSize;
                _resizeHandle.IndexInParent = ChildrenCount - 1;
            }
            ApplyPresentationSize();
            PerformLayout();
        }

        /// <summary>Re-measures content after controls have been added or removed.</summary>
        public void RefreshContentSize()
        {
            ApplyPresentationSize();
            PerformLayout();
            Host.PerformLayout();
        }

        private void ApplyPresentationSize()
        {
            if (LayoutMode == ViewportOverlayLayoutMode.Collapsed)
            {
                Width = UsesDockedChrome ? 14.0f : 110.0f;
                Height = UsesDockedChrome ? 14.0f : HeaderHeight;
                return;
            }
            var contentSize = _preferredContentSize;
            if (_content is IViewportOverlayResponsiveContent responsive)
                contentSize.Y = responsive.GetDesiredHeight(contentSize.X);
            if (_content is ToolStrip toolStrip)
            {
                toolStrip.DrawOverlayBackground = Dock != ViewportOverlayDock.Toolbar;
                toolStrip.ConsumeMouseWheel = Dock != ViewportOverlayDock.Toolbar;
                var orientation = LayoutMode == ViewportOverlayLayoutMode.Vertical ? Orientation.Vertical : Orientation.Horizontal;
                toolStrip.LayoutOrientation = orientation;
                if (orientation == Orientation.Vertical)
                    contentSize = toolStrip.GetDesiredSize(orientation);
                else
                    contentSize = _preferredContentSize;
            }
            if (Dock == ViewportOverlayDock.Toolbar && Host.PrimaryToolStrip != null)
            {
                Width = contentSize.X + 14.0f;
                _content.Size = contentSize;
                Height = Host.PrimaryToolStrip.ItemsHeight;
                return;
            }
            float chromeWidth = UsesDockedChrome && UsesHorizontalDockedChrome ? 14.0f : 0.0f;
            float chromeHeight = UsesDockedChrome && !UsesHorizontalDockedChrome ? 14.0f : 0.0f;
            Width = UsesDockedChrome ? contentSize.X + chromeWidth : Mathf.Max(110.0f, contentSize.X);
            if (_content != null)
                _content.Size = new Float2(Mathf.Max(0.0f, contentSize.X), Mathf.Max(0.0f, contentSize.Y));
            Height = UsesDockedChrome ? contentSize.Y + chromeHeight : HeaderHeight + Mathf.Max(0.0f, contentSize.Y);
        }

        private void ResizeFloating(Float2 startContentSize, Float2 delta)
        {
            if (Dock != ViewportOverlayDock.Floating || LayoutMode == ViewportOverlayLayoutMode.Collapsed)
                return;
            float maxWidth = Mathf.Max(180.0f, Host.Width - FloatingLocation.X);
            float maxHeight = Mathf.Max(28.0f, Host.Height - FloatingLocation.Y - HeaderHeight);
            _preferredContentSize.X = Mathf.Clamp(startContentSize.X + delta.X, 180.0f, maxWidth);
            if (_content is not IViewportOverlayResponsiveContent && _content is not ToolStrip)
                _preferredContentSize.Y = Mathf.Clamp(startContentSize.Y + delta.Y, 28.0f, maxHeight);
            ApplyPresentationSize();
            PerformLayout();
            Host.PerformLayout();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            bool dockedChrome = UsesDockedChrome;
            bool horizontalChrome = UsesHorizontalDockedChrome;
            bool toolStripChrome = Dock == ViewportOverlayDock.Toolbar && Host.PrimaryToolStrip != null;
            _resizeHandle.Visible = Dock == ViewportOverlayDock.Floating && LayoutMode != ViewportOverlayLayoutMode.Collapsed;
            _resizeHandle.Enabled = _resizeHandle.Visible;
            _resizeHandle.Bounds = new Rectangle(Mathf.Max(0.0f, Width - 14.0f), Mathf.Max(0.0f, Height - 14.0f), 14.0f, 14.0f);
            _header.Bounds = dockedChrome
                ? horizontalChrome ? new Rectangle(0, 0, 14.0f, Height) : new Rectangle(0, 0, Width, 14.0f)
                : new Rectangle(0, 0, Width, HeaderHeight);
            if (_content == null)
                return;
            bool collapsed = LayoutMode == ViewportOverlayLayoutMode.Collapsed;
            _content.Visible = !collapsed;
            _content.Enabled = !collapsed;
            _content.Location = dockedChrome
                ? horizontalChrome ? new Float2(14.0f, toolStripChrome ? -Host.PrimaryToolStrip.ItemsMargin.Top : 0.0f) : new Float2(0.0f, 14.0f)
                : new Float2(0, HeaderHeight);
            if (collapsed)
                Height = dockedChrome ? 14.0f : HeaderHeight;
            else
            {
                if (!dockedChrome)
                    _content.Width = Width;
                Height = toolStripChrome
                    ? Host.PrimaryToolStrip.ItemsHeight
                    : dockedChrome
                    ? _content.Height + (horizontalChrome ? 0.0f : 14.0f)
                    : HeaderHeight + _content.Height;
            }
        }

        /// <inheritdoc />
        public override void DrawSelf()
        {
            if (UsesDockedChrome)
            {
                // Toolstrip-attached overlays inherit the primary strip background. Edge-docked
                // overlays need their own opaque chrome so controls remain legible over the scene.
                if (Dock != ViewportOverlayDock.Toolbar)
                {
                    var dockedStyle = Style.Current;
                    Render2D.FillRectangle(new Rectangle(Float2.Zero, Size), dockedStyle.SecondaryBackground);
                    StyleRendering.DrawRoundedRectangle(new Rectangle(Float2.Zero, Size), Color.Transparent,
                                                        dockedStyle.BorderNormal.AlphaMultiplied(0.8f), 1.0f, 2.0f);
                }
                return;
            }
            var style = Style.Current;
            var bounds = new Rectangle(Float2.Zero, Size);
            StyleRendering.DrawRoundedRectangle(bounds, style.Background.AlphaMultiplied(0.9f), style.BorderNormal.AlphaMultiplied(0.75f), 1.0f, 4.0f);
        }

        private void UpdateEffectiveVisibility()
        {
            Visible = _userVisible && _contextVisible;
            Enabled = Visible;
            if (Parent is ToolStrip toolStrip)
                toolStrip.PerformLayout();
        }

        private void ShowSettings(Float2 location)
        {
            var menu = new ContextMenu { MinimumWidth = 190 };
            var layoutMenu = menu.AddChildMenu("Layout").ContextMenu;
            AddChecked(layoutMenu, "Panel", LayoutMode == ViewportOverlayLayoutMode.Panel, () => SetLayoutMode(ViewportOverlayLayoutMode.Panel));
            AddChecked(layoutMenu, "Horizontal", LayoutMode == ViewportOverlayLayoutMode.Horizontal, () => SetLayoutMode(ViewportOverlayLayoutMode.Horizontal));
            AddChecked(layoutMenu, "Vertical", LayoutMode == ViewportOverlayLayoutMode.Vertical, () => SetLayoutMode(ViewportOverlayLayoutMode.Vertical));
            AddChecked(layoutMenu, "Collapsed", LayoutMode == ViewportOverlayLayoutMode.Collapsed, () => SetLayoutMode(ViewportOverlayLayoutMode.Collapsed));

            var dockMenu = menu.AddChildMenu("Dock").ContextMenu;
            foreach (ViewportOverlayDock dock in Enum.GetValues(typeof(ViewportOverlayDock)))
            {
                var captured = dock;
                AddChecked(dockMenu, GetDockLabel(dock), Dock == dock, () => SetDock(captured));
            }
            menu.AddSeparator();
            menu.AddButton("Hide", () => UserVisible = false);
            menu.AddButton("Reset Position", () => Host.ResetOverlay(this));
            menu.Show(_header, location);
        }

        private void SetDock(ViewportOverlayDock value)
        {
            Host.DockOverlay(this, value, true);
        }

        private void SetLayoutMode(ViewportOverlayLayoutMode value)
        {
            LayoutMode = value;
            Host.NotifyLayoutChanged();
        }

        private static void AddChecked(ContextMenu menu, string text, bool isChecked, Action action)
        {
            var button = menu.AddButton(text, action);
            button.Checked = isChecked;
        }

        private static string GetDockLabel(ViewportOverlayDock dock)
        {
            return dock switch
            {
                ViewportOverlayDock.TopLeft => "Top Left",
                ViewportOverlayDock.TopRight => "Top Right",
                ViewportOverlayDock.BottomLeft => "Bottom Left",
                ViewportOverlayDock.BottomRight => "Bottom Right",
                ViewportOverlayDock.Toolbar => "Viewport Toolstrip",
                _ => dock.ToString(),
            };
        }
    }

    /// <summary>
    /// Hosts viewport overlays, manages docking/drop zones, visibility, and layout persistence.
    /// </summary>
    [HideInEditor]
    public sealed class ViewportOverlayHost : ContainerControl
    {
        private struct SavedState
        {
            public ViewportOverlayDock Dock;
            public ViewportOverlayLayoutMode Layout;
            public Float2 Location;
            public Float2 Size;
            public bool HasSize;
            public bool Visible;
        }

        private readonly List<ViewportOverlayContainer> _overlays = new List<ViewportOverlayContainer>();
        private readonly Dictionary<string, SavedState> _savedStates = new Dictionary<string, SavedState>();
        private ViewportOverlayContainer _dragged;
        private Float2 _dragOffset;
        private Rectangle _ghostBounds;
        private ViewportOverlayDock _ghostDock;
        private ViewportOverlayLayoutMode _ghostLayout;
        private ToolStripAnchor _ghostToolbarAnchor;
        private int _ghostToolbarIndex;
        private Float2 _lastDragPointer;
        private ViewportOverlayDock? _dropTarget;
        private ToolStripAnchor _toolStripDropAnchor;

        /// <summary>Spacing between docked overlays.</summary>
        public float OverlaySpacing = 6.0f;

        /// <summary>Top inset reserved for the viewport's primary toolstrip.</summary>
        public float TopInset = 34.0f;

        /// <summary>The viewport toolstrip that hosts overlays attached to the toolstrip docking target.</summary>
        public ToolStrip PrimaryToolStrip { get; set; }

        /// <summary>Raised after a user-visible layout change.</summary>
        public event Action LayoutChanged;

        /// <summary>Registered overlays.</summary>
        public IReadOnlyList<ViewportOverlayContainer> Overlays => _overlays;

        /// <summary>Creates an empty full-viewport overlay host.</summary>
        public ViewportOverlayHost()
        {
            AnchorPreset = AnchorPresets.StretchAll;
            Offsets = Margin.Zero;
            AutoFocus = false;
            ClipChildren = false;
        }

        /// <summary>Registers an overlay with a stable identifier and default placement.</summary>
        public ViewportOverlayContainer AddOverlay(string id, string title, Control content, Float2 contentSize,
                                                   ViewportOverlayDock defaultDock = ViewportOverlayDock.Floating,
                                                   ViewportOverlayLayoutMode defaultLayout = ViewportOverlayLayoutMode.Panel,
                                                   Float2 defaultLocation = default)
        {
            if (Find(id) != null)
                throw new ArgumentException($"Viewport overlay '{id}' is already registered.", nameof(id));
            var state = new SavedState
            {
                Dock = defaultDock,
                Layout = defaultLayout,
                Location = defaultLocation,
                Size = contentSize,
                HasSize = true,
                Visible = true,
            };
            if (_savedStates.TryGetValue(id, out var saved))
                state = saved;
            var result = new ViewportOverlayContainer(this, id, title, content, state.HasSize ? state.Size : contentSize, state.Dock, state.Layout, state.Location)
            {
                Parent = this,
            };
            result.FloatingLocation = state.Location;
            result.SetUserVisible(state.Visible, false);
            _overlays.Add(result);
            if (state.Dock == ViewportOverlayDock.Toolbar && PrimaryToolStrip != null)
                AttachToToolStrip(result, GetToolStripAnchor(state.Location.X), -1, true, false);
            PerformLayout();
            return result;
        }

        /// <summary>Finds an overlay by stable identifier.</summary>
        public ViewportOverlayContainer Find(string id)
        {
            for (int i = 0; i < _overlays.Count; i++)
            {
                if (string.Equals(_overlays[i].Id, id, StringComparison.Ordinal))
                    return _overlays[i];
            }
            return null;
        }

        /// <summary>Appends overlay visibility and reset actions to a menu.</summary>
        public void PopulateMenu(ContextMenu menu)
        {
            if (menu == null)
                return;
            for (int i = 0; i < _overlays.Count; i++)
            {
                var overlay = _overlays[i];
                var button = menu.AddButton(overlay.Title, () => overlay.UserVisible = !overlay.UserVisible);
                button.CloseMenuOnClick = false;
                button.Checked = overlay.UserVisible;
            }
            menu.AddSeparator();
            menu.AddButton("Show All", ShowAll);
            menu.AddButton("Reset Overlay Layout", ResetLayout);
        }

        /// <summary>Captures all registered overlay placements in a compact string.</summary>
        public string CaptureLayout()
        {
            var entries = new List<string>(_overlays.Count);
            foreach (var pair in _savedStates)
            {
                if (Find(pair.Key) != null)
                    continue;
                var state = pair.Value;
                if (state.HasSize)
                {
                    entries.Add(string.Join("@", pair.Key, (int)state.Dock, (int)state.Layout,
                                            state.Location.X.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                            state.Location.Y.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                            state.Visible ? "1" : "0",
                                            state.Size.X.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                            state.Size.Y.ToString(System.Globalization.CultureInfo.InvariantCulture)));
                }
                else
                {
                    entries.Add(string.Join("@", pair.Key, (int)state.Dock, (int)state.Layout,
                                            state.Location.X.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                            state.Location.Y.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                            state.Visible ? "1" : "0"));
                }
            }
            for (int i = 0; i < _overlays.Count; i++)
            {
                var overlay = _overlays[i];
                var location = overlay.Dock == ViewportOverlayDock.Floating ? overlay.Location : overlay.FloatingLocation;
                entries.Add(string.Join("@", overlay.Id, (int)overlay.Dock, (int)overlay.LayoutMode,
                                        location.X.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                        location.Y.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                        overlay.UserVisible ? "1" : "0",
                                        overlay.PreferredContentSize.X.ToString(System.Globalization.CultureInfo.InvariantCulture),
                                        overlay.PreferredContentSize.Y.ToString(System.Globalization.CultureInfo.InvariantCulture)));
            }
            return string.Join("|", entries);
        }

        /// <summary>Loads saved overlay placements. Unknown overlay ids are retained until registration.</summary>
        public void ApplyLayout(string layout)
        {
            _savedStates.Clear();
            if (string.IsNullOrWhiteSpace(layout))
                return;
            var entries = layout.Split('|');
            for (int i = 0; i < entries.Length; i++)
            {
                var parts = entries[i].Split('@');
                float width = 0.0f;
                float height = 0.0f;
                if ((parts.Length != 6 && parts.Length != 8) ||
                    !int.TryParse(parts[1], out int dock) ||
                    !int.TryParse(parts[2], out int presentation) ||
                    !float.TryParse(parts[3], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float x) ||
                    !float.TryParse(parts[4], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float y) ||
                    dock < 0 || dock > (int)ViewportOverlayDock.Toolbar ||
                    presentation < 0 || presentation > (int)ViewportOverlayLayoutMode.Collapsed ||
                    (parts.Length == 8 &&
                     (!float.TryParse(parts[6], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out width) ||
                      !float.TryParse(parts[7], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out height) ||
                      width <= 0.0f || height <= 0.0f)))
                    continue;
                _savedStates[parts[0]] = new SavedState
                {
                    Dock = (ViewportOverlayDock)dock,
                    Layout = (ViewportOverlayLayoutMode)presentation,
                    Location = new Float2(x, y),
                    Size = parts.Length == 8 ? new Float2(width, height) : Float2.Zero,
                    HasSize = parts.Length == 8,
                    Visible = parts[5] != "0",
                };
            }
        }

        internal void BeginDrag(ViewportOverlayContainer overlay, Float2 pointer)
        {
            _ghostDock = overlay.Dock;
            _ghostLayout = overlay.LayoutMode;
            _ghostToolbarAnchor = ToolStripAnchor.Left;
            _ghostToolbarIndex = -1;
            var hostLocation = overlay.Location;
            if (overlay.Parent == PrimaryToolStrip)
            {
                _ghostToolbarAnchor = PrimaryToolStrip.GetItemAnchor(overlay);
                _ghostToolbarIndex = PrimaryToolStrip.GetItemIndex(overlay);
                hostLocation = PointFromWindow(overlay.PointToWindow(Float2.Zero));
                PrimaryToolStrip.RemoveItem(overlay, true);
                overlay.Parent = this;
                overlay.Location = hostLocation;
            }
            _dragged = overlay;
            _ghostBounds = new Rectangle(hostLocation, overlay.Size);
            _dragOffset = pointer - hostLocation;
            _lastDragPointer = pointer;
            overlay.FloatingLocation = hostLocation;
            overlay.Dock = ViewportOverlayDock.Floating;
            overlay.LayoutMode = ViewportOverlayLayoutMode.Horizontal;
            UpdateDrag(overlay, pointer);
        }

        internal void UpdateDrag(ViewportOverlayContainer overlay, Float2 pointer)
        {
            if (_dragged != overlay)
                return;
            var location = pointer - _dragOffset;
            location.X = Mathf.Clamp(location.X, 0.0f, Mathf.Max(0.0f, Width - overlay.Width));
            location.Y = Mathf.Clamp(location.Y, TopInset, Mathf.Max(TopInset, Height - overlay.Height));
            overlay.Location = location;
            overlay.FloatingLocation = location;
            _lastDragPointer = pointer;
            _dropTarget = FindDropTarget(pointer);
        }

        internal void EndDrag(ViewportOverlayContainer overlay, bool commit)
        {
            if (_dragged != overlay)
                return;
            if (commit && _dropTarget.HasValue)
                DockOverlay(overlay, _dropTarget.Value, false);
            else if (!commit)
            {
                overlay.FloatingLocation = _ghostBounds.Location;
                if (_ghostDock == ViewportOverlayDock.Toolbar && PrimaryToolStrip != null)
                    AttachToToolStrip(overlay, _ghostToolbarAnchor, _ghostToolbarIndex, false, true);
                else
                {
                    overlay.Dock = _ghostDock;
                    overlay.LayoutMode = _ghostLayout;
                    overlay.Location = _ghostBounds.Location;
                }
            }
            _dragged = null;
            _dropTarget = null;
            PerformLayout();
            if (commit)
                NotifyLayoutChanged();
        }

        internal void ResetOverlay(ViewportOverlayContainer overlay)
        {
            if (overlay == null)
                return;
            DetachFromToolStrip(overlay, true);
            overlay.Dock = ViewportOverlayDock.Floating;
            overlay.LayoutMode = ViewportOverlayLayoutMode.Panel;
            overlay.FloatingLocation = new Float2(12.0f + _overlays.IndexOf(overlay) * 18.0f, TopInset + 12.0f + _overlays.IndexOf(overlay) * 18.0f);
            overlay.Location = overlay.FloatingLocation;
            overlay.UserVisible = true;
            NotifyLayoutChanged();
        }

        internal void NotifyLayoutChanged()
        {
            LayoutChanged?.Invoke();
        }

        internal void DockOverlay(ViewportOverlayContainer overlay, ViewportOverlayDock dock, bool notify)
        {
            if (overlay == null)
                return;
            bool detaching = overlay.Dock != ViewportOverlayDock.Floating && dock == ViewportOverlayDock.Floating;
            if (dock == ViewportOverlayDock.Toolbar && PrimaryToolStrip != null)
            {
                float pointerX = _dragged == overlay ? _lastDragPointer.X : overlay.FloatingLocation.X + overlay.Width * 0.5f;
                AttachToToolStrip(overlay, GetToolStripAnchor(pointerX), -1, false, true);
                if (notify)
                    NotifyLayoutChanged();
                return;
            }
            DetachFromToolStrip(overlay, true);
            overlay.Dock = dock;
            if (detaching)
                overlay.LayoutMode = ViewportOverlayLayoutMode.Horizontal;
            if (overlay.Content is ToolStrip)
            {
                if (dock == ViewportOverlayDock.Left || dock == ViewportOverlayDock.Right)
                    overlay.LayoutMode = ViewportOverlayLayoutMode.Vertical;
                else if (dock == ViewportOverlayDock.Top || dock == ViewportOverlayDock.Bottom ||
                         dock == ViewportOverlayDock.TopLeft || dock == ViewportOverlayDock.TopRight ||
                         dock == ViewportOverlayDock.BottomLeft || dock == ViewportOverlayDock.BottomRight ||
                         dock == ViewportOverlayDock.Toolbar)
                    overlay.LayoutMode = ViewportOverlayLayoutMode.Horizontal;
            }
            PerformLayout();
            if (notify)
                NotifyLayoutChanged();
        }

        private ToolStripAnchor GetToolStripAnchor(float x)
        {
            float normalized = Width > 0.0f ? x / Width : 0.0f;
            return normalized < 0.33f ? ToolStripAnchor.Left : normalized > 0.67f ? ToolStripAnchor.Right : ToolStripAnchor.Center;
        }

        private void AttachToToolStrip(ViewportOverlayContainer overlay, ToolStripAnchor anchor, int index, bool applySavedPlacement, bool notify)
        {
            if (PrimaryToolStrip == null || overlay == null)
                return;
            overlay.Dock = ViewportOverlayDock.Toolbar;
            if (overlay.Content is ToolStrip)
                overlay.LayoutMode = ViewportOverlayLayoutMode.Horizontal;
            if (overlay.Parent != PrimaryToolStrip)
                PrimaryToolStrip.AddContextItem(overlay, anchor, overlay.Id, index, applySavedPlacement);
            else
                PrimaryToolStrip.SetItemPlacement(overlay, anchor, index, overlay.Id, applySavedPlacement);

            if (!applySavedPlacement && index < 0)
            {
                var pointer = PrimaryToolStrip.PointFromWindow(PointToWindow(_lastDragPointer));
                PrimaryToolStrip.UpdateItemDrag(overlay, pointer);
                PrimaryToolStrip.EndItemDrag(overlay, true);
            }
            else if (notify)
            {
                PrimaryToolStrip.SetItemPlacement(overlay, anchor, index, overlay.Id, false, true);
            }
            PrimaryToolStrip.ScrollToAnchor(PrimaryToolStrip.GetItemAnchor(overlay));
        }

        private void DetachFromToolStrip(ViewportOverlayContainer overlay, bool notify)
        {
            if (PrimaryToolStrip == null || overlay?.Parent != PrimaryToolStrip)
                return;
            var location = PointFromWindow(overlay.PointToWindow(Float2.Zero));
            PrimaryToolStrip.RemoveItem(overlay, notify);
            overlay.Parent = this;
            overlay.Location = location;
            overlay.FloatingLocation = location;
        }

        /// <summary>Gets whether this host or one of its descendants currently owns mouse capture.</summary>
        public bool OwnsMouseCapture
        {
            get
            {
                for (Control control = RootWindow?.TrackingControl; control != null; control = control.Parent)
                {
                    if (control == this)
                        return true;
                    for (int i = 0; i < _overlays.Count; i++)
                    {
                        if (control == _overlays[i])
                            return true;
                    }
                }
                return false;
            }
        }

        /// <summary>Returns true when a point overlaps an actual visible overlay or an overlay child owns mouse capture.</summary>
        public bool ContainsInteractiveOverlay(Float2 location)
        {
            // Captured controls must retain ownership after the pointer leaves the overlay bounds.
            // Otherwise the viewport interprets the held button as a new press and can replace the
            // capture (for example, ending a ValueBox drag to begin scene selection).
            if (OwnsMouseCapture)
                return true;
            for (int i = _overlays.Count - 1; i >= 0; i--)
            {
                var overlay = _overlays[i];
                if (overlay.Parent == this && overlay.Visible && overlay.Enabled && overlay.Bounds.Contains(ref location))
                    return true;
            }
            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            if (base.OnMouseWheel(location, delta))
                return true;
            // Preserve viewport zoom in empty host space, but never pass a wheel event through
            // visible overlay chrome or panel content.
            return ContainsInteractiveOverlay(location);
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            float topX = Mathf.Max(0.0f, (Width - MeasureDockSpan(ViewportOverlayDock.Top, true)) * 0.5f);
            float bottomX = Mathf.Max(0.0f, (Width - MeasureDockSpan(ViewportOverlayDock.Bottom, true)) * 0.5f);
            float availableHeight = Mathf.Max(0.0f, Height - TopInset);
            float leftY = TopInset + Mathf.Max(0.0f, (availableHeight - MeasureDockSpan(ViewportOverlayDock.Left, false)) * 0.5f);
            float rightY = TopInset + Mathf.Max(0.0f, (availableHeight - MeasureDockSpan(ViewportOverlayDock.Right, false)) * 0.5f);
            float topLeftY = TopInset;
            float topRightY = TopInset;
            float bottomLeftY = Height;
            float bottomRightY = Height;

            for (int i = 0; i < _overlays.Count; i++)
            {
                var overlay = _overlays[i];
                if (overlay.Parent != this || !overlay.Visible || overlay == _dragged)
                    continue;
                switch (overlay.Dock)
                {
                case ViewportOverlayDock.Floating:
                    overlay.Location = ClampFloating(overlay, overlay.FloatingLocation);
                    break;
                case ViewportOverlayDock.Top:
                    overlay.Location = new Float2(topX, TopInset);
                    topX += overlay.Width + OverlaySpacing;
                    break;
                case ViewportOverlayDock.Bottom:
                    overlay.Location = new Float2(bottomX, Height - overlay.Height);
                    bottomX += overlay.Width + OverlaySpacing;
                    break;
                case ViewportOverlayDock.Left:
                    overlay.Location = new Float2(0.0f, leftY);
                    leftY += overlay.Height + OverlaySpacing;
                    break;
                case ViewportOverlayDock.Right:
                    overlay.Location = new Float2(Width - overlay.Width, rightY);
                    rightY += overlay.Height + OverlaySpacing;
                    break;
                case ViewportOverlayDock.TopLeft:
                    overlay.Location = new Float2(0.0f, topLeftY);
                    topLeftY += overlay.Height + OverlaySpacing;
                    break;
                case ViewportOverlayDock.TopRight:
                    overlay.Location = new Float2(Width - overlay.Width, topRightY);
                    topRightY += overlay.Height + OverlaySpacing;
                    break;
                case ViewportOverlayDock.BottomLeft:
                    bottomLeftY -= overlay.Height;
                    overlay.Location = new Float2(0.0f, bottomLeftY);
                    bottomLeftY -= OverlaySpacing;
                    break;
                case ViewportOverlayDock.BottomRight:
                    bottomRightY -= overlay.Height;
                    overlay.Location = new Float2(Width - overlay.Width, bottomRightY);
                    bottomRightY -= OverlaySpacing;
                    break;
                case ViewportOverlayDock.Toolbar:
                    break;
                }
            }
        }

        private float MeasureDockSpan(ViewportOverlayDock dock, bool horizontal)
        {
            float result = 0.0f;
            int count = 0;
            for (int i = 0; i < _overlays.Count; i++)
            {
                var overlay = _overlays[i];
                if (overlay.Parent != this || !overlay.Visible || overlay == _dragged || overlay.Dock != dock)
                    continue;
                result += horizontal ? overlay.Width : overlay.Height;
                count++;
            }
            if (count > 1)
                result += (count - 1) * OverlaySpacing;
            return result;
        }

        /// <inheritdoc />
        protected override void DrawChildren()
        {
            base.DrawChildren();
            if (_dragged == null)
                return;
            var blue = new Color(0.0f, 0.66f, 0.94f, 1.0f);
            DrawDropZones(blue.AlphaMultiplied(0.18f));
            if (_dropTarget.HasValue)
                StyleRendering.FillRoundedRectangle(GetDropRectangle(_dropTarget.Value), blue.AlphaMultiplied(0.5f), 3.0f);
        }

        private void ShowAll()
        {
            for (int i = 0; i < _overlays.Count; i++)
                _overlays[i].UserVisible = true;
            NotifyLayoutChanged();
        }

        private void ResetLayout()
        {
            for (int i = 0; i < _overlays.Count; i++)
                ResetOverlay(_overlays[i]);
            NotifyLayoutChanged();
        }

        private Float2 ClampFloating(ViewportOverlayContainer overlay, Float2 location)
        {
            location.X = Mathf.Clamp(location.X, 0.0f, Mathf.Max(0.0f, Width - overlay.Width));
            location.Y = Mathf.Clamp(location.Y, TopInset, Mathf.Max(TopInset, Height - overlay.Height));
            overlay.FloatingLocation = location;
            return location;
        }

        private ViewportOverlayDock? FindDropTarget(Float2 pointer)
        {
            const float edge = 56.0f;
            const float corner = 48.0f;
            if (pointer.Y <= TopInset)
            {
                _toolStripDropAnchor = GetToolStripAnchor(pointer.X);
                return ViewportOverlayDock.Toolbar;
            }
            if (pointer.X <= corner && pointer.Y <= TopInset + corner)
                return ViewportOverlayDock.TopLeft;
            if (pointer.X >= Width - corner && pointer.Y <= TopInset + corner)
                return ViewportOverlayDock.TopRight;
            if (pointer.X <= corner && pointer.Y >= Height - corner)
                return ViewportOverlayDock.BottomLeft;
            if (pointer.X >= Width - corner && pointer.Y >= Height - corner)
                return ViewportOverlayDock.BottomRight;
            if (pointer.Y <= TopInset + edge)
                return ViewportOverlayDock.Top;
            if (pointer.Y >= Height - edge)
                return ViewportOverlayDock.Bottom;
            if (pointer.X <= edge)
                return ViewportOverlayDock.Left;
            if (pointer.X >= Width - edge)
                return ViewportOverlayDock.Right;
            return null;
        }

        private void DrawDropZones(Color color)
        {
            StyleRendering.FillRoundedRectangle(GetToolStripDropRectangle(ToolStripAnchor.Left), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetToolStripDropRectangle(ToolStripAnchor.Center), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetToolStripDropRectangle(ToolStripAnchor.Right), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.Top), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.Bottom), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.Left), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.Right), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.TopLeft), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.TopRight), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.BottomLeft), color, 3.0f);
            StyleRendering.FillRoundedRectangle(GetDropRectangle(ViewportOverlayDock.BottomRight), color, 3.0f);
        }

        private Rectangle GetDropRectangle(ViewportOverlayDock dock)
        {
            const float edge = 28.0f;
            const float corner = 72.0f;
            return dock switch
            {
                ViewportOverlayDock.Top => new Rectangle(corner + 4, TopInset, Mathf.Max(0, Width - corner * 2 - 8), edge),
                ViewportOverlayDock.Bottom => new Rectangle(corner + 4, Height - edge, Mathf.Max(0, Width - corner * 2 - 8), edge),
                ViewportOverlayDock.Left => new Rectangle(0, TopInset + corner + 4, edge, Mathf.Max(0, Height - TopInset - corner * 2 - 8)),
                ViewportOverlayDock.Right => new Rectangle(Width - edge, TopInset + corner + 4, edge, Mathf.Max(0, Height - TopInset - corner * 2 - 8)),
                ViewportOverlayDock.TopLeft => new Rectangle(0, TopInset, corner, corner),
                ViewportOverlayDock.TopRight => new Rectangle(Width - corner, TopInset, corner, corner),
                ViewportOverlayDock.BottomLeft => new Rectangle(0, Height - corner, corner, corner),
                ViewportOverlayDock.BottomRight => new Rectangle(Width - corner, Height - corner, corner, corner),
                ViewportOverlayDock.Toolbar => GetToolStripDropRectangle(_toolStripDropAnchor),
                _ => Rectangle.Empty,
            };
        }

        private Rectangle GetToolStripDropRectangle(ToolStripAnchor anchor)
        {
            const float gap = 3.0f;
            float zoneWidth = Mathf.Max(0.0f, (Width - gap * 4.0f) / 3.0f);
            return new Rectangle(gap + (zoneWidth + gap) * (int)anchor, gap, zoneWidth, Mathf.Max(0.0f, TopInset - gap * 2.0f));
        }
    }
}
