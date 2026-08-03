// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Input
{
    /// <summary>
    /// Base class for text boxes for float/int value editing. Supports slider and range clamping.
    /// </summary>
    /// <typeparam name="T">The value type.</typeparam>
    /// <seealso cref="FlaxEngine.GUI.TextBox" />
    [HideInEditor]
    public abstract class ValueBox<T> : TextBox where T : struct, IComparable<T>
    {
        /// <summary>
        /// The sliding box size.
        /// </summary>
        protected const float SlidingBoxSize = 12.0f;

        /// <summary>
        /// The current value.
        /// </summary>
        protected T _value;

        /// <summary>
        /// The minimum value.
        /// </summary>
        protected T _min;

        /// <summary>
        /// The maximum value.
        /// </summary>
        protected T _max;

        /// <summary>
        /// The slider speed.
        /// </summary>
        protected float _slideSpeed;

        /// <summary>
        /// True if slider is in use.
        /// </summary>
        protected bool _isSliding;

        /// <summary>
        /// The value cached on sliding start.
        /// </summary>
        protected T _startSlideValue;

        /// <summary>
        /// The text cached on editing start. Used to compare with the end result to detect changes.
        /// </summary>
        protected string _startEditText;

        private Float2 _startSlideLocation;
        private double _clickStartTime = -1;
        private bool _cursorChanged;
        private bool _isSlidingPending;
        private Float2 _mouseClickedPosition;

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action ValueChanged;

        /// <summary>
        /// Occurs when value gets changed.
        /// </summary>
        public event Action<ValueBox<T>> BoxValueChanged;

        /// <summary>
        /// Gets or sets the value.
        /// </summary>
        public abstract T Value { get; set; }

        /// <summary>
        /// Gets or sets the minimum value.
        /// </summary>
        public abstract T MinValue { get; set; }

        /// <summary>
        /// Gets or sets the maximum value.
        /// </summary>
        public abstract T MaxValue { get; set; }

        /// <summary>
        /// Gets a value indicating whether user is using a slider.
        /// </summary>
        public bool IsSliding => _isSliding;

        /// <summary>
        /// The color of the highlight to the left of the value box.
        /// </summary>
        public Color HighlightColor;

        /// <summary>
        /// Occurs when sliding starts.
        /// </summary>
        public event Action SlidingStart;

        /// <summary>
        /// Occurs when sliding ends.
        /// </summary>
        public event Action SlidingEnd;

        /// <summary>
        /// If enabled, pressing the arrow up or down key increments/ decrements the value.
        /// </summary>
        public bool ArrowKeysIncrement = true;

        /// <summary>
        /// The base increment used by the up and down arrow keys.
        /// </summary>
        public float ArrowKeyStep = 1.0f;

        /// <summary>
        /// The arrow-key increment multiplier used while holding Shift.
        /// </summary>
        public float ShiftArrowKeyMultiplier = 10.0f;

        /// <summary>
        /// The arrow-key increment multiplier used while holding Control.
        /// </summary>
        public float ControlArrowKeyMultiplier = 100.0f;

        /// <summary>
        /// The arrow-key increment multiplier used while holding Alt.
        /// </summary>
        public float AltArrowKeyMultiplier = 0.1f;

        /// <summary>
        /// Gets or sets the slider speed. A value of zero uses <see cref="ArrowKeyStep"/> for drag adjustment.
        /// </summary>
        public float SlideSpeed
        {
            get => _slideSpeed;
            set => _slideSpeed = value;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ValueBox{T}"/> class.
        /// </summary>
        /// <param name="value">The value.</param>
        /// <param name="x">The x.</param>
        /// <param name="y">The y.</param>
        /// <param name="width">The width.</param>
        /// <param name="min">The minimum.</param>
        /// <param name="max">The maximum.</param>
        /// <param name="sliderSpeed">The slider speed.</param>
        protected ValueBox(T value, float x, float y, float width, T min, T max, float sliderSpeed)
        : base(false, x, y, width)
        {
            _value = value;
            _min = min;
            _max = max;
            _slideSpeed = sliderSpeed;
            HorizontalAlignment = TextAlignment.Near;
        }

        /// <summary>
        /// Updates the text of the textbox.
        /// </summary>
        protected abstract void UpdateText();

        /// <summary>
        /// Tries the get value from the textbox text.
        /// </summary>
        protected abstract void TryGetValue();

        /// <summary>
        /// Applies the sliding delta to the value.
        /// </summary>
        /// <param name="delta">The delta (scaled).</param>
        protected abstract void ApplySliding(float delta);

        /// <summary>
        /// Called when value gets changed.
        /// </summary>
        protected virtual void OnValueChanged()
        {
            ValueChanged?.Invoke();
            BoxValueChanged?.Invoke(this);
        }

        /// <summary>
        /// Gets a value indicating whether this value box can use sliding.
        /// </summary>
        protected virtual bool CanUseSliding => true;

        /// <summary>
        /// Gets whether the active drag should snap to the value grid.
        /// </summary>
        protected bool IsGridSnapping => Root != null && Root.GetKey(KeyboardKeys.Control);

        /// <summary>
        /// Gets the value grid step used while snapping.
        /// </summary>
        protected float GridSnapStep => Mathf.Max(Mathf.Abs(ArrowKeyStep), Mathf.Epsilon);

        /// <summary>
        /// Gets the slide rectangle.
        /// </summary>
        protected virtual Rectangle SlideRect
        {
            get
            {
                return new Rectangle(Float2.Zero, Size);
            }
        }

        private void BeginSliding()
        {
            _isSlidingPending = false;
            _isSliding = true;
            StartMouseCapture(true);
            EndEditOnClick = false;

            // Hide cursor and cache location
            Cursor = CursorType.Hidden;
            _mouseClickedPosition = PointToWindow(_startSlideLocation);
            _cursorChanged = true;

            SlidingStart?.Invoke();
        }

        private float GetSlidingDelta(float mouseDelta)
        {
            var speed = Mathf.Abs(_slideSpeed) > Mathf.Epsilon ? _slideSpeed : ArrowKeyStep;
            var multiplier = Root.GetKey(KeyboardKeys.Alt) ? 0.1f : (Root.GetKey(KeyboardKeys.Shift) ? 10.0f : 1.0f);
            return Mathf.RoundToInt(mouseDelta) * speed * multiplier;
        }

        private void EndSliding()
        {
            _isSlidingPending = false;
            _isSliding = false;
            EndEditOnClick = true;
            EndMouseCapture();
            if (_cursorChanged)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }
            SlidingEnd?.Invoke();
            Defocus();
            Parent?.Focus();
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            if (_isSliding)
            {
                var style = Style.Current;
                var bounds = new Rectangle(Float2.Zero, Size);
                Render2D.FillRectangle(bounds, style.Selection);
                Render2D.DrawRectangle(bounds, style.SelectionBorder);
            }

            if (HighlightColor != Color.Transparent)
            {
                var highlightRect = new Rectangle(-3.0f, 0.0f, 3.0f, Height);
                Render2D.FillRectangle(highlightRect, HighlightColor);
            }
        }

        /// <inheritdoc />
        public override void OnGotFocus()
        {
            base.OnGotFocus();

            SelectAll();
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            // Check if was sliding
            if (_isSliding)
            {
                EndSliding();

                base.OnLostFocus();
            }
            else
            {
                base.OnLostFocus();

                // Update
                UpdateText();
            }

            Cursor = CursorType.Default;

            ResetViewOffset();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (ArrowKeysIncrement && (key == KeyboardKeys.ArrowUp || key == KeyboardKeys.ArrowDown))
            {
                bool altDown = Root.GetKey(KeyboardKeys.Alt);
                bool shiftDown = Root.GetKey(KeyboardKeys.Shift);
                bool controlDown = Root.GetKey(KeyboardKeys.Control);
                float multiplier = altDown ? AltArrowKeyMultiplier : (shiftDown ? ShiftArrowKeyMultiplier : (controlDown ? ControlArrowKeyMultiplier : 1.0f));
                float deltaValue = ArrowKeyStep * multiplier;
                float slideDelta = key == KeyboardKeys.ArrowUp ? deltaValue : -deltaValue;

                _startSlideValue = Value;
                ApplySliding(slideDelta);
                EndSliding();
                Focus();
                return true;
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && CanUseSliding && SlideRect.Contains(location))
            {
                // A click remains a normal text edit; crossing the short drag threshold starts sliding.
                _isSlidingPending = true;
                _startSlideLocation = location;
                _startSlideValue = _value;
            }

            if (button == MouseButton.Left && !IsFocused)
                _clickStartTime = Platform.TimeSeconds;

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
#if !PLATFORM_SDL
            if (_isSlidingPending && Mathf.Abs(location.X - _startSlideLocation.X) >= 2.0f)
                BeginSliding();
            if (_isSliding && !RootWindow.Window.IsMouseFlippingHorizontally)
            {
                // Update sliding
                var slideLocation = location + Root.TrackingMouseOffset;
                ApplySliding(GetSlidingDelta(slideLocation.X - _startSlideLocation.X));
                return;
            }
#endif

            // Update cursor type so user knows they can slide value
            if (CanUseSliding && SlideRect.Contains(location) && !_isSliding)
            {
                Cursor = CursorType.SizeWE;
                _cursorChanged = true;
            }
            else if (_cursorChanged && !_isSliding)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }

            base.OnMouseMove(location);
        }

#if PLATFORM_SDL
        /// <inheritdoc />
        public override void OnMouseMoveRelative(Float2 motion)
        {
            var location = Root.TrackingMouseOffset;
            if (_isSliding)
            {
                // Update sliding
                ApplySliding(GetSlidingDelta(Root.TrackingMouseOffset.X));
                return;
            }

            // Update cursor type so user knows they can slide value
            if (CanUseSliding && SlideRect.Contains(location) && !_isSliding)
            {
                Cursor = CursorType.SizeWE;
                _cursorChanged = true;
            }
            else if (_cursorChanged && !_isSliding)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }

            base.OnMouseMoveRelative(motion);
        }
