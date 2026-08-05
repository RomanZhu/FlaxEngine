// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.CustomEditors.Elements
{
    /// <summary>
    /// The button element.
    /// </summary>
    /// <seealso cref="FlaxEditor.CustomEditors.LayoutElement" />
    public class ButtonElement : LayoutElement
    {
        private static readonly Color PropertyButtonBackgroundColor = new Color(0.2470588f, 0.2470588f, 0.2588235f, 1.0f);

        /// <summary>
        /// The button.
        /// </summary>
        public readonly Button Button = new Button();

        /// <inheritdoc />
        public override Control Control => Button;

        /// <summary>
        /// Applies shared styling for action buttons shown in property panels.
        /// </summary>
        /// <param name="button">The button.</param>
        public static void ApplyPropertyButtonStyle(Button button)
        {
            var style = Style.Current;

            button.BackgroundColor = PropertyButtonBackgroundColor;
            button.BackgroundColorHighlighted = PropertyButtonBackgroundColor.RGBMultiplied(1.12f);
            button.BackgroundColorSelected = style?.BorderSelected ?? PropertyButtonBackgroundColor.RGBMultiplied(0.9f);
            button.BorderColor = Color.Transparent;
            button.BorderColorHighlighted = Color.Transparent;
            button.BorderColorSelected = Color.Transparent;
            button.HasBorder = false;
            button.CornerRadius = 3.0f;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ButtonElement"/> class.
        /// </summary>
        public ButtonElement()
        {
            ApplyPropertyButtonStyle(Button);
        }
    }
}
