// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;

namespace FlaxEngine.GUI
{
    /// <summary>
    /// Drop Panel arranges control vertically and provides feature to collapse contents.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    [ActorToolbox("GUI")]
    public class DropPanel : ContainerControl
    {
        /// <summary>
        /// Size of the drop down icon. 
        /// </summary>
        public const float DropDownIconSize = 12.0f;

        private const float DropDownIconPadding = 4.0f;
        private const float DropDownIconTextSpacing = 6.0f;
        private const float InnerBorderInset = 1.5f;
        private static readonly Color InnerBorderColor = new Color(1.0f, 1.0f, 1.0f, 0.05f);

        /// <summary>
        /// The header height.
        /// </summary>
        protected float _headerHeight = 14.0f;

        /// <summary>
        /// The header text margin.
        /// </summary>
        protected Margin _headerTextMargin = new Margin(2.0f);

        /// <summary>
        /// The 'is closed' flag.
        /// </summary>
        protected bool _isClosed;

        /// <summary>
        /// The 'mouse over header' flag (over header).
        /// </summary>
        protected bool _mouseOverHeader;

        /// <summary>
        /// The 'mouse down' flag (over header) for the left mouse button.
        /// </summary>
        protected bool _mouseButtonLeftDown;

        /// <summary>
        /// The 'mouse down' flag (over header) for the right mouse button.
        /// </summary>
        protected bool _mouseButtonRightDown;

        private bool _dropDownIconMouseCaptureActive;
        private bool _dropDownIconMouseCaptureSuppressActions;
        private bool _dropDownIconMouseCaptureClose;
        private ContainerControl _dropDownIconMouseCaptureParent;
        private HashSet<DropPanel> _dropDownIconMouseCaptureProcessedPanels;

        /// <summary>
        /// The animation progress (normalized).
        /// </summary>
        protected float _animationProgress = 1.0f;

        /// <summary>
        /// The cached height of the control.
        /// </summary>
        protected float _cachedHeight = 16.0f;

        /// <summary>
        /// The items spacing.
        /// </summary>
        protected float _itemsSpacing = 2.0f;

        /// <summary>
        /// The items margin.
        /// </summary>
        protected Margin _itemsMargin = new Margin(2.0f, 2.0f, 2.0f, 2.0f);

        /// <summary>
        /// Gets or sets the header text.
        /// </summary>
        [EditorOrder(10), Tooltip("The text to show on a panel header.")]
        public string HeaderText { get; set; }

        /// <summary>
        /// The active search text query used to highlight matching parts of the header text.
        /// </summary>
        public string SearchText = string.Empty;

        private string _lastSearchText;
        private string _lastHeaderText;
        private int _highlightIndex = -1;

        private void UpdateHighlights()
        {
            if (_lastSearchText == SearchText && _lastHeaderText == HeaderText)
                return;

            _lastSearchText = SearchText;
            _lastHeaderText = HeaderText;

            if (!string.IsNullOrEmpty(SearchText) && !string.IsNullOrEmpty(HeaderText))
            {
                _highlightIndex = HeaderText.IndexOf(SearchText, StringComparison.OrdinalIgnoreCase);
            }
            else
            {
                _highlightIndex = -1;
            }
        }

