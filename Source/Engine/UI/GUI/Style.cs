// Copyright (c) Wojciech Figat. All rights reserved.

using System.IO;

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
        /// The text shadow color. Transparent color disables shadow drawing.
        /// </summary>
        [EditorOrder(116)]
        public Color TextShadowColor;

        /// <summary>
        /// The text shadow offset. Set to zero to disable shadow drawing.
        /// </summary>
        [EditorOrder(117)]
        public Float2 TextShadowOffset;

        /// <summary>
        /// Gets a value indicating whether text shadow drawing is enabled.
        /// </summary>
        [HideInEditor]
        public bool HasTextShadow => TextShadowColor.A > 0.0f && !TextShadowOffset.IsZero;

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
        /// The fallback corner radius for style-aware chrome. Category-specific radius values inherit this when set to zero.
        /// </summary>
        [EditorOrder(207)]
        public float CornerRadius;

        /// <summary>
        /// The preferred corner radius for regular buttons. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(208)]
        public float ButtonCornerRadius;

        /// <summary>
        /// The preferred corner radius for text boxes, combo boxes, and dropdown fields. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(209)]
        public float InputCornerRadius;

        /// <summary>
        /// The preferred left-side corner radius for inline value-box prefixes. A value of zero inherits <see cref="InputCornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(210)]
        public float ValueBoxPrefixCornerRadius;

        /// <summary>
        /// The horizontal offset for inline value-box prefixes. Negative values move the prefix segment to the left.
        /// </summary>
        [EditorOrder(211)]
        public float ValueBoxPrefixOffset;

        /// <summary>
        /// The preferred corner radius for panels and framed containers. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(210)]
        public float PanelCornerRadius;

        /// <summary>
        /// The preferred corner radius for popups and context menus. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(211)]
        public float PopupCornerRadius;

        /// <summary>
        /// The preferred corner radius for combo boxes and dropdown fields. A value of zero inherits <see cref="InputCornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(212)]
        public float DropdownCornerRadius;

        /// <summary>
        /// The preferred corner radius for individual toolbar buttons. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(212)]
        public float ToolStripButtonCornerRadius;

        /// <summary>
        /// The preferred corner radius for toolbar item groups. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(213)]
        public float ToolStripGroupCornerRadius;

        /// <summary>
        /// The preferred corner radius for document and window tabs. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(214)]
        public float TabCornerRadius;

        /// <summary>
        /// The preferred corner radius for selected rows, list items, and hover highlights. A value of zero inherits <see cref="CornerRadius"/>. A negative value preserves square corners.
        /// </summary>
        [EditorOrder(215)]
        public float SelectionCornerRadius;

        /// <summary>
        /// The preferred height for compact interactive controls. A value of zero lets controls use their legacy size.
        /// </summary>
        [EditorOrder(220)]
        public float ControlHeight;

        /// <summary>
        /// The preferred height of the global editor toolbar.
        /// </summary>
        [EditorOrder(221)]
        public float ToolbarHeight;

        /// <summary>
        /// The preferred height of window and document tabs.
        /// </summary>
        [EditorOrder(222)]
        public float TabHeight;

        /// <summary>
        /// The preferred height of compact hierarchy rows.
        /// </summary>
        [EditorOrder(223)]
        public float TreeRowHeight;

        /// <summary>
        /// The preferred height of property editor rows.
        /// </summary>
        [EditorOrder(224)]
        public float PropertyRowHeight;

        /// <summary>
        /// The preferred outer padding for property editor panels. A value of zero inherits <see cref="PanelPadding"/>.
        /// </summary>
        [EditorOrder(225)]
        public float PropertyPanelPadding;

        /// <summary>
        /// The preferred spacing between top-level property editor panels. A value of zero inherits <see cref="PropertyPanelPadding"/>.
        /// </summary>
        [EditorOrder(226)]
        public float PropertyPanelSpacing;

        /// <summary>
        /// The preferred inner padding for property groups. A value of zero inherits <see cref="PropertyPanelPadding"/>.
        /// </summary>
        [EditorOrder(227)]
        public float PropertyGroupPadding;

        /// <summary>
        /// The preferred spacing between controls inside property groups. A value of zero inherits <see cref="PropertyPanelSpacing"/>.
        /// </summary>
        [EditorOrder(228)]
        public float PropertyGroupSpacing;

        /// <summary>
        /// The preferred slot spacing for property editor grids. A value of zero inherits <see cref="PropertyPanelSpacing"/>.
        /// </summary>
        [EditorOrder(229)]
        public float PropertyGridSpacing;

        /// <summary>
        /// The preferred inset used inside editor panels.
        /// </summary>
        [EditorOrder(230)]
        public float PanelPadding;

        /// <summary>
        /// The maximum size of standard interface glyphs. Content previews and viewport gizmos are excluded.
        /// </summary>
        [EditorOrder(231)]
        public float IconSize;

        /// <summary>
        /// The preferred size of regular button icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(232)]
        public float ButtonIconSize;

        /// <summary>
        /// The preferred size of toolstrip button icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(233)]
        public float ToolStripIconSize;

        /// <summary>
        /// The preferred size of context menu icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(234)]
        public float MenuIconSize;

        /// <summary>
        /// The preferred size of tab icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(235)]
        public float TabIconSize;

        /// <summary>
        /// The preferred size of generic tree row icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(236)]
        public float TreeIconSize;

        /// <summary>
        /// The preferred size of content tree row icons. A value of zero inherits <see cref="TreeIconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(237)]
        public float ContentTreeIconSize;

        /// <summary>
        /// The preferred size of scene tree row icons. A value of zero inherits <see cref="TreeIconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(238)]
        public float SceneTreeIconSize;

        /// <summary>
        /// The preferred size of property group header icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(239)]
        public float PropertyIconSize;

        /// <summary>
        /// The preferred size of timeline row icons. A value of zero inherits <see cref="IconSize"/>. A negative value hides this icon group.
        /// </summary>
        [EditorOrder(240)]
        public float TimelineIconSize;

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
        /// Gets the resolved button corner radius.
        /// </summary>
        public float GetButtonCornerRadius()
        {
            return ResolveCornerRadius(ButtonCornerRadius);
        }

        /// <summary>
        /// Gets the resolved input field corner radius.
        /// </summary>
        public float GetInputCornerRadius()
        {
            return ResolveCornerRadius(InputCornerRadius);
        }

        /// <summary>
        /// Gets the resolved value-box prefix corner radius.
        /// </summary>
        public float GetValueBoxPrefixCornerRadius()
        {
            if (ValueBoxPrefixCornerRadius < 0.0f)
                return 0.0f;
            return ValueBoxPrefixCornerRadius > 0.0f ? ValueBoxPrefixCornerRadius : GetInputCornerRadius();
        }

        /// <summary>
        /// Gets the resolved value-box prefix horizontal offset.
        /// </summary>
        public float GetValueBoxPrefixOffset()
        {
            return ValueBoxPrefixOffset;
        }

        /// <summary>
        /// Gets the resolved panel corner radius.
        /// </summary>
        public float GetPanelCornerRadius()
        {
            return ResolveCornerRadius(PanelCornerRadius);
        }

        /// <summary>
        /// Gets the resolved popup corner radius.
        /// </summary>
        public float GetPopupCornerRadius()
        {
            return ResolveCornerRadius(PopupCornerRadius);
        }

        /// <summary>
        /// Gets the resolved combo box and dropdown field corner radius.
        /// </summary>
        public float GetDropdownCornerRadius()
        {
            if (DropdownCornerRadius < 0.0f)
                return 0.0f;
            return DropdownCornerRadius > 0.0f ? DropdownCornerRadius : GetInputCornerRadius();
        }

        /// <summary>
        /// Gets the resolved toolbar button corner radius.
        /// </summary>
        public float GetToolStripButtonCornerRadius()
        {
            return ResolveCornerRadius(ToolStripButtonCornerRadius);
        }

        /// <summary>
        /// Gets the resolved toolbar group corner radius.
        /// </summary>
        public float GetToolStripGroupCornerRadius()
        {
            return ResolveCornerRadius(ToolStripGroupCornerRadius);
        }

        /// <summary>
        /// Gets the resolved tab corner radius.
        /// </summary>
        public float GetTabCornerRadius()
        {
            return ResolveCornerRadius(TabCornerRadius);
        }

        /// <summary>
        /// Gets the resolved selection and hover highlight corner radius.
        /// </summary>
        public float GetSelectionCornerRadius()
        {
            return ResolveCornerRadius(SelectionCornerRadius);
        }

        private float ResolveCornerRadius(float radius)
        {
            if (radius < 0.0f)
                return 0.0f;
            return radius > 0.0f ? radius : CornerRadius;
        }

        /// <summary>
        /// Gets the resolved regular button icon size.
        /// </summary>
        public float GetButtonIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(ButtonIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved toolstrip button icon size.
        /// </summary>
        public float GetToolStripIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(ToolStripIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved context menu icon size.
        /// </summary>
        public float GetMenuIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(MenuIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved tab icon size.
        /// </summary>
        public float GetTabIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(TabIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved generic tree row icon size.
        /// </summary>
        public float GetTreeIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(TreeIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved content tree row icon size.
        /// </summary>
        public float GetContentTreeIconSize(float fallback = 16.0f)
        {
            if (ContentTreeIconSize < 0.0f)
                return 0.0f;
            return ContentTreeIconSize > 0.0f ? ContentTreeIconSize : GetTreeIconSize(fallback);
        }

        /// <summary>
        /// Gets the resolved scene tree row icon size.
        /// </summary>
        public float GetSceneTreeIconSize(float fallback = 16.0f)
        {
            if (SceneTreeIconSize < 0.0f)
                return 0.0f;
            return SceneTreeIconSize > 0.0f ? SceneTreeIconSize : GetTreeIconSize(fallback);
        }

        /// <summary>
        /// Gets the resolved property group header icon size.
        /// </summary>
        public float GetPropertyIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(PropertyIconSize, fallback);
        }

        /// <summary>
        /// Gets the resolved timeline row icon size.
        /// </summary>
        public float GetTimelineIconSize(float fallback = 16.0f)
        {
            return ResolveIconSize(TimelineIconSize, fallback);
        }

        private float ResolveIconSize(float iconSize, float fallback)
        {
            if (iconSize < 0.0f)
                return 0.0f;
            if (iconSize > 0.0f)
                return iconSize;
            return IconSize > 0.0f ? IconSize : fallback;
        }

        /// <summary>
        /// Gets the resolved outer padding for property editor panels.
        /// </summary>
        public float GetPropertyPanelPadding(float fallback = 2.0f)
        {
            return ResolveLayoutSize(PropertyPanelPadding, PanelPadding > 0.0f ? PanelPadding : fallback);
        }

        /// <summary>
        /// Gets the resolved spacing between top-level property editor panels.
        /// </summary>
        public float GetPropertyPanelSpacing(float fallback = 2.0f)
        {
            return ResolveLayoutSize(PropertyPanelSpacing, GetPropertyPanelPadding(fallback));
        }

        /// <summary>
        /// Gets the resolved inner padding for property groups.
        /// </summary>
        public float GetPropertyGroupPadding(float fallback = 2.0f)
        {
            return ResolveLayoutSize(PropertyGroupPadding, GetPropertyPanelPadding(fallback));
        }

        /// <summary>
        /// Gets the resolved spacing between controls inside property groups.
        /// </summary>
        public float GetPropertyGroupSpacing(float fallback = 2.0f)
        {
            return ResolveLayoutSize(PropertyGroupSpacing, GetPropertyPanelSpacing(fallback));
        }

        /// <summary>
        /// Gets the resolved slot spacing for property editor grids.
        /// </summary>
        public float GetPropertyGridSpacing(float fallback = 2.0f)
        {
            return ResolveLayoutSize(PropertyGridSpacing, GetPropertyPanelSpacing(fallback));
        }

        private static float ResolveLayoutSize(float value, float fallback)
        {
            return value > 0.0f ? value : Mathf.Max(0.0f, fallback);
        }

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
        /// The top-left and bottom-left corners.
        /// </summary>
        Left = TopLeft | BottomLeft,

        /// <summary>
        /// The top-right and bottom-right corners.
        /// </summary>
        Right = TopRight | BottomRight,

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
        private const int RoundedRectangleMaskSize = 64;
        private const float RoundedRectangleMaskAssetScale = 2.0f;
        private const float RoundedRectangleMaskRadiusTolerance = 1.25f;
        private const float RoundedRectangleBorderThicknessTolerance = 0.01f;
        private const float RoundedRectangleSquareMaskSlice = 1.0f;
        private static Texture _roundedRectangleSquareFillTexture;

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
            if (TryDrawMaskFill(bounds, color, radius, corners))
                return;

            FillRoundedRectangleFallback(bounds, color, radius, corners);
        }

        private static void FillRoundedRectangleFallback(Rectangle bounds, Color color, float radius, RoundedCorners corners)
        {
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
            radius = Mathf.Min(radius, Mathf.Min(bounds.Width, bounds.Height) * 0.5f);
            if (borderThickness <= 0.0f)
            {
                FillRoundedRectangle(bounds, fillColor, radius, corners);
                return;
            }

            if (TryDrawMaskRectangle(bounds, fillColor, borderColor, borderThickness, radius, corners))
                return;

            FillRoundedRectangleFallback(bounds, borderColor, radius, corners);
            var inner = bounds.MakeExpanded(-borderThickness);
            if (inner.Width > 0.0f && inner.Height > 0.0f)
                FillRoundedRectangleFallback(inner, fillColor, Mathf.Max(0.0f, radius - borderThickness), corners);
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
            if (TryDrawMaskBorder(bounds, color, thickness, radius, corners))
                return;

            DrawRoundedRectangleBorderFallback(bounds, color, thickness, radius, corners);
        }

        private static void DrawRoundedRectangleBorderFallback(Rectangle bounds, Color color, float thickness, float radius, RoundedCorners corners)
        {
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

        private static bool TryDrawMaskFill(Rectangle bounds, Color color, float radius, RoundedCorners corners)
        {
            if (bounds.Width <= 0.0f || bounds.Height <= 0.0f || color.A <= 0.0f)
                return true;

            if (corners == RoundedCorners.None || radius < 1.0f)
            {
                var squareTexture = GetSquareFillTexture();
                if (squareTexture == null)
                    return false;

                DrawMaskTexture(squareTexture, bounds, RoundedRectangleSquareMaskSlice, RoundedRectangleSquareMaskSlice * RoundedRectangleMaskAssetScale, color);
                return true;
            }

            if (TryDrawClippedMaskFill(bounds, color, radius, corners))
                return true;

            var roundedMask = FindRoundedMask(radius, corners);
            var roundedTexture = roundedMask?.GetFillTexture();
            if (roundedTexture == null)
                return false;

            DrawMaskTexture(roundedTexture, bounds, roundedMask.Slice, roundedMask.SourceSlice, color);
            return true;
        }

        private static bool TryDrawClippedMaskFill(Rectangle bounds, Color color, float radius, RoundedCorners corners)
        {
            if (corners == RoundedCorners.All)
                return false;

            var roundedMask = FindRoundedMask(radius, RoundedCorners.All);
            var roundedTexture = roundedMask?.GetFillTexture();
            if (roundedTexture == null)
                return false;

            var drawBounds = bounds;
            var overflow = roundedMask.Slice;
            switch (corners)
            {
            case RoundedCorners.Top:
                drawBounds.Height += overflow;
                break;
            case RoundedCorners.Bottom:
                drawBounds.Y -= overflow;
                drawBounds.Height += overflow;
                break;
            case RoundedCorners.Left:
                drawBounds.Width += overflow;
                break;
            case RoundedCorners.Right:
                drawBounds.X -= overflow;
                drawBounds.Width += overflow;
                break;
            default:
                return false;
            }

            Render2D.PushClip(ref bounds);
            DrawMaskTexture(roundedTexture, drawBounds, roundedMask.Slice, roundedMask.SourceSlice, color);
            Render2D.PopClip();
            return true;
        }

        private static bool TryDrawMaskRectangle(Rectangle bounds, Color fillColor, Color borderColor, float borderThickness, float radius, RoundedCorners corners)
        {
            if (bounds.Width <= 0.0f || bounds.Height <= 0.0f)
                return true;

            if (!CanUseRoundedBorderMask(borderThickness))
                return false;

            if (borderColor.A <= 0.0f)
                return TryDrawMaskFill(bounds, fillColor, radius, corners);

            var roundedMask = FindRoundedMask(radius, corners);
            if (roundedMask == null)
                return false;

            var fillTexture = fillColor.A > 0.0f ? roundedMask.GetFillTexture() : null;
            var borderTexture = roundedMask.GetBorderTexture();
            if (borderTexture == null || (fillColor.A > 0.0f && fillTexture == null))
                return false;

            if (fillTexture != null)
                DrawMaskTexture(fillTexture, bounds, roundedMask.Slice, roundedMask.SourceSlice, fillColor);
            DrawMaskTexture(borderTexture, bounds, roundedMask.Slice, roundedMask.SourceSlice, borderColor);
            return true;
        }

        private static bool TryDrawMaskBorder(Rectangle bounds, Color color, float thickness, float radius, RoundedCorners corners)
        {
            if (bounds.Width <= 0.0f || bounds.Height <= 0.0f)
                return true;
            if (!CanUseRoundedBorderMask(thickness))
                return false;

            var roundedMask = FindRoundedMask(radius, corners);
            var borderTexture = roundedMask?.GetBorderTexture();
            if (borderTexture == null)
                return false;

            DrawMaskTexture(borderTexture, bounds, roundedMask.Slice, roundedMask.SourceSlice, color);
            return true;
        }

        private static bool CanUseRoundedBorderMask(float thickness)
        {
            return Mathf.Abs(thickness - 1.0f) <= RoundedRectangleBorderThicknessTolerance;
        }

        private static RoundedRectangleMaskInfo FindRoundedMask(float radius, RoundedCorners corners)
        {
            if (corners != RoundedCorners.All || radius < 1.0f || RoundedRectangleMasks.Length == 0)
                return null;

            RoundedRectangleMaskInfo best = null;
            var bestDelta = float.MaxValue;
            for (int i = 0; i < RoundedRectangleMasks.Length; i++)
            {
                var mask = RoundedRectangleMasks[i];
                var delta = Mathf.Abs(mask.Radius - radius);
                if (delta < bestDelta)
                {
                    best = mask;
                    bestDelta = delta;
                }
            }

            return bestDelta <= RoundedRectangleMaskRadiusTolerance ? best : null;
        }

        private static void DrawMaskTexture(TextureBase texture, Rectangle bounds, float destinationSlice, float sourceSlice, Color color)
        {
            var horizontalSlice = Mathf.Min(destinationSlice, bounds.Width * 0.5f);
            var verticalSlice = Mathf.Min(destinationSlice, bounds.Height * 0.5f);
            var border = new Float4(horizontalSlice, horizontalSlice, verticalSlice, verticalSlice);
            var uv = sourceSlice / RoundedRectangleMaskSize;
            var borderUV = new Float4(uv, uv, uv, uv);
            Render2D.Draw9SlicingTexture(texture, bounds, border, borderUV, color);
        }

        private sealed class RoundedRectangleMaskInfo
        {
            public readonly float Radius;
            public readonly float Slice;
            public readonly float SourceSlice;

            private readonly ushort[] _fillRle;
            private readonly ushort[] _borderRle;
            private readonly string _fillFileName;
            private readonly string _borderFileName;
            private Texture _fillTexture;
            private Texture _borderTexture;

            public RoundedRectangleMaskInfo(float radius, float slice, ushort[] fillRle, ushort[] borderRle)
            {
                Radius = radius;
                Slice = slice;
                SourceSlice = slice * RoundedRectangleMaskAssetScale;
                _fillRle = fillRle;
                _borderRle = borderRle;
                _fillFileName = $"RoundedFill_R{(int)radius}.png";
                _borderFileName = $"RoundedBorder_R{(int)radius}_T1.png";
            }

            public Texture GetFillTexture()
            {
                return GetMaskTexture(ref _fillTexture, _fillFileName, _fillRle);
            }

            public Texture GetBorderTexture()
            {
                return GetMaskTexture(ref _borderTexture, _borderFileName, _borderRle);
            }
        }

        private static Texture GetSquareFillTexture()
        {
            return GetMaskTexture(ref _roundedRectangleSquareFillTexture, "SquareFill.png", RoundedRectangleSquareFillRle, "SquareFill_R5.png");
        }

        private static Texture GetMaskTexture(ref Texture texture, string fileName, ushort[] fallbackRle, string legacyFileName = null)
        {
            if (texture)
                return texture;

            texture = LoadExternalTexture(fileName);
            if (!texture && !string.IsNullOrEmpty(legacyFileName))
                texture = LoadExternalTexture(legacyFileName);
            if (texture)
                return texture;

            texture = CreateTextureFromRle(fallbackRle);
            return texture;
        }

        private static Texture LoadExternalTexture(string fileName)
        {
            var startupFolder = Globals.StartupFolder;
            if (string.IsNullOrEmpty(startupFolder))
                return null;

            var path = Path.Combine(startupFolder, "Docs", fileName);
            if (!File.Exists(path))
            {
                path = Path.Combine(startupFolder, "docs", fileName);
                if (!File.Exists(path))
                    return null;
            }

            try
            {
                return Texture.FromFile(path);
            }
            catch
            {
                return null;
            }
        }

        private static Texture CreateTextureFromRle(ushort[] rle)
        {
            if (rle == null)
                return null;

            var data = DecodeRleMask(rle);
            var texture = Content.CreateVirtualAsset<Texture>();
            var initData = new TextureBase.InitData
            {
                Format = PixelFormat.R8G8B8A8_UNorm,
                Width = RoundedRectangleMaskSize,
                Height = RoundedRectangleMaskSize,
                ArraySize = 1,
                GenerateMips = false,
                GenerateMipsLinear = false,
                Mips = new[]
                {
                    new TextureBase.InitData.MipData
                    {
                        Data = data,
                        RowPitch = RoundedRectangleMaskSize * 4,
                        SlicePitch = RoundedRectangleMaskSize * RoundedRectangleMaskSize * 4,
                    }
                },
            };
            texture.Init(ref initData);
            return texture;
        }

        private static byte[] DecodeRleMask(ushort[] rle)
        {
            var data = new byte[RoundedRectangleMaskSize * RoundedRectangleMaskSize * 4];
            var pixel = 0;
            for (int i = 0; i < rle.Length; i += 2)
            {
                var runLength = rle[i];
                var alpha = (byte)rle[i + 1];
                for (int j = 0; j < runLength; j++)
                {
                    var offset = pixel++ * 4;
                    data[offset] = 255;
                    data[offset + 1] = 255;
                    data[offset + 2] = 255;
                    data[offset + 3] = alpha;
                }
            }
            return data;
        }

        // BEGIN ROUNDED RECTANGLE MASK DATA
        private static readonly ushort[] RoundedRectangleSquareFillRle =
        {
            4096, 255,
        };

        private static readonly ushort[] RoundedRectangleFillR1Rle =
        {
            1, 0, 1, 32, 1, 191, 1, 239, 57, 255, 1, 175, 1, 32, 1, 0, 1, 32,
            1, 239, 60, 255, 1, 239, 1, 32, 1, 175, 62, 255, 1, 191, 63, 255, 1, 239,
            3584, 255, 1, 239, 63, 255, 1, 191, 62, 255, 1, 175, 1, 32, 1, 239, 60, 255,
            1, 239, 1, 32, 1, 0, 1, 32, 1, 175, 57, 255, 1, 239, 1, 191, 1, 32,
            1, 0,
        };
        private static readonly ushort[] RoundedRectangleBorderR1T1Rle =
        {
            1, 0, 1, 32, 1, 191, 1, 239, 57, 255, 1, 175, 1, 32, 1, 0, 1, 32,
            1, 239, 60, 255, 1, 239, 1, 32, 1, 175, 1, 255, 1, 159, 1, 16, 56, 0,
            1, 32, 1, 160, 1, 255, 1, 191, 2, 255, 1, 32, 58, 0, 1, 16, 1, 255,
            1, 239, 2, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 2, 255, 1, 239, 1, 255, 1, 16,
            58, 0, 1, 32, 2, 255, 1, 191, 1, 255, 1, 159, 1, 32, 56, 0, 1, 16,
            1, 160, 1, 255, 1, 175, 1, 32, 1, 239, 60, 255, 1, 239, 1, 32, 1, 0,
            1, 32, 1, 175, 57, 255, 1, 239, 1, 191, 1, 32, 1, 0,
        };

        private static readonly ushort[] RoundedRectangleFillR2Rle =
        {
            2, 0, 1, 16, 1, 128, 1, 191, 54, 255, 1, 207, 1, 95, 4, 0, 1, 32,
            1, 223, 58, 255, 1, 223, 1, 32, 2, 0, 1, 223, 60, 255, 1, 223, 1, 16,
            1, 96, 62, 255, 1, 127, 1, 207, 62, 255, 1, 191, 3456, 255, 1, 191, 62, 255,
            1, 207, 1, 127, 62, 255, 1, 95, 1, 16, 1, 223, 60, 255, 1, 223, 2, 0,
            1, 32, 1, 223, 58, 255, 1, 223, 1, 32, 4, 0, 1, 96, 1, 207, 54, 255,
            1, 191, 1, 127, 1, 16, 2, 0,
        };
        private static readonly ushort[] RoundedRectangleBorderR2T1Rle =
        {
            2, 0, 1, 16, 1, 128, 1, 191, 54, 255, 1, 207, 1, 95, 4, 0, 1, 32,
            1, 223, 58, 255, 1, 223, 1, 32, 2, 0, 1, 223, 1, 255, 1, 223, 1, 63,
            1, 16, 53, 0, 1, 79, 1, 223, 1, 255, 1, 223, 1, 16, 1, 96, 1, 255,
            1, 223, 1, 16, 56, 0, 1, 16, 1, 223, 1, 255, 1, 127, 1, 207, 1, 255,
            1, 79, 58, 0, 1, 64, 1, 255, 1, 191, 2, 255, 59, 0, 1, 16, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 1, 16, 59, 0, 2, 255, 1, 191,
            1, 255, 1, 64, 58, 0, 1, 80, 1, 255, 1, 207, 1, 127, 1, 255, 1, 223,
            1, 16, 56, 0, 1, 16, 1, 223, 1, 255, 1, 95, 1, 16, 1, 223, 1, 255,
            1, 223, 1, 80, 53, 0, 1, 16, 1, 64, 1, 223, 1, 255, 1, 223, 2, 0,
            1, 32, 1, 223, 58, 255, 1, 223, 1, 32, 4, 0, 1, 96, 1, 207, 54, 255,
            1, 191, 1, 127, 1, 16, 2, 0,
        };

        private static readonly ushort[] RoundedRectangleFillR3Rle =
        {
            4, 0, 1, 64, 1, 143, 1, 223, 50, 255, 1, 207, 1, 159, 1, 32, 6, 0,
            1, 16, 1, 144, 56, 255, 1, 143, 1, 16, 3, 0, 1, 16, 1, 207, 58, 255,
            1, 207, 1, 16, 2, 0, 1, 144, 60, 255, 1, 143, 1, 0, 1, 32, 62, 255,
            1, 64, 1, 159, 62, 255, 1, 143, 1, 207, 62, 255, 1, 223, 3200, 255, 1, 223,
            62, 255, 1, 207, 1, 143, 62, 255, 1, 159, 1, 64, 62, 255, 1, 32, 1, 0,
            1, 143, 60, 255, 1, 143, 2, 0, 1, 16, 1, 207, 58, 255, 1, 207, 1, 16,
            3, 0, 1, 16, 1, 143, 56, 255, 1, 143, 1, 16, 6, 0, 1, 32, 1, 159,
            1, 207, 50, 255, 1, 223, 1, 143, 1, 63, 4, 0,
        };
        private static readonly ushort[] RoundedRectangleBorderR3T1Rle =
        {
            4, 0, 1, 64, 1, 143, 1, 223, 50, 255, 1, 207, 1, 159, 1, 32, 6, 0,
            1, 16, 1, 144, 56, 255, 1, 143, 1, 16, 3, 0, 1, 16, 1, 207, 1, 255,
            1, 239, 1, 127, 1, 63, 50, 0, 1, 48, 1, 159, 2, 255, 1, 207, 1, 16,
            2, 0, 1, 144, 1, 255, 1, 223, 1, 32, 54, 0, 1, 32, 1, 223, 1, 255,
            1, 143, 1, 0, 1, 32, 2, 255, 1, 32, 56, 0, 1, 32, 1, 239, 1, 255,
            1, 64, 1, 159, 1, 255, 1, 159, 58, 0, 1, 127, 1, 255, 1, 143, 1, 207,
            1, 255, 1, 48, 58, 0, 1, 64, 1, 255, 1, 223, 2, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 2, 255, 1, 223,
            1, 255, 1, 64, 58, 0, 1, 48, 1, 255, 1, 207, 1, 143, 1, 255, 1, 127,
            58, 0, 1, 159, 1, 255, 1, 159, 1, 64, 1, 255, 1, 239, 1, 32, 56, 0,
            1, 32, 2, 255, 1, 32, 1, 0, 1, 143, 1, 255, 1, 223, 1, 32, 54, 0,
            1, 32, 1, 223, 1, 255, 1, 143, 2, 0, 1, 16, 1, 207, 2, 255, 1, 159,
            1, 48, 50, 0, 1, 64, 1, 128, 1, 239, 1, 255, 1, 207, 1, 16, 3, 0,
            1, 16, 1, 143, 56, 255, 1, 143, 1, 16, 6, 0, 1, 32, 1, 159, 1, 207,
            50, 255, 1, 223, 1, 143, 1, 63, 4, 0,
        };

        private static readonly ushort[] RoundedRectangleFillR4Rle =
        {
            5, 0, 1, 16, 1, 111, 1, 175, 1, 223, 46, 255, 1, 207, 1, 175, 1, 95,
            10, 0, 1, 80, 1, 239, 52, 255, 1, 223, 1, 95, 7, 0, 1, 144, 56, 255,
            1, 128, 5, 0, 1, 128, 58, 255, 1, 143, 3, 0, 1, 96, 60, 255, 1, 80,
            2, 0, 1, 223, 60, 255, 1, 239, 1, 16, 1, 96, 62, 255, 1, 111, 1, 175,
            62, 255, 1, 175, 1, 207, 62, 255, 1, 223, 2944, 255, 1, 223, 62, 255, 1, 207,
            1, 175, 62, 255, 1, 175, 1, 111, 62, 255, 1, 95, 1, 16, 1, 239, 60, 255,
            1, 223, 2, 0, 1, 80, 60, 255, 1, 95, 3, 0, 1, 143, 58, 255, 1, 127,
            5, 0, 1, 127, 56, 255, 1, 143, 7, 0, 1, 96, 1, 223, 52, 255, 1, 239,
            1, 80, 10, 0, 1, 96, 1, 175, 1, 207, 46, 255, 1, 223, 1, 175, 1, 111,
            1, 16, 5, 0,
        };
        private static readonly ushort[] RoundedRectangleBorderR4T1Rle =
        {
            5, 0, 1, 16, 1, 111, 1, 175, 1, 223, 46, 255, 1, 207, 1, 175, 1, 95,
            10, 0, 1, 80, 1, 239, 52, 255, 1, 223, 1, 95, 7, 0, 1, 144, 2, 255,
            1, 191, 1, 111, 1, 31, 46, 0, 1, 48, 1, 96, 1, 223, 2, 255, 1, 128,
            5, 0, 1, 128, 1, 255, 1, 239, 1, 111, 52, 0, 1, 112, 1, 239, 1, 255,
            1, 143, 3, 0, 1, 96, 1, 255, 1, 239, 1, 48, 54, 0, 1, 48, 1, 239,
            1, 255, 1, 80, 2, 0, 1, 223, 1, 255, 1, 111, 56, 0, 1, 112, 1, 255,
            1, 239, 1, 16, 1, 96, 1, 255, 1, 223, 58, 0, 1, 191, 1, 255, 1, 111,
            1, 175, 1, 255, 1, 95, 58, 0, 1, 111, 1, 255, 1, 175, 1, 207, 1, 255,
            1, 48, 58, 0, 1, 32, 1, 255, 1, 223, 2, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 2, 255, 1, 223,
            1, 255, 1, 31, 58, 0, 1, 48, 1, 255, 1, 207, 1, 175, 1, 255, 1, 111,
            58, 0, 1, 96, 1, 255, 1, 175, 1, 111, 1, 255, 1, 191, 58, 0, 1, 223,
            1, 255, 1, 95, 1, 16, 1, 239, 1, 255, 1, 112, 56, 0, 1, 112, 1, 255,
            1, 223, 2, 0, 1, 80, 1, 255, 1, 239, 1, 48, 54, 0, 1, 48, 1, 239,
            1, 255, 1, 95, 3, 0, 1, 143, 1, 255, 1, 239, 1, 112, 52, 0, 1, 112,
            1, 239, 1, 255, 1, 127, 5, 0, 1, 127, 2, 255, 1, 223, 1, 95, 1, 48,
            46, 0, 1, 32, 1, 111, 1, 191, 2, 255, 1, 143, 7, 0, 1, 96, 1, 223,
            52, 255, 1, 239, 1, 80, 10, 0, 1, 96, 1, 175, 1, 207, 46, 255, 1, 223,
            1, 175, 1, 111, 1, 16, 5, 0,
        };

        private static readonly ushort[] RoundedRectangleFillR5Rle =
        {
            7, 0, 1, 16, 1, 128, 1, 191, 1, 223, 42, 255, 1, 239, 1, 191, 1, 111,
            1, 32, 12, 0, 1, 32, 1, 191, 50, 255, 1, 159, 1, 32, 9, 0, 1, 112,
            1, 239, 52, 255, 1, 239, 1, 112, 7, 0, 1, 128, 56, 255, 1, 128, 5, 0,
            1, 112, 58, 255, 1, 112, 3, 0, 1, 32, 1, 239, 58, 255, 1, 239, 1, 32,
            2, 0, 1, 159, 60, 255, 1, 191, 1, 0, 1, 32, 62, 255, 1, 16, 1, 111,
            62, 255, 1, 127, 1, 191, 62, 255, 1, 191, 1, 239, 62, 255, 1, 223, 2688, 255,
            1, 223, 62, 255, 1, 239, 1, 191, 62, 255, 1, 191, 1, 127, 62, 255, 1, 111,
            1, 16, 62, 255, 1, 32, 1, 0, 1, 191, 60, 255, 1, 159, 2, 0, 1, 32,
            1, 239, 58, 255, 1, 239, 1, 32, 3, 0, 1, 112, 58, 255, 1, 111, 5, 0,
            1, 127, 56, 255, 1, 127, 7, 0, 1, 112, 1, 239, 52, 255, 1, 239, 1, 111,
            9, 0, 1, 32, 1, 159, 50, 255, 1, 191, 1, 32, 12, 0, 1, 32, 1, 111,
            1, 191, 1, 239, 42, 255, 1, 223, 1, 191, 1, 127, 1, 16, 7, 0,
        };
        private static readonly ushort[] RoundedRectangleBorderR5T1Rle =
        {
            7, 0, 1, 16, 1, 128, 1, 191, 1, 223, 42, 255, 1, 239, 1, 191, 1, 111,
            1, 32, 12, 0, 1, 32, 1, 191, 50, 255, 1, 159, 1, 32, 9, 0, 1, 112,
            1, 239, 1, 255, 1, 239, 1, 143, 1, 79, 1, 31, 42, 0, 1, 48, 1, 79,
            1, 159, 2, 255, 1, 239, 1, 112, 7, 0, 1, 128, 2, 255, 1, 175, 1, 16,
            48, 0, 1, 32, 1, 159, 2, 255, 1, 128, 5, 0, 1, 112, 2, 255, 1, 111,
            52, 0, 1, 127, 2, 255, 1, 112, 3, 0, 1, 32, 1, 239, 1, 255, 1, 127,
            54, 0, 1, 112, 1, 255, 1, 239, 1, 32, 2, 0, 1, 159, 1, 255, 1, 159,
            56, 0, 1, 175, 1, 255, 1, 191, 1, 0, 1, 32, 2, 255, 1, 32, 56, 0,
            1, 16, 1, 239, 1, 255, 1, 16, 1, 111, 1, 255, 1, 159, 58, 0, 1, 143,
            1, 255, 1, 127, 1, 191, 1, 255, 1, 79, 58, 0, 1, 80, 1, 255, 1, 191,
            1, 239, 1, 255, 1, 48, 58, 0, 1, 32, 1, 255, 1, 223, 2, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0,
            4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255, 60, 0, 4, 255,
            60, 0, 2, 255, 1, 223, 1, 255, 1, 31, 58, 0, 1, 48, 1, 255, 1, 239,
            1, 191, 1, 255, 1, 79, 58, 0, 1, 80, 1, 255, 1, 191, 1, 127, 1, 255,
            1, 143, 58, 0, 1, 159, 1, 255, 1, 111, 1, 16, 1, 255, 1, 239, 1, 16,
            56, 0, 1, 32, 2, 255, 1, 32, 1, 0, 1, 191, 1, 255, 1, 175, 56, 0,
            1, 159, 1, 255, 1, 159, 2, 0, 1, 32, 1, 239, 1, 255, 1, 112, 54, 0,
            1, 128, 1, 255, 1, 239, 1, 32, 3, 0, 1, 112, 2, 255, 1, 128, 52, 0,
            1, 112, 2, 255, 1, 111, 5, 0, 1, 127, 2, 255, 1, 159, 1, 32, 48, 0,
            1, 16, 1, 175, 2, 255, 1, 127, 7, 0, 1, 112, 1, 239, 2, 255, 1, 159,
            1, 80, 1, 48, 42, 0, 1, 32, 1, 80, 1, 143, 1, 239, 1, 255, 1, 239,
            1, 111, 9, 0, 1, 32, 1, 159, 50, 255, 1, 191, 1, 32, 12, 0, 1, 32,
            1, 111, 1, 191, 1, 239, 42, 255, 1, 223, 1, 191, 1, 127, 1, 16, 7, 0,
        };

        private static readonly RoundedRectangleMaskInfo[] RoundedRectangleMasks =
        {
            new RoundedRectangleMaskInfo(1.0f, 2.0f, RoundedRectangleFillR1Rle, RoundedRectangleBorderR1T1Rle),
            new RoundedRectangleMaskInfo(2.0f, 3.0f, RoundedRectangleFillR2Rle, RoundedRectangleBorderR2T1Rle),
            new RoundedRectangleMaskInfo(3.0f, 4.0f, RoundedRectangleFillR3Rle, RoundedRectangleBorderR3T1Rle),
            new RoundedRectangleMaskInfo(4.0f, 5.0f, RoundedRectangleFillR4Rle, RoundedRectangleBorderR4T1Rle),
            new RoundedRectangleMaskInfo(5.0f, 6.0f, RoundedRectangleFillR5Rle, RoundedRectangleBorderR5T1Rle),
        };
        // END ROUNDED RECTANGLE MASK DATA

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
