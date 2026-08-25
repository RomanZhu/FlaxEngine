// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEditor.Viewport.Overlays;
using FlaxEditor.Viewport;
using FlaxEditor.Windows;
using System.Text;
using System.Collections.Generic;

namespace FlaxEditor.FMOD
{
    /// <summary>Small diagnostics overlay shared by Scene and Game views.</summary>
    public sealed class FmodDebugOverlay : Label
    {
        public enum RuntimeSort
        {
            Voices,
            Name,
            Plays,
            Volume,
            Time,
        }

        private static FmodDebugOverlay _scene;
        private static FmodDebugOverlay _game;
        private static EventLabelOverlay _sceneLabels;
        private static EventLabelOverlay _gameLabels;
        private static DateTime _lastRefresh;
        private static float _clock;
        private static readonly Dictionary<ulong, TrackedLabel> _labels = new Dictionary<ulong, TrackedLabel>();

        private static readonly TimeSpan RefreshInterval = TimeSpan.FromMilliseconds(100);

        private sealed class TrackedLabel
        {
            public Vector3 Position;
            public string Name;
            public AudioEventHandle Handle;
            public AudioEventPlaybackState State;
            public float Volume = 1.0f;
            public int TimelinePosition;
            public int Plays;
            public bool IsVirtual;
            public float LastSeen;
            public float PulseUntil;
            public bool StoppedPulse;
            public bool Visible;
        }

        private sealed class EventLabelOverlay : Control
        {
            private readonly bool _sceneView;

            public EventLabelOverlay(bool sceneView)
            {
                _sceneView = sceneView;
                AnchorPreset = AnchorPresets.StretchAll;
                Offsets = Margin.Zero;
                AutoFocus = false;
            }

            public override bool ContainsPoint(ref Float2 location, bool precise = false)
            {
                return false;
            }

            public override void Draw()
            {
                DrawEventLabelsForView(_sceneView, this);
            }
        }

        private FmodDebugOverlay()
        : base(4, 4, 540, 260)
        {
            TextColor = Color.Wheat;
            BackgroundColor = new Color(0.02f, 0.02f, 0.02f, 0.72f);
            AutoHeight = true;
            HorizontalAlignment = TextAlignment.Near;
            VerticalAlignment = TextAlignment.Near;
            ClipText = false;
        }

        public static void Attach(Editor editor)
        {
            AudioEventSystem.EventCallback += OnEventCallback;
            if (editor.Windows?.EditWin?.Viewport != null && _scene == null)
            {
                _scene = new FmodDebugOverlay();
                editor.Windows.EditWin.Viewport.ViewportOverlays.AddOverlay(
                    "Flax.Audio.FMOD.Diagnostics", "FMOD Diagnostics", _scene, new Float2(540, 260),
                    ViewportOverlayDock.TopLeft, ViewportOverlayLayoutMode.Panel, new Float2(8, 28));
                _sceneLabels = new EventLabelOverlay(true) { Parent = editor.Windows.EditWin.Viewport };
            }
            if (editor.Windows?.GameWin?.Viewport != null && _game == null)
            {
                _game = new FmodDebugOverlay
                {
                    Parent = editor.Windows.GameWin.Viewport,
                    AnchorPreset = AnchorPresets.TopLeft,
                };
                _gameLabels = new EventLabelOverlay(false) { Parent = editor.Windows.GameWin.Viewport };
            }
        }

        public static void Refresh()
        {
            if (DateTime.UtcNow - _lastRefresh < RefreshInterval)
                return;
            _lastRefresh = DateTime.UtcNow;
            _clock += (float)RefreshInterval.TotalSeconds;
            var manager = Level.FindActor<FMODAudioManager>();
            if (_scene != null)
                _scene.Visible = manager != null && manager.ShowSceneOverlay;
            if (_game != null)
                _game.Visible = manager != null && manager.ShowGameOverlay;
            if (manager == null || (!manager.ShowSceneOverlay && !manager.ShowGameOverlay))
                return;

            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            var text = FormatDiagnostics(snapshot, manager.SceneDiagnostics);
            if (_scene != null)
                _scene.Text = text;
            if (_game != null)
                _game.Text = text;
            UpdateEventLabels(manager, snapshot);
        }

