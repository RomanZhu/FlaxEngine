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
            radius = Mathf.Min(radius, Mathf.Min(bounds.Width, bounds.Height) * 0.5f);
            if (radius < 1.0f)
            {
                Render2D.FillRectangle(bounds, color);
                return;
            }

            var steps = Mathf.CeilToInt(radius);
            Render2D.FillRectangle(new Rectangle(bounds.X + radius, bounds.Y, bounds.Width - radius * 2.0f, bounds.Height), color);
            Render2D.FillRectangle(new Rectangle(bounds.X, bounds.Y + radius, bounds.Width, bounds.Height - radius * 2.0f), color);
            for (int i = 0; i < steps; i++)
            {
                var dy = radius - i - 0.5f;
                var inset = radius - Mathf.Sqrt(Mathf.Max(0.0f, radius * radius - dy * dy));
                var width = Mathf.Max(0.0f, bounds.Width - inset * 2.0f);
                Render2D.FillRectangle(new Rectangle(bounds.X + inset, bounds.Y + i, width, 1.0f), color);
                Render2D.FillRectangle(new Rectangle(bounds.X + inset, bounds.Bottom - i - 1.0f, width, 1.0f), color);
            }
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
            if (borderThickness <= 0.0f)
            {
                FillRoundedRectangle(bounds, fillColor, radius);
                return;
            }

            FillRoundedRectangle(bounds, borderColor, radius);
            var inner = bounds.MakeExpanded(-borderThickness);
            if (inner.Width > 0.0f && inner.Height > 0.0f)
                FillRoundedRectangle(inner, fillColor, Mathf.Max(0.0f, radius - borderThickness));
        }
    }
}
