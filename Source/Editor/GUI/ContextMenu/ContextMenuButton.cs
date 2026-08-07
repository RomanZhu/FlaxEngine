// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.ContextMenu
{
    /// <summary>
    /// Context Menu button control.
    /// </summary>
    /// <seealso cref="ContextMenuItem" />
    [HideInEditor]
    public class ContextMenuButton : ContextMenuItem
    {
        private const float DefaultIconSize = 16.0f;
        private const float IconRightPadding = 5.0f;
        private const float AccessKeyUnderlineBottomOffset = 3.0f;
        private const float AccessKeyUnderlineHeight = 1.0f;
        private const float CheckBoxSize = 12.0f;
        private const float CheckedValueMultiplier = 0.8f;
        private static readonly Color ItemHighlightColor = Color.FromBgra(0xFF424247);

        private bool _isMouseDown;
        
        /// <summary>
        /// The amount to adjust the short keys and arrow image by in x coordinates.
        /// </summary>
        public float ExtraAdjustmentAmount = 0;

        /// <summary>
        /// Event fired when user clicks on the button.
        /// </summary>
        public event Action Clicked;

        /// <summary>
        /// Event fired when user clicks on the button.
        /// </summary>
        public event Action<ContextMenuButton> ButtonClicked;

        /// <summary>
        /// The button text.
        /// </summary>
        public string Text;

        /// <summary>
        /// The button short keys information (eg. 'Ctrl+C').
        /// </summary>
        public string ShortKeys;

        /// <summary>
        /// Item icon (best is 16x16).
        /// </summary>
        public SpriteHandle Icon;

        /// <summary>
        /// The checked state.
        /// </summary>
        public bool Checked;

        /// <summary>
        /// The automatic check mode.
        /// </summary>
        public bool AutoCheck;

        /// <summary>
        /// Closes the context menu after clicking the button, otherwise menu will stay open.
        /// </summary>
        public bool CloseMenuOnClick = true;

        /// <summary>
        /// Initializes a new instance of the <see cref="ContextMenuButton"/> class.
        /// </summary>
        /// <param name="parent">The parent context menu.</param>
        /// <param name="text">The text.</param>
        /// <param name="shortKeys">The short keys tip.</param>
        public ContextMenuButton(ContextMenu parent, string text, string shortKeys = "")
        : base(parent, 8, Style.Current.ControlHeight > 0.0f ? Style.Current.ControlHeight : 22.0f)
        {
            Text = text;
            ShortKeys = shortKeys;
        }

        /// <summary>
        /// Sets the automatic check mode. In auto check mode the button sets the check sprite as an icon when user clicks it.
        /// </summary>
        /// <param name="value">True if use auto check, otherwise false.</param>
        /// <returns>This button.</returns>
        public ContextMenuButton SetAutoCheck(bool value)
        {
            AutoCheck = value;
            return this;
        }

        /// <summary>
        /// Sets the checked state.
        /// </summary>
        /// <param name="value">True if check it, otherwise false.</param>
        /// <returns>This button.</returns>
        public ContextMenuButton SetChecked(bool value)
        {
            Checked = value;
            return this;
        }

        /// <summary>
        /// Clicks this button.
        /// </summary>
        public void Click()
        {
            if (CloseMenuOnClick)
            {
                InvokeAfterMenuClosed(this, _ => InvokeClickHandlers(), "button clicked: " + Text);
                return;
            }

            InvokeClickHandlers();
        }

        private void InvokeClickHandlers()
        {
            // Auto check logic
            if (AutoCheck)
                Checked = !Checked;

            // Fire event
            Clicked?.Invoke();
            ButtonClicked?.Invoke(this);
            ParentContextMenu?.OnButtonClicked(this);
        }

        /// <summary>
        /// Defers the button action until the topmost context menu is fully closed.
        /// </summary>
        /// <param name="clicked">The action to invoke.</param>
        public void DeferClickUntilMenuClosed(Action<ContextMenuButton> clicked)
        {
            CloseMenuOnClick = false;
            ButtonClicked += button => InvokeAfterMenuClosed(button, clicked);
        }

        private static void InvokeAfterMenuClosed(ContextMenuButton button, Action<ContextMenuButton> clicked, string hideReason = null)
        {
            var contextMenu = button?.ParentContextMenu?.TopmostCM;
            if (contextMenu == null || !contextMenu.Visible)
            {
                clicked?.Invoke(button);
                return;
            }

            if (hideReason != null)
                contextMenu.HideWithReason(hideReason);
            else
                contextMenu.Hide();
            InvokeWhenClosed();

            void InvokeWhenClosed()
            {
                FlaxEngine.Scripting.InvokeOnUpdate(() =>
                {
                    if (contextMenu.Visible)
                    {
                        InvokeWhenClosed();
                        return;
                    }

                    clicked?.Invoke(button);
                });
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            var style = Style.Current;
            var backgroundRect = new Rectangle(-X + 3, 0, Parent.Width - 6, Height);
            var textRect = new Rectangle(0, 0, Width - 8, Height);
            var textColor = Enabled ? style.Foreground : style.ForegroundDisabled;
            var selectionCornerRadius = style.GetSelectionCornerRadius();
            if (Checked && Enabled)
                textColor = Color.White;

            // Draw background
            if (Checked)
            {
                var checkedColor = style.BorderSelected;
                if (Enabled && (IsMouseOver || IsFocused))
                    checkedColor = MultiplyValue(checkedColor, CheckedValueMultiplier);
                if (!Enabled)
                    checkedColor = Color.Lerp(checkedColor, style.Background, 0.45f);
                StyleRendering.FillRoundedRectangle(backgroundRect.MakeExpanded(-2.0f), checkedColor, selectionCornerRadius);
            }
            else if (IsMouseOver && Enabled)
                StyleRendering.FillRoundedRectangle(backgroundRect.MakeExpanded(-2.0f), ItemHighlightColor, selectionCornerRadius);
            else if (IsFocused)
                StyleRendering.FillRoundedRectangle(backgroundRect.MakeExpanded(-2.0f), ItemHighlightColor, selectionCornerRadius);

            base.Draw();

            // Draw text
            Render2D.DrawText(style.FontMedium, Text, textRect, textColor, TextAlignment.Near, TextAlignment.Center);
            DrawAccessKeyUnderline(style.FontMedium, Text, textRect, textColor, TextAlignment.Near, TextAlignment.Center, ParentContextMenu?.GetAccessKeyIndex(this) ?? -1);

            if (!string.IsNullOrEmpty(ShortKeys))
            {
                // Draw short keys
                Render2D.DrawText(style.FontMedium, ShortKeys, new Rectangle(textRect.X + ExtraAdjustmentAmount, textRect.Y, textRect.Width, textRect.Height), textColor, TextAlignment.Far, TextAlignment.Center);
            }

            // Draw icon
            var iconSize = Mathf.Min(Mathf.Max(0.0f, style.GetMenuIconSize(DefaultIconSize)), Mathf.Max(0.0f, Height - 2.0f));
            var drawCheckBox = Icon.IsValid && Icon == style.CheckBoxTick;
            if (drawCheckBox && style.CheckBoxTick.IsValid)
            {
                var checkBoxSize = Mathf.Min(CheckBoxSize, Mathf.Max(0.0f, Height - 2.0f));
                var checkBoxRect = new Rectangle(-IconRightPadding - DefaultIconSize * 0.5f - checkBoxSize * 0.5f, (Height - checkBoxSize) * 0.5f, checkBoxSize, checkBoxSize);
                var checkBoxColor = Enabled ? style.BorderSelected : Color.Lerp(style.BorderSelected, style.Background, 0.45f);
                if (Enabled && (IsMouseOver || IsFocused))
                    checkBoxColor = Color.Lerp(checkBoxColor, Color.White, 0.28f);
                StyleRendering.FillCheckBox(checkBoxRect, checkBoxColor);
                Render2D.DrawSprite(style.CheckBoxTick, checkBoxRect, Enabled ? Color.White : style.ForegroundDisabled);
            }
            else if (Icon.IsValid && iconSize > 0.0f)
            {
                Render2D.DrawSprite(Icon, new Rectangle(-iconSize - IconRightPadding, (Height - iconSize) / 2, iconSize, iconSize), textColor);
            }
        }

        private static Color MultiplyValue(Color color, float multiplier)
        {
            var hsv = color.ToHSV();
            hsv.Z = Mathf.Saturate(hsv.Z * multiplier);
            return Color.FromHSV(hsv, color.A);
        }

        internal static void DrawAccessKeyUnderline(Font font, string text, Rectangle textRect, Color color, TextAlignment horizontalAlignment, TextAlignment verticalAlignment, int accessKeyIndex)
        {
            if (font == null || string.IsNullOrEmpty(text) || accessKeyIndex < 0 || accessKeyIndex >= text.Length)
                return;

            var textSize = font.MeasureText(text);
            var prefixWidth = accessKeyIndex > 0 ? font.MeasureText(text.Substring(0, accessKeyIndex)).X : 0.0f;
            var keyWidth = font.MeasureText(text.Substring(accessKeyIndex, 1)).X;

            float x = textRect.X;
            switch (horizontalAlignment)
            {
            case TextAlignment.Center:
                x += (textRect.Width - textSize.X) * 0.5f;
                break;
            case TextAlignment.Far:
                x += textRect.Width - textSize.X;
                break;
            }

            var underlineY = Mathf.Min(textRect.Bottom - AccessKeyUnderlineBottomOffset, textRect.Bottom - AccessKeyUnderlineHeight);
            underlineY = Mathf.Floor(Mathf.Max(textRect.Y, underlineY));
            var underlineRect = new Rectangle(Mathf.Floor(x + prefixWidth), Mathf.Floor(underlineY), Mathf.Max(1.0f, Mathf.Ceil(keyWidth)), AccessKeyUnderlineHeight);
            Render2D.FillRectangle(underlineRect, color);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            _isMouseDown = false;

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (base.OnMouseDown(location, button))
                return true;

            _isMouseDown = true;
            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (base.OnMouseUp(location, button))
                return true;

            if (_isMouseDown)
            {
                _isMouseDown = false;
                Click();
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (base.OnKeyDown(key))
                return true;

            switch (key)
            {
            case KeyboardKeys.ArrowUp:
                for (int i = IndexInParent - 1; i >= 0; i--)
                {
                    if (ParentContextMenu.ItemsContainer.Children[i] is ContextMenuButton item && item.Visible && item.Enabled)
                    {
                        item.Focus();
                        ParentContextMenu.ItemsContainer.ScrollViewTo(item);
                        return true;
                    }
                }
                break;
            case KeyboardKeys.ArrowDown:
                for (int i = IndexInParent + 1; i < ParentContextMenu.ItemsContainer.Children.Count; i++)
                {
                    if (ParentContextMenu.ItemsContainer.Children[i] is ContextMenuButton item && item.Visible && item.Enabled)
                    {
                        item.Focus();
                        ParentContextMenu.ItemsContainer.ScrollViewTo(item);
                        return true;
                    }
                }
                break;
            case KeyboardKeys.Return:
                Click();
                return true;
            case KeyboardKeys.Escape:
                ParentContextMenu.HideWithReason("Escape key on menu item: " + Text);
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            _isMouseDown = false;

            base.OnLostFocus();
        }

        /// <inheritdoc />
        public override float MinimumWidth
        {
            get
            {
                var style = Style.Current;
                float width = 20;
                if (style.FontMedium)
                {
                    width += style.FontMedium.MeasureText(Text).X;
                    if (!string.IsNullOrEmpty(ShortKeys))
                        width += 40 + style.FontMedium.MeasureText(ShortKeys).X;
                }

                return Mathf.Max(width, base.MinimumWidth);
            }
        }
    }
}
