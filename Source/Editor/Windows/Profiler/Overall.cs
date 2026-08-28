// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_PROFILER
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// The general profiling mode with major game performance charts and stats.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.Profiler.ProfilerMode" />
    internal sealed class Overall : ProfilerMode
    {
        private readonly SingleChart _fpsChart;
        private readonly SingleChart _frameTimeChart;
        private readonly SingleChart _updateTimeChart;
        private readonly SingleChart _drawTimeCPUChart;
        private readonly SingleChart _drawTimeGPUChart;
        private readonly SingleChart _cpuMemChart;
        private readonly SingleChart _gpuMemChart;

        public Overall(ProfilerHistoryView historyView)
        : base("Overall", historyView)
        {
            // Layout
            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = this,
            };
            var layout = new VerticalPanel
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = Margin.Zero,
                Pivot = Float2.Zero,
                IsScrollable = true,
                Parent = panel,
            };

            // Charts
            _fpsChart = new SingleChart(historyView)
            {
                Title = "FPS",
                Parent = layout,
            };
            _fpsChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _frameTimeChart = new SingleChart(historyView)
            {
                Title = "Frame Time",
                DrawBars = true,
                UseFrameBudget = true,
                FormatSample = v => (Mathf.RoundToInt(v * 10.0f) / 10.0f) + " ms",
                Parent = layout,
            };
            _frameTimeChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _updateTimeChart = new SingleChart(historyView)
            {
                Title = "Update Time",
                DrawBars = true,
                UseFrameBudget = true,
                FormatSample = v => (Mathf.RoundToInt(v * 10.0f) / 10.0f) + " ms",
                Parent = layout,
            };
            _updateTimeChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _drawTimeCPUChart = new SingleChart(historyView)
            {
                Title = "Draw Time (CPU)",
                DrawBars = true,
                UseFrameBudget = true,
                FormatSample = v => (Mathf.RoundToInt(v * 10.0f) / 10.0f) + " ms",
                Parent = layout,
            };
            _drawTimeCPUChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _drawTimeGPUChart = new SingleChart(historyView)
            {
                Title = "Draw Time (GPU)",
                DrawBars = true,
                UseFrameBudget = true,
                FormatSample = v => (Mathf.RoundToInt(v * 10.0f) / 10.0f) + " ms",
                Parent = layout,
            };
            _drawTimeGPUChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _cpuMemChart = new SingleChart(historyView)
            {
                Title = "CPU Memory",
                FormatSample = v => ((int)v) + " MB",
                Parent = layout,
            };
            _cpuMemChart.SelectedSampleChanged += OnSelectedSampleChanged;
            _gpuMemChart = new SingleChart(historyView)
            {
                Title = "GPU Memory",
                FormatSample = v => ((int)v) + " MB",
                Parent = layout,
            };
            _gpuMemChart.SelectedSampleChanged += OnSelectedSampleChanged;
        }

        /// <inheritdoc />
        public override void Clear()
        {
            _fpsChart.Clear();
            _frameTimeChart.Clear();
            _updateTimeChart.Clear();
            _drawTimeCPUChart.Clear();
            _drawTimeGPUChart.Clear();
            _cpuMemChart.Clear();
            _gpuMemChart.Clear();
        }

        /// <inheritdoc />
        public override void Update(ref SharedUpdateData sharedData)
        {
            _fpsChart.AddSample(sharedData.Stats.FPS);
            _frameTimeChart.AddSample(Mathf.Max(sharedData.Stats.UpdateTimeMs + sharedData.Stats.DrawCPUTimeMs, sharedData.Stats.DrawGPUTimeMs));
            _updateTimeChart.AddSample(sharedData.Stats.UpdateTimeMs);
            _drawTimeCPUChart.AddSample(sharedData.Stats.DrawCPUTimeMs);
            _drawTimeGPUChart.AddSample(sharedData.Stats.DrawGPUTimeMs);
            _cpuMemChart.AddSample(sharedData.Stats.ProcessMemory.UsedPhysicalMemory / 1024 / 1024);
            _gpuMemChart.AddSample(sharedData.Stats.MemoryGPU.Used / 1024 / 1024);
        }

        /// <inheritdoc />
        public override void UpdateView(int selectedFrame, bool showOnlyLastUpdateEvents)
        {
            _fpsChart.SelectedSampleIndex = selectedFrame;
            _frameTimeChart.SelectedSampleIndex = selectedFrame;
            _updateTimeChart.SelectedSampleIndex = selectedFrame;
            _drawTimeCPUChart.SelectedSampleIndex = selectedFrame;
            _drawTimeGPUChart.SelectedSampleIndex = selectedFrame;
            _cpuMemChart.SelectedSampleIndex = selectedFrame;
            _gpuMemChart.SelectedSampleIndex = selectedFrame;
        }
    }
}
#endif
