// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Tabs;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// Editor tool window for profiling games.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.EditorWindow" />
    public sealed class ProfilerWindow : EditorWindow
    {
#if USE_PROFILER
        private readonly ToolStripButton _liveRecordingButton;
        private readonly ToolStripButton _clearButton;
        private readonly ToolStripButton _prevFrameButton;
        private readonly ToolStripButton _nextFrameButton;
        private readonly ToolStripButton _lastFrameButton;
        private readonly ToolStripButton _previousPeakButton;
        private readonly ToolStripButton _nextPeakButton;
        private readonly ToolStripButton _showOnlyLastUpdateEventsButton;
        private readonly ToolStripButton _cpuTracksButton;
        private readonly ComboBox _frameBudget;
        private readonly ToolStripButton _statusLabel;
        private readonly Tabs _tabs;
        private readonly ProfilerHistoryView _historyView = new ProfilerHistoryView();
        private readonly SamplesBuffer<float> _frameTimes = new SamplesBuffer<float>();
        private readonly List<int> _peakFrames = new List<int>();
        private CPU _cpuMode;
        private int _frameIndex = -1;
        private int _selectedPeakFrame = -1;
        private float _selectedPeakTime;
        private bool _peaksDirty = true;
        private int _framesCount;
        private bool _showOnlyLastUpdateEvents = true;
        private long _lastManagedMemory = 0;
        private long _lastManagedMemoryProfiler = 0;

        /// <summary>
        /// Gets or sets a value indicating whether live events recording is enabled.
        /// </summary>
        public bool LiveRecording
        {
            get => _liveRecordingButton.Checked;
            set
            {
                if (value != LiveRecording)
                {
                    _liveRecordingButton.Checked = value;
                    OnLiveRecordingChanged();
                }
            }
        }

        /// <summary>
        /// Gets or sets the index of the selected frame to view (note: some view modes may not use it). -1 for the last frame.
        /// </summary>
        public int ViewFrameIndex
        {
            get => _frameIndex;
            set
            {
                value = Mathf.Clamp(value, -1, _framesCount - 1);
                if (_frameIndex != value)
                {
                    _frameIndex = value;
                    _selectedPeakFrame = -1;
                    UpdateButtons();
                    UpdateView();
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether show only last update events and hide events from the other callbacks (e.g. draw or fixed update).
        /// </summary>
        public bool ShowOnlyLastUpdateEvents
        {
            get => _showOnlyLastUpdateEvents;
            set
            {
                if (_showOnlyLastUpdateEvents != value)
                {
                    _showOnlyLastUpdateEvents = value;
                    UpdateButtons();
                    UpdateView();
                }
            }
        }
#endif

        /// <summary>
        /// Initializes a new instance of the <see cref="ProfilerWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public ProfilerWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Profiler";

#if USE_PROFILER
            var toolstrip = new ToolStrip
            {
                Parent = this,
            };
            _liveRecordingButton = toolstrip.AddButton(editor.Icons.Play64, "Record");
            _liveRecordingButton.LinkTooltip("Start or stop profiler recording. Recording automatically suspends while Play Mode is paused.");
            _liveRecordingButton.AutoCheck = true;
            _liveRecordingButton.Clicked += OnLiveRecordingChanged;
            _clearButton = toolstrip.AddButton(editor.Icons.DeleteFile64, "Clear", Clear);
            _clearButton.LinkTooltip("Clear all captured profiler data");
            toolstrip.AddSeparator();
            _prevFrameButton = toolstrip.AddButton(editor.Icons.Left64, PreviousFrame);
            _prevFrameButton.LinkTooltip("Previous frame");
            _nextFrameButton = toolstrip.AddButton(editor.Icons.Right64, () => ViewFrameIndex++);
            _nextFrameButton.LinkTooltip("Next frame");
            _lastFrameButton = toolstrip.AddButton(editor.Icons.Skip64, FollowLatestFrame);
            _lastFrameButton.LinkTooltip("Follow the latest frame while preserving the current zoom level");
            _previousPeakButton = toolstrip.AddButton(editor.Icons.Left64, "Peak", () => NavigatePeak(-1));
            _previousPeakButton.LinkTooltip("Previous significant frame-time peak (wraps around)");
            _nextPeakButton = toolstrip.AddButton(editor.Icons.Right64, "Peak", () => NavigatePeak(1));
            _nextPeakButton.LinkTooltip("Next significant frame-time peak (wraps around)");
            toolstrip.AddSeparator();
            _showOnlyLastUpdateEventsButton = toolstrip.AddButton(editor.Icons.CenterView64, () => ShowOnlyLastUpdateEvents = !ShowOnlyLastUpdateEvents);
            _showOnlyLastUpdateEventsButton.LinkTooltip("Show only last update events and hide events from the other callbacks (e.g. draw or fixed update)");
            _cpuTracksButton = toolstrip.AddButton("CPU Tracks", ShowCpuTracksMenu);
            _cpuTracksButton.DrawMenuChevron = true;
            _cpuTracksButton.LinkTooltip("Choose which CPU thread tracks are visible");
            _frameBudget = toolstrip.AddItem(new ComboBox
            {
                Width = 60,
                Height = toolstrip.ItemsHeight,
                TooltipText = "Global frame-time budget used by profiler charts, timelines, and timing tables.",
            }, ToolStripAnchor.Left, "Profiler.FrameBudget");
            _frameBudget.SetItems(new[] { "30 FPS", "60 FPS", "120 FPS" });
            _frameBudget.SelectedIndex = 1;
            _frameBudget.SelectedIndexChanged += OnFrameBudgetChanged;
            toolstrip.AddSeparator();
            _statusLabel = toolstrip.AddButton("Idle");
            _statusLabel.DrawAsTextLabel = true;
            _statusLabel.LinkTooltip("Profiler recording state and selected frame");

            _tabs = new Tabs
            {
                Orientation = Orientation.Vertical,
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, toolstrip.Bottom, 0),
                TabsSize = new Float2(120, 32),
                Parent = this
            };
            _tabs.SelectedTabChanged += OnSelectedTabChanged;

            FlaxEditor.Utilities.Utils.SetupCommonInputActions(this);
            InputActions.Bindings.RemoveAll(x => x.Callback == this.FocusOrShow);
            InputActions.Add(options => options.ProfilerWindow, Hide);
#endif
        }

#if USE_PROFILER
        private void OnLiveRecordingChanged()
        {
            UpdateRecordingState();
        }

        private void FollowLatestFrame()
        {
            _historyView.FollowLatest();
            _frameIndex = -1;
            _selectedPeakFrame = -1;
            UpdateButtons();
            UpdateView();
        }

        private void PreviousFrame()
        {
            ViewFrameIndex = _frameIndex == -1 ? _framesCount - 2 : _frameIndex - 1;
        }

        private void RebuildPeakFrames()
        {
            if (!_peaksDirty)
                return;

            _peaksDirty = false;
            _peakFrames.Clear();
            int count = _frameTimes.Count;
            if (count < 3)
                return;

            double sum = 0;
            double sumSquared = 0;
            for (int i = 0; i < count; i++)
            {
                float value = _frameTimes[i];
                sum += value;
                sumSquared += value * value;
            }
            float average = (float)(sum / count);
            float deviation = Mathf.Sqrt(Mathf.Max((float)(sumSquared / count - average * average), 0.0f));
            float threshold = average + Mathf.Max(deviation, average * 0.25f);
            int highestLocalPeak = -1;
            for (int i = 1; i < count - 1; i++)
            {
                float value = _frameTimes[i];
                if (value >= _frameTimes[i - 1] && value > _frameTimes[i + 1])
                {
                    if (highestLocalPeak == -1 || value > _frameTimes[highestLocalPeak])
                        highestLocalPeak = i;
                    if (value >= threshold)
                        _peakFrames.Add(i);
                }
            }

            // Short or very stable captures might not cross the adaptive threshold. Keep their highest local peak navigable.
            if (_peakFrames.Count == 0 && highestLocalPeak != -1)
                _peakFrames.Add(highestLocalPeak);
        }

        private void NavigatePeak(int direction)
        {
            RebuildPeakFrames();
            if (_peakFrames.Count == 0)
                return;

            int peakListIndex;
            if (_frameIndex == -1)
            {
                peakListIndex = direction < 0 ? _peakFrames.Count - 1 : 0;
            }
            else
            {
                peakListIndex = direction < 0 ? _peakFrames.Count - 1 : 0;
                for (int i = 0; i < _peakFrames.Count; i++)
                {
                    if (direction > 0 && _peakFrames[i] > _frameIndex)
                    {
                        peakListIndex = i;
                        break;
                    }
                    if (direction < 0 && _peakFrames[i] < _frameIndex)
                        peakListIndex = i;
                }
            }

            int frame = _peakFrames[peakListIndex];
            _historyView.ViewEnd = -1;
            _frameIndex = frame;
            _selectedPeakFrame = frame;
            _selectedPeakTime = _frameTimes[frame];
            UpdateButtons();
            UpdateView();
        }

        private void ShowCpuTracksMenu()
        {
            if (_cpuMode == null)
                return;
            var menu = new ContextMenu
            {
                MinimumWidth = 190,
            };
            menu.AddButton("Main thread", () => _cpuMode.ShowMainThread = !_cpuMode.ShowMainThread).Checked = _cpuMode.ShowMainThread;
            menu.AddButton("Job System threads", () => _cpuMode.ShowJobSystemThreads = !_cpuMode.ShowJobSystemThreads).Checked = _cpuMode.ShowJobSystemThreads;
            menu.AddButton("Other threads", () => _cpuMode.ShowOtherThreads = !_cpuMode.ShowOtherThreads).Checked = _cpuMode.ShowOtherThreads;
            menu.AddSeparator();
            menu.AddButton("Show all", () =>
            {
                _cpuMode.ShowMainThread = true;
                _cpuMode.ShowJobSystemThreads = true;
                _cpuMode.ShowOtherThreads = true;
            });
            menu.AddButton("Main only", () =>
            {
                _cpuMode.ShowMainThread = true;
                _cpuMode.ShowJobSystemThreads = false;
                _cpuMode.ShowOtherThreads = false;
            });
            menu.Show(_cpuTracksButton, new Float2(0, _cpuTracksButton.Height));
        }

        private void OnFrameBudgetChanged(ComboBox comboBox)
        {
            _historyView.TargetFps = comboBox.SelectedIndex == 0 ? 30 : comboBox.SelectedIndex == 2 ? 120 : 60;
            UpdateView();
        }

        private bool UpdateRecordingState()
        {
            bool gamePaused = Editor.StateMachine.IsPlayMode && Editor.StateMachine.PlayingState.IsPaused;
            bool isCollecting = LiveRecording && !gamePaused;
            if (ProfilingTools.Enabled != isCollecting)
                ProfilingTools.Enabled = isCollecting;

            if (!LiveRecording)
            {
                SetRecordingButton(Editor.Icons.Play64, "Record");
            }
            else if (gamePaused)
            {
                SetRecordingButton(Editor.Icons.Pause64, "Paused");
            }
            else
            {
                SetRecordingButton(Editor.Icons.Stop64, "Recording");
            }
            UpdateButtons();
            return gamePaused;
        }

        private void SetRecordingButton(SpriteHandle icon, string text)
        {
            if (!_liveRecordingButton.Icon.Equals(icon))
                _liveRecordingButton.Icon = icon;
            if (_liveRecordingButton.Text != text)
                _liveRecordingButton.Text = text;
        }

        /// <summary>
        /// Adds the mode.
        /// </summary>
        /// <remarks>
        /// To remove the mode simply call <see cref="Control.Dispose"/> on mode.
        /// </remarks>
        /// <param name="mode">The mode.</param>
        public void AddMode(ProfilerMode mode)
        {
            if (mode == null)
                throw new ArgumentNullException();
            mode.Init();
            _tabs.AddTab(mode);
            mode.SelectedSampleChanged += ModeOnSelectedSampleChanged;
        }

        private void ModeOnSelectedSampleChanged(int frameIndex)
        {
            ViewFrameIndex = frameIndex;
        }

        /// <summary>
        /// Clears data.
        /// </summary>
        public void Clear()
        {
            _frameIndex = -1;
            _framesCount = 0;
            _selectedPeakFrame = -1;
            _selectedPeakTime = 0;
            _frameTimes.Clear();
            _peakFrames.Clear();
            _peaksDirty = true;
            _lastManagedMemory = 0;
            _lastManagedMemoryProfiler = 0;
            for (int i = 0; i < _tabs.ChildrenCount; i++)
            {
                if (_tabs.Children[i] is ProfilerMode mode)
                {
                    mode.Clear();
                    FlaxEngine.Profiler.BeginEvent("ProfilerWindow.UpdateView");
                    mode.UpdateView(ViewFrameIndex, _showOnlyLastUpdateEvents);
                    FlaxEngine.Profiler.EndEvent();
                }
            }

            UpdateButtons();
        }

        private void OnSelectedTabChanged(Tabs tabs)
        {
            if (tabs.SelectedTab is ProfilerMode mode)
            {
                FlaxEngine.Profiler.BeginEvent("ProfilerWindow.UpdateView");
                mode.UpdateView(ViewFrameIndex, _showOnlyLastUpdateEvents);
                FlaxEngine.Profiler.EndEvent();
            }
        }

        private void UpdateButtons()
        {
            _clearButton.Enabled = _framesCount > 0;
            _prevFrameButton.Enabled = _framesCount > 1 && (_frameIndex == -1 || _frameIndex > 0);
            _nextFrameButton.Enabled = _frameIndex >= 0 && _frameIndex < _framesCount - 1;
            _lastFrameButton.Enabled = _framesCount > 0;
            _previousPeakButton.Enabled = _framesCount > 2;
            _nextPeakButton.Enabled = _framesCount > 2;
            _lastFrameButton.Checked = _frameIndex == -1 && _historyView.ViewEnd == -1;
            _showOnlyLastUpdateEventsButton.Checked = _showOnlyLastUpdateEvents;
            string status;
            if (_selectedPeakFrame == _frameIndex && _frameIndex != -1)
                status = $"Peak - Frame {_frameIndex + 1:N0} / {_framesCount:N0} - {_selectedPeakTime:0.0} ms";
            else if (_frameIndex != -1)
                status = $"Frame {_frameIndex + 1:N0} / {_framesCount:N0}";
            else if (LiveRecording && Editor.StateMachine.IsPlayMode && Editor.StateMachine.PlayingState.IsPaused)
                status = $"Paused - {_framesCount:N0} frames";
            else if (LiveRecording && _historyView.ViewEnd != -1)
                status = $"Browsing - {_framesCount:N0} frames";
            else if (LiveRecording)
                status = $"Recording - {_framesCount:N0} frames";
            else
                status = _framesCount == 0 ? "Idle" : $"Stopped - {_framesCount:N0} frames";
            if (_statusLabel.Text != status)
                _statusLabel.Text = status;
        }

        private void UpdateView()
        {
            if (_tabs.SelectedTab is ProfilerMode mode)
            {
                FlaxEngine.Profiler.BeginEvent("ProfilerWindow.UpdateView");
                mode.UpdateView(_frameIndex, _showOnlyLastUpdateEvents);
                FlaxEngine.Profiler.EndEvent();
            }
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            // Create default modes
            AddMode(new Overall(_historyView));
            _cpuMode = new CPU(_historyView);
            AddMode(_cpuMode);
            AddMode(new GPU(_historyView));
            AddMode(new MemoryGPU(_historyView));
            AddMode(new Memory(_historyView));
            AddMode(new Assets(_historyView));
            AddMode(new Network(_historyView));
            AddMode(new Physics(_historyView));

            // Init view
            _frameIndex = -1;
            for (int i = 0; i < _tabs.ChildrenCount; i++)
            {
                if (_tabs.Children[i] is ProfilerMode mode)
                    mode.UpdateView(_frameIndex, _showOnlyLastUpdateEvents);
            }

            UpdateButtons();

            ScriptsBuilder.ScriptsReloadEnd += Clear; // Prevent crashes if any of the profiler tabs has some scripting types cached (eg. asset type info)
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            for (int i = 0; i < _tabs.ChildrenCount; i++)
            {
                if (_tabs.Children[i] is ProfilerMode mode)
                    mode.UpdateStats();
            }

            bool gamePaused = UpdateRecordingState();
            if (LiveRecording && !gamePaused)
            {
                FlaxEngine.Profiler.BeginEvent("ProfilerWindow.OnUpdate");

                // Get memory allocations during last frame
                long managedMemory = GC.GetAllocatedBytesForCurrentThread();
                if (_lastManagedMemory == 0)
                    _lastManagedMemory = managedMemory;
                var managedAllocs = managedMemory - _lastManagedMemory - _lastManagedMemoryProfiler;
                _lastManagedMemory = managedMemory;

                ProfilerMode.SharedUpdateData sharedData = new ProfilerMode.SharedUpdateData();
                sharedData.Begin();
                sharedData.ManagedMemoryAllocation = (int)managedAllocs;
                _frameTimes.Add(Mathf.Max(sharedData.Stats.UpdateTimeMs + sharedData.Stats.DrawCPUTimeMs, sharedData.Stats.DrawGPUTimeMs));
                _peaksDirty = true;
                for (int i = 0; i < _tabs.ChildrenCount; i++)
                {
                    if (_tabs.Children[i] is ProfilerMode mode)
                    {
                        FlaxEngine.Profiler.BeginEvent(mode.GetType().FullName);
                        mode.Update(ref sharedData);
                        FlaxEngine.Profiler.EndEvent();
                    }
                }
                {
                    if (_tabs.SelectedTab is ProfilerMode mode)
                    {
                        FlaxEngine.Profiler.BeginEvent("ProfilerWindow.UpdateView");
                        mode.UpdateView(_frameIndex, _showOnlyLastUpdateEvents);
                        FlaxEngine.Profiler.EndEvent();
                    }
                }
                sharedData.End();

                _framesCount++;
                UpdateButtons();

                // Get memory allocations within profiler window update to exclude from stats
                managedMemory = GC.GetAllocatedBytesForCurrentThread();
                _lastManagedMemoryProfiler = managedMemory - _lastManagedMemory;

                FlaxEngine.Profiler.EndEvent();
            }
            else if (LiveRecording)
            {
                // Do not attribute allocations performed by the Editor while the game is paused to the first resumed frame.
                _lastManagedMemory = GC.GetAllocatedBytesForCurrentThread();
                _lastManagedMemoryProfiler = 0;
            }
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (base.OnKeyDown(key))
                return true;

            switch (key)
            {
            case KeyboardKeys.ArrowLeft:
                ViewFrameIndex--;
                return true;
            case KeyboardKeys.ArrowRight:
                ViewFrameIndex++;
                return true;
            }

            return false;
        }
#endif
    }
}