        /// <summary>Formats the read-only backend and scene diagnostics shared by editor surfaces.</summary>
        public static string FormatDiagnostics(AudioDiagnosticsSnapshot snapshot, AudioSceneDiagnostics scene)
        {
            var builder = new StringBuilder(512);
            builder.Append("FMOD ").Append(snapshot.BackendName ?? "Unknown").Append(" ")
                .Append(snapshot.RuntimeVersion ?? string.Empty).Append(" | Ready: ").Append(snapshot.Initialized).Append('\n');
            builder.Append("Device: ").Append(snapshot.ActiveDevice ?? "Default")
                .Append(" | Live update: ").Append(snapshot.LiveUpdateEnabled).Append('\n');
            builder.Append("Events: ").Append(snapshot.ActiveInstances)
                .Append(" active / ").Append(snapshot.TotalInstancesCreated).Append(" created | plays ")
                .Append(snapshot.TotalPlays).Append(" / stopped ").Append(snapshot.TotalStopped).Append('\n');
            builder.Append("Voices real/virtual: ").Append(snapshot.RealVoices).Append('/').Append(snapshot.VirtualVoices)
                .Append(" | Peak instances: ").Append(snapshot.PeakActiveInstances).Append('\n');
            builder.Append("Banks: ").Append(snapshot.LoadedBanks).Append(" (sample data ")
                .Append(snapshot.LoadedSampleDataBanks).Append(") | CPU: ").Append(snapshot.CpuUsage.ToString("0.00"))
                .Append("% [studio ").Append(snapshot.StudioUpdateCpu.ToString("0.00"))
                .Append(" / mixer ").Append(snapshot.MixerCpu.ToString("0.00"))
                .Append(" / stream ").Append(snapshot.StreamCpu.ToString("0.00")).Append("]\n");
            builder.Append("Memory: ").Append(FormatBytes(snapshot.MemoryAllocated)).Append(" / peak ")
                .Append(FormatBytes(snapshot.MemoryPeak)).Append(" | Output: ").Append(snapshot.OutputSampleRate)
                .Append(" Hz ").Append(snapshot.OutputChannels).Append("ch, DSP ").Append(snapshot.DspBufferLength)
                .Append('x').Append(snapshot.DspBufferCount).Append('\n');
            builder.Append("Master: RMS ").Append(snapshot.CombinedOutputRms.ToString("0.000000"))
                .Append(" (").Append(snapshot.CombinedOutputDbfs.ToString("0.0")).Append(" dBFS), peak ")
                .Append(snapshot.CombinedOutputPeak.ToString("0.000")).Append(snapshot.OutputClipping ? " CLIPPING" : string.Empty)
                .Append(" | listeners ").Append(snapshot.ListenerCount).Append('\n');
            if (snapshot.LastErrorCode != 0)
                builder.Append("Last FMOD error: ").Append(snapshot.LastErrorCode).Append(' ').Append(snapshot.LastError).Append('\n');
            builder.Append("Callbacks: queue ").Append(snapshot.CallbackQueueDepth).Append(", dropped ")
                .Append(snapshot.DroppedCallbacks).Append(" | Occlusion: ").Append(scene.OcclusionQueries)
                .Append(" queries, ").Append(scene.OcclusionDeferred).Append(" deferred\n");
            builder.Append("Scene: ").Append(scene.Emitters).Append(" emitters, ").Append(scene.Volumes)
                .Append(" volumes, ").Append(scene.ActiveZones).Append(" zones, ")
                .Append(scene.PersistentInteractions).Append(" interactions");

            if (snapshot.Banks != null && snapshot.Banks.Length > 0)
            {
                builder.Append("\nBanks:");
                foreach (var bank in snapshot.Banks)
                {
                    builder.Append("\n  ").Append(bank.Name ?? bank.Path ?? "<unnamed>")
                        .Append(" [").Append(bank.State).Append("] refs ").Append(bank.RefCount)
                        .Append(" samples ").Append(bank.SampleDataLoaded)
                        .Append(" revision ").Append(bank.FileRevision)
                        .Append(" result ").Append(bank.LastResult);
                }
            }
            return builder.ToString();
        }

