// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.CustomEditors.Dedicated;
using FlaxEditor.CustomEditors;
using FlaxEngine;
using FlaxEngine.GUI;
using System;

namespace FlaxEditor.FMOD
{
    /// <summary>Live diagnostics controls for the FMOD observer actor.</summary>
    [CustomEditor(typeof(FMODAudioManager)), DefaultEditor]
    public sealed class FMODAudioManagerEditor : ActorEditor
    {
        private Label _snapshot;
        private Label _events;
        private DateTime _lastRefresh;
        private FmodDebugOverlay.RuntimeSort _sort = FmodDebugOverlay.RuntimeSort.Voices;
        private bool _descending = true;
        private static readonly TimeSpan RefreshInterval = TimeSpan.FromMilliseconds(200);

        public override void Initialize(LayoutElementsContainer layout)
        {
            base.Initialize(layout);
            var group = layout.Group("FMOD Diagnostics");
            _snapshot = group.Label(string.Empty).Label;
            _snapshot.AutoHeight = true;
            var table = layout.Group("Live Instances");
            _events = table.Label(string.Empty).Label;
            _events.AutoHeight = true;
            var sortButtons = table.UniformGrid();
            sortButtons.CustomControl.Height = Button.DefaultHeight;
            sortButtons.CustomControl.SlotsHorizontally = 5;
            sortButtons.CustomControl.SlotsVertically = 1;
            sortButtons.Button("Voices").Button.Clicked += () => SetSort(FmodDebugOverlay.RuntimeSort.Voices);
            sortButtons.Button("Name").Button.Clicked += () => SetSort(FmodDebugOverlay.RuntimeSort.Name);
            sortButtons.Button("Plays").Button.Clicked += () => SetSort(FmodDebugOverlay.RuntimeSort.Plays);
            sortButtons.Button("Volume").Button.Clicked += () => SetSort(FmodDebugOverlay.RuntimeSort.Volume);
            sortButtons.Button("Time").Button.Clicked += () => SetSort(FmodDebugOverlay.RuntimeSort.Time);
            var buttons = group.UniformGrid();
            buttons.CustomControl.Height = Button.DefaultHeight;
            buttons.CustomControl.SlotsHorizontally = 4;
            buttons.CustomControl.SlotsVertically = 1;
            buttons.Button("Capture").Button.Clicked += Capture;
            buttons.Button("Stop All").Button.Clicked += StopAll;
            buttons.Button("Reload Banks").Button.Clicked += ReloadBanks;
            buttons.Button("Restart Event Backend").Button.Clicked += RestartEventBackend;
        }

        private void Capture()
        {
            foreach (var value in Values)
                if (value is FMODAudioManager manager)
                    manager.CaptureDiagnostics();
            RefreshDiagnostics(true);
        }

        private void StopAll()
        {
            foreach (var value in Values)
                if (value is FMODAudioManager manager)
                    manager.StopAllEvents();
            RefreshDiagnostics(true);
        }

        private void ReloadBanks()
        {
            Editor.Instance?.FMOD?.ReloadBanks();
            RefreshDiagnostics(true);
        }

        private void RestartEventBackend()
        {
            Editor.Instance?.FMOD?.RestartEventBackend();
            RefreshDiagnostics(true);
        }

        private void SetSort(FmodDebugOverlay.RuntimeSort sort)
        {
            if (_sort == sort)
                _descending = !_descending;
            else
            {
                _sort = sort;
                _descending = true;
            }
            RefreshDiagnostics(true);
        }

        public override void Refresh()
        {
            base.Refresh();
            RefreshDiagnostics(false);
        }

        private void RefreshDiagnostics(bool force)
        {
            if (_snapshot == null)
                return;
            if (!force && DateTime.UtcNow - _lastRefresh < RefreshInterval)
                return;
            _lastRefresh = DateTime.UtcNow;
            foreach (var value in Values)
            {
                if (value is FMODAudioManager manager)
                {
                    var snapshot = manager.Snapshot;
                    _snapshot.Text = FmodDebugOverlay.FormatDiagnostics(snapshot, manager.SceneDiagnostics);
                    if (_events != null)
                        _events.Text = FmodDebugOverlay.FormatRuntimeEvents(snapshot, _sort, _descending);
                    break;
                }
            }
        }
    }
}
