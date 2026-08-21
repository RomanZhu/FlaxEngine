// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// Events timeline control.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Panel" />
    public class Timeline : Panel
    {
        private static readonly float[] RulerStepsMs = { 0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000 };
        private bool _isPanning;
        private Float2 _lastPanLocation;
        private float _lastDurationMs;
        private Panel _verticalPanPanel;
        private Panel _preservedVerticalPanel;
        private float _preservedHorizontalScroll;
        private float _preservedVerticalScroll;
        private bool _preservingNavigation;
        private readonly Dictionary<long, Event> _eventsByKey = new Dictionary<long, Event>();
        private Event _selectedEvent;
        private long _selectedSourceKey = long.MinValue;

        private const float MinPixelsPerMillisecond = 0.01f;
        private const float MaxPixelsPerMillisecond = 20000.0f;

        /// <summary>
        /// The height of the fixed time ruler.
        /// </summary>
        public const float RulerHeight = 24.0f;

        /// <summary>
        /// The horizontal position where timing data begins after track labels.
        /// </summary>
        public float ContentOffset;

        /// <summary>
        /// Number of horizontal pixels used to represent one millisecond.
        /// </summary>
        public float PixelsPerMillisecond = 100.0f;

        /// <summary>
        /// True when the selected frame should be fitted to the available width.
        /// </summary>
        public bool AutoFit = true;

        /// <summary>
        /// Optional global frame budget displayed on the ruler. Set to zero to hide it.
        /// </summary>
        public float FrameBudgetMs;

        /// <summary>
        /// True to allow middle-dragging vertically. Profiler timing views leave this disabled.
        /// </summary>
        public bool AllowVerticalPanning;

        /// <summary>
        /// Fits the given duration to the available timing area unless the user has manually navigated the timeline.
        /// </summary>
        public void FitToDuration(float durationMs)
        {
            _lastDurationMs = durationMs;
            if (!AutoFit || durationMs <= Mathf.Epsilon)
                return;
            float oldScale = PixelsPerMillisecond;
            float newScale = Mathf.Clamp((Width - ContentOffset - 24.0f) / durationMs, MinPixelsPerMillisecond, MaxPixelsPerMillisecond);
            float ratio = newScale / oldScale;
            PixelsPerMillisecond = newScale;
            if (!Mathf.NearEqual(ratio, 1.0f))
            {
                UpdateEventScale();
                PerformLayout();
            }
            if (HScrollBar != null)
                HScrollBar.TargetValue = 0;
        }

        private void UpdateEventScale()
        {
            for (int i = 0; i < Children.Count; i++)
            {
                if (Children[i] is Event e)
                    e.UpdateHorizontal(ContentOffset, PixelsPerMillisecond);
            }
        }

        private Panel FindVerticalPanPanel()
        {
            if (VScrollBar != null && VScrollBar.Maximum > VScrollBar.Minimum)
                return this;
            for (var parent = Parent; parent != null; parent = parent.Parent)
            {
                if (parent is Panel panel && panel.VScrollBar != null && panel.VScrollBar.Maximum > panel.VScrollBar.Minimum)
                    return panel;
            }
            return null;
        }

        /// <summary>
        /// Preserves the current navigation while live event controls are rebuilt.
        /// </summary>
        public void BeginContentUpdate()
        {
            if (_preservingNavigation)
                return;
            _preservingNavigation = true;
            _preservedHorizontalScroll = HScrollBar?.TargetValue ?? 0.0f;
            _preservedVerticalPanel = AllowVerticalPanning ? FindVerticalPanPanel() : null;
            _preservedVerticalScroll = _preservedVerticalPanel?.VScrollBar?.TargetValue ?? 0.0f;
            _eventsByKey.Clear();
            _selectedEvent = null;
        }

        /// <summary>
        /// Restores navigation after live event controls have been rebuilt.
        /// </summary>
        public void EndContentUpdate()
        {
            if (!_preservingNavigation)
                return;
            _preservingNavigation = false;
            if (!AutoFit && HScrollBar != null)
                HScrollBar.TargetValue = _preservedHorizontalScroll;
            if (_preservedVerticalPanel?.VScrollBar != null)
                _preservedVerticalPanel.VScrollBar.TargetValue = _preservedVerticalScroll;
            _preservedVerticalPanel = null;
        }

        internal void RegisterEvent(Event timelineEvent)
        {
            _eventsByKey[timelineEvent.SourceKey] = timelineEvent;
            timelineEvent.IsSelected = timelineEvent.SourceKey == _selectedSourceKey;
            if (timelineEvent.IsSelected)
                _selectedEvent = timelineEvent;
        }

        internal void LinkRow(long sourceKey, Row row)
        {
            if (!_eventsByKey.TryGetValue(sourceKey, out var timelineEvent))
                return;
            timelineEvent.LinkedRow = row;
            row.Selected = timelineEvent.IsSelected;
        }

        private void SelectEvent(Event timelineEvent)
        {
            if (_selectedEvent != null)
            {
                _selectedEvent.IsSelected = false;
                if (_selectedEvent.LinkedRow != null)
                    _selectedEvent.LinkedRow.Selected = false;
            }
            _selectedEvent = timelineEvent;
            _selectedSourceKey = timelineEvent.SourceKey;
            timelineEvent.IsSelected = true;
            if (timelineEvent.LinkedRow != null)
            {
                timelineEvent.LinkedRow.RevealHierarchy();
                timelineEvent.LinkedRow.Selected = true;
                timelineEvent.LinkedRow.Focus();
                for (Control parent = timelineEvent.LinkedRow.Parent; parent != null; parent = parent.Parent)
                {
                    if (parent is Panel panel && panel.VScrollBar != null)
                    {
                        panel.ScrollViewTo(timelineEvent.LinkedRow, true);
                        break;
                    }
                }
            }
        }

        /// <summary>
        /// Single timeline event control.
        /// </summary>
        /// <seealso cref="ContainerControl" />
        public class Event : ContainerControl
        {
            private static readonly Color[] Colors =
            {
                new Color(0.8f, 0.894117653f, 0.709803939f, 1f),
                new Color(0.1254902f, 0.698039234f, 0.6666667f, 1f),
                new Color(0.4831376f, 0.6211768f, 0.0219608f, 1f),
                new Color(0.3827448f, 0.2886272f, 0.5239216f, 1f),
                new Color(0.8f, 0.4423528f, 0f, 1f),
                new Color(0.4486272f, 0.4078432f, 0.050196f, 1f),
                new Color(0.4831376f, 0.6211768f, 0.0219608f, 1f),
                new Color(0.4831376f, 0.6211768f, 0.0219608f, 1f),
                new Color(0.2070592f, 0.5333336f, 0.6556864f, 1f),
                new Color(0.8f, 0.4423528f, 0f, 1f),
                new Color(0.4486272f, 0.4078432f, 0.050196f, 1f),
                new Color(0.7749016f, 0.6368624f, 0.0250984f, 1f),
                new Color(0.5333336f, 0.16f, 0.0282352f, 1f),
                new Color(0.3827448f, 0.2886272f, 0.5239216f, 1f),
                new Color(0.478431374f, 0.482352942f, 0.117647059f, 1f),
                new Color(0.9411765f, 0.5019608f, 0.5019608f, 1f),
                new Color(0.6627451f, 0.6627451f, 0.6627451f, 1f),
                new Color(0.545098066f, 0f, 0.545098066f, 1f),
            };

            private Color _color;
            private string _name;
            private float _nameLength = -1;

            internal float StartTimeMs;
            internal float DurationMs;
            internal long SourceKey;
            internal Row LinkedRow;
            internal bool IsSelected;

            internal void UpdateHorizontal(float contentOffset, float pixelsPerMillisecond)
            {
                X = contentOffset + StartTimeMs * pixelsPerMillisecond;
                Width = Mathf.Max(DurationMs * pixelsPerMillisecond, 1.0f);
            }

            /// <summary>
            /// The default height of the event.
            /// </summary>
            public const float DefaultHeight = 25.0f;

            /// <summary>
            /// Gets or sets the event name.
            /// </summary>
            public string Name
            {
                get => _name;
                set
                {
                    _name = value;
                    _nameLength = -1;
                }
            }

            /// <inheritdoc />
            protected override void OnParentChangedInternal()
            {
                base.OnParentChangedInternal();

                int key = (HasParent ? Parent.GetChildIndex(this) : 1) * (string.IsNullOrEmpty(Name) ? 1 : Name[0]);
                _color = Colors[key % Colors.Length] * 0.8f;
            }

            /// <inheritdoc />
            public override void Draw()
            {
                base.Draw();

                var style = Style.Current;
                var bounds = new Rectangle(Float2.Zero, Size);
                Color color = _color;
                if (IsMouseOver)
                    color *= 1.1f;

                Render2D.FillRectangle(bounds, color);
                Render2D.DrawRectangle(bounds, color * 0.5f);
                if (IsSelected)
                    Render2D.DrawRectangle(bounds, style.SelectionBorder, 2.0f);

                if (Parent is Timeline timeline && timeline.FrameBudgetMs > Mathf.Epsilon)
                {
                    float ratio = DurationMs / timeline.FrameBudgetMs;
                    Color severity = ratio >= 0.25f ? Color.Red : ratio >= 0.10f ? Color.Orange : ratio >= 0.05f ? Color.Yellow : Color.Green;
                    Render2D.FillRectangle(new Rectangle(0, Height - 2, Width, 2), severity.AlphaMultiplied(ratio >= 0.05f ? 0.9f : 0.45f));
                }

                if (_nameLength < 0 && style.FontMedium)
                    _nameLength = style.FontMedium.MeasureText(_name).X;

                if (_nameLength < bounds.Width + 4)
                {
                    Render2D.PushClip(bounds);
                    Render2D.DrawText(style.FontMedium, _name, bounds, Style.Current.Foreground, TextAlignment.Center, TextAlignment.Center);
                    Render2D.PopClip();
                }
            }

            /// <inheritdoc />
            public override void OnMouseEnter(Float2 location)
            {
                if (LinkedRow != null)
                    LinkedRow.Highlighted = true;
                base.OnMouseEnter(location);
            }

            /// <inheritdoc />
            public override void OnMouseLeave()
            {
                if (LinkedRow != null)
                    LinkedRow.Highlighted = false;
                base.OnMouseLeave();
            }

            /// <inheritdoc />
            public override bool OnMouseWheel(Float2 location, float delta)
            {
                if (Parent is Timeline timeline)
                    return timeline.OnMouseWheel(PointToParent(timeline, location), delta);
                return base.OnMouseWheel(location, delta);
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Middle && Parent is Timeline timeline)
                    return timeline.OnMouseDown(PointToParent(timeline, location), button);
                if (button == MouseButton.Left && Parent is Timeline owner)
                {
                    owner.SelectEvent(this);
                    return true;
                }
                return base.OnMouseDown(location, button);
            }
        }

        /// <summary>
        /// Timeline track label
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
        public class TrackLabel : ContainerControl
        {
            /// <summary>
            /// Gets or sets the name.
            /// </summary>
            public string Name { get; set; }

            /// <inheritdoc />
            public override void Draw()
            {
                base.Draw();

                var style = Style.Current;
                var rect = new Rectangle(Float2.Zero, Size);
                Render2D.PushClip(rect);
                Render2D.DrawText(style.FontMedium, Name, rect, Style.Current.Foreground, TextAlignment.Center, TextAlignment.Center, TextWrapping.WrapChars);
                Render2D.PopClip();
            }

            /// <inheritdoc />
            public override bool OnMouseWheel(Float2 location, float delta)
            {
                if (Parent is Timeline timeline)
                    return timeline.OnMouseWheel(PointToParent(timeline, location), delta);
                return base.OnMouseWheel(location, delta);
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Middle && Parent is Timeline timeline)
                    return timeline.OnMouseDown(PointToParent(timeline, location), button);
                return base.OnMouseDown(location, button);
            }
        }

        /// <summary>
        /// Gets the events container control. Use it to remove/add events to the timeline.
        /// </summary>
        public ContainerControl EventsContainer => this;

        /// <summary>
        /// Initializes a new instance of the <see cref="Timeline"/> class.
        /// </summary>
        public Timeline()
        : base(ScrollBars.Both)
        {
            TooltipText = "Ctrl+mouse wheel: zoom timing, middle-drag: pan";
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            if (Root != null && Root.GetKey(KeyboardKeys.Control) && Math.Abs(delta) > Mathf.Epsilon)
            {
                float oldScale = PixelsPerMillisecond;
                float newScale = Mathf.Clamp(oldScale * (delta > 0 ? 1.25f : 0.8f), MinPixelsPerMillisecond, MaxPixelsPerMillisecond);
                if (Mathf.NearEqual(oldScale, newScale))
                    return true;
                float scroll = HScrollBar?.TargetValue ?? -ViewOffset.X;
                float anchorX = Mathf.Clamp(location.X, ContentOffset, Width);
                float timeAtCursor = (scroll + anchorX - ContentOffset) / oldScale;

                AutoFit = false;
                PixelsPerMillisecond = newScale;
                UpdateEventScale();
                PerformLayout();

                if (HScrollBar != null)
                    HScrollBar.TargetValue = ContentOffset + timeAtCursor * newScale - anchorX;
                return true;
            }
            return base.OnMouseWheel(location, delta);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Middle)
            {
                AutoFit = false;
                _isPanning = true;
                _lastPanLocation = location;
                _verticalPanPanel = AllowVerticalPanning ? FindVerticalPanPanel() : null;
                StartMouseCapture();
                Cursor = AllowVerticalPanning ? CursorType.SizeAll : CursorType.SizeWE;
                return true;
            }
            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && location.Y <= RulerHeight)
            {
                AutoFit = true;
                FitToDuration(_lastDurationMs);
                return true;
            }
            return base.OnMouseDoubleClick(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            if (_isPanning)
            {
                var delta = location - _lastPanLocation;
                if (HScrollBar != null)
                    HScrollBar.TargetValue -= delta.X;
                if (_verticalPanPanel?.VScrollBar != null)
                    _verticalPanPanel.VScrollBar.TargetValue -= delta.Y;
                _lastPanLocation = location;
                return;
            }
            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Middle && _isPanning)
            {
                EndMouseCapture();
                return true;
            }
            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            _isPanning = false;
            _verticalPanPanel = null;
            Cursor = CursorType.Default;
            base.OnEndMouseCapture();
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            var style = Style.Current;
            float scroll = HScrollBar?.Value ?? -ViewOffset.X;
            float stepMs = RulerStepsMs[RulerStepsMs.Length - 1];
            for (int i = 0; i < RulerStepsMs.Length; i++)
            {
                if (RulerStepsMs[i] * PixelsPerMillisecond >= 80.0f)
                {
                    stepMs = RulerStepsMs[i];
                    break;
                }
            }

            float firstMs = Mathf.Max(Mathf.Floor((scroll - ContentOffset) / PixelsPerMillisecond / stepMs) * stepMs, 0.0f);
            var gridColor = style.ForegroundDisabled.AlphaMultiplied(0.18f);
            for (float timeMs = firstMs; ; timeMs += stepMs)
            {
                float x = ContentOffset + timeMs * PixelsPerMillisecond - scroll;
                if (x > Width)
                    break;
                if (x >= ContentOffset)
                {
                    Render2D.DrawLine(new Float2(x, RulerHeight), new Float2(x, Height), gridColor);
                    Render2D.DrawText(style.FontMedium, $"{timeMs:0.###} ms", new Rectangle(x + 4, 0, 76, RulerHeight), style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
                }
            }

            if (FrameBudgetMs > Mathf.Epsilon)
            {
                float budgetX = ContentOffset + FrameBudgetMs * PixelsPerMillisecond - scroll;
                if (budgetX >= ContentOffset && budgetX <= Width)
                {
                    var budgetColor = Color.Orange.AlphaMultiplied(0.8f);
                    Render2D.DrawLine(new Float2(budgetX, 0), new Float2(budgetX, Height), budgetColor, 1.5f);
                    Render2D.DrawText(style.FontSmall, $"Budget {FrameBudgetMs:0.#} ms", new Rectangle(budgetX + 4, 0, 100, RulerHeight), budgetColor, TextAlignment.Near, TextAlignment.Center);
                }
            }

            if (_eventsByKey.Count == 0 && _lastDurationMs > Mathf.Epsilon)
            {
                Render2D.DrawText(style.FontMedium, "No timing events captured for this frame", new Rectangle(ContentOffset, RulerHeight, Width - ContentOffset, Mathf.Max(Height - RulerHeight, 0.0f)), style.ForegroundDisabled, TextAlignment.Center, TextAlignment.Center);
            }

            Render2D.FillRectangle(new Rectangle(0, 0, ContentOffset, RulerHeight), style.SecondaryBackground);
            Render2D.DrawLine(new Float2(0, RulerHeight - 1), new Float2(Width, RulerHeight - 1), style.ForegroundDisabled.AlphaMultiplied(0.4f));
        }
    }
}