        /// <summary>Formats the bounded live instance table used by the manager inspector.</summary>
        public static string FormatRuntimeEvents(AudioDiagnosticsSnapshot snapshot, RuntimeSort sort, bool descending)
        {
            var events = new List<AudioEventRuntimeInfo>();
            if (snapshot.Events != null)
                events.AddRange(snapshot.Events);
            events.Sort((a, b) =>
            {
                var result = 0;
                switch (sort)
                {
                    case RuntimeSort.Voices:
                        result = (a.RealVoices + a.VirtualVoices).CompareTo(b.RealVoices + b.VirtualVoices);
                        break;
                    case RuntimeSort.Name:
                        result = string.Compare(a.Path, b.Path, StringComparison.OrdinalIgnoreCase);
                        break;
                    case RuntimeSort.Plays:
                        result = a.PlayCount.CompareTo(b.PlayCount);
                        break;
                    case RuntimeSort.Volume:
                        result = a.Volume.CompareTo(b.Volume);
                        break;
                    case RuntimeSort.Time:
                        result = (a.TimeSeconds > 0.0f ? a.TimeSeconds : a.TimelinePosition / 1000.0f).CompareTo(
                            b.TimeSeconds > 0.0f ? b.TimeSeconds : b.TimelinePosition / 1000.0f);
                        break;
                }
                return descending ? -result : result;
            });

            var builder = new StringBuilder(256);
            builder.Append("State | Name | Voices | Gain/Audibility | Distance | Silence cause\n");
            foreach (var eventInfo in events)
            {
                var voices = eventInfo.RealVoices + eventInfo.VirtualVoices;
                if (voices == 0)
                    voices = eventInfo.IsVirtual ? 0 : 1;
                var plays = eventInfo.PlayCount;
                if (plays == 0 && eventInfo.PlaybackState != AudioEventPlaybackState.Stopped)
                    plays = 1;
                var time = eventInfo.TimeSeconds > 0.0f ? eventInfo.TimeSeconds : eventInfo.TimelinePosition / 1000.0f;
                builder.Append(eventInfo.ReachingOutput ? "OUTPUT" : eventInfo.Audible ? "AUDIBLE" : eventInfo.IsVirtual ? "VIRTUAL" : eventInfo.Playing ? "PLAYING" : eventInfo.Started ? "STARTED" : "CREATED")
                    .Append(" | ").Append(eventInfo.Path ?? "<unnamed>")
                    .Append(" | ").Append(voices).Append(" (real ").Append(eventInfo.RealVoices).Append("/virtual ")
                    .Append(eventInfo.VirtualVoices).Append(") | ").Append(eventInfo.FinalVolume.ToString("0.00"))
                    .Append('/').Append(eventInfo.Audibility.ToString("0.000"))
                    .Append(" | ").Append(eventInfo.DistanceMeters.ToString("0.00")).Append("m [")
                    .Append(eventInfo.MinimumDistanceMeters.ToString("0.00")).Append('-')
                    .Append(eventInfo.MaximumDistanceMeters.ToString("0.00")).Append("m] | ")
                    .Append(string.IsNullOrEmpty(eventInfo.SilenceCause) ? "reaching output" : eventInfo.SilenceCause)
                    .Append(" | plays ").Append(plays).Append(" @ ").Append(time.ToString("0.0")).Append("s\n");
            }
            if (events.Count == 0)
                builder.Append("<no active instances>\n");
            return builder.ToString();
        }

        private static string FormatBytes(ulong bytes)
        {
            if (bytes >= 1024UL * 1024UL)
                return $"{bytes / (1024.0 * 1024.0):0.0} MiB";
            if (bytes >= 1024UL)
                return $"{bytes / 1024.0:0.0} KiB";
            return $"{bytes} B";
        }

        private static void OnEventCallback(AudioEventCallback callback)
        {
            var key = MakeKey(callback.Handle);
            if (_labels.TryGetValue(key, out var tracked))
            {
                tracked.LastSeen = _clock;
                tracked.PulseUntil = _clock + 0.75f;
                tracked.StoppedPulse = callback.Type == AudioEventCallbackType.Stopped;
            }
        }

        private static ulong MakeKey(AudioEventHandle handle)
        {
            return ((ulong)handle.Index << 32) | handle.Generation;
        }