#endif

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _isSliding)
            {
#if !PLATFORM_SDL
                // End sliding and return mouse to original location
                RootWindow.MousePosition = _mouseClickedPosition;
#endif
                EndSliding();
                return true;
            }

            if (button == MouseButton.Left)
                _isSlidingPending = false;

            if (button == MouseButton.Left && _clickStartTime > 0 && (Platform.TimeSeconds - _clickStartTime) < 0.2f)
            {
                _clickStartTime = -1;
                OnSelectingEnd();
                SelectAll();
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            if (_cursorChanged)
            {
                Cursor = CursorType.Default;
                _cursorChanged = false;
            }

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        protected override void OnEditBegin()
        {
            base.OnEditBegin();

            _startEditText = _text;
        }

        /// <inheritdoc />
        protected override void OnEditEnd()
        {
            if (_startEditText != _text)
            {
                // Update value
                TryGetValue();
            }
            _startEditText = null;

            base.OnEditEnd();
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
                base.OnEndMouseCapture();
            }
        }

        /// <inheritdoc />
        protected override Rectangle TextRectangle
        {
            get
            {
                return base.TextRectangle;
            }
        }

        /// <inheritdoc />
        protected override Rectangle TextClipRectangle
        {
            get
            {
                return base.TextRectangle;
            }
        }
    }
}
