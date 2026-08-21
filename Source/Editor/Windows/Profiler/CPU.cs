// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_PROFILER
using System;
using System.Collections.Generic;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Input;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEngine
{
    partial class ProfilerCPU
    {
        partial struct Event
        {
            /// <summary>
            /// Gets the event name.
            /// </summary>
            public unsafe string Name
            {
                get
                {
                    fixed (short* name = Name0)
                        return new string((char*)name);
                }
            }

            internal unsafe bool NameStartsWith(string prefix)
            {
                fixed (short* name = Name0)
                {
                    fixed (char* p = prefix)
                        return Utils.MemoryCompare(new IntPtr(name), new IntPtr(p), (ulong)(prefix.Length * 2)) == 0;
                }
            }
        }
    }
}

namespace FlaxEditor.Windows.Profiler
{
    /// <summary>
    /// The CPU performance profiling mode.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.Profiler.ProfilerMode" />
    internal sealed unsafe class CPU : ProfilerMode
    {
        private readonly SingleChart _mainChart;
        private readonly Timeline _timeline;
        private readonly Table _table;
        private readonly SearchBox _timerSearch;
        private readonly ProfilerHistoryView _historyView;
        private SamplesBuffer<ProfilingTools.ThreadStats[]> _events;
        private List<Timeline.TrackLabel> _timelineLabelsCache;
        private List<Timeline.Event> _timelineEventsCache;
        private List<Row> _tableRowsCache;
        private readonly List<bool> _eventSearchMatches = new List<bool>();
        private readonly List<int> _eventSearchAncestors = new List<int>();
        private bool _showOnlyLastUpdateEvents;
        private bool _showMainThread = true;
        private bool _showJobSystemThreads;
        private bool _showOtherThreads = true;

        internal bool ShowMainThread
        {
            get => _showMainThread;
            set
            {
                if (_showMainThread != value)
                {
                    _showMainThread = value;
                    RefreshView();
                }
            }
        }

        internal bool ShowJobSystemThreads
        {
            get => _showJobSystemThreads;
            set
            {
                if (_showJobSystemThreads != value)
                {
                    _showJobSystemThreads = value;
                    RefreshView();
                }
            }
        }

        internal bool ShowOtherThreads
        {
            get => _showOtherThreads;
            set
            {
                if (_showOtherThreads != value)
                {
                    _showOtherThreads = value;
                    RefreshView();
                }
            }
        }

        public CPU(ProfilerHistoryView historyView)
        : base("CPU", historyView)
        {
            _historyView = historyView;
            // Layout
            var mainPanel = new Panel(ScrollBars.None)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = this,
            };
            