        private static void UpdateEventLabels(FMODAudioManager manager, AudioDiagnosticsSnapshot snapshot)
        {
            foreach (var tracked in _labels.Values)
                tracked.Visible = false;
            if (manager == null || !manager.ShowEventLabels)
                return;

            // FMOD instances are the source of truth here. Gameplay events can be
            // attached directly to an owner (for example player footsteps) without
            // having an AudioEmitter actor, just like Sonity draws its live voice pool.
            if (snapshot.Events != null)
                foreach (var eventInfo in snapshot.Events)
                {
                    if (eventInfo.Handle.Generation == 0)
                        continue;
                    var key = MakeKey(eventInfo.Handle);
                    var wasStopped = false;
                    if (_labels.TryGetValue(key, out var tracked))
                        wasStopped = tracked.State == AudioEventPlaybackState.Stopped;
                    else
                    {
                        tracked = new TrackedLabel { Handle = eventInfo.Handle, State = AudioEventPlaybackState.Stopped };
                        _labels[key] = tracked;
                        wasStopped = true;
                    }

                    tracked.Name = eventInfo.Path;
                    tracked.State = eventInfo.PlaybackState;
                    tracked.Volume = eventInfo.FinalVolume;
                    tracked.TimelinePosition = eventInfo.TimelinePosition;
                    tracked.IsVirtual = eventInfo.IsVirtual;
                    tracked.Plays = Math.Max(tracked.Plays, eventInfo.PlayCount);
                    if (eventInfo.Has3DAttributes)
                        tracked.Position = eventInfo.SourcePositionCentimeters;
                    else if (eventInfo.OwnerId != Guid.Empty)
                    {
                        var owner = Level.FindActor(eventInfo.OwnerId);
                        if (owner != null)
                            tracked.Position = owner.Position;
                    }

                    var playing = eventInfo.PlaybackState != AudioEventPlaybackState.Stopped;
                    if (playing)
                    {
                        tracked.Visible = true;
                        tracked.LastSeen = _clock;
                        if (wasStopped)
                        {
                            tracked.PulseUntil = _clock + 0.75f;
                            tracked.StoppedPulse = false;
                        }
                    }
                }

            // Scene emitters remain useful as an optional inactive-authoring view.
            // Active emitters are already represented by their runtime instances.
            if (manager.ShowInactiveEmitters)
            {
                foreach (var emitter in Level.GetActors<AudioEmitter>())
                {
                    if (emitter == null || emitter.Handle.Generation == 0)
                        continue;
                    var key = MakeKey(emitter.Handle);
                    if (_labels.TryGetValue(key, out var existing) && existing.Visible)
                        continue;
                    AudioEventSystem.QueryInstance(emitter.Handle, out var state);
                    if (state.PlaybackState != AudioEventPlaybackState.Stopped)
                        continue;
                    var tracked = existing;
                    if (tracked == null)
                    {
                        tracked = new TrackedLabel { Handle = emitter.Handle };
                        _labels[key] = tracked;
                    }
                    tracked.Position = emitter.Position;
                    tracked.Name = emitter.EventPath;
                    tracked.State = state.PlaybackState;
                    tracked.Volume = state.Volume;
                    tracked.TimelinePosition = state.TimelinePosition;
                    tracked.Visible = true;
                    tracked.LastSeen = _clock;
                }
            }

            if (!manager.ShowStaleLabels)
            {
                var expiredKeys = new List<ulong>();
                foreach (var pair in _labels)
                    if (_clock - pair.Value.LastSeen > Math.Max(0.0f, manager.StaleLabelLifetime))
                        expiredKeys.Add(pair.Key);
                foreach (var key in expiredKeys)
                    _labels.Remove(key);
                return;
            }
            var expired = new List<ulong>();
            foreach (var pair in _labels)
            {
                var tracked = pair.Value;
                if (_clock - tracked.LastSeen > Math.Max(0.0f, manager.StaleLabelLifetime))
                {
                    expired.Add(pair.Key);
                    continue;
                }
            }
            foreach (var key in expired)
                _labels.Remove(key);
        }

        private static void DrawEventLabelsForView(bool sceneView, Control overlay)
        {
            var manager = Level.FindActor<FMODAudioManager>();
            if (manager == null || !manager.ShowEventLabels || (sceneView ? !manager.ShowSceneOverlay : !manager.ShowGameOverlay))
                return;
            var count = 0;
            var labels = new List<TrackedLabel>(_labels.Values);
            labels.Sort((a, b) =>
            {
                if (a.Visible != b.Visible)
                    return a.Visible ? -1 : 1;
                return GetLabelDistance(a.Position, sceneView).CompareTo(GetLabelDistance(b.Position, sceneView));
            });
            foreach (var tracked in labels)
            {
                if (count >= Math.Max(0, manager.MaxLabels))
                    break;
                var stale = !tracked.Visible;
                if (stale && !manager.ShowStaleLabels)
                    continue;
                var distance = GetLabelDistance(tracked.Position, sceneView);
                if (distance > manager.MaxLabelDistance || !TryProjectLabel(tracked.Position, sceneView, overlay, out var screen))
                    continue;
                DrawTrackedLabel(tracked, manager, stale, distance, screen);
                count++;
            }
        }

