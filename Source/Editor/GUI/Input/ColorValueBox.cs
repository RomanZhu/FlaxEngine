// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.GUI.Dialogs;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Input
{
    /// <summary>
    /// Color value editor with picking support.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Control" />
    [HideInEditor]
    public class ColorValueBox : Control
    {
        private const float PrefixWidth = 16.0f;
        private const float PrefixPadding = 4.0f;
        private const float PreviewRightPadding = 4.0f;
        private const float PreviewVerticalPadding = 3.0f;

        private bool _isMouseDown;
        private bool _linear;

        /// <summary>
        /// Delegate function used for the color picker events handling.
        /// </summary>
        /// <param name="color">The selected color.</param>
        /// <param name="sliding">True if user is using a slider, otherwise false.</param>
        public delegate void ColorPickerEvent(Color color, bool sliding);

        /// <summary>
        /// Delegate function used for the color picker close event handling.
        /// </summary>
        public delegate void ColorPickerClosedEvent();

        /// <summary>
        /// Delegate function used to handle showing color picking dialog.
        /// </summary>
        /// <param name="targetControl">The GUI control that invokes the picker.</param>
        /// <param name="initialValue">The initial value.</param>
        /// <param name="colorChanged">The color changed event.</param>
        /// <param name="pickerClosed">The color editing end event.</param>
        /// <param name="useDynamicEditing">True if allow dynamic value editing (slider-like usage), otherwise will fire color change event only on editing end.</param>
        /// <returns>The created color picker dialog or null if failed.</returns>
        public delegate IColorPickerDialog ShowPickColorDialogDelegate(Control targetControl, Color initialValue, ColorPickerEvent colorChanged, ColorPickerClosedEvent pickerClosed = null, bool useDynamicEditing = true);

        /// <summary>
        /// Shows picking color dialog (see <see cref="ShowPickColorDialogDelegate"/>).
        /// </summary>
        public static ShowPickColorDialogDelegate ShowPickColorDialog;

        /// <summary>
        /// The current opened dialog.
        /// </summary>
        protected IColorPickerDialog _currentDialog;

        /// <summary>
        /// True if slider is in use.
        /// </summary>
        protected bool _isSliding;

        /// <summary>
        /// The value.
        /// </summary>
        protected Color _value;

        private static float GetRightRoundedInset(float localY, float height, float radius)
        {
            if (radius < 1.0f)
                return 0.0f;

            var centerY = localY + 0.5f;
            if (centerY < radius)
            {
                var dy = radius - centerY;
                return radius - (float)Math.Sqrt(Mathf.Max(0.0f, radius * radius - dy * dy));
            }

            if (centerY > height - radius)
            {
                var dy = centerY - (height - radius);
                return radius - (float)Math.Sqrt(Mathf.Max(0.0f, radius * radius - dy * dy));
            }

            return 0.0f;
        }

        private static void FillRightRoundedCheckerboard(Rectangle bounds, float radius, float cellSize)
        {
            if (bounds.Width <= 0.0f || bounds.Height <= 0.0f || cellSize <= 0.0f)
                return;

            radius = Mathf.Clamp(radius, 0.0f, Mathf.Min(bounds.Width, bounds.Height) * 0.5f);
            var rowCount = Mathf.CeilToInt(bounds.Height);
            for (int row = 0; row < rowCount; row++)
            {
                var rowHeight = Mathf.Min(1.0f, bounds.Height - row);
                if (rowHeight <= 0.0f)
                    continue;

                var rowLeft = bounds.X;
                var rowRight = bounds.X + bounds.Width - GetRightRoundedInset(row, bounds.Height, radius);
                if (rowRight <= rowLeft)
                    continue;

                var cellY = Mathf.FloorToInt(row / cellSize);
                var endCellX = Mathf.CeilToInt((rowRight - bounds.X) / cellSize);
                for (int cellX = 0; cellX < endCellX; cellX++)
                {
                    if ((cellX + cellY) % 2 != 0)
                        continue;

                    var x1 = Mathf.Max(rowLeft, bounds.X + cellX * cellSize);
                    var x2 = Mathf.Min(rowRight, bounds.X + (cellX + 1) * cellSize);
                    if (x2 > x1)
                        Render2D.FillRectangle(new Rectangle(x1, bounds.Y + row, x2 - x1, rowHeight), Color.Gray);
                }
            }
        }

        /// <summary>
        /// Enables live preview of the selected value from the picker. Otherwise will update the value only when user confirms it on dialog closing.
        /// </summary>
        public bool UseDynamicEditing = true;

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action ValueChanged;

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action<ColorValueBox> ColorValueChanged;

        /// <summary>
        /// Gets or sets the color value.
        /// </summary>
        public Color Value
        {
            get => _value;
            set
            {
                if (_value != value)
                {
                    _value = value;
                    OnValueChanged();
                }
            }
        }

        /// <summary>
        /// Gets a value indicating whether user is using a slider.
        /// </summary>
        public bool IsSliding => _isSliding;

        /// <summary>
        /// Initializes a new instance of the <see cref="ColorValueBox"/> class.
        /// </summary>
        public ColorValueBox()
        : base(0, 0, 56, 18)
        {
            _linear = !Graphics.GammaColorSpace;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ColorValueBox"/> class.
        /// </summary>
        /// <param name="value">The initial value.</param>
        /// <param name="x">The x location</param>
        /// <param name="y">The y location</param>
        public ColorValueBox(Color value, float x, float y)
        : base(x, y, 56, 18)
        {
            _value = value;
            _linear = !Graphics.GammaColorSpace;
        }

        /// <summary>
        /// Called when value gets changed.
        /// </summary>
        protected virtual void OnValueChanged()
        {
            ValueChanged?.Invoke();
            ColorValueChanged?.Invoke(this);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            var value = _value;
            if (_linear)
                value = value.ToSRgb();
            var isTransparent = value.A < 1;
            var style = Style.Current;
            var enabled = VisuallyEnabledInHierarchy;
            var disabledValue = Color.Lerp(value, style.TextBoxBackground, 0.6f);
            var fullRect = new Rectangle(0, 0, Width, Height);
            var cornerRadius = style.GetInputCornerRadius();
            var borderColor = enabled && (IsMouseOver || IsNavFocused) ? style.BorderSelected : style.BorderNormal;
            var backgroundColor = style.TextBoxBackground;
            if (!enabled)
            {
                backgroundColor = StyleRendering.GetDisabledInputColor(backgroundColor);
                borderColor = StyleRendering.GetDisabledInputAccentColor(borderColor);
            }

            StyleRendering.DrawRoundedRectangle(fullRect, backgroundColor, borderColor, 1.0f, cornerRadius);
            DrawPrefix(style, enabled);

            var previewRect = GetPreviewRect();
            if (previewRect.Width <= 0.0f || previewRect.Height <= 0.0f)
                return;

            var colorValue = enabled ? value : disabledValue;
            colorValue.A = 1.0f;
            if (isTransparent)
            {
                var colorRect = previewRect;
                colorRect.Width *= 0.7f;
                var alphaRect = new Rectangle(colorRect.Right, 0, Width - colorRect.Right, Height);
                alphaRect.Y = previewRect.Y;
                alphaRect.Height = previewRect.Height;
                alphaRect.Width = previewRect.Right - colorRect.Right;

                // Draw checkerboard pattern to part of the color value box
                StyleRendering.FillRoundedRectangle(alphaRect, Color.White, cornerRadius, RoundedCorners.Right);
                var smallRectSize = 7.9f;
                FillRightRoundedCheckerboard(alphaRect, cornerRadius, smallRectSize);
                StyleRendering.FillRoundedRectangle(alphaRect, enabled ? value : disabledValue, cornerRadius, RoundedCorners.Right);
                StyleRendering.FillRoundedRectangle(colorRect, colorValue, cornerRadius, RoundedCorners.None);
            }
            else
            {
                StyleRendering.FillRoundedRectangle(previewRect, colorValue, cornerRadius, RoundedCorners.Right);
            }
        }

        private Rectangle GetPreviewRect()
        {
            var previewX = TextBoxBase.DefaultMargin + PrefixWidth + PrefixPadding;
            return new Rectangle(previewX, PreviewVerticalPadding, Mathf.Max(0.0f, Width - previewX - PreviewRightPadding), Mathf.Max(0.0f, Height - PreviewVerticalPadding * 2.0f));
        }

        private void DrawPrefix(Style style, bool enabled)
        {
            var prefixOffset = style.GetValueBoxPrefixOffset();
            var prefixRight = TextBoxBase.DefaultMargin + PrefixWidth;
            var prefixLeft = Mathf.Max(0.0f, prefixOffset);
            var prefixRect = new Rectangle(prefixLeft, 0.0f, Mathf.Max(0.0f, prefixRight - prefixLeft), Height);
            var cornerRadius = style.GetValueBoxPrefixCornerRadius();
            var backgroundColor = enabled ? style.SecondaryBackground : StyleRendering.GetDisabledInputAccentColor(style.SecondaryBackground);
            if (cornerRadius > 0.0f)
                StyleRendering.FillRoundedRectangle(prefixRect, backgroundColor, cornerRadius, RoundedCorners.Left);
            else
                Render2D.FillRectangle(prefixRect, backgroundColor);

            var editor = global::FlaxEditor.Editor.Instance;
            var icon = editor != null ? editor.Icons.ColorWheel128 : SpriteHandle.Invalid;
            if (!icon.IsValid)
                return;

            var prefixContentOffset = style.GetValueBoxPrefixContentOffset();
            var prefixContentRect = new Rectangle(Mathf.Max(0.0f, TextBoxBase.DefaultMargin + prefixOffset + prefixContentOffset), 0.0f, PrefixWidth, Height);
            var iconSize = Mathf.Min(Mathf.Min(prefixContentRect.Width - 4.0f, prefixContentRect.Height - 6.0f), 12.0f);
            if (iconSize <= 0.0f)
                return;

            var iconColor = enabled ? Color.White : Color.White.AlphaMultiplied(0.45f);
            Render2D.DrawSprite(icon, new Rectangle(prefixContentRect.X + (prefixContentRect.Width - iconSize) * 0.5f, prefixContentRect.Y + (prefixContentRect.Height - iconSize) * 0.5f, iconSize, iconSize), iconColor);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            _isMouseDown = true;
            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (_isMouseDown)
            {
                _isMouseDown = false;
                Focus();
                OnSubmit();
            }
            return true;
        }

        /// <inheritdoc />
        public override void OnSubmit()
        {
            base.OnSubmit();

            // Show color picker dialog
            _currentDialog = ShowPickColorDialog?.Invoke(this, _value, OnColorChanged, OnPickerClosed, UseDynamicEditing);
        }

        private void OnColorChanged(Color color, bool sliding)
        {
            // Force send ValueChanged event is sliding state gets modified by the color picker (e.g the color picker window closing event)
            if (_isSliding != sliding)
            {
                _isSliding = sliding;
                _value = color;
                OnValueChanged();
            }
            else
            {
                Value = color;
            }
        }

        private void OnPickerClosed()
        {
            _currentDialog = null;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (_currentDialog != null)
            {
                _currentDialog.ClosePicker();
                _currentDialog = null;
            }

            base.OnDestroy();
        }
    }
}
