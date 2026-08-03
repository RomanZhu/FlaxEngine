// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.CustomEditors.Elements
{
    /// <summary>
    /// The layout group element.
    /// </summary>
    /// <seealso cref="FlaxEditor.CustomEditors.LayoutElement" />
    public class GroupElement : LayoutElementsContainer
    {
        private const float GroupValueOffset = 6.0f;
        private const float GroupBorderValueDelta = 1.5f;
        private const float NestedGroupValueStep = 1.5f;
        private const float MinimumGroupValueOffset = 1.0f;
        private const float NestedGroupIndent = 12.0f;

        /// <summary>
        /// The drop panel.
        /// </summary>
        public readonly DropPanel Panel = new DropPanel
        {
            Pivot = Float2.Zero,
            ArrowImageClosed = new SpriteBrush(Style.Current.ArrowRight),
            ArrowImageOpened = new SpriteBrush(Style.Current.ArrowDown),
            EnableDropDownIcon = true,
            EnableDropDownIconDragOpenClose = true,
            ItemsMargin = new Margin(Mathf.Max(4.0f, Style.Current.PanelPadding > 0.0f ? Style.Current.PanelPadding : Utilities.Constants.UIMargin)),
            ItemsSpacing = 2.0f,
            HeaderHeight = Style.Current.PropertyRowHeight > 0.0f ? Style.Current.PropertyRowHeight : 20.0f,
            EnableContainmentLines = false,
        };

        /// <summary>
        /// Event is fired if the group can setup a context menu and the context menu is being setup.
        /// </summary>
        public Action<ContextMenu, DropPanel> SetupContextMenu;

        /// <summary>
        /// Initializes a new instance of the <see cref="GroupElement"/> class.
        /// </summary>
        public GroupElement()
        {
            Panel.HeaderTextMargin = new Margin(0, 4, 0, 0);
            ApplyHierarchyStyle(0);
        }

        /// <summary>
        /// Applies a theme-relative section style based on the group nesting depth.
        /// </summary>
        /// <param name="parentGroupDepth">The number of parent groups above this one.</param>
        public void ApplyHierarchyStyle(int parentGroupDepth)
        {
            var style = Style.Current;
            var panelColor = BuildHierarchyColor(style.Background, parentGroupDepth, 0.0f);
            var borderColor = BuildHierarchyColor(style.Background, parentGroupDepth, GroupBorderValueDelta);
            Panel.BackgroundColor = panelColor;
            Panel.BorderColor = borderColor;
            Panel.HeaderColor = panelColor;
            Panel.HeaderColorMouseOver = Color.Lerp(panelColor, style.Foreground, 0.035f);
            Panel.DropDownIconIndent = parentGroupDepth * NestedGroupIndent;
        }

        private static Color BuildHierarchyColor(Color background, int parentGroupDepth, float borderValueDelta)
        {
            var hsv = background.ToHSV();
            var darkTheme = hsv.Z < 0.5f;
            var offset = Mathf.Max(MinimumGroupValueOffset, GroupValueOffset - parentGroupDepth * NestedGroupValueStep) + borderValueDelta;
            if (!darkTheme)
                offset = -offset;
            hsv.Z = Mathf.Saturate(hsv.Z + offset / 100.0f);
            return Color.FromHSV(hsv, background.A);
        }

        /// <inheritdoc />
        public override ContainerControl ContainerControl => Panel;

        /// <summary>
        /// Add utility settings button to the group header.
        /// </summary>
        /// <returns>The created control.</returns>
        public Image AddSettingsButton()
        {
            return AddHeaderButton("Settings", 0, Style.Current.Settings);
        }

        /// <summary>
        /// Adds a button to the group header.
        /// </summary>
        /// <returns>The created control.</returns>
        public Image AddHeaderButton(string tooltipText, float xOffset, SpriteHandle sprite)
        {
            var style = Style.Current;
            var settingsButtonSize = Panel.HeaderHeight;
            var iconSize = Mathf.Min(16.0f, style.IconSize > 0.0f ? style.IconSize : 16.0f);
            var iconMargin = Mathf.Max(1.0f, (settingsButtonSize - iconSize) * 0.5f);
            Panel.HeaderTextMargin = Panel.HeaderTextMargin with { Right = settingsButtonSize + Utilities.Constants.UIMargin };
            return new Image
            {
                TooltipText = tooltipText,
                AutoFocus = true,
                AnchorPreset = AnchorPresets.TopRight,
                Parent = Panel,
                Bounds = new Rectangle(Panel.Width - settingsButtonSize - xOffset, 0, settingsButtonSize, settingsButtonSize),
                IsScrollable = false,
                Color = style.ForegroundGrey,
                Margin = new Margin(iconMargin),
                Brush = new SpriteBrush(sprite),
            };
        }

        /// <inheritdoc />
        protected override void OnAddElement(LayoutElement element)
        {
            base.OnAddElement(element);

            if (element is PropertiesListElement propertiesList)
            {
                propertiesList.Properties.BackgroundColor = Color.Transparent;
                propertiesList.SetPropertySectionBackgroundColor(Panel.BackgroundColor);
            }
        }
    }
}
