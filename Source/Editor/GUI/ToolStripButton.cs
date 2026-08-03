// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Small solid glyphs used by high-frequency editor toolbar actions.
    /// </summary>
    public enum ToolStripGlyph
    {
        /// <summary>No built-in glyph.</summary>
        None,
        /// <summary>Play.</summary>
        Play,
        /// <summary>Stop.</summary>
        Stop,
        /// <summary>Pause.</summary>
        Pause,
        /// <summary>Step one frame.</summary>
        Step,
        /// <summary>Add.</summary>
        Add,
        /// <summary>Import.</summary>
        Import,
        /// <summary>Navigate left.</summary>
        Left,
        /// <summary>Navigate right.</summary>
        Right,
        /// <summary>Navigate up.</summary>
        Up,
    }

    /// <summary>
    /// Tool strip button control.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Control" />
    [HideInEditor]
    public class ToolStripButton : Control
    {
        /// <summary>
        /// The default margin for button parts (icon, text, etc.).
        /// </summary>
        public const int DefaultMargin = 6;

        private SpriteHandle _icon;
        private ToolStripGlyph _glyph;
        private string _text;
        private bool _primaryMouseDown;
        private bool _secondaryMouseDown;
        private bool _ctrlDragCandidate;
        private bool _ctrlDragging;
        private Float2 _ctrlDragStart;

        /// <summary>
        /// Event fired when user clicks the button.
        /// </summary>
        public Action Clicked;

        /// <summary>
        /// Event fired when user clicks the button.
        /// </summary>
        public Action SecondaryClicked;

        /// <summary>
        /// The checked state.
        /// </summary>
        public bool Checked;

        /// <summary>
        /// The automatic check mode.
        /// </summary>
        public bool AutoCheck;

        /// <summary>
        /// Gets or sets the button text.
        /// </summary>
        public string Text
        {
            get => _text;
            set
            {
                _text = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// The icon.
        /// </summary>
        public SpriteHandle Icon
        {
            get => _icon;
            set
            {
                _icon = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// Optional compact solid glyph. When set, it replaces <see cref="Icon"/>.
        /// </summary>
        public ToolStripGlyph Glyph
        {
            get => _glyph;
            set
            {
                _glyph = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// A reference to a context menu to raise when the secondary mouse button is pressed.
        /// </summary>
        public ContextMenu.ContextMenu ContextMenu;

        /// <summary>
        /// Initializes a new instance of the <see cref="ToolStripButton"/> class.
        /// </summary>
        /// <param name="height">The height.</param>
        /// <param name="icon">The icon.</param>
        public ToolStripButton(float height, ref SpriteHandle icon)
        : base(0, 0, height, height)
        {
            _icon = icon;
        }

        /// <summary>
        /// Sets the automatic check mode.
        /// </summary>
        /// <param name="value">True if use auto check, otherwise false.</param>
        /// <returns>This button.</returns>
        public ToolStripButton SetAutoCheck(bool value)
        {
            AutoCheck = value;
            return this;
        }

        /// <summary>
        /// Sets the checked state.
        /// </summary>
        /// <param name="value">True if check it, otherwise false.</param>
        /// <returns>This button.</returns>
        public ToolStripButton SetChecked(bool value)
        {
            Checked = value;
            return this;
        }

        /// <summary>
        /// Links the tooltip with input binding info.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="inputBinding">The input key binding.</param>
        /// <returns>This tooltip.</returns>
        public ToolStripButton LinkTooltip(string text, ref Options.InputBinding inputBinding)
        {
            var input = inputBinding.ToString();
            if (input.Length != 0)
                text = $"{text} ({input})";
            LinkTooltip(text);
            return this;
        }

        private void OnClicked()
        {
            if (AutoCheck)
                Checked = !Checked;
            Clicked?.Invoke();
            (Parent as ToolStrip)?.OnButtonClicked(this);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            // Cache data
            var style = Style.Current;
            float iconSize = Mathf.Min(12.0f, style.IconSize > 0.0f ? style.IconSize : 12.0f);
            var clientRect = new Rectangle(Float2.Zero, Size);
            bool hasText = !string.IsNullOrEmpty(_text);
            float iconX = hasText ? DefaultMargin : (Width - iconSize) * 0.5f;
            var iconRect = new Rectangle(iconX, (Height - iconSize) * 0.5f, iconSize, iconSize);
            var textRect = new Rectangle(DefaultMargin, 0, 0, Height);
            bool enabled = VisuallyEnabledInHierarchy;
            bool mouseButtonDown = _primaryMouseDown || _secondaryMouseDown;

            // Draw background
            if (enabled && (IsMouseOver || IsNavFocused || Checked))
                StyleRendering.FillRoundedRectangle(clientRect.MakeExpanded(-2.0f), Checked ? style.BackgroundSelected : mouseButtonDown ? style.BackgroundHighlighted : (style.LightBackground * 1.3f), style.CornerRadius);

            // Draw icon
            if (_glyph != ToolStripGlyph.None)
            {
                var iconColor = !enabled ? style.ForegroundDisabled : Checked ? style.BorderSelected : style.Foreground;
                DrawGlyph(_glyph, iconRect, iconColor);
                textRect.Location.X = iconRect.Right + DefaultMargin;
            }
            else if (_icon.IsValid)
            {
                var iconColor = !enabled ? style.ForegroundDisabled : Checked ? style.BorderSelected : style.Foreground;
                Render2D.DrawSprite(_icon, iconRect, iconColor);
                textRect.Location.X = iconRect.Right + DefaultMargin;
            }

            // Draw text
            if (!string.IsNullOrEmpty(_text))
            {
                textRect.Size.X = Width - DefaultMargin - textRect.Left;
                Render2D.DrawText(style.FontMedium, _text, textRect, enabled ? style.Foreground : style.ForegroundDisabled, TextAlignment.Near, TextAlignment.Center);
            }
        }

        /// <inheritdoc />
        public override void PerformLayout(bool force = false)
        {
            var style = Style.Current;
            float iconSize = Mathf.Min(12.0f, style.IconSize > 0.0f ? style.IconSize : 12.0f);
            bool hasSprite = _icon.IsValid || _glyph != ToolStripGlyph.None;
            float width = DefaultMargin * 2;

            if (hasSprite)
                width += iconSize;
            if (!string.IsNullOrEmpty(_text) && style.FontMedium)
                width += style.FontMedium.MeasureText(_text).X + (hasSprite ? DefaultMargin : 0);

            Width = hasSprite && string.IsNullOrEmpty(_text) ? Mathf.Max(style.ControlHeight, width) : width;
        }

        private static void DrawGlyph(ToolStripGlyph glyph, Rectangle bounds, Color color)
        {
            float x = Mathf.Floor(bounds.X);
            float y = Mathf.Floor(bounds.Y);
            switch (glyph)
            {
            case ToolStripGlyph.Play:
                DrawPlay(x + 2, y + 1, color, false);
                break;
            case ToolStripGlyph.Stop:
                StyleRendering.FillRoundedRectangle(new Rectangle(x + 2, y + 2, 8, 8), color, 1.0f);
                break;
            case ToolStripGlyph.Pause:
                StyleRendering.FillRoundedRectangle(new Rectangle(x + 2, y + 1, 3, 10), color, 1.0f);
                StyleRendering.FillRoundedRectangle(new Rectangle(x + 7, y + 1, 3, 10), color, 1.0f);
                break;
            case ToolStripGlyph.Step:
                DrawPlay(x + 1, y + 2, color, true);
                Render2D.FillRectangle(new Rectangle(x + 9, y + 2, 2, 8), color);
                break;
            case ToolStripGlyph.Add:
                Render2D.FillRectangle(new Rectangle(x + 1, y + 5, 10, 2), color);
                Render2D.FillRectangle(new Rectangle(x + 5, y + 1, 2, 10), color);
                break;
            case ToolStripGlyph.Import:
                Render2D.FillRectangle(new Rectangle(x + 5, y + 1, 2, 6), color);
                Render2D.FillRectangle(new Rectangle(x + 3, y + 5, 6, 2), color);
                Render2D.FillRectangle(new Rectangle(x + 4, y + 7, 4, 2), color);
                Render2D.FillRectangle(new Rectangle(x + 1, y + 10, 10, 2), color);
                break;
            case ToolStripGlyph.Left:
                DrawArrow(x, y, color, false);
                break;
            case ToolStripGlyph.Right:
                DrawArrow(x, y, color, true);
                break;
            case ToolStripGlyph.Up:
                Render2D.FillRectangle(new Rectangle(x + 5, y + 4, 2, 7), color);
                Render2D.FillRectangle(new Rectangle(x + 3, y + 3, 6, 2), color);
                Render2D.FillRectangle(new Rectangle(x + 4, y + 2, 4, 2), color);
                Render2D.FillRectangle(new Rectangle(x + 5, y + 1, 2, 2), color);
                break;
            }
        }

        private static void DrawPlay(float x, float y, Color color, bool compact)
        {
            int rows = compact ? 8 : 10;
            float maxWidth = compact ? 7.0f : 8.0f;
            for (int row = 0; row < rows; row++)
            {
                float distance = Mathf.Abs(row - (rows - 1) * 0.5f);
                float width = Mathf.Max(1.0f, maxWidth - distance * 1.35f);
                Render2D.FillRectangle(new Rectangle(x, y + row, width, 1), color);
            }
        }

        private static void DrawArrow(float x, float y, Color color, bool right)
        {
            Render2D.FillRectangle(new Rectangle(x + 2, y + 5, 8, 2), color);
            for (int row = 0; row < 8; row++)
            {
                float distance = Mathf.Abs(row - 3.5f);
                float width = Mathf.Max(1.0f, 5.0f - distance);
                float rowX = right ? x + 7 : x + 5 - width;
                Render2D.FillRectangle(new Rectangle(rowX, y + 2 + row, width, 1), color);
            }
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left)
            {
                if (Root != null && Root.GetKey(KeyboardKeys.Control))
                {
                    _ctrlDragCandidate = true;
                    _ctrlDragging = false;
                    _ctrlDragStart = location;
                    StartMouseCapture();
                    Focus();
                    return true;
                }
                _primaryMouseDown = true;
                Focus();
                return true;
            }
            if (button == MouseButton.Right)
            {
                _secondaryMouseDown = true;
                Focus();
                return true;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _ctrlDragCandidate)
            {
                var parent = Parent as ToolStrip;
                parent?.EndItemDrag(this, _ctrlDragging);
                _ctrlDragCandidate = false;
                _ctrlDragging = false;
                EndMouseCapture();
                return true;
            }
            if (button == MouseButton.Left && _primaryMouseDown)
            {
                _primaryMouseDown = false;
                OnClicked();
                return true;
            }
            if (button == MouseButton.Right && _secondaryMouseDown)
            {
                _secondaryMouseDown = false;
                SecondaryClicked?.Invoke();
                (Parent as ToolStrip)?.OnSecondaryButtonClicked(this);
                ContextMenu?.Show(this, new Float2(0, Height));
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            if (_ctrlDragCandidate)
            {
                if (!_ctrlDragging && Float2.DistanceSquared(ref location, ref _ctrlDragStart) > 16.0f)
                    _ctrlDragging = true;
                if (_ctrlDragging)
                    (Parent as ToolStrip)?.UpdateItemDrag(this, PointToParent(location));
                return;
            }
            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left)
            {
                OnClicked();
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            _primaryMouseDown = false;
            _secondaryMouseDown = false;

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            _primaryMouseDown = false;
            _secondaryMouseDown = false;

            base.OnLostFocus();
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            if (_ctrlDragCandidate)
                (Parent as ToolStrip)?.EndItemDrag(this, false);
            _ctrlDragCandidate = false;
            _ctrlDragging = false;
            base.OnEndMouseCapture();
        }
    }
}
