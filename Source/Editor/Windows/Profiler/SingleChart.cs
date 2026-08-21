// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// Shared navigation state for every history chart in a profiler window.
    /// </summary>
    internal sealed class ProfilerHistoryView
    {
        public int ViewEnd = -1;
        public float PointsOffset = 4;
        public int TargetFps = 60;

        public float FrameBudgetMs => 1000.0f / TargetFps;

        public void FollowLatest()
        {
            ViewEnd = -1;
        }
    }

    /// <summary>
    /// Draws simple chart.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Control" />
    internal class SingleChart : Control
    {
        internal const float DefaultHeight = TitleHeight + 60;
        private const float TitleHeight = 20;
        private const float DefaultPointsOffset = 4;
        private const float MinPointsOffset = 0.5f;
        private const float MaxPointsOffset = 32;
        private readonly SamplesBuffer<float> _samples;
        private readonly ProfilerHistoryView _historyView;
        private string _sample;
        private int _selectedSampleIndex = -1;
        private bool _isSelecting;
        private bool _isPanning;
        private Float2 _lastMouseLocation;
        private float _panRemainder;

        /// <summary>
        /// Gets or sets the chart title.
        /// </summary>
        public string Title { get; set; }

        /// <summary>
        /// True to render samples as frame bars instead of a connected line.
        /// </summary>
        public bool DrawBars { get; set; } = true;

        /// <summary>
        /// True to show the global frame-time budget and color samples against it.
        /// </summary>
        public bool UseFrameBudget { get; set; }

        /// <summary>
        /// Gets the index of the selected sample. Value -1 is used to indicate no selection (using the latest sample).
        /// </summary>
        public int SelectedSampleIndex
        {
            get => _selectedSampleIndex;
            set
            {
                value = Mathf.Clamp(value, -1, _samples.Count - 1);
                if (_selectedSampleIndex != value)
                {
                    _selectedSampleIndex = value;
                    if (value == -1)
                    {
                        // Return to the live edge when the current frame is selected.
                        _historyView.ViewEnd = -1;
                    }
                    else if (_samples.Count != 0)
                    {
                        GetVisibleSampleRange(out int first, out int last);
                        if (value < first || value > last)
                            _historyView.ViewEnd = -1;
                    }
                    _sample = _samples.Count == 0 ? string.Empty : FormatSample(_samples.Get(_selectedSampleIndex));

                    SelectedSampleChanged?.Invoke(_selectedSampleIndex);
                }
            }
        }

        /// <summary>
        /// Gets the selected sample value.
        /// </summary>
        public float SelectedSample => _samples.Get(_selectedSampleIndex);

        /// <summary>
        /// Occurs when selected sample gets changed.
        /// </summary>
        public event Action<int> SelectedSampleChanged;

        /// <summary>
        /// The handler function to format sample value for label text.
        /// </summary>
        public Func<float, string> FormatSample = (v) => v.ToString();

        /// <summary>
        /// Initializes a new instance of the <see cref="SingleChart"/> class.
        /// </summary>
        /// <param name="historyView">The navigation state shared by all charts in the profiler window.</param>
        /// <param name="initialSamplesCapacity">The initial number of samples to reserve.</param>
#if USE_PROFILER
        public SingleChart(ProfilerHistoryView historyView, int initialSamplesCapacity = ProfilerMode.InitialSamplesCapacity)
#else
        public SingleChart(ProfilerHistoryView historyView, int initialSamplesCapacity = 600)
#endif
        : base(0, 0, 100, DefaultHeight)
        {
            _historyView = historyView ?? throw new ArgumentNullException(nameof(historyView));
            _samples = new SamplesBuffer<float>(initialSamplesCapacity);
            _sample = string.Empty;
            TooltipText = "Mouse wheel: scroll history, Ctrl+wheel: zoom, middle-drag: pan";
        }

        /// <summary>
        /// Clears all the samples.
        /// </summary>
        public void Clear()
        {
            _samples.Clear();
            _sample = string.Empty;
            _selectedSampleIndex = -1;
            _historyView.ViewEnd = -1;
            _historyView.PointsOffset = DefaultPointsOffset;
        }

        /// <summary>
        /// Adds the sample value.
        /// </summary>
        /// <param name="value">The value.</param>
        public void AddSample(float value)
        {
            _samples.Add(value);
            if (_selectedSampleIndex == -1)
                _sample = FormatSample(value);
        }

        private void GetVisibleSampleRange(out int first, out int last)
        {
            int visibleCount = Mathf.Max((int)(Width / _historyView.PointsOffset) + 1, 2);
            if (_historyView.ViewEnd >= 0)
            {
                last = Mathf.Min(_historyView.ViewEnd, _samples.Count - 1);
            }
            else if (_selectedSampleIndex == -1)
            {
                last = _samples.Count - 1;
            }
            else
            {
                last = Mathf.Clamp(_selectedSampleIndex + visibleCount / 2, visibleCount - 1, _samples.Count - 1);
            }
            first = Mathf.Max(last - visibleCount + 1, 0);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            var style = Style.Current;
            float chartHeight = Height - TitleHeight;

            // Draw chart
            if (_samples.Count > 0)
            {
                var chartRect = new Rectangle(0, 0, Width, chartHeight);
                Render2D.PushClip(ref chartRect);

                GetVisibleSampleRange(out int firstSample, out int lastSample);
                Rectangle selectedFrameRect = Rectangle.Empty;
                if (_selectedSampleIndex != -1)
                {
                    float selectedX = Width - (lastSample - _selectedSampleIndex) * _historyView.PointsOffset;
                    float selectedWidth = Mathf.Max(_historyView.PointsOffset, 4.0f);
                    float selectedCenter = selectedX - _historyView.PointsOffset * 0.5f;
                    selectedFrameRect = new Rectangle(selectedCenter - selectedWidth * 0.5f, 0, selectedWidth, chartHeight);
                    Render2D.FillRectangle(selectedFrameRect, style.Selection.AlphaMultiplied(0.32f));
                }

                float maxValue = _samples[firstSample];
                for (int i = firstSample + 1; i <= lastSample; i++)
                    maxValue = Mathf.Max(maxValue, _samples[i]);
                if (UseFrameBudget)
                    maxValue = Mathf.Max(maxValue, _historyView.FrameBudgetMs);

                Color chartColor = style.BackgroundSelected;
                var chartRoot = chartRect.BottomRight;
                float samplesRange = Mathf.Max(maxValue * 1.1f, Mathf.Epsilon);
                float samplesCoeff = -chartHeight / samplesRange;
                if (DrawBars)
                {
                    float barWidth = Mathf.Max(_historyView.PointsOffset - 1.0f, 1.0f);
                    for (int i = lastSample; i >= firstSample; i--)
                    {
                        float x = Width - (lastSample - i) * _historyView.PointsOffset;
                        float y = _samples[i] * samplesCoeff;
                        Color barColor = i == _selectedSampleIndex ? style.SelectionBorder : GetFrameTimeColor(_samples[i], chartColor);
                        Render2D.FillRectangle(new Rectangle(x - barWidth, chartHeight + y, barWidth, -y), barColor);
                    }
                }

                if (UseFrameBudget)
                {
                    float budgetY = chartHeight + _historyView.FrameBudgetMs * samplesCoeff;
                    var budgetColor = Color.Orange.AlphaMultiplied(0.8f);
                    Render2D.DrawLine(new Float2(0, budgetY), new Float2(Width, budgetY), budgetColor, 1.0f);
                    Render2D.DrawText(style.FontSmall, $"{_historyView.FrameBudgetMs:0.#} ms", new Rectangle(Width - 64, budgetY - 17, 60, 16), budgetColor, TextAlignment.Far, TextAlignment.Center);
                }
                else
                {
                    var posPrev = chartRoot + new Float2(0, _samples[lastSample] * samplesCoeff);
                    float posX = -_historyView.PointsOffset;
                    for (int i = lastSample - 1; i >= firstSample; i--)
                    {
                        float sample = _samples[i];
                        var pos = chartRoot + new Float2(posX, sample * samplesCoeff);
                        Render2D.DrawLine(posPrev, pos, chartColor);
                        posPrev = pos;
                        posX -= _historyView.PointsOffset;
                    }
                }

                if (_selectedSampleIndex != -1)
                    Render2D.DrawRectangle(selectedFrameRect, style.SelectionBorder, 1.5f);

                Render2D.PopClip();
            }

            // Draw title
            var headerRect = new Rectangle(0, chartHeight, Width, TitleHeight);
            var headerTextRect = new Rectangle(2, chartHeight, Width - 4, TitleHeight);
            Render2D.FillRectangle(headerRect, style.BackgroundNormal);
            string title = Title;
            if (_samples.Count != 0)
            {
                GetVisibleSampleRange(out int firstSample, out int lastSample);
                title += $"  [{firstSample + 1:N0}-{lastSample + 1:N0} / {_samples.Count:N0}]";
                if (_selectedSampleIndex != -1)
                    title += $"  Selected frame {_selectedSampleIndex + 1:N0}";
            }
            Render2D.DrawText(style.FontMedium, title, headerTextRect, style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
            Render2D.DrawText(style.FontMedium, _sample, headerTextRect, style.Foreground, TextAlignment.Far, TextAlignment.Center);
        }

        private Color GetFrameTimeColor(float value, Color fallback)
        {
            if (!UseFrameBudget)
                return fallback;
            float ratio = value / _historyView.FrameBudgetMs;
            if (ratio > 1.5f)
                return Color.Red * 0.85f;
            if (ratio > 1.0f)
                return Color.Orange * 0.85f;
            if (ratio > 0.8f)
                return Color.Yellow * 0.75f;
            return Color.Green * 0.65f;
        }

        private void OnClick(ref Float2 location)
        {
            GetVisibleSampleRange(out int firstSample, out int lastSample);
            int index = lastSample - (int)((Width - location.X) / _historyView.PointsOffset);
            SelectedSampleIndex = Mathf.Clamp(index, firstSample, lastSample);
        }

        private void ClampView()
        {
            if (_historyView.ViewEnd >= 0)
                _historyView.ViewEnd = Mathf.Clamp(_historyView.ViewEnd, 0, _samples.Count - 1);
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && location.Y < (Height - TitleHeight))
            {
                GetVisibleSampleRange(out _, out _historyView.ViewEnd);
                _isSelecting = true;
                OnClick(ref location);
                StartMouseCapture();
                return true;
            }
            if (button == MouseButton.Middle && location.Y < (Height - TitleHeight))
            {
                GetVisibleSampleRange(out _, out _historyView.ViewEnd);
                _isPanning = true;
                _lastMouseLocation = location;
                _panRemainder = 0;
                StartMouseCapture();
                return true;
            }

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            if (_isSelecting)
            {
                OnClick(ref location);
            }
            else if (_isPanning)
            {
                _panRemainder += location.X - _lastMouseLocation.X;
                int samplesDelta = (int)(_panRemainder / _historyView.PointsOffset);
                if (samplesDelta != 0)
                {
                    _historyView.ViewEnd -= samplesDelta;
                    _panRemainder -= samplesDelta * _historyView.PointsOffset;
                    ClampView();
                }
                _lastMouseLocation = location;
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left && _isSelecting)
            {
                _isSelecting = false;
                EndMouseCapture();
                return true;
            }
            if (button == MouseButton.Middle && _isPanning)
            {
                _isPanning = false;
                EndMouseCapture();
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            _isSelecting = false;
            _isPanning = false;
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            if (_samples.Count == 0 || location.Y >= Height - TitleHeight)
                return base.OnMouseWheel(location, delta);

            GetVisibleSampleRange(out _, out int oldLast);
            bool wasFollowingLatest = _historyView.ViewEnd == -1;
            if (Root.GetKey(KeyboardKeys.Control))
            {
                // Keep the sample under the pointer stable while changing the horizontal scale.
                float anchorDistance = (Width - location.X) / _historyView.PointsOffset;
                int anchorSample = oldLast - Mathf.RoundToInt(anchorDistance);
                _historyView.PointsOffset = Mathf.Clamp(_historyView.PointsOffset * (delta > 0 ? 1.25f : 0.8f), MinPointsOffset, MaxPointsOffset);
                if (!wasFollowingLatest)
                    _historyView.ViewEnd = anchorSample + Mathf.RoundToInt((Width - location.X) / _historyView.PointsOffset);
            }
            else
            {
                int visibleSamples = Mathf.Max((int)(Width / _historyView.PointsOffset), 1);
                _historyView.ViewEnd = oldLast - Mathf.RoundToInt(delta * Mathf.Max(visibleSamples / 10.0f, 1.0f));
            }
            ClampView();
            return true;
        }
    }
}