        /// <summary>
        /// Gets or sets the height of the header.
        /// </summary>
        [EditorOrder(20), Limit(1, 10000, 0.1f), Tooltip("The height of the panel header.")]
        public float HeaderHeight
        {
            get => _headerHeight;
            set
            {
                if (!Mathf.NearEqual(_headerHeight, value))
                {
                    _headerHeight = value;
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the header margin.
        /// </summary>
        [EditorOrder(30), Tooltip("The margin of the header text area.")]
        public Margin HeaderTextMargin
        {
            get => _headerTextMargin;
            set
            {
                if (_headerTextMargin != value)
                {
                    _headerTextMargin = value;
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether enable drop down icon drawing.
        /// </summary>
        [EditorOrder(2)]
        public bool EnableDropDownIcon { get; set; }

        /// <summary>
        /// Gets or sets a value indicating whether dragging from the drop down icon opens/closes sibling panels.
        /// </summary>
        [EditorOrder(2)]
        public bool EnableDropDownIconDragOpenClose { get; set; }

        /// <summary>
        /// Gets or sets additional left indentation for the drop down icon and header text.
        /// </summary>
        [EditorDisplay("Icon Style"), EditorOrder(2029), Tooltip("Additional left indentation for the drop down icon and header text.")]
        public float DropDownIconIndent { get; set; }

        /// <summary>
        /// Get or sets a value indicating whether the panel can be opened or closed via the user interacting with the ui.
        /// Changing the open/ closed state from code or the Properties panel will still work regardless.
        /// </summary>
        [EditorOrder(1)]
        public bool CanOpenClose { get; set; } = true;

        /// <summary>
        /// Optional local-space hit test for header open/close mouse input. Null uses the whole header.
        /// </summary>
        [NoSerialize, HideInEditor]
        public Func<Float2, bool> HeaderInputHitTest { get; set; }

        /// <summary>
        /// Gets or sets a value indicating whether to enable containment line drawing,
        /// </summary>
        [EditorOrder(3)]
        public bool EnableContainmentLines { get; set; } = false;

        /// <summary>
        /// Gets or sets the color used to draw header text.
        /// </summary>
        [EditorDisplay("Header Style"), EditorOrder(2010), ExpandGroups]
        public Color HeaderTextColor;

        /// <summary>
        /// Gets or sets the color of the header.
        /// </summary>
        [EditorDisplay("Header Style"), EditorOrder(2011)]
        public Color HeaderColor { get; set; }

        /// <summary>
        /// Gets or sets the color of the header when mouse is over.
        /// </summary>
        [EditorDisplay("Header Style"), EditorOrder(2012)]
        public Color HeaderColorMouseOver { get; set; }

        /// <summary>
        /// Gets or sets the border color (transparent if not used).
        /// </summary>
        [EditorDisplay("Border Style"), EditorOrder(2013)]
        public Color BorderColor { get; set; }

        /// <summary>
        /// Gets or sets the font used to render panel header text.
        /// </summary>
        [EditorDisplay("Header Text Style"), EditorOrder(2020), ExpandGroups]
        public FontReference HeaderTextFont { get; set; }

        /// <summary>
        /// Gets or sets the custom material used to render the text. It must has domain set to GUI and have a public texture parameter named Font used to sample font atlas texture with font characters data.
        /// </summary>
        [EditorDisplay("Header Text Style"), EditorOrder(2021), Tooltip("Custom material used to render the header text. It must has domain set to GUI and have a public texture parameter named Font used to sample font atlas texture with font characters data.")]
        public MaterialBase HeaderTextMaterial { get; set; }

        /// <summary>
        /// Occurs when mouse right-clicks over the header.
        /// </summary>
        public event Action<DropPanel, Float2> MouseButtonRightClicked;

        /// <summary>
        /// Occurs when drop panel is opened or closed.
        /// </summary>
        public event Action<DropPanel> IsClosedChanged;

        /// <summary>
        /// Gets or sets a value indicating whether this panel is closed.
        /// </summary>
        [EditorOrder(0), Tooltip("If checked, the panel is closed, otherwise is open.")]
        public bool IsClosed
        {
            get => _isClosed;
            set
            {
                if (_isClosed != value)
                {
                    if (value)
                        Close(false);
                    else
                        Open(false);
                }
            }
        }

        /// <summary>
        /// Gets or sets the item slots margin (the space around items).
        /// </summary>
        [EditorOrder(10)]
        public Margin ItemsMargin
        {
            get => _itemsMargin;
            set
            {
                if (_itemsMargin != value)
                {
                    _itemsMargin = value;
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the item slots spacing (the margin between items).
        /// </summary>
        [EditorOrder(11)]
        public float ItemsSpacing
        {
            get => _itemsSpacing;
            set
            {
                if (!Mathf.NearEqual(_itemsSpacing, value))
                {
                    _itemsSpacing = value;
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets or sets the panel close/open animation duration (in seconds).
        /// </summary>
        [EditorOrder(10), Limit(0, 100, 0.01f), Tooltip("The panel close/open animation duration (in seconds).")]
        public float CloseAnimationTime { get; set; } = 0.2f;

        /// <summary>
        /// Gets or sets the image used to render drop panel drop arrow icon when panel is opened.
        /// </summary>
        [EditorDisplay("Icon Style"), EditorOrder(2030), Tooltip("The image used to render drop panel drop arrow icon when panel is opened."), ExpandGroups]
        public IBrush ArrowImageOpened { get; set; }

        /// <summary>
        /// Gets or sets the image used to render drop panel drop arrow icon when panel is closed.
        /// </summary>
        [EditorDisplay("Icon Style"), EditorOrder(2031), Tooltip("The image used to render drop panel drop arrow icon when panel is closed.")]
        public IBrush ArrowImageClosed { get; set; }

        /// <summary>
        /// Gets the header rectangle.
        /// </summary>
        protected Rectangle HeaderRectangle => new Rectangle(0, 0, Width, HeaderHeight);

        /// <summary>
        /// Gets the drop down icon rectangle.
        /// </summary>
        protected Rectangle DropDownIconRectangle => EnableDropDownIcon ? new Rectangle(DropDownIconPadding + DropDownIconIndent, (HeaderHeight - DropDownIconSize) * 0.5f, DropDownIconSize, DropDownIconSize) : Rectangle.Empty;

        private bool IsHeaderInputLocation(Float2 location)
        {
            return HeaderInputHitTest != null ? HeaderInputHitTest(location) : HeaderRectangle.Contains(location);
        }

        /// <inheritdoc />
        protected override bool ShowTooltip => base.ShowTooltip && _mouseOverHeader;

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            var result = base.OnShowTooltip(out text, out location, out area);

            // Change the position
            location = new Float2(Width * 0.5f, HeaderHeight);

            return result;
        }

        /// <inheritdoc />
        public override bool OnTestTooltipOverControl(ref Float2 location)
        {
            return IsHeaderInputLocation(location);
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="DropPanel"/> class.
        /// </summary>
        public DropPanel()
        : base(0, 0, 64, 16.0f)
        {
            AutoFocus = false;

            var style = Style.Current;
            HeaderColor = style.BackgroundNormal;
            HeaderColorMouseOver = style.BackgroundHighlighted;
            BorderColor = Color.Transparent;
            HeaderTextFont = new FontReference(style.FontMedium);
            HeaderTextColor = style.Foreground;
            ArrowImageOpened = new SpriteBrush(style.ArrowDown);
            ArrowImageClosed = new SpriteBrush(style.ArrowRight);
        }

        /// <summary>
        /// Opens the group.
        /// </summary>
        /// <param name="animate">Enable/disable animation feature.</param>
        public void Open(bool animate = false)
        {
            // Check if state will change
            if (_isClosed)
            {
                // Set flag
                _isClosed = false;
                if (animate)
                    _animationProgress = 1 - _animationProgress;
                else
                    _animationProgress = 1.0f;

                // Update
                PerformLayout();

                // Fire event
                IsClosedChanged?.Invoke(this);
            }
        }

        /// <summary>
        /// Closes the group.
        /// </summary>
        /// <param name="animate">Enable/disable animation feature.</param>
        public void Close(bool animate = false)
        {
            // Check if state will change
            if (!_isClosed)
            {
                // Set flag
                _isClosed = true;
                if (animate)
                    _animationProgress = 1 - _animationProgress;
                else
                    _animationProgress = 1.0f;

                // Update
                PerformLayout();

                // Fire event
                IsClosedChanged?.Invoke(this);
            }
        }

        /// <summary>
        /// Toggles open state
        /// </summary>
        public void Toggle()
        {
            if (_isClosed)
                Open(true);
            else
                Close(true);
        }

        private bool CanDragDropDownIcon => EnableDropDownIconDragOpenClose && EnableDropDownIcon && CanOpenClose;

        private void BeginDropDownIconMouseCapture()
        {
            _dropDownIconMouseCaptureSuppressActions = true;
            _dropDownIconMouseCaptureActive = true;
            _dropDownIconMouseCaptureClose = !_isClosed;
            _dropDownIconMouseCaptureParent = Parent;

            if (_dropDownIconMouseCaptureProcessedPanels == null)
                _dropDownIconMouseCaptureProcessedPanels = new HashSet<DropPanel>();
            else
                _dropDownIconMouseCaptureProcessedPanels.Clear();

            StartMouseCapture();
            ApplyDropDownIconMouseCapture(this);
        }

        private void ClearDropDownIconMouseCapture()
        {
            _dropDownIconMouseCaptureActive = false;
            _dropDownIconMouseCaptureSuppressActions = false;
            _dropDownIconMouseCaptureParent = null;
            _dropDownIconMouseCaptureProcessedPanels?.Clear();
        }

        private void EndDropDownIconMouseCapture()
        {
            if (!_dropDownIconMouseCaptureActive && !_dropDownIconMouseCaptureSuppressActions)
                return;

            var wasCapturing = _dropDownIconMouseCaptureActive;
            ClearDropDownIconMouseCapture();
            if (wasCapturing)
                EndMouseCapture();
        }

        private void UpdateDropDownIconMouseCapture(Float2 location)
        {
            var parent = _dropDownIconMouseCaptureParent;
            if (parent == null)
                return;

            var parentLocation = PointToParent(parent, location);
            var children = parent.Children;
            for (int i = children.Count - 1; i >= 0; i--)
            {
                if (children[i] is DropPanel dropPanel && dropPanel.Visible && dropPanel.Enabled && dropPanel.CanDragDropDownIcon)
                {
                    var dropPanelLocation = dropPanel.PointFromParent(parent, parentLocation);
                    if (dropPanel.IsHeaderInputLocation(dropPanelLocation))
                    {
                        ApplyDropDownIconMouseCapture(dropPanel);
                        break;
                    }
                }
            }
        }

        private void ApplyDropDownIconMouseCapture(DropPanel dropPanel)
        {
            if (dropPanel == null || dropPanel.Parent != _dropDownIconMouseCaptureParent || !dropPanel.CanDragDropDownIcon)
                return;
            if (!_dropDownIconMouseCaptureProcessedPanels.Add(dropPanel))
                return;

            if (_dropDownIconMouseCaptureClose)
                dropPanel.Close(true);
            else
                dropPanel.Open(true);
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // Drop/down animation
            if (_animationProgress < 1.0f)
            {
                float openCloseAnimationTime = CloseAnimationTime;
                bool isDeltaSlow = deltaTime > (1 / 20.0f);

                // Update progress
                if (isDeltaSlow || openCloseAnimationTime < Mathf.Epsilon)
                {
                    _animationProgress = 1.0f;
                }
                else
                {
                    _animationProgress += deltaTime / openCloseAnimationTime;
                    if (_animationProgress > 1.0f)
                        _animationProgress = 1.0f;
                }

                // Arrange controls
                PerformLayout();
            }
            else
            {
                base.Update(deltaTime);
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            var style = Style.Current;
            var enabled = VisuallyEnabledInHierarchy;
            var cornerRadius = 5.0f;

            // Draw Background
            var backgroundColor = BackgroundColor;
            if (backgroundColor.A > 0.0f)
            {
                var backgroundRect = new Rectangle(Float2.Zero, Size);
                if (cornerRadius > 0.0f)
                    StyleRendering.FillRoundedRectangle(backgroundRect, backgroundColor, cornerRadius);
                else
                    Render2D.FillRectangle(backgroundRect, backgroundColor);
            }

            if (BorderColor.A > 0.0f)
            {
                var borderRect = new Rectangle(Float2.Zero, Size);
                if (cornerRadius > 0.0f)
                    StyleRendering.DrawRoundedRectangleBorder(borderRect, BorderColor, 1.0f, cornerRadius);
                else
                    Render2D.DrawRectangle(borderRect, BorderColor);
            }

            // Header
            var color = _mouseOverHeader && CanOpenClose ? HeaderColorMouseOver : HeaderColor;
            if (color.A > 0.0f)
            {
                var headerRect = new Rectangle(0, 0, Width, HeaderHeight);
                if (cornerRadius > 0.0f && backgroundColor.A > 0.0f)
                {
                    StyleRendering.FillRoundedRectangle(headerRect, color, cornerRadius, _isClosed ? RoundedCorners.All : RoundedCorners.Top);
                }
                else
                {
                    Render2D.FillRectangle(headerRect, color);
                }
            }

            if (BorderColor.A > 0.0f)
            {
                var innerBorderRect = new Rectangle(Float2.Zero, Size).MakeExpanded(-InnerBorderInset);
                if (innerBorderRect.Width > 0.0f && innerBorderRect.Height > 0.0f)
                {
                    if (cornerRadius > 0.0f)
                        StyleRendering.DrawRoundedRectangleBorder(innerBorderRect, InnerBorderColor, 1.0f, Mathf.Max(0.0f, cornerRadius - InnerBorderInset));
                    else
                        Render2D.DrawRectangle(innerBorderRect, InnerBorderColor);
                }
            }

            // Drop down icon
            float textLeft = 0;
            if (EnableDropDownIcon)
            {
                var dropDownRect = DropDownIconRectangle;
                textLeft += dropDownRect.Right + DropDownIconTextSpacing;
                var arrowColor = _mouseOverHeader ? style.Foreground : style.ForegroundGrey;
                if (_isClosed)
                    ArrowImageClosed?.Draw(dropDownRect, arrowColor);
                else
                    ArrowImageOpened?.Draw(dropDownRect, arrowColor);
            }

            // Header text
            var textRect = new Rectangle(textLeft, 0, Width - textLeft, HeaderHeight);
            _headerTextMargin.ShrinkRectangle(ref textRect);
            var textColor = HeaderTextColor;
            if (!enabled)
            {
                textColor *= 0.6f;
            }

            Render2D.PushClip(textRect);
            Render2D.DrawText(HeaderTextFont.GetFont(), HeaderTextMaterial, HeaderText, textRect, textColor, TextAlignment.Near, TextAlignment.Center);
            UpdateHighlights();
            if (_highlightIndex >= 0)
            {
                var font = HeaderTextFont.GetFont();
                if (font != null)
                {
                    var highlightColor = Style.Current.ProgressNormal * 0.6f;
                    var textSize = font.MeasureText(HeaderText);
                    var textY = (HeaderHeight - textSize.Y) * 0.5f;
                    var start = font.GetCharPosition(HeaderText, _highlightIndex);
                    var end = font.GetCharPosition(HeaderText, _highlightIndex + SearchText.Length);
                    var highlightRect = new Rectangle(start.X + textRect.X, textY, end.X - start.X, textSize.Y);
                    Render2D.FillRectangle(highlightRect, highlightColor);
                }
            }
            Render2D.PopClip();

            if (!_isClosed && EnableContainmentLines)
            {
                Color lineColor = Style.Current.ForegroundGrey - new Color(0, 0, 0, 100);
                float lineThickness = 0.05f;
                Render2D.DrawLine(new Float2(1, HeaderHeight), new Float2(1, Height), lineColor, lineThickness);
                Render2D.DrawLine(new Float2(1, Height), new Float2(Width, Height), lineColor, lineThickness);
                Render2D.DrawLine(new Float2(Width, HeaderHeight), new Float2(Width, Height), lineColor, lineThickness);
            }

            // Children
            DrawChildren();
        }

        /// <inheritdoc />
        protected override void DrawChildren()
        {
            // Draw child controls that are not arranged (pined to the header, etc.)
            for (int i = 0; i < _children.Count; i++)
            {
                var child = _children[i];
                if (!child.IsScrollable && child.Visible)
                {
                    Render2D.PushTransform(ref child._cachedTransform);
                    child.Draw();
                    Render2D.PopTransform();
                }
            }

            // Check if isn't fully closed
            if (!_isClosed || _animationProgress < 0.998f)
            {
                // Draw children with clipping mask (so none of the controls will override the header)
                Rectangle clipMask = new Rectangle(0, HeaderHeight, Width, Height - HeaderHeight);
                Render2D.PushClip(ref clipMask);
                if (CullChildren)
                {
                    Render2D.PeekClip(out var globalClipping);
                    Render2D.PeekTransform(out var globalTransform);
                    for (int i = 0; i < _children.Count; i++)
                    {
                        var child = _children[i];
                        if (child.IsScrollable && child.Visible)
                        {
                            Matrix3x3.Multiply(ref child._cachedTransform, ref globalTransform, out var globalChildTransform);
                            var childGlobalRect = new Rectangle(globalChildTransform.M31, globalChildTransform.M32, child.Width * globalChildTransform.M11, child.Height * globalChildTransform.M22);
                            if (globalClipping.Intersects(ref childGlobalRect))
                            {
                                Render2D.PushTransform(ref child._cachedTransform);
                                child.Draw();
                                Render2D.PopTransform();
                            }
                        }
                    }
                }
                else
                {
                    for (int i = 0; i < _children.Count; i++)
                    {
                        var child = _children[i];
                        if (child.IsScrollable && child.Visible)
                        {
                            Render2D.PushTransform(ref child._cachedTransform);
                            child.Draw();
                            Render2D.PopTransform();
                        }
                    }
                }

                Render2D.PopClip();
            }
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (base.OnMouseDown(location, button))
                return true;

            _mouseOverHeader = IsHeaderInputLocation(location);
            if (button == MouseButton.Left && _mouseOverHeader)
            {
                _mouseButtonLeftDown = true;
                if (CanDragDropDownIcon)
                    BeginDropDownIconMouseCapture();
                return true;
            }
            if (button == MouseButton.Right && _mouseOverHeader)
            {
                _mouseButtonRightDown = true;
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            _mouseOverHeader = IsHeaderInputLocation(location);

            if (_dropDownIconMouseCaptureActive || _dropDownIconMouseCaptureSuppressActions)
            {
                if (_dropDownIconMouseCaptureActive)
                    UpdateDropDownIconMouseCapture(location);
                return;
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && (_dropDownIconMouseCaptureActive || _dropDownIconMouseCaptureSuppressActions))
            {
                _mouseOverHeader = IsHeaderInputLocation(location);
                if (_dropDownIconMouseCaptureActive)
                    UpdateDropDownIconMouseCapture(location);
                _mouseButtonLeftDown = false;
                EndDropDownIconMouseCapture();
                Focus();
                return true;
            }

            if (base.OnMouseUp(location, button))
                return true;

            _mouseOverHeader = IsHeaderInputLocation(location);
            if (button == MouseButton.Left && _mouseButtonLeftDown)
            {
                _mouseButtonLeftDown = false;
                if (_mouseOverHeader && CanOpenClose)
                    Toggle();
                return true;
            }
            if (button == MouseButton.Right && _mouseButtonRightDown)
            {
                _mouseButtonRightDown = false;
                MouseButtonRightClicked?.Invoke(this, location);
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && (_dropDownIconMouseCaptureActive || _dropDownIconMouseCaptureSuppressActions))
            {
                _mouseButtonLeftDown = false;
                EndDropDownIconMouseCapture();
                return true;
            }

            if (base.OnMouseDoubleClick(location, button))
                return true;
            
            _mouseOverHeader = IsHeaderInputLocation(location);
            if (button == MouseButton.Left && _mouseOverHeader)
            {
                _mouseButtonLeftDown = true;
                return true;
            }

            if (button == MouseButton.Left && _mouseButtonLeftDown)
            {
                _mouseButtonLeftDown = false;
                if (_mouseOverHeader && CanOpenClose)
                    Toggle();
                return true;
            }
            return false;
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            if (_dropDownIconMouseCaptureActive || _dropDownIconMouseCaptureSuppressActions)
            {
                _mouseOverHeader = false;
                base.OnMouseLeave();
                return;
            }

            _mouseButtonLeftDown = false;
            _mouseButtonRightDown = false;
            _mouseOverHeader = false;

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            if (_dropDownIconMouseCaptureActive || _dropDownIconMouseCaptureSuppressActions)
            {
                ClearDropDownIconMouseCapture();
                _mouseButtonLeftDown = false;
            }

            base.OnEndMouseCapture();
        }

        /// <inheritdoc />
        public override void OnChildResized(Control control)
        {
            base.OnChildResized(control);

            PerformLayout();
        }

        /// <inheritdoc />
        public override void GetDesireClientArea(out Rectangle rect)
        {
            var topMargin = HeaderHeight;
            rect = new Rectangle(0, topMargin, Width, Height - topMargin);
        }

        private void Arrange()
        {
            // Arrange anchored controls
            GetDesireClientArea(out var clientArea);
            float dropOffset = _cachedHeight * (_isClosed ? _animationProgress : 1.0f - _animationProgress);
            clientArea.Location.Y -= dropOffset;
            UpdateChildrenBounds();

            // Arrange scrollable controls
            var slotsMargin = _itemsMargin;
            var slotsLeft = clientArea.Left + slotsMargin.Left;
            var slotsWidth = clientArea.Width - slotsMargin.Width;
            float minHeight = HeaderHeight;
            float y = clientArea.Top + slotsMargin.Top;
            bool anyAdded = false;
            for (int i = 0; i < _children.Count; i++)
            {
                Control c = _children[i];
                if (c.IsScrollable && c.Visible)
                {
                    var h = c.Height;
                    c.Bounds = new Rectangle(slotsLeft, y, slotsWidth, h);
                    h += _itemsSpacing;
                    y += h;
                    anyAdded = true;
                }
            }

            // Update panel height
            if (anyAdded)
                y -= _itemsSpacing;
            if (anyAdded)
                y += slotsMargin.Bottom;
            float height = dropOffset + y;
            _cachedHeight = height;
            if (_animationProgress >= 1.0f && _isClosed)
                y = minHeight;
            var size = new Float2(Width, Mathf.Max(minHeight, y));
            Resize(ref size);
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            Arrange();
        }

        /// <inheritdoc />
        protected override void PerformLayoutAfterChildren()
        {
            Arrange();
        }

        /// <inheritdoc />
        protected override bool CanNavigateChild(Control child)
        {
            // Closed panel skips navigation for hidden children
            if (IsClosed && child.IsScrollable)
                return false;
            return base.CanNavigateChild(child);
        }
    }
}
