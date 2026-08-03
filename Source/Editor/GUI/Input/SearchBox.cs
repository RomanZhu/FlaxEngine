using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Input
{
    /// <summary>
    /// Search box control which can gather text search input from the user.
    /// </summary>
    public class SearchBox : TextBox
    {
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
            // Search is a primary navigation control. Keep its resting affordance visible
            // instead of relying on hover to reveal an otherwise toolbar-colored field.
            BackgroundColor = style.BackgroundNormal;
            BackgroundSelectedColor = Color.Lerp(style.BackgroundNormal, style.Foreground, 0.06f);
            BorderColor = style.BorderNormal;
            BorderSelectedColor = style.BorderSelected;
            WatermarkTextColor = style.ForegroundGrey;

            ClearSearchButton = new Button
            {
                Parent = this,
                Width = 12.0f,
                Height = 12.0f,
                AnchorPreset = AnchorPresets.TopRight,
                Text = "",
                TooltipText = "Cancel Search.",
                BackgroundColor = TextColor,
                BorderColor = Color.Transparent,
                BackgroundColorHighlighted = style.ForegroundGrey,
                BorderColorHighlighted = Color.Transparent,
                BackgroundColorSelected = style.ForegroundGrey,
                BorderColorSelected = Color.Transparent,
                BackgroundBrush = new SpriteBrush(Editor.Instance.Icons.Cross12),
                Visible = false,
            };
            ClearSearchButton.LocalY = (Height - ClearSearchButton.Height) * 0.5f;
            ClearSearchButton.LocalX -= 4;
            ClearSearchButton.Clicked += Clear;
            ClearSearchButton.HoverBegin += () =>
            {
                _changeCursor = false;
                Cursor = CursorType.Default;
            };
            ClearSearchButton.HoverEnd += () => _changeCursor = true;

            TextChanged += () => ClearSearchButton.Visible = !string.IsNullOrEmpty(Text);
        }

        /// <inheritdoc />
        public override void PerformLayout(bool force = false)
        {
            base.PerformLayout(force);
            if (ClearSearchButton == null)
                return;
            ClearSearchButton.LocalX = -4.0f;
            ClearSearchButton.LocalY = (Height - ClearSearchButton.Height) * 0.5f;
        }
    }
}