        private static bool TryProjectLabel(Vector3 position, bool sceneView, Control overlay, out Float2 screen)
        {
            screen = Float2.Minimum;
            // Perspective projection can mirror points behind the view into valid screen coordinates.
            if (sceneView)
            {
                var viewport = Editor.Instance?.Windows?.EditWin?.Viewport;
                if (viewport == null)
                    return false;
                if (Vector3.Dot(position - viewport.ViewPosition, (Vector3)viewport.ViewDirection) <= 0.0f)
                    return false;
                viewport.ProjectPoint(position, out screen);
            }
            else
            {
                var camera = Camera.MainCamera;
                if (camera == null)
                    return false;
                if (Vector3.Dot(position - camera.Position, (Vector3)camera.Forward) <= 0.0f)
                    return false;
                var viewport = new FlaxEngine.Viewport(Float2.Zero, overlay.Size);
                camera.ProjectPoint(position, out screen, ref viewport);
            }
            return screen.X >= 0.0f && screen.Y >= 0.0f && screen.X <= overlay.Width && screen.Y <= overlay.Height;
        }

        private static void DrawTrackedLabel(TrackedLabel tracked, FMODAudioManager manager, bool stale, float distance, Float2 screen)
        {
            var age = Math.Max(0.0f, _clock - tracked.LastSeen);
            var fade = manager.LabelFadeLength > 0.001f ? Mathf.Clamp(1.0f - age / manager.LabelFadeLength, 0.0f, 1.0f) : 1.0f;
            var distanceFade = manager.MaxLabelDistance > 0.0f ? Mathf.Clamp(1.0f - distance / manager.MaxLabelDistance, 0.0f, 1.0f) : 1.0f;
            var color = stale ? manager.LabelStaleColor : Color.Lerp(manager.LabelStartColor, manager.LabelEndColor, (float)(1.0f - distanceFade));
            var opacity = Mathf.Clamp(manager.LabelLifetimeOpacity, 0.0f, 1.0f) * (stale ? fade : 1.0f) * Mathf.Clamp(manager.LabelVolumeOpacity * tracked.Volume, 0.0f, 1.0f);
            color = color.AlphaMultiplied(opacity);
            if (tracked.PulseUntil > _clock)
                color = Color.Lerp(color, tracked.StoppedPulse ? manager.LabelStoppedPulseColor : manager.LabelStartedPulseColor, Mathf.Clamp((tracked.PulseUntil - _clock) / 0.75f, 0.0f, 1.0f));
            var distanceText = manager.HideDistance ? string.Empty : $" {distance / 100.0f:0.0}m";
            var voice = stale ? "STALE" : (tracked.State == AudioEventPlaybackState.Stopped ? "STOPPED" : (tracked.IsVirtual ? "VIRTUAL" : "REAL"));
            var label = $"[{tracked.State}] {tracked.Name}\nhandle {tracked.Handle.Index}:{tracked.Handle.Generation}{distanceText} vol {tracked.Volume:0.00} plays {tracked.Plays} time {tracked.TimelinePosition / 1000.0f:0.0}s\n{voice}";
            var size = Math.Max(8, manager.LabelFontSize);
            var scale = size / 14.0f;
            var bounds = new Rectangle(screen.X - 240.0f, screen.Y - 32.0f, 480.0f, 72.0f);
            var outline = manager.LabelOutlineColor.AlphaMultiplied(opacity);
            for (var x = -1; x <= 1; x++)
                for (var y = -1; y <= 1; y++)
                    if (x != 0 || y != 0)
                        Render2D.DrawText(Style.Current.FontMedium, label, new Rectangle(bounds.X + x, bounds.Y + y, bounds.Width, bounds.Height), outline, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, scale);
            Render2D.DrawText(Style.Current.FontMedium, label, bounds, color, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, scale);
        }

        private static float GetLabelDistance(Vector3 position, bool sceneView)
        {
            if (sceneView && Editor.Instance?.Windows?.EditWin?.Viewport != null)
                return (float)Vector3.Distance(position, Editor.Instance.Windows.EditWin.Viewport.ViewPosition);
            if (!sceneView && Camera.MainCamera != null)
                return (float)Vector3.Distance(position, Camera.MainCamera.Position);
            return float.MaxValue;
        }

        public static void Detach()
        {
            AudioEventSystem.EventCallback -= OnEventCallback;
            _labels.Clear();
            if (_scene != null)
            {
                _scene.Parent = null;
                _scene = null;
            }
            if (_game != null)
            {
                _game.Parent = null;
                _game = null;
            }
            if (_sceneLabels != null)
            {
                _sceneLabels.Parent = null;
                _sceneLabels = null;
            }
            if (_gameLabels != null)
            {
                _gameLabels.Parent = null;
                _gameLabels = null;
            }
        }
    }
}
