// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Rebuild
{
    /// <summary>
    /// Type of a CSG rebuild request.
    /// </summary>
    public enum CSGRebuildRequestKind
    {
        /// <summary>Throttled interactive preview.</summary>
        Preview,
        /// <summary>Immediate transaction commit or rollback result.</summary>
        Final,
        /// <summary>Ordinary editor mutation using the configured native debounce.</summary>
        External,
    }

    /// <summary>
    /// User-facing state of a scene CSG rebuild queue.
    /// </summary>
    public enum CSGRebuildVisualState
    {
        /// <summary>No unapplied request is known.</summary>
        UpToDate,
        /// <summary>A newer preview is waiting for the throttle window.</summary>
        Pending,
        /// <summary>The latest request was submitted to the native builder.</summary>
        Submitted,
        /// <summary>Source brushes changed while automatic rebuilding is disabled.</summary>
        Stale,
    }

    /// <summary>
    /// Immutable rebuild dispatch emitted by <see cref="CSGRebuildQueue"/>.
    /// </summary>
    public struct CSGRebuildDispatch
    {
        /// <summary>The scene identifier.</summary>
        public Guid SceneId;
        /// <summary>The request revision.</summary>
        public long Revision;
        /// <summary>The native builder debounce in milliseconds.</summary>
        public float TimeoutMs;
    }

    /// <summary>
    /// Snapshot of rebuild queue state used by overlays and tests.
    /// </summary>
    public struct CSGRebuildStatus
    {
        /// <summary>The current visual state.</summary>
        public CSGRebuildVisualState State;
        /// <summary>The newest requested revision.</summary>
        public long RequestedRevision;
        /// <summary>The newest submitted revision.</summary>
        public long SubmittedRevision;
        /// <summary>The newest acknowledged revision.</summary>
        public long CompletedRevision;
        /// <summary>Total logical requests.</summary>
        public int RequestCount;
        /// <summary>Total native dispatches.</summary>
        public int DispatchCount;
    }

    /// <summary>
    /// Pure revisioned and throttled CSG rebuild queue.
    /// </summary>
    public sealed class CSGRebuildQueue
    {
        private sealed class Entry
        {
            public long RequestedRevision;
            public long PendingRevision;
            public long SubmittedRevision;
            public long CompletedRevision;
            public double NextPreviewTime;
            public float PendingTimeoutMs;
            public int RequestCount;
            public int DispatchCount;
            public CSGRebuildVisualState State;
        }

        private readonly Dictionary<Guid, Entry> _entries = new Dictionary<Guid, Entry>();

        /// <summary>
        /// Minimum time between managed preview dispatches. Native CSG requests remain debounced too.
        /// </summary>
        public double PreviewIntervalSeconds { get; set; } = 0.05;

        /// <summary>
        /// Adds or replaces a scene rebuild request.
        /// </summary>
        public long Request(Guid sceneId, CSGRebuildRequestKind kind, bool autoRebuild, float timeoutMs, double now, out CSGRebuildDispatch dispatch)
        {
            dispatch = default;
            if (sceneId == Guid.Empty)
                return 0;
            if (!_entries.TryGetValue(sceneId, out var entry))
            {
                entry = new Entry();
                _entries.Add(sceneId, entry);
            }

            long revision = ++entry.RequestedRevision;
            entry.RequestCount++;
            if (!autoRebuild)
            {
                entry.PendingRevision = revision;
                entry.PendingTimeoutMs = timeoutMs;
                entry.State = CSGRebuildVisualState.Stale;
                return revision;
            }

            if (kind == CSGRebuildRequestKind.Preview && entry.SubmittedRevision != 0 && now < entry.NextPreviewTime)
            {
                entry.PendingRevision = revision;
                entry.PendingTimeoutMs = timeoutMs;
                entry.State = CSGRebuildVisualState.Pending;
                return revision;
            }

            float dispatchTimeout = kind == CSGRebuildRequestKind.Final ? 0.0f : Mathf.Max(timeoutMs, 0.0f);
            Submit(sceneId, entry, revision, dispatchTimeout, now, out dispatch);
            return revision;
        }

        /// <summary>
        /// Emits the newest throttled preview for a scene when its dispatch window opens.
        /// </summary>
        public bool TryDequeue(Guid sceneId, bool autoRebuild, double now, out CSGRebuildDispatch dispatch)
        {
            dispatch = default;
            if (!autoRebuild || !_entries.TryGetValue(sceneId, out var entry) || entry.PendingRevision == 0 || now < entry.NextPreviewTime)
                return false;
            Submit(sceneId, entry, entry.PendingRevision, entry.PendingTimeoutMs, now, out dispatch);
            return true;
        }

        /// <summary>
        /// Acknowledges a completed revision and rejects stale completions.
        /// </summary>
        public bool TryAcknowledge(Guid sceneId, long revision)
        {
            if (!_entries.TryGetValue(sceneId, out var entry) || revision != entry.SubmittedRevision)
                return false;
            entry.CompletedRevision = revision;
            entry.State = entry.PendingRevision > revision ? CSGRebuildVisualState.Pending : CSGRebuildVisualState.UpToDate;
            return true;
        }

        /// <summary>
        /// Gets queue state for a scene.
        /// </summary>
        public CSGRebuildStatus GetStatus(Guid sceneId)
        {
            if (!_entries.TryGetValue(sceneId, out var entry))
                return new CSGRebuildStatus { State = CSGRebuildVisualState.UpToDate };
            return new CSGRebuildStatus
            {
                State = entry.State,
                RequestedRevision = entry.RequestedRevision,
                SubmittedRevision = entry.SubmittedRevision,
                CompletedRevision = entry.CompletedRevision,
                RequestCount = entry.RequestCount,
                DispatchCount = entry.DispatchCount,
            };
        }

        /// <summary>
        /// Clears state for an unloaded scene.
        /// </summary>
        public void Remove(Guid sceneId)
        {
            _entries.Remove(sceneId);
        }

        private void Submit(Guid sceneId, Entry entry, long revision, float timeoutMs, double now, out CSGRebuildDispatch dispatch)
        {
            entry.PendingRevision = 0;
            entry.SubmittedRevision = revision;
            entry.DispatchCount++;
            entry.NextPreviewTime = now + Math.Max(PreviewIntervalSeconds, 0.0);
            entry.State = CSGRebuildVisualState.Submitted;
            dispatch = new CSGRebuildDispatch
            {
                SceneId = sceneId,
                Revision = revision,
                TimeoutMs = timeoutMs,
            };
        }
    }

    /// <summary>
    /// Editor-facing CSG rebuild scheduler layered over <see cref="Scene.BuildCSG(float)"/>.
    /// </summary>
    public sealed class CSGRebuildScheduler
    {
        private readonly Dictionary<Guid, Scene> _scenes = new Dictionary<Guid, Scene>();
        private readonly List<Guid> _sceneIds = new List<Guid>(8);
        private readonly CSGRebuildQueue _queue = new CSGRebuildQueue();

        /// <summary>The shared editor scheduler.</summary>
        public static CSGRebuildScheduler Shared { get; } = new CSGRebuildScheduler();

        /// <summary>
        /// Requests a throttled interactive preview rebuild.
        /// </summary>
        public long RequestPreview(Scene scene)
        {
            return Request(scene, CSGRebuildRequestKind.Preview);
        }

        /// <summary>
        /// Requests an immediate final rebuild after commit or rollback.
        /// </summary>
        public long RequestFinal(Scene scene)
        {
            return Request(scene, CSGRebuildRequestKind.Final);
        }

        /// <summary>
        /// Routes an ordinary editor mutation through the revision tracker and native debounce.
        /// </summary>
        public long RequestExternal(Scene scene)
        {
            return Request(scene, CSGRebuildRequestKind.External);
        }

        /// <summary>
        /// Flushes previews whose throttle window has opened.
        /// </summary>
        public void Update()
        {
            var editor = Editor.Instance;
            if (editor == null)
                return;
            bool autoRebuild = editor.Options.Options.General.AutoRebuildCSG && !editor.StateMachine.IsPlayMode;
            double now = GetTime();
            _sceneIds.Clear();
            _sceneIds.AddRange(_scenes.Keys);
            for (int i = 0; i < _sceneIds.Count; i++)
            {
                var id = _sceneIds[i];
                if (!_scenes.TryGetValue(id, out var scene) || scene == null)
                {
                    _scenes.Remove(id);
                    _queue.Remove(id);
                    continue;
                }
                if (_queue.TryDequeue(id, autoRebuild, now, out var dispatch))
                    Dispatch(ref dispatch);
            }
        }

        /// <summary>
        /// Gets the tracked state for a scene.
        /// </summary>
        public CSGRebuildStatus GetStatus(Scene scene)
        {
            return scene != null ? _queue.GetStatus(scene.ID) : new CSGRebuildStatus { State = CSGRebuildVisualState.UpToDate };
        }

        /// <summary>
        /// Forwards a builder completion when a future native completion callback is available.
        /// Stale revisions are deliberately ignored.
        /// </summary>
        public bool TryAcknowledge(Scene scene, long revision)
        {
            return scene != null && _queue.TryAcknowledge(scene.ID, revision);
        }

        private long Request(Scene scene, CSGRebuildRequestKind kind)
        {
            if (scene == null)
                return 0;
            var editor = Editor.Instance;
            bool autoRebuild = editor != null && editor.Options.Options.General.AutoRebuildCSG && !editor.StateMachine.IsPlayMode;
            float timeoutMs = editor?.Options.Options.General.AutoRebuildCSGTimeoutMs ?? 50.0f;
            _scenes[scene.ID] = scene;
            long revision = _queue.Request(scene.ID, kind, autoRebuild, timeoutMs, GetTime(), out var dispatch);
            if (dispatch.Revision != 0)
                Dispatch(ref dispatch);
            return revision;
        }

        private void Dispatch(ref CSGRebuildDispatch dispatch)
        {
            if (_scenes.TryGetValue(dispatch.SceneId, out var scene) && scene != null)
                scene.BuildCSG(dispatch.TimeoutMs);
        }

        private static double GetTime()
        {
            return (double)Stopwatch.GetTimestamp() / Stopwatch.Frequency;
        }
    }
}
