using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Input
{
    /// <summary>
    /// Search box control which can gather text search input from the user.
    /// </summary>
    public class SearchBox : TextBox
    {
        private const float BackgroundValueOffset = -6.0f;
        private const float ClearButtonSize = 16.0f;
        private const float ClearButtonMargin = 2.0f;
        private const float ClearGlyphSize = 10.0f;

        private sealed class SearchClearButton : Button
        {
            /// <inheritdoc />
            public override void DrawSelf()
            {
                base.DrawSelf();

                var center = Size * 0.5f;
                var extent = ClearGlyphSize * 0.5f;
                var color = IsMouseOver || IsPressed ? TextColorHighlighted : TextColor;
                if (!VisuallyEnabledInHierarchy)
                    color *= 0.5f;
                Render2D.DrawLine(center - new Float2(extent), center + new Float2(extent), color, 1.5f);
                Render2D.DrawLine(center + new Float2(-extent, extent), center + new Float2(extent, -extent), color, 1.5f);
            }
        }

        /// <summary>
        /// A button that clears the search bar.
        /// </summary>
        public Button ClearSearchButton { get; }

        /// <summary>
        /// Init search box
        /// </summary>
        public SearchBox()
        : this(false, 0, 0)
        {
        }

        /// <summary>
        /// Init search box
        /// </summary>
        public SearchBox(bool isMultiline, float x, float y, float width = 120)
        : base(isMultiline, x, y, width)
        {
            WatermarkText = "Search...";
            var style = Style.Current;
            var backgroundColor = AdjustValueUnits(style.BackgroundNormal, BackgroundValueOffset);
            // Search is a primary navigation control. Keep its resting affordance visible
            // instead of relying on hover to reveal an otherwise toolbar-colored field.
            BackgroundColor = backgroundColor;
            BackgroundSelectedColor = style.SecondaryBackground;
            BorderColor = style.BorderNormal;
            BorderSelectedColor = style.BorderSelected;
            WatermarkTextColor = style.ForegroundGrey;

            ClearSearchButton = new SearchClearButton
            {
                Parent = this,
                Width = ClearButtonSize,
                Height = ClearButtonSize,
                AnchorPreset = AnchorPresets.TopLeft,
                Text = "",
                TooltipText = "Cancel Search.",
                HasBorder = false,
                BackgroundColor = Color.Transparent,
                BorderColor = Color.Transparent,
                BackgroundColorHighlighted = Color.Transparent,
                BorderColorHighlighted = Color.Transparent,
                BackgroundColorSelected = Color.Transparent,
                BorderColorSelected = Color.Transparent,
                TextColor = style.ForegroundGrey,
                TextColorHighlighted = style.Foreground,
                Visible = false,
            };
            UpdateClearButtonBounds();
            ClearSearchButton.Clicked += Clear;
            ClearSearchButton.HoverBegin += () =>
            {
                _changeCursor = false;
                Cursor = CursorType.Default;
            };
            ClearSearchButton.HoverEnd += () => _changeCursor = true;

            TextChanged += () => ClearSearchButton.Visible = !string.IsNullOrEmpty(Text);
        }

        private static Color AdjustValueUnits(Color color, float units)
        {
            var hsv = color.ToHSV();
            hsv.Z = Mathf.Saturate(hsv.Z + units / 100.0f);
            return Color.FromHSV(hsv, color.A);
        }

        /// <inheritdoc />
        public override void PerformLayout(bool force = false)
        {
            base.PerformLayout(force);
            if (ClearSearchButton == null)
                return;
            UpdateClearButtonBounds();
        }

        private void UpdateClearButtonBounds()
        {
            ClearSearchButton.Bounds = new Rectangle(
                Width - ClearSearchButton.Width - ClearButtonMargin,
                (Height - ClearSearchButton.Height) * 0.5f,
                ClearSearchButton.Width,
                ClearSearchButton.Height);
        }
    }
}