            // Chart
            _mainChart = new SingleChart(historyView)
            {
                Title = "Update",
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = Margin.Zero,
                Height = SingleChart.DefaultHeight,
                DrawBars = true,
                UseFrameBudget = true,
                FormatSample = v => (Mathf.RoundToInt(v * 10.0f) / 10.0f) + " ms",
                Parent = mainPanel,
            };
            _mainChart.SelectedSampleChanged += OnSelectedSampleChanged;
            
            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, _mainChart.Height + 2, 0),
                Parent = mainPanel,
            };
            //panel.Y = _mainChart.Height + 2;
            var layout = new VerticalPanel
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = Margin.Zero,
                Pivot = Float2.Zero,
                IsScrollable = true,
                Parent = panel,
            };
            
            // Timeline
            _timeline = new Timeline
            {
                Height = 340,
                ContentOffset = 90,
                FrameBudgetMs = historyView.FrameBudgetMs,
                Parent = layout,
            };

            // Timer table controls
            var style = Style.Current;
            var timerToolbar = new ContainerControl
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0, 0, 0, 29),
                Parent = layout,
            };
            _timerSearch = new SearchBox
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(4, 4, 3, 26),
                WatermarkText = "Search timers or groups...",
                TooltipText = "Filter CPU timers and thread groups. Matching timers keep their parent path visible.",
                Parent = timerToolbar,
            };
            _timerSearch.TextChanged += RefreshTable;

            // Table
            var headerColor = style.SecondaryBackground;
            var textColor = style.Foreground;
            _table = new Table
            {
                RowColorEven = style.Background * 1.08f,
                RowColorOdd = style.Background * 1.28f,
                Columns = new[]
                {
                    new ColumnDefinition
                    {
                        UseExpandCollapseMode = true,
                        CellAlignment = TextAlignment.Near,
                        Title = "Event",
                        TitleBackgroundColor = headerColor,
                        TitleColor = textColor,
                    },
                    new ColumnDefinition
                    {
                        Title = "Total",
                        TitleBackgroundColor = headerColor,
                        FormatValue = FormatCellPercentage,
                        TitleColor = textColor,
                    },
                    new ColumnDefinition
                    {
                        Title = "Self",
                        TitleBackgroundColor = headerColor,
                        FormatValue = FormatCellPercentage,
                        TitleColor = textColor,
                    },
                    new ColumnDefinition
                    {
                        Title = "Time ms",
                        TitleBackgroundColor = headerColor,
                        FormatValue = FormatCellMs,
                        TitleColor = textColor,
                    },
                    new ColumnDefinition
                    {
                        Title = "Self ms",
                        TitleBackgroundColor = headerColor,
                        FormatValue = FormatCellMs,
                        TitleColor = textColor,
                    },
                    new ColumnDefinition
                    {
                        Title = "Memory",
                        TitleBackgroundColor = headerColor,
                        FormatValue = FormatCellBytes,
                        TitleColor = textColor,
                    },
                },
                Parent = layout,
            };
            _table.Splits = new[]
            {
                0.5f,
                0.1f,
                0.1f,
                0.1f,
                0.1f,
                0.1f,
            };
        }

        private string FormatCellPercentage(object x)
        {
            return ((float)x).ToString("0.0") + '%';
        }

        private string FormatCellMs(object x)
        {
            return ((float)x).ToString("0.00");
        }

        private string FormatCellBytes(object x)
        {
            return Utilities.Utils.FormatBytesCount(Convert.ToUInt64(x));
        }

        private void RefreshView()
        {
            if (_events != null)
                UpdateView(_mainChart.SelectedSampleIndex, _showOnlyLastUpdateEvents);
        }

        private void RefreshTable()
        {
            if (_events == null || _tableRowsCache == null)
                return;
            var viewRange = _showOnlyLastUpdateEvents ? GetMainThreadUpdateRange() : ViewRange.Full;
            UpdateTable(ref viewRange);
        }

        private Color GetBudgetColor(double timeMs, float alpha)
        {
            float ratio = (float)(timeMs / _historyView.FrameBudgetMs);
            Color color = ratio >= 0.25f ? Color.Red : ratio >= 0.10f ? Color.Orange : ratio >= 0.05f ? Color.Yellow : Color.Green;
            return color.AlphaMultiplied(alpha);
        }

        private void ApplyBudgetColors(Row row, double totalTimeMs, double selfTimeMs)
        {
            var totalColor = GetBudgetColor(totalTimeMs, totalTimeMs >= _historyView.FrameBudgetMs * 0.05f ? 0.34f : 0.10f);
            var selfColor = GetBudgetColor(selfTimeMs, selfTimeMs >= _historyView.FrameBudgetMs * 0.05f ? 0.34f : 0.10f);
            row.BackgroundColors[0] = totalColor.AlphaMultiplied(0.55f);
            row.BackgroundColors[1] = totalColor;
            row.BackgroundColors[2] = selfColor;
            row.BackgroundColors[3] = totalColor;
            row.BackgroundColors[4] = selfColor;
        }

        private bool BuildSearchMatches(ProfilerCPU.Event[] events, string searchText)
        {
            while (_eventSearchMatches.Count < events.Length)
                _eventSearchMatches.Add(false);
            for (int i = 0; i < events.Length; i++)
                _eventSearchMatches[i] = false;
            _eventSearchAncestors.Clear();

            bool anyMatch = false;
            for (int i = 0; i < events.Length; i++)
            {
                int depth = events[i].Depth;
                while (_eventSearchAncestors.Count <= depth)
                    _eventSearchAncestors.Add(-1);
                _eventSearchAncestors[depth] = i;
                string name = events[i].Name.Replace("::", ".");
                if (name.IndexOf(searchText, StringComparison.OrdinalIgnoreCase) == -1)
                    continue;

                anyMatch = true;
                for (int ancestorDepth = 0; ancestorDepth <= depth; ancestorDepth++)
                {
                    int ancestor = _eventSearchAncestors[ancestorDepth];
                    if (ancestor >= 0)
                        _eventSearchMatches[ancestor] = true;
                }
            }
            return anyMatch;
        }

        private Row GetTableRow()
        {
            Row row;
            if (_tableRowsCache.Count != 0)
            {
                var last = _tableRowsCache.Count - 1;
                row = _tableRowsCache[last];
                _tableRowsCache.RemoveAt(last);
            }
            else
            {
                row = new Row
                {
                    Values = new object[6],
                    BackgroundColors = new Color[6],
                };
            }
            for (int i = 0; i < row.BackgroundColors.Length; i++)
                row.BackgroundColors[i] = Color.Transparent;
            row.Highlighted = false;
            row.Selected = false;
            return row;
        }

        private static long GetEventKey(int threadIndex, int eventIndex)
        {
            return ((long)threadIndex << 32) | (uint)eventIndex;
        }

        private bool IsThreadVisible(string name)
        {
            if (name == "Main")
                return _showMainThread;
            if (name != null && name.StartsWith("Job System", StringComparison.Ordinal))
                return _showJobSystemThreads;
            return _showOtherThreads;
        }

        /// <inheritdoc />
        public override void Clear()
        {
            _mainChart.Clear();
            _events?.Clear();
        }

        /// <inheritdoc />
        public override void Update(ref SharedUpdateData sharedData)
        {
            _mainChart.AddSample(sharedData.Stats.UpdateTimeMs);

            // Gather CPU events
            var events = sharedData.GetEventsCPU();
            if (_events == null)
                _events = new SamplesBuffer<ProfilingTools.ThreadStats[]>();
            _events.Add(events);
        }

        /// <inheritdoc />
        public override void UpdateView(int selectedFrame, bool showOnlyLastUpdateEvents)
        {
            _showOnlyLastUpdateEvents = showOnlyLastUpdateEvents;
            _mainChart.SelectedSampleIndex = selectedFrame;
            _timeline.FrameBudgetMs = _historyView.FrameBudgetMs;

            if (_events == null)
                return;
            if (_timelineLabelsCache == null)
                _timelineLabelsCache = new List<Timeline.TrackLabel>();
            if (_timelineEventsCache == null)
                _timelineEventsCache = new List<Timeline.Event>();
            if (_tableRowsCache == null)
                _tableRowsCache = new List<Row>();

            var viewRange = _showOnlyLastUpdateEvents ? GetMainThreadUpdateRange() : ViewRange.Full;
            _timeline.FitToDuration(_mainChart.SelectedSample);
            UpdateTimeline(ref viewRange);
            UpdateTable(ref viewRange);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _timelineLabelsCache?.Clear();
            _timelineEventsCache?.Clear();
            _tableRowsCache?.Clear();

            base.OnDestroy();
        }

        private struct ViewRange
        {
            public double Start;
            public double End;

            public static ViewRange Full = new ViewRange
            {
                Start = float.MinValue,
                End = float.MaxValue
            };

            public ViewRange(ref ProfilerCPU.Event e)
            {
                Start = e.Start - MinEventTimeMs;
                End = e.End + MinEventTimeMs;
            }

            public bool SkipEvent(ref ProfilerCPU.Event e)
            {
                return e.Start < Start || e.Start > End;
            }
        }

        private ViewRange GetMainThreadUpdateRange()
        {
            if (_events != null && _events.Count != 0)
            {
                var threads = _events.Get(_mainChart.SelectedSampleIndex);
                if (threads != null)
                {
                    for (int j = 0; j < threads.Length; j++)
                    {
                        var thread = threads[j];
                        if (thread.Name != "Main" || thread.Events == null)
                            continue;
                        for (int i = 0; i < thread.Events.Length; i++)
                        {
                            ref var e = ref thread.Events[i];
                            if (e.Depth == 0 && e.Name == "Update")
                                return new ViewRange(ref e);
                        }
                    }
                }
            }
            return ViewRange.Full;
        }

        private void AddEvent(double startTime, int maxDepth, float xOffset, int depthOffset, int threadIndex, int index, ProfilerCPU.Event[] events, ContainerControl parent)
        {
            ref ProfilerCPU.Event e = ref events[index];

            double length = e.End - e.Start;
            if (length <= 0.0)
                return;
            double scale = _timeline.PixelsPerMillisecond;
            float x = (float)((e.Start - startTime) * scale);
            float width = Mathf.Max((float)(length * scale), 1.0f);
            Timeline.Event control;
            if (_timelineEventsCache.Count != 0)
            {
                var last = _timelineEventsCache.Count - 1;
                control = _timelineEventsCache[last];
                _timelineEventsCache.RemoveAt(last);
            }
            else
            {
                control = new Timeline.Event();
            }
            control.StartTimeMs = (float)(e.Start - startTime);
            control.DurationMs = (float)length;
            control.SourceKey = GetEventKey(threadIndex, index);
            control.LinkedRow = null;
            control.Bounds = new Rectangle(x + xOffset, Timeline.RulerHeight + (e.Depth + depthOffset) * Timeline.Event.DefaultHeight, width, Timeline.Event.DefaultHeight - 1);
            control.Name = e.Name.Replace("::", ".");
            control.TooltipText = string.Format("{0}, {1} ms", control.Name, ((int)(length * 1000.0) / 1000.0f));
            control.Parent = parent;
            _timeline.RegisterEvent(control);

            // Spawn sub events
            int childrenDepth = e.Depth + 1;
            if (childrenDepth <= maxDepth)
            {
                while (++index < events.Length)
                {
                    int subDepth = events[index].Depth;
                    if (subDepth <= e.Depth)
                        break;
                    if (subDepth == childrenDepth)
                    {
                        AddEvent(startTime, maxDepth, xOffset, depthOffset, threadIndex, index, events, parent);
                    }
                }
            }
        }

        private void UpdateTimeline(ref ViewRange viewRange)
        {
            var container = _timeline.EventsContainer;
            _timeline.BeginContentUpdate();

            container.IsLayoutLocked = true;
            int idx = 0;
            while (container.Children.Count > idx)
            {
                var child = container.Children[idx];
                if (child is Timeline.Event e)
                {
                    _timelineEventsCache.Add(e);
                    child.Parent = null;
                }
                else if (child is Timeline.TrackLabel l)
                {
                    _timelineLabelsCache.Add(l);
                    child.Parent = null;
                }
                else
                {
                    idx++;
                }
            }

            _timeline.Height = UpdateTimelineInner(ref viewRange);

            container.UnlockChildrenRecursive();
            container.PerformLayout();
            _timeline.EndContentUpdate();
        }

        private float UpdateTimelineInner(ref ViewRange viewRange)
        {
            if (_events.Count == 0)
                return 0;
            var data = _events.Get(_mainChart.SelectedSampleIndex);
            if (data == null || data.Length == 0)
                return 0;

            // Find the first event start time (for the timeline start time)
            double startTime = double.MaxValue;
            if (viewRange.Start > 0)
            {
                startTime = viewRange.Start;
            }
            else
            {
                var r = GetMainThreadUpdateRange();
                if (r.Start > 0)
                {
                    startTime = r.Start;
                }
                else
                {
                    for (int i = 0; i < data.Length; i++)
                    {
                        if (data[i].Events != null && data[i].Events.Length != 0)
                            startTime = Math.Min(startTime, data[i].Events[0].Start);
                    }
                    if (startTime >= double.MaxValue)
                        return 0;
                }
            }

            var container = _timeline.EventsContainer;

            // Create timeline track per thread
            int depthOffset = 0;
            for (int i = 0; i < data.Length; i++)
            {
                if (!IsThreadVisible(data[i].Name))
                    continue;
                var events = data[i].Events;
                if (events == null)
                    continue;

                // Check maximum depth
                int maxDepth = -1;
                for (int j = 0; j < events.Length; j++)
                {
                    var e = events[j];
                    if (viewRange.SkipEvent(ref e))
                        continue;
                    maxDepth = Mathf.Max(maxDepth, e.Depth);
                }

                // Skip empty tracks
                if (maxDepth == -1)
                    continue;

                // Add thread label
                float xOffset = 90;
                Timeline.TrackLabel trackLabel;
                if (_timelineLabelsCache.Count != 0)
                {
                    var last = _timelineLabelsCache.Count - 1;
                    trackLabel = _timelineLabelsCache[last];
                    _timelineLabelsCache.RemoveAt(last);
                }
                else
                {
                    trackLabel = new Timeline.TrackLabel();
                }
                trackLabel.Bounds = new Rectangle(0, Timeline.RulerHeight + depthOffset * Timeline.Event.DefaultHeight, xOffset, (maxDepth + 2) * Timeline.Event.DefaultHeight);
                trackLabel.Name = data[i].Name;
                trackLabel.BackgroundColor = Style.Current.Background * 1.1f;
                trackLabel.Parent = container;

                // Add events
                for (int j = 0; j < events.Length; j++)
                {
                    var e = events[j];
                    if (e.Depth == 0)
                    {
                        if (viewRange.SkipEvent(ref e))
                            continue;
                        AddEvent(startTime, maxDepth, xOffset, depthOffset, i, j, events, container);
                    }
                }

                depthOffset += maxDepth + 2;
            }

            return Timeline.RulerHeight + Timeline.Event.DefaultHeight * depthOffset;
        }

        private void UpdateTable(ref ViewRange viewRange)
        {
            _table.IsLayoutLocked = true;

            RecycleTableRows(_table, _tableRowsCache);
            UpdateTableInner(ref viewRange);

            _table.UnlockChildrenRecursive();
            _table.PerformLayout();
        }

        private void UpdateTableInner(ref ViewRange viewRange)
        {
            if (_events.Count == 0)
                return;
            var data = _events.Get(_mainChart.SelectedSampleIndex);
            if (data == null || data.Length == 0)
                return;
            float totalTimeMs = _mainChart.SelectedSample;
            string searchText = _timerSearch.Text?.Trim();
            bool searching = !string.IsNullOrEmpty(searchText);
            int timerRowsAdded = 0;

            // Add thread groups and timer rows
            for (int j = 0; j < data.Length; j++)
            {
                if (!IsThreadVisible(data[j].Name))
                    continue;
                var events = data[j].Events;
                if (events == null)
                    continue;

                bool threadMatches = searching && data[j].Name?.IndexOf(searchText, StringComparison.OrdinalIgnoreCase) >= 0;
                bool hasTimerMatches = !searching || threadMatches || BuildSearchMatches(events, searchText);
                if (!hasTimerMatches)
                    continue;

                double threadTime = 0;
                int threadMemory = 0;
                for (int i = 0; i < events.Length; i++)
                {
                    if (events[i].Depth == 0 && events[i].End > events[i].Start && !viewRange.SkipEvent(ref events[i]))
                    {
                        threadTime += events[i].End - events[i].Start;
                        threadMemory += events[i].ManagedMemoryAllocation + events[i].NativeMemoryAllocation;
                    }
                }
                var groupRow = GetTableRow();
                groupRow.Values[0] = data[j].Name;
                groupRow.Values[1] = totalTimeMs > Mathf.Epsilon ? (float)(threadTime / totalTimeMs * 100.0) : 0.0f;
                groupRow.Values[2] = 0.0f;
                groupRow.Values[3] = (float)threadTime;
                groupRow.Values[4] = 0.0f;
                groupRow.Values[5] = threadMemory;
                ApplyBudgetColors(groupRow, threadTime, 0);
                groupRow.TooltipText = $"Thread group: {threadTime:0.###} ms ({threadTime / _historyView.FrameBudgetMs * 100.0:0.0}% of the selected frame budget). Alt-click to expand or collapse everything inside.";
                groupRow.Depth = 0;
                groupRow.Width = _table.Width;
                groupRow.Visible = true;
                groupRow.Parent = _table;

                for (int i = 0; i < events.Length; i++)
                {
                    var e = events[i];
                    var time = Math.Max(e.End - e.Start, MinEventTimeMs);
                    if (e.End <= 0.0f || viewRange.SkipEvent(ref e))
                        continue;
                    if (searching && !threadMatches && !_eventSearchMatches[i])
                        continue;

                    // Count sub-events time
                    double subEventsTimeTotal = 0;
                    int subEventsMemoryTotal = e.ManagedMemoryAllocation + e.NativeMemoryAllocation;
                    for (int k = i + 1; k < events.Length; k++)
                    {
                        var sub = events[k];
                        if (sub.Depth == e.Depth + 1 && e.End > 0.0f)
                        {
                            subEventsTimeTotal += Math.Max(sub.End - sub.Start, MinEventTimeMs);
                        }
                        else if (sub.Depth <= e.Depth)
                        {
                            break;
                        }
                        subEventsMemoryTotal += sub.ManagedMemoryAllocation + sub.NativeMemoryAllocation;
                    }

                    string name = e.Name.Replace("::", ".");

                    Row row = GetTableRow();
                    {
                        double selfTime = Math.Max(time - subEventsTimeTotal, 0.0);

                        // Event
                        row.Values[0] = name;

                        // Total (%)
                        float rowTotalTimePerc = totalTimeMs > Mathf.Epsilon ? (float)(time / totalTimeMs) : 0.0f;
                        row.Values[1] = (int)(rowTotalTimePerc * 1000.0f) / 10.0f;

                        // Self (%)
                        float rowSelfTimePerc = totalTimeMs > Mathf.Epsilon ? (float)(selfTime / totalTimeMs) : 0.0f;
                        row.Values[2] = (int)(rowSelfTimePerc * 1000.0f) / 10.0f;

                        // Time ms
                        row.Values[3] = (float)((time * 10000.0f) / 10000.0f);

                        // Self ms
                        row.Values[4] = (float)((selfTime * 10000.0f) / 10000.0f);

                        // Memory Alloc
                        row.Values[5] = subEventsMemoryTotal;
                        ApplyBudgetColors(row, time, selfTime);
                        row.TooltipText = $"Inclusive: {time:0.###} ms ({time / _historyView.FrameBudgetMs * 100.0:0.0}% budget). Self: {selfTime:0.###} ms ({selfTime / _historyView.FrameBudgetMs * 100.0:0.0}% budget). Alt-click a group to recursively expand or collapse it.";
                    }
                    row.Depth = e.Depth + 1;
                    row.Width = _table.Width;
                    row.Visible = searching || e.Depth < 2;
                    row.Parent = _table;
                    _timeline.LinkRow(GetEventKey(j, i), row);
                    timerRowsAdded++;
                }
            }

            if (timerRowsAdded == 0)
            {
                var messageRow = GetTableRow();
                messageRow.Values[0] = "No completed CPU timer events in this frame - choose a neighboring frame";
                messageRow.Values[1] = 0.0f;
                messageRow.Values[2] = 0.0f;
                messageRow.Values[3] = 0.0f;
                messageRow.Values[4] = 0.0f;
                messageRow.Values[5] = 0;
                messageRow.TooltipText = "The frame summary was captured, but the CPU profiler had no completed timer events for this exact frame.";
                messageRow.Depth = 0;
                messageRow.Width = _table.Width;
                messageRow.Visible = true;
                messageRow.Parent = _table;
            }
        }
    }
}
#endif
