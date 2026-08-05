// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Globalization;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Input
{
    /// <summary>
    /// Float value editor with fixed size text box and slider.
    /// </summary>
    [HideInEditor]
    public class SliderControl : ContainerControl
    {
        /// <summary>
        /// The horizontal slider control.
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.Control" />
        [HideInEditor]
        protected class Slider : Control
        {
            /// <summary>
            /// The default size.
            /// </summary>
            public const int DefaultSize = 16;

            /// <summary>
            /// The default thickness.
            /// </summary>
            public const int DefaultThickness = 6;

            /// <summary>
            /// The minimum value (constant)
            /// </summary>
            public const float Minimum = 0.0f;

            /// <summary>
            /// The maximum value (constant).
            /// </summary>
            public const float Maximum = 100.0f;

            private float _value;
            private Rectangle _fillRect;
            private bool _isSliding;
            private bool _isSlidingPending;
            private Float2 _startSlideLocation;
            private float _startSlideValue;

            /// <summary>
            /// Gets or sets the value (normalized to range 0-100).
            /// </summary>
            public float Value
            {
                get => _value;
                set
                {
                    value = Mathf.Clamp(value, Minimum, Maximum);
                    if (value != _value)
                    {
                        _value = value;

                        // Update
                        UpdateFill();
                        ValueChanged?.Invoke();
                    }
                }
            }

            /// <summary>
            /// The value text to draw over the slider body.
            /// </summary>
            public string DisplayText;

            /// <summary>
            /// Occurs when value gets changed.
            /// </summary>
            public Action ValueChanged;

            /// <summary>
            /// The color of the slider track line.
            /// </summary>
            public Color TrackLineColor { get; set; }

            /// <summary>
            /// The color of the slider thumb when it's not selected.
            /// </summary>
            public Color ThumbColor { get; set; }

            /// <summary>
            /// The color of the slider thumb when it's selected.
            /// </summary>
            public Color ThumbColorSelected { get; set; }

            /// <summary>
            /// The color of the slider thumb when it's hovered.
            /// </summary>
            public Color ThumbColorHovered { get; set; }

            /// <summary>
            /// Gets a value indicating whether user is using a slider.
            /// </summary>
            public bool IsSliding => _isSliding;

            /// <summary>
            /// Occurs when sliding starts.
            /// </summary>
            public Action SlidingStart;

            /// <summary>
            /// Occurs when sliding ends.
            /// </summary>
            public Action SlidingEnd;

            /// <summary>
            /// Occurs when the user clicks the slider body without dragging.
            /// </summary>
            public Action EditRequested;

            /// <summary>
            /// Initializes a new instance of the <see cref="Slider"/> class.
            /// </summary>
            /// <param name="width">The width.</param>
            /// <param name="height">The height.</param>
            public Slider(float width, float height)
            : base(0, 0, width, height)
            {
                var style = Style.Current;
                TrackLineColor = style.TextBoxBackground;
                ThumbColor = style.BorderSelected.AlphaMultiplied(0.72f);
                ThumbColorSelected = style.BorderSelected;
                ThumbColorHovered = style.BorderSelected.AlphaMultiplied(0.88f);
            }

            private void UpdateFill()
            {
                // Cache data
                float trackSize = TrackSize;
                float range = Maximum - Minimum;
                float perc = (_value - Minimum) / range;
                _fillRect = new Rectangle(0.0f, 0.0f, Mathf.RoundToInt(perc * trackSize), Height);
            }

            private void EndSliding()
            {
                _isSlidingPending = false;
                _isSliding = false;
                EndMouseCapture();
                SlidingEnd?.Invoke();
                Defocus();
                Parent?.Focus();
            }

            /// <summary>
            /// Gets the size of the track.
            /// </summary>
            private float TrackSize => Mathf.Max(Width, 1.0f);

            /// <inheritdoc />
            public override void Draw()
            {
                base.Draw();

                var style = Style.Current;
                var rect = new Rectangle(Float2.Zero, Size);
                var cornerRadius = style.GetInputCornerRadius();
                var visuallyEnabled = VisuallyEnabledInHierarchy;
                var isActive = visuallyEnabled && (IsFocused || _isSliding || _isSlidingPending);
                var backgroundColor = isActive ? style.SecondaryBackground : TrackLineColor;
                var borderColor = isActive ? style.BorderSelected : style.BorderNormal.AlphaMultiplied(IsMouseOver ? 0.9f : 0.45f);
                if (!visuallyEnabled)
                {
                    backgroundColor = StyleRendering.GetDisabledInputColor(backgroundColor);
                    borderColor = StyleRendering.GetDisabledInputAccentColor(borderColor);
                }
                StyleRendering.DrawRoundedRectangle(rect, backgroundColor, borderColor, 1.0f, cornerRadius);

                if (_fillRect.Width > 0.0f)
                {
                    var fillColor = _isSliding ? ThumbColorSelected : IsMouseOver ? ThumbColorHovered : ThumbColor;
                    if (!visuallyEnabled)
                        fillColor = StyleRendering.GetDisabledInputAccentColor(fillColor);
                    StyleRendering.FillRoundedRectangle(_fillRect, fillColor, cornerRadius);
                }

                if (!string.IsNullOrEmpty(DisplayText))
                {
                    var textRect = new Rectangle(6.0f, 0.0f, Mathf.Max(0.0f, Width - 12.0f), Height);
                    Render2D.DrawText(style.FontMedium, DisplayText, textRect, visuallyEnabled ? style.Foreground : style.ForegroundDisabled, TextAlignment.Near, TextAlignment.Center, TextWrapping.NoWrap);
                }
            }

            /// <inheritdoc />
            public override void OnLostFocus()
            {
                if (_isSliding)
                {
                    EndSliding();
                }
                else
                {
                    _isSlidingPending = false;
                }

                base.OnLostFocus();
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left)
                {
                    Focus();
                    _isSlidingPending = true;
                    _startSlideLocation = location;
                    _startSlideValue = Value;
                    return true;
                }

                return base.OnMouseDown(location, button);
            }

            /// <inheritdoc />
            public override void OnMouseMove(Float2 location)
            {
                if (_isSlidingPending && Mathf.Abs(location.X - _startSlideLocation.X) >= 2.0f)
                {
                    _isSlidingPending = false;
                    _isSliding = true;
                    StartMouseCapture();
                    SlidingStart?.Invoke();
                }

                if (_isSliding)
                {
                    // Update sliding
                    var slidePosition = location + Root.TrackingMouseOffset;
                    Value = _startSlideValue + (slidePosition.X - _startSlideLocation.X) / TrackSize * (Maximum - Minimum);
                    if (Mathf.NearEqual(Value, Maximum))
                        Value = Maximum;
                    else if (Mathf.NearEqual(Value, Minimum))
                        Value = Minimum;
                }
                else
                {
                    base.OnMouseMove(location);
                }
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left && _isSliding)
                {
                    EndSliding();
                    return true;
                }
                if (button == MouseButton.Left && _isSlidingPending)
                {
                    _isSlidingPending = false;
                    EditRequested?.Invoke();
                    Defocus();
                    return true;
                }

                return base.OnMouseUp(location, button);
            }

            /// <inheritdoc />
            public override void OnMouseEnter(Float2 location)
            {
                Cursor = CursorType.SizeWE;

                base.OnMouseEnter(location);
            }

            /// <inheritdoc />
            public override void OnMouseLeave()
            {
                if (!_isSliding)
                    Cursor = CursorType.Default;

                base.OnMouseLeave();
            }

            /// <inheritdoc />
            public override void OnEndMouseCapture()
            {
                // Check if was sliding
                if (_isSliding)
                {
                    EndSliding();
                }
                else
                {
                    _isSlidingPending = false;
                    base.OnEndMouseCapture();
                }
            }

            /// <inheritdoc />
            protected override void OnSizeChanged()
            {
                base.OnSizeChanged();

                UpdateFill();
            }
        }

        /// <summary>
        /// Text box used by the slider edit overlay.
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.TextBox" />
        [HideInEditor]
        protected class SliderTextBox : TextBox
        {
            /// <summary>
            /// Occurs when editing ends, even if the text did not change.
            /// </summary>
            public Action EditingEnded;

            /// <summary>
            /// Initializes a new instance of the <see cref="SliderTextBox"/> class.
            /// </summary>
            public SliderTextBox()
            : base(false, 0, 0)
            {
            }

            /// <inheritdoc />
            protected override void OnEditEnd()
            {
                bool wasEditing = IsEditing;
                base.OnEditEnd();

                if (wasEditing)
                    EditingEnded?.Invoke();
            }
        }

        /// <summary>
        /// The slider.
        /// </summary>
        protected Slider _slider;

        /// <summary>
        /// The text box.
        /// </summary>
        protected SliderTextBox _textBox;

        private float _value;
        private float _min, _max;
        private float _defaultValue;

        private bool _valueIsChanging;

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action ValueChanged;

        /// <summary>
        /// Gets or sets the value.
        /// </summary>
        public float Value
        {
            get => _value;
            set
            {
                value = Mathf.Clamp(value, _min, _max);
                if (Math.Abs(_value - value) > Mathf.Epsilon)
                {
                    // Set value
                    _value = value;

                    // Update
                    _valueIsChanging = true;
                    UpdateText();
                    UpdateSlider();
                    _valueIsChanging = false;
                    OnValueChanged();
                }
            }
        }

        /// <summary>
        /// Gets or sets the minimum value.
        /// </summary>
        public float MinValue
        {
            get => _min;
            set
            {
                if (_min != value)
                {
                    if (value > _max)
                        throw new ArgumentException();

                    _min = value;
                    Value = Value;
                }
            }
        }

        /// <summary>
        /// Gets or sets the maximum value.
        /// </summary>
        public float MaxValue
        {
            get => _max;
            set
            {
                if (_max != value)
                {
                    if (value < _min)
                        throw new ArgumentException();

                    _max = value;
                    Value = Value;
                }
            }
        }

        /// <summary>
        /// Gets or sets the default value used when committing an empty edit.
        /// </summary>
        public float DefaultValue
        {
            get => _defaultValue;
            set => _defaultValue = Mathf.Clamp(value, _min, _max);
        }

        /// <summary>
        /// Gets a value indicating whether user is using a slider.
        /// </summary>
        public bool IsSliding => _slider.IsSliding;

        /// <summary>
        /// Occurs when sliding starts.
        /// </summary>
        public event Action SlidingStart;

        /// <summary>
        /// Occurs when sliding ends.
        /// </summary>
        public event Action SlidingEnd;

        /// <summary>
        /// Initializes a new instance of the <see cref="SliderControl"/> class.
        /// </summary>
        /// <param name="value">The value.</param>
        /// <param name="x">The position x.</param>
        /// <param name="y">The position y.</param>
        /// <param name="width">The width.</param>
        /// <param name="min">The minimum value.</param>
        /// <param name="max">The maximum value.</param>
        public SliderControl(float value, float x = 0, float y = 0, float width = 120, float min = float.MinValue, float max = float.MaxValue)
        : base(x, y, width, TextBox.DefaultHeight)
        {
            AutoFocus = true;

            _min = min;
            _max = max;
            _value = Mathf.Clamp(value, min, max);
            _defaultValue = _value;

            _slider = new Slider(Width, Height)
            {
                Parent = this,
            };
            _slider.ValueChanged += SliderOnValueChanged;
            _slider.SlidingStart += SlidingStart;
            _slider.SlidingEnd += SliderOnSliderEnd;
            _slider.EditRequested += BeginTextEdit;
            _textBox = new SliderTextBox
            {
                Text = _value.ToString(CultureInfo.InvariantCulture),
                Parent = this,
                Visible = false,
            };
            _textBox.EditingEnded += OnTextBoxEditEnd;
            UpdateText();
            UpdateSlider();
        }

        private void BeginTextEdit()
        {
            UpdateText();
            _textBox.Visible = true;
            _textBox.Focus();
            _textBox.SelectAll();
        }

        private void SliderOnSliderEnd()
        {
            SlidingEnd?.Invoke();
            Defocus();
            Parent?.Focus();
        }

        private void SliderOnValueChanged()
        {
            if (_valueIsChanging)
                return;

            Value = Mathf.Remap(_slider.Value, Slider.Minimum, Slider.Maximum, MinValue, MaxValue);
        }

        private void OnTextBoxEditEnd()
        {
            if (_valueIsChanging)
            {
                _textBox.Visible = false;
                return;
            }

            var text = _textBox.Text.Replace(',', '.');
            if (string.IsNullOrWhiteSpace(text))
            {
                Value = DefaultValue;
                UpdateText();
            }
            else if (double.TryParse(text, NumberStyles.Float | NumberStyles.AllowThousands, CultureInfo.InvariantCulture, out var value))
            {
                Value = (float)Math.Round(value, 5);
                UpdateText();
            }
            else
            {
                UpdateText();
            }
            _textBox.Visible = false;
            Defocus();
            Parent?.Focus();
        }

        /// <summary>
        /// Sets the limits from the attribute.
        /// </summary>
        /// <param name="limits">The limits.</param>
        public void SetLimits(RangeAttribute limits)
        {
            _min = limits.Min;
            _max = Mathf.Max(_min, limits.Max);
            Value = Value;
            DefaultValue = DefaultValue;
        }

        /// <summary>
        /// Sets the limits from the attribute.
        /// </summary>
        /// <param name="limits">The limits.</param>
        public void SetLimits(LimitAttribute limits)
        {
            _min = limits.Min;
            _max = Mathf.Max(_min, limits.Max);
            Value = Value;
            DefaultValue = DefaultValue;
        }

        /// <summary>
        /// Updates the text of the textbox.
        /// </summary>
        protected virtual void UpdateText()
        {
            var text = _value.ToString(CultureInfo.InvariantCulture);
            _textBox.Text = text;
            _slider.DisplayText = text;
        }

        /// <summary>
        /// Updates the slider value.
        /// </summary>
        protected virtual void UpdateSlider()
        {
            _slider.Value = Mathf.Remap(_value, MinValue, MaxValue, Slider.Minimum, Slider.Maximum);
        }

        /// <summary>
        /// Called when value gets changed.
        /// </summary>
        protected virtual void OnValueChanged()
        {
            ValueChanged?.Invoke();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            base.PerformLayoutBeforeChildren();

            _slider.Bounds = new Rectangle(0, 0, Width, Height);
            _textBox.Bounds = new Rectangle(0, 0, Width, Height);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _slider = null;
            _textBox = null;

            base.OnDestroy();
        }
    }
}
