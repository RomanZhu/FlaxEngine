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
        /// <summary>The target (scene or CSGModel) identifier.</summary>
        public Guid TargetId;
        /// <summary>The request revision.</summary>
        public long Revision;
        /// <summary>The native builder debounce in milliseconds.</summary>
        public float TimeoutMs;
        /// <summary>The request kind.</summary>
        public CSGRebuildRequestKind Kind;

        /// <summary>Legacy alias for TargetId.</summary>
        public Guid SceneId
        {
            get => TargetId;
            set => TargetId = value;
        }
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
            public CSGRebuildRequestKind PendingKind;
            public int RequestCount;
            public int DispatchCount;
            public CSGRebuildVisualState State;
        }

        private readonly Dictionary<Guid, Entry> _entries = new Dictionary<Guid, Entry>();

        /// <summary>
        /// Minimum time between managed preview dispatches. Dispatched previews use no additional
        /// native debounce so interactive geometry catches up during a continuous drag.
        /// </summary>
        public double PreviewIntervalSeconds { get; set; } = 0.05;

        /// <summary>
        /// Adds or replaces a target rebuild request.
        /// </summary>
        public long Request(Guid targetId, CSGRebuildRequestKind kind, bool autoRebuild, float timeoutMs, double now, out CSGRebuildDispatch dispatch, bool deferDispatch = false)
        {
            dispatch = default;
            if (targetId == Guid.Empty)
                return 0;
            if (!_entries.TryGetValue(targetId, out var entry))
            {
                entry = new Entry();
                _entries.Add(targetId, entry);
            }

            long revision = ++entry.RequestedRevision;
            entry.RequestCount++;
            if (!autoRebuild)
            {
                entry.PendingRevision = revision;
                entry.PendingTimeoutMs = timeoutMs;
                entry.PendingKind = kind;
                entry.State = CSGRebuildVisualState.Stale;
                return revision;
            }

            float dispatchTimeout = kind == CSGRebuildRequestKind.External ? Mathf.Max(timeoutMs, 0.0f) : 0.0f;
            if (deferDispatch)
            {
                entry.PendingRevision = revision;
                entry.PendingTimeoutMs = dispatchTimeout;
                entry.PendingKind = kind;
                entry.State = CSGRebuildVisualState.Pending;
                return revision;
            }
            if (kind == CSGRebuildRequestKind.Preview && entry.SubmittedRevision != 0 && now < entry.NextPreviewTime)
            {
                entry.PendingRevision = revision;
                entry.PendingTimeoutMs = dispatchTimeout;
                entry.PendingKind = kind;
                entry.State = CSGRebuildVisualState.Pending;
                return revision;
            }

            Submit(targetId, entry, revision, dispatchTimeout, kind, now, out dispatch);
            return revision;
        }

        /// <summary>
        /// Emits the newest throttled preview for a target when its dispatch window opens.
        /// </summary>
        public bool TryDequeue(Guid targetId, bool autoRebuild, double now, out CSGRebuildDispatch dispatch)
        {
            dispatch = default;
            if (!autoRebuild || !_entries.TryGetValue(targetId, out var entry) || entry.PendingRevision == 0 || now < entry.NextPreviewTime)
                return false;
            Submit(targetId, entry, entry.PendingRevision, entry.PendingTimeoutMs, entry.PendingKind, now, out dispatch);
            return true;
        }

        /// <summary>
        /// Acknowledges a completed revision and rejects stale completions.
        /// </summary>
        public bool TryAcknowledge(Guid targetId, long revision)
        {
            if (!_entries.TryGetValue(targetId, out var entry) || revision != entry.SubmittedRevision)
                return false;
            entry.CompletedRevision = revision;
            entry.State = entry.PendingRevision > revision ? CSGRebuildVisualState.Pending : CSGRebuildVisualState.UpToDate;
            return true;
        }

        /// <summary>
        /// Gets queue state for a target.
        /// </summary>
        public CSGRebuildStatus GetStatus(Guid targetId)
        {
            if (!_entries.TryGetValue(targetId, out var entry))
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
        /// Clears state for an unloaded target.
        /// </summary>
        public void Remove(Guid targetId)
        {
            _entries.Remove(targetId);
        }

        private void Submit(Guid targetId, Entry entry, long revision, float timeoutMs, CSGRebuildRequestKind kind, double now, out CSGRebuildDispatch dispatch)
        {
            entry.PendingRevision = 0;
            entry.PendingKind = CSGRebuildRequestKind.Preview;
            entry.SubmittedRevision = revision;
            entry.DispatchCount++;
            entry.NextPreviewTime = now + Math.Max(PreviewIntervalSeconds, 0.0);
            entry.State = CSGRebuildVisualState.Submitted;
            dispatch = new CSGRebuildDispatch
            {
                TargetId = targetId,
                Revision = revision,
                TimeoutMs = timeoutMs,
                Kind = kind,
            };
        }
    }

    /// <summary>
    /// Editor-facing CSG rebuild scheduler supporting Scene and CSGModel targets.
    /// </summary>
    public sealed class CSGRebuildScheduler
    {
        private readonly Dictionary<Guid, Actor> _targets = new Dictionary<Guid, Actor>();
        private readonly Dictionary<Guid, double> _dispatchNotBefore = new Dictionary<Guid, double>();
        private readonly List<Guid> _targetIds = new List<Guid>(8);
        private readonly CSGRebuildQueue _queue = new CSGRebuildQueue();

        private const double FinalDispatchGraceSeconds = 0.12;

        /// <summary>The shared editor scheduler.</summary>
        public static CSGRebuildScheduler Shared { get; } = new CSGRebuildScheduler();

        /// <summary>
        /// Resolves the compilation target root (CSGModel or Scene) for any actor.
        /// </summary>
        public static Actor ResolveTarget(Actor actor)
        {
            if (actor == null)
                return null;
            Actor current = actor.Parent;
            while (current != null)
            {
                if (current is CSGModel model)
                    return model;
                current = current.Parent;
            }
            return actor.Scene;
        }

        /// <summary>
        /// Requests a throttled interactive preview rebuild.
        /// </summary>
        public long RequestPreview(Actor target)
        {
            return Request(target, CSGRebuildRequestKind.Preview);
        }

        /// <summary>
        /// Requests a throttled interactive preview rebuild for a scene.
        /// </summary>
        public long RequestPreview(Scene scene)
        {
            return Request(scene, CSGRebuildRequestKind.Preview);
        }

        /// <summary>
        /// Requests an immediate final rebuild after commit or rollback.
        /// </summary>
        public long RequestFinal(Actor target)
        {
            return Request(target, CSGRebuildRequestKind.Final);
        }

        /// <summary>
        /// Requests an immediate final rebuild for a scene after commit or rollback.
        /// </summary>
        public long RequestFinal(Scene scene)
        {
            return Request(scene, CSGRebuildRequestKind.Final);
        }

        /// <summary>
        /// Routes an ordinary editor mutation through the revision tracker and native debounce.
        /// </summary>
        public long RequestExternal(Actor target)
        {
            return Request(target, CSGRebuildRequestKind.External);
        }

        /// <summary>
        /// Routes an ordinary editor mutation for a scene through the revision tracker and native debounce.
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
            // Publishing rebuilt CSG while RMB navigation owns the cursor can break native mouse
            // capture. Keep the latest revision queued until navigation releases it.
            if (IsViewportNavigationActive(editor))
                return;
            bool autoRebuild = editor.Options.Options.General.AutoRebuildCSG && !editor.StateMachine.IsPlayMode;
            double now = GetTime();
            _targetIds.Clear();
            _targetIds.AddRange(_targets.Keys);
            for (int i = 0; i < _targetIds.Count; i++)
            {
                var id = _targetIds[i];
                if (!_targets.TryGetValue(id, out var target) || target == null)
                {
                    _targets.Remove(id);
                    _dispatchNotBefore.Remove(id);
                    _queue.Remove(id);
                    continue;
                }
                if (_dispatchNotBefore.TryGetValue(id, out var notBefore) && now < notBefore)
                    continue;
                if (_queue.TryDequeue(id, autoRebuild, now, out var dispatch))
                {
                    _dispatchNotBefore.Remove(id);
                    Dispatch(ref dispatch);
                }
            }
        }

        /// <summary>
        /// Gets the tracked state for a target.
        /// </summary>
        public CSGRebuildStatus GetStatus(Actor target)
        {
            return target != null ? _queue.GetStatus(target.ID) : new CSGRebuildStatus { State = CSGRebuildVisualState.UpToDate };
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
        public bool TryAcknowledge(Actor target, long revision)
        {
            return target != null && _queue.TryAcknowledge(target.ID, revision);
        }

        /// <summary>
        /// Forwards a builder completion for a scene.
        /// </summary>
        public bool TryAcknowledge(Scene scene, long revision)
        {
            return scene != null && _queue.TryAcknowledge(scene.ID, revision);
        }

        private long Request(Actor target, CSGRebuildRequestKind kind)
        {
            if (target == null)
                return 0;
            var editor = Editor.Instance;
            bool autoRebuild = editor != null && editor.Options.Options.General.AutoRebuildCSG && !editor.StateMachine.IsPlayMode;
            float timeoutMs = editor?.Options.Options.General.AutoRebuildCSGTimeoutMs ?? 50.0f;
            _targets[target.ID] = target;
            double now = GetTime();
            bool deferDispatch = kind == CSGRebuildRequestKind.Final || IsViewportNavigationActive(editor);
            if (kind == CSGRebuildRequestKind.Final)
                _dispatchNotBefore[target.ID] = now + FinalDispatchGraceSeconds;
            long revision = _queue.Request(target.ID, kind, autoRebuild, timeoutMs, now, out var dispatch, deferDispatch);
            if (dispatch.Revision != 0)
                Dispatch(ref dispatch);
            return revision;
        }

        private void Dispatch(ref CSGRebuildDispatch dispatch)
        {
            if (_targets.TryGetValue(dispatch.TargetId, out var target) && target != null)
            {
                if (target is Scene scene)
                    scene.BuildCSG(dispatch.TimeoutMs);
                else if (target is CSGModel model)
                    model.BuildCSG(dispatch.TimeoutMs);
            }
        }

        private static double GetTime()
        {
            return (double)Stopwatch.GetTimestamp() / Stopwatch.Frequency;
        }

        private static bool IsViewportNavigationActive(Editor editor)
        {
            var viewport = editor?.Windows?.EditWin?.Viewport;
            return viewport != null &&
                   (viewport.IsRightMouseButtonDown || viewport.IsAltKeyDown);
        }
    }
}
