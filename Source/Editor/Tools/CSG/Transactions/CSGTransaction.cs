// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Transactions
{
    /// <summary>
    /// Lifecycle state of a CSG authoring transaction.
    /// </summary>
    public enum CSGTransactionState
    {
        /// <summary>No transaction is active.</summary>
        Inactive,
        /// <summary>Preview mutations may be applied.</summary>
        Preview,
        /// <summary>The transaction was committed to undo history.</summary>
        Committed,
        /// <summary>The transaction restored its initial state.</summary>
        RolledBack,
    }

    /// <summary>
    /// Lightweight telemetry exposed to diagnostics and the viewport overlay.
    /// </summary>
    public struct CSGTransactionTelemetry
    {
        /// <summary>The transaction state.</summary>
        public CSGTransactionState State;
        /// <summary>Number of captured brushes.</summary>
        public int TouchedBrushCount;
        /// <summary>Number of preview updates.</summary>
        public int PreviewUpdateCount;
        /// <summary>Duration of the latest preview update.</summary>
        public double LastPreviewUpdateMs;
        /// <summary>Managed bytes allocated by the latest preview update.</summary>
        public long LastPreviewAllocatedBytes;
        /// <summary>The most recent invalidation reason.</summary>
        public string InvalidationReason;
    }

    /// <summary>
    /// Reusable snapshot, preview, commit, rollback, duplicate, and invalidation contract for CSG tools.
    /// </summary>
    public sealed class CSGTransaction : IDisposable
    {
        private readonly List<CSGBoxBrushState> _before = new List<CSGBoxBrushState>(8);
        private readonly HashSet<Guid> _touched = new HashSet<Guid>();
        private readonly List<IUndoAction> _performedActions = new List<IUndoAction>(2);
        private readonly List<Action> _rollbackCallbacks = new List<Action>(2);
        private CSGTransactionTelemetry _telemetry;

        /// <summary>Gets whether preview mutation is active.</summary>
        public bool IsActive => _telemetry.State == CSGTransactionState.Preview;

        /// <summary>Gets current transaction telemetry.</summary>
        public CSGTransactionTelemetry Telemetry => _telemetry;

        /// <summary>
        /// Starts a clean preview transaction and captures the initial brushes.
        /// </summary>
        public void Begin(IEnumerable<BoxBrush> brushes = null)
        {
            if (IsActive)
                throw new InvalidOperationException("A CSG transaction is already active.");
            Clear(false);
            _telemetry.State = CSGTransactionState.Preview;
            if (brushes == null)
                return;
            foreach (var brush in brushes)
                Touch(brush);
        }

        /// <summary>
        /// Captures a brush before its first preview mutation.
        /// </summary>
        public bool Touch(BoxBrush brush)
        {
            if (!IsActive || brush == null || !_touched.Add(brush.ID))
                return false;
            _before.Add(CSGBoxBrushState.Capture(brush));
            _telemetry.TouchedBrushCount = _before.Count;
            return true;
        }

        /// <summary>
        /// Registers an already-performed create, duplicate, or selection action. Commit transfers
        /// it to undo history; rollback undoes and disposes it.
        /// </summary>
        public void RegisterPerformedAction(IUndoAction action)
        {
            if (!IsActive)
                throw new InvalidOperationException("No active CSG transaction.");
            if (action != null)
                _performedActions.Add(action);
        }

        /// <summary>
        /// Registers extra rollback-only cleanup such as restoring a tool-local selection snapshot.
        /// </summary>
        public void RegisterRollback(Action callback)
        {
            if (!IsActive)
                throw new InvalidOperationException("No active CSG transaction.");
            if (callback != null)
                _rollbackCallbacks.Add(callback);
        }

        /// <summary>
        /// Records preview cost and optionally schedules coalesced scene rebuilds for touched brushes.
        /// </summary>
        /// <param name="durationMs">Preview update duration in milliseconds.</param>
        /// <param name="allocatedBytes">Managed bytes allocated by the preview update.</param>
        /// <param name="requestRebuild">Whether to request an interactive scene rebuild.</param>
        public void RecordPreview(double durationMs, long allocatedBytes, bool requestRebuild = true)
        {
            if (!IsActive)
                return;
            _telemetry.PreviewUpdateCount++;
            _telemetry.LastPreviewUpdateMs = Math.Max(durationMs, 0.0);
            _telemetry.LastPreviewAllocatedBytes = Math.Max(allocatedBytes, 0);
            if (requestRebuild)
                RequestRebuilds(false);
        }

        /// <summary>
        /// Commits changed brush states and performed actions as exactly one undo entry.
        /// </summary>
        public bool Commit(Undo undo, string actionString = "Edit CSG")
        {
            if (!IsActive)
                return false;
            var actions = new List<IUndoAction>(_performedActions.Count + 1);
            actions.AddRange(_performedActions);
            var before = new List<CSGBoxBrushState>(_before.Count);
            var after = new List<CSGBoxBrushState>(_before.Count);
            for (int i = 0; i < _before.Count; i++)
            {
                var initial = _before[i];
                var brush = initial.Resolve();
                if (brush == null)
                    continue;
                var final = CSGBoxBrushState.Capture(brush);
                if (!initial.Matches(ref final))
                {
                    before.Add(initial);
                    after.Add(final);
                }
            }
            if (before.Count != 0)
                actions.Add(new EditBoxBrushAction(before.ToArray(), after.ToArray()));

            if (actions.Count != 0 && undo != null)
            {
                IUndoAction action = actions.Count == 1 ? actions[0] : new MultiUndoAction(actions, actionString);
                undo.AddAction(action);
            }
            if (actions.Count != 0)
                RequestRebuilds(true);
            _telemetry.State = CSGTransactionState.Committed;
            _performedActions.Clear();
            _rollbackCallbacks.Clear();
            return actions.Count != 0;
        }

        /// <summary>
        /// Restores every captured value and removes already-performed duplicate/create actions.
        /// </summary>
        public bool Rollback(string reason = null)
        {
            if (!IsActive)
                return false;
            for (int i = _before.Count - 1; i >= 0; i--)
                _before[i].Apply();
            for (int i = _performedActions.Count - 1; i >= 0; i--)
            {
                _performedActions[i].Undo();
                _performedActions[i].Dispose();
            }
            for (int i = _rollbackCallbacks.Count - 1; i >= 0; i--)
                _rollbackCallbacks[i]();
            RequestRebuilds(true);
            _telemetry.State = CSGTransactionState.RolledBack;
            _telemetry.InvalidationReason = reason;
            _performedActions.Clear();
            _rollbackCallbacks.Clear();
            return true;
        }

        /// <summary>
        /// Rolls back due to scene, actor, mode, focus, or play-state invalidation.
        /// </summary>
        public bool Invalidate(string reason)
        {
            return Rollback(reason ?? "Invalidated");
        }

        /// <inheritdoc />
        public void Dispose()
        {
            if (IsActive)
                Rollback("Disposed");
            Clear(false);
        }

        private void RequestRebuilds(bool final)
        {
            var scenes = new HashSet<Guid>();
            for (int i = 0; i < _before.Count; i++)
            {
                var brush = _before[i].Resolve();
                if (brush?.Scene == null || !scenes.Add(brush.Scene.ID))
                    continue;
                if (final)
                    CSGRebuildScheduler.Shared.RequestFinal(brush.Scene);
                else
                    CSGRebuildScheduler.Shared.RequestPreview(brush.Scene);
            }
        }

        private void Clear(bool disposeActions)
        {
            if (disposeActions)
            {
                for (int i = 0; i < _performedActions.Count; i++)
                    _performedActions[i].Dispose();
            }
            _before.Clear();
            _touched.Clear();
            _performedActions.Clear();
            _rollbackCallbacks.Clear();
            _telemetry = default;
            _telemetry.State = CSGTransactionState.Inactive;
        }
    }
}
