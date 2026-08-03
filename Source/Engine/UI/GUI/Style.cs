// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEngine.GUI
{
    /// <summary>
    /// Describes GUI controls style (which fonts and colors use etc.). Defines the default values used by the GUI control.s
    /// </summary>
    public class Style
    {
        /// <summary>
        /// Global GUI style used by all the controls.
        /// </summary>
        public static Style Current { get; set; }

        [Serialize]
        private FontReference _fontTitle;

        /// <summary>
        /// The font title.
        /// </summary>
        [NoSerialize]
        [EditorOrder(10)]
        public Font FontTitle
        {
            get => _fontTitle?.GetFont();
            set => _fontTitle = new FontReference(value);
        }

        [Serialize]
        private FontReference _fontLarge;

        /// <summary>
        /// The font large.
        /// </summary>
        [NoSerialize]
        [EditorOrder(20)]
        public Font FontLarge
        {
            get => _fontLarge?.GetFont();
            set => _fontLarge = new FontReference(value);
        }

        [Serialize]
        private FontReference _fontMedium;

        /// <summary>
        /// The font medium.
        /// </summary>
        [NoSerialize]
        [EditorOrder(30)]
        public Font FontMedium
        {
            get => _fontMedium?.GetFont();
            set => _fontMedium = new FontReference(value);
        }

        [Serialize]
        private FontReference _fontSmall;

        /// <summary>
        /// The font small.
        /// </summary>
        [NoSerialize]
        [EditorOrder(40)]
        public Font FontSmall
        {
            get => _fontSmall?.GetFont();
            set => _fontSmall = new FontReference(value);
        }

        /// <summary>
        /// The background color.
        /// </summary>
        [EditorOrder(60)]
        public Color Background;

        /// <summary>
        /// The secondary background color.
        /// </summary>
        [EditorOrder(70)]
        public Color SecondaryBackground;

        /// <summary>
        /// The drag window color.
        /// </summary>
        [EditorOrder(80)]
        public Color DragWindow;

        /// <summary>
        /// The foreground color.
        /// </summary>
        [EditorOrder(90)]
        public Color Foreground;

        /// <summary>
        /// The foreground grey.
        /// </summary>
        [EditorOrder(100)]
        public Color ForegroundGrey;

        /// <summary>
        /// The foreground disabled.
        /// </summary>
        [EditorOrder(110)]
        public Color ForegroundDisabled;

        /// <summary>
        /// The foreground color in viewports (usually have a dark background)
        /// </summary>
        [EditorOrder(115)]
        public Color ForegroundViewport;

        /// <summary>
        /// The background highlighted color.
        /// </summary>
        [EditorOrder(120)]
        public Color BackgroundHighlighted;

        /// <summary>
        /// The border highlighted color.
        /// </summary>
        [EditorOrder(130)]
        public Color BorderHighlighted;

        /// <summary>
        /// The background selected color.
        /// </summary>
        [EditorOrder(140)]
        public Color BackgroundSelected;

        /// <summary>
        /// The border selected color.
        /// </summary>
        [EditorOrder(150)]
        public Color BorderSelected;

        /// <summary>
        /// The background normal color.
        /// </summary>
        [EditorOrder(160)]
        public Color BackgroundNormal;

        /// <summary>
        /// The border normal color.
        /// </summary>
        [EditorOrder(170)]
        public Color BorderNormal;

        /// <summary>
        /// The text box background color.
        /// </summary>
        [EditorOrder(180)]
        public Color TextBoxBackground;

        /// <summary>
        /// The text box background selected color.
        /// </summary>
        [EditorOrder(190)]
        public Color TextBoxBackgroundSelected;

        /// <summary>
        /// The collection background color.
        /// </summary>
        [EditorOrder(195)]
        public Color CollectionBackgroundColor;

        /// <summary>
        /// The progress normal color.
        /// </summary>
        [EditorOrder(200)]
        public Color ProgressNormal;

        /// <summary>
        /// The selection and drag drop highlights colors.
        /// </summary>
        [EditorOrder(205)]
        public Color Selection;

        /// <summary>
        /// The selection and drag drop highlights border colors.
        /// </summary>
        [EditorOrder(206)]
        public Color SelectionBorder;

        /// <summary>
        /// The preferred corner radius for interactive controls. A value of zero preserves square corners.
        /// </summary>
        [EditorOrder(207)]
        public float CornerRadius;

        /// <summary>
        /// The preferred height for compact interactive controls. A value of zero lets controls use their legacy size.
        /// </summary>
        [EditorOrder(208)]
        public float ControlHeight;

        /// <summary>
        /// The preferred height of the global editor toolbar.
        /// </summary>
        [EditorOrder(209)]
        public float ToolbarHeight;

        /// <summary>
        /// The preferred height of window and document tabs.
        /// </summary>
        [EditorOrder(210)]
        public float TabHeight;

        /// <summary>
        /// The preferred height of compact hierarchy rows.
        /// </summary>
        [EditorOrder(211)]
        public float TreeRowHeight;

        /// <summary>
        /// The preferred height of property editor rows.
        /// </summary>
        [EditorOrder(212)]
        public float PropertyRowHeight;

        /// <summary>
        /// The preferred inset used inside editor panels.
        /// </summary>
        [EditorOrder(213)]
        public float PanelPadding;

        /// <summary>
        /// The maximum size of standard interface glyphs. Content previews and viewport gizmos are excluded.
        /// </summary>
        [EditorOrder(214)]
        public float IconSize;

        /// <summary>
        /// The status bar style
        /// </summary>
        [EditorOrder(220)]
        public StatusbarStyle Statusbar;

        /// <summary>
        /// The arrow right icon.
        /// </summary>
        [EditorOrder(220)]
        public SpriteHandle ArrowRight;

        /// <summary>
        /// The arrow down icon.
        /// </summary>
        [EditorOrder(230)]
        public SpriteHandle ArrowDown;

        /// <summary>
        /// The search icon.
        /// </summary>
        [EditorOrder(240)]
        public SpriteHandle Search;

        /// <summary>
        /// The settings icon.
        /// </summary>
        [EditorOrder(250)]
        public SpriteHandle Settings;

        /// <summary>
        /// The cross icon.
        /// </summary>
        [EditorOrder(260)]
        public SpriteHandle Cross;

        /// <summary>
        /// The CheckBox intermediate icon.
        /// </summary>
        [EditorOrder(270)]
        public SpriteHandle CheckBoxIntermediate;

        /// <summary>
        /// The CheckBox tick icon.
        /// </summary>
        [EditorOrder(280)]
        public SpriteHandle CheckBoxTick;

        /// <summary>
        /// The status bar size grip icon.
        /// </summary>
        [EditorOrder(290)]
        public SpriteHandle StatusBarSizeGrip;

        /// <summary>
        /// The translate icon.
        /// </summary>
        [EditorOrder(300)]
        public SpriteHandle Translate;

        /// <summary>
        /// The rotate icon.
        /// </summary>
        [EditorOrder(310)]
        public SpriteHandle Rotate;

        /// <summary>
        /// The scale icon.
        /// </summary>
        [EditorOrder(320)]
        public SpriteHandle Scale;

        /// <summary>
        /// The scalar icon.
        /// </summary>
        [EditorOrder(330)]
        public SpriteHandle Scalar;

        /// <summary>
        /// The shared tooltip control used by the controls if no custom tooltip is provided.
        /// </summary>
        [EditorOrder(340)]
        public Tooltip SharedTooltip;

        /// <summary>
        /// Style for the Statusbar
        /// </summary>
        [System.Serializable, ShowInEditor]
        public struct StatusbarStyle
        {
            /// <summary>
            /// Color of the Statusbar when in Play Mode
            /// </summary>
            public Color PlayMode;

            /// <summary>
            /// Color of the Statusbar when in loading state (e.g. when importing assets)
            /// </summary>
            public Color Loading;

            /// <summary>
            /// Color of the Statusbar in its failed state (e.g. with compilation errors)
            /// </summary>
            public Color Failed;
        }
    }

    /// <summary>
    /// Rounded rectangle corner mask.
    /// </summary>
    [System.Flags]
    public enum RoundedCorners
    {
        /// <summary>
        /// No rounded corners.
        /// </summary>
        None = 0,

        /// <summary>
        /// The top-left corner.
        /// </summary>
        TopLeft = 1,

        /// <summary>
        /// The top-right corner.
        /// </summary>
        TopRight = 2,

        /// <summary>
        /// The bottom-left corner.
        /// </summary>
        BottomLeft = 4,

        /// <summary>
        /// The bottom-right corner.
        /// </summary>
        BottomRight = 8,

        /// <summary>
        /// The top-left and top-right corners.
        /// </summary>
        Top = TopLeft | TopRight,

        /// <summary>
        /// The bottom-left and bottom-right corners.
        /// </summary>
        Bottom = BottomLeft | BottomRight,

        /// <summary>
        /// All corners.
        /// </summary>
        All = Top | Bottom,
    }

    /// <summary>
    /// Shared rendering helpers for style-aware GUI chrome.
    /// </summary>
    public static class StyleRendering
    {
        /// <summary>
        /// Draws a filled rectangle with compact rounded corners.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="color">The fill color.</param>
        /// <param name="radius">The corner radius.</param>
        public static void FillRoundedRectangle(Rectangle bounds, Color color, float radius)
        {
            FillRoundedRectangle(bounds, color, radius, RoundedCorners.All);
        }

        /// <summary>
        /// Draws a filled rectangle with compact rounded corners.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="color">The fill color.</param>
        /// <param name="radius">The corner radius.</param>
        /// <param name="corners">The corners to round.</param>
        public static void FillRoundedRectangle(Rectangle bounds, Color color, float radius, RoundedCorners corners)
        {
            radius = Mathf.Min(radius, Mathf.Min(bounds.Width, bounds.Height) * 0.5f);
            if (radius < 1.0f || corners == RoundedCorners.None)
            {
                Render2D.FillRectangle(bounds, color);
                return;
            }

            var steps = Mathf.CeilToInt(radius);
            var topSteps = (corners & RoundedCorners.Top) != 0 ? steps : 0;
            var bottomSteps = (corners & RoundedCorners.Bottom) != 0 ? steps : 0;
            var middleHeight = bounds.Height - topSteps - bottomSteps;

            if (middleHeight > 0.0f)
                Render2D.FillRectangle(new Rectangle(bounds.X, bounds.Y + topSteps, bounds.Width, middleHeight), color);

            for (int i = 0; i < topSteps; i++)
            {
                var inset = GetCornerInset(radius, i);
                var leftInset = (corners & RoundedCorners.TopLeft) != 0 ? inset : 0.0f;
                var rightInset = (corners & RoundedCorners.TopRight) != 0 ? inset : 0.0f;
                Render2D.FillRectangle(new Rectangle(bounds.X + leftInset, bounds.Y + i, Mathf.Max(0.0f, bounds.Width - leftInset - rightInset), 1.0f), color);
            }

            for (int i = 0; i < bottomSteps; i++)
            {
                var inset = GetCornerInset(radius, i);
                var leftInset = (corners & RoundedCorners.BottomLeft) != 0 ? inset : 0.0f;
                var rightInset = (corners & RoundedCorners.BottomRight) != 0 ? inset : 0.0f;
                Render2D.FillRectangle(new Rectangle(bounds.X + leftInset, bounds.Bottom - i - 1.0f, Mathf.Max(0.0f, bounds.Width - leftInset - rightInset), 1.0f), color);
            }
        }

        private static float GetCornerInset(float radius, int row)
        {
            var dy = radius - row - 0.5f;
            return radius - Mathf.Sqrt(Mathf.Max(0.0f, radius * radius - dy * dy));
        }

        /// <summary>
        /// Draws a filled rounded rectangle with an inset border.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="fillColor">The fill color.</param>
        /// <param name="borderColor">The border color.</param>
        /// <param name="borderThickness">The border thickness.</param>
        /// <param name="radius">The corner radius.</param>
        public static void DrawRoundedRectangle(Rectangle bounds, Color fillColor, Color borderColor, float borderThickness, float radius)
        {
            DrawRoundedRectangle(bounds, fillColor, borderColor, borderThickness, radius, RoundedCorners.All);
        }

        /// <summary>
        /// Draws a filled rounded rectangle with an inset border.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="fillColor">The fill color.</param>
        /// <param name="borderColor">The border color.</param>
        /// <param name="borderThickness">The border thickness.</param>
        /// <param name="radius">The corner radius.</param>
        /// <param name="corners">The corners to round.</param>
        public static void DrawRoundedRectangle(Rectangle bounds, Color fillColor, Color borderColor, float borderThickness, float radius, RoundedCorners corners)
        {
            if (borderThickness <= 0.0f)
            {
                FillRoundedRectangle(bounds, fillColor, radius, corners);
                return;
            }

            FillRoundedRectangle(bounds, borderColor, radius, corners);
            var inner = bounds.MakeExpanded(-borderThickness);
            if (inner.Width > 0.0f && inner.Height > 0.0f)
                FillRoundedRectangle(inner, fillColor, Mathf.Max(0.0f, radius - borderThickness), corners);
        }

        /// <summary>
        /// Draws a rounded rectangle border without filling the body.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="color">The border color.</param>
        /// <param name="thickness">The border thickness.</param>
        /// <param name="radius">The corner radius.</param>
        public static void DrawRoundedRectangleBorder(Rectangle bounds, Color color, float thickness, float radius)
        {
            DrawRoundedRectangleBorder(bounds, color, thickness, radius, RoundedCorners.All);
        }

        /// <summary>
        /// Draws a rounded rectangle border without filling the body.
        /// </summary>
        /// <param name="bounds">The rectangle bounds.</param>
        /// <param name="color">The border color.</param>
        /// <param name="thickness">The border thickness.</param>
        /// <param name="radius">The corner radius.</param>
        /// <param name="corners">The corners to round.</param>
        public static void DrawRoundedRectangleBorder(Rectangle bounds, Color color, float thickness, float radius, RoundedCorners corners)
        {
            if (color.A <= 0.0f || thickness <= 0.0f)
                return;

            radius = Mathf.Min(radius, Mathf.Min(bounds.Width, bounds.Height) * 0.5f);
            if (radius < 1.0f || corners == RoundedCorners.None)
            {
                Render2D.DrawRectangle(bounds, color, thickness);
                return;
            }

            var halfThickness = thickness * 0.5f;
            var left = bounds.X + halfThickness;
            var top = bounds.Y + halfThickness;
            var right = bounds.Right - halfThickness;
            var bottom = bounds.Bottom - halfThickness;
            var r = Mathf.Max(0.0f, radius - halfThickness);
            var k = r * 0.55228475f;

            var roundTopLeft = (corners & RoundedCorners.TopLeft) != 0;
            var roundTopRight = (corners & RoundedCorners.TopRight) != 0;
            var roundBottomLeft = (corners & RoundedCorners.BottomLeft) != 0;
            var roundBottomRight = (corners & RoundedCorners.BottomRight) != 0;

            Render2D.DrawLine(new Float2(left + (roundTopLeft ? r : 0.0f), top), new Float2(right - (roundTopRight ? r : 0.0f), top), color, thickness);
            Render2D.DrawLine(new Float2(right, top + (roundTopRight ? r : 0.0f)), new Float2(right, bottom - (roundBottomRight ? r : 0.0f)), color, thickness);
            Render2D.DrawLine(new Float2(right - (roundBottomRight ? r : 0.0f), bottom), new Float2(left + (roundBottomLeft ? r : 0.0f), bottom), color, thickness);
            Render2D.DrawLine(new Float2(left, bottom - (roundBottomLeft ? r : 0.0f)), new Float2(left, top + (roundTopLeft ? r : 0.0f)), color, thickness);

            if (roundTopLeft)
                Render2D.DrawBezier(new Float2(left + r, top), new Float2(left + r - k, top), new Float2(left, top + r - k), new Float2(left, top + r), color, thickness);
            if (roundTopRight)
                Render2D.DrawBezier(new Float2(right - r, top), new Float2(right - r + k, top), new Float2(right, top + r - k), new Float2(right, top + r), color, thickness);
            if (roundBottomRight)
                Render2D.DrawBezier(new Float2(right, bottom - r), new Float2(right, bottom - r + k), new Float2(right - r + k, bottom), new Float2(right - r, bottom), color, thickness);
            if (roundBottomLeft)
                Render2D.DrawBezier(new Float2(left + r, bottom), new Float2(left + r - k, bottom), new Float2(left, bottom - r + k), new Float2(left, bottom - r), color, thickness);
        }

        /// <summary>
        /// Draws a compact square checkbox fill with a subtle bottom value shift.
        /// </summary>
        /// <param name="bounds">The checkbox bounds.</param>
        /// <param name="color">The fill color.</param>
        public static void FillCheckBox(Rectangle bounds, Color color)
        {
            Render2D.FillRectangle(bounds, color);

            var shadow = color.ToHSV();
            shadow.Z = Mathf.Saturate(shadow.Z - 0.03f);
            Render2D.FillRectangle(new Rectangle(bounds.X, bounds.Bottom - 1.0f, bounds.Width, 1.0f), Color.FromHSV(shadow, color.A));
        }
    }
}
