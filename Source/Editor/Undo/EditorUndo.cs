// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.History;
using FlaxEditor.Options;
using FlaxEditor.SceneEditing;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Implementation of <see cref="Undo"/> customized for the <see cref="Editor"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Undo" />
    public class EditorUndo : Undo
    {
        private struct SceneState
        {
            public int Before;
            public int After;
        }

        private readonly Editor _editor;
        private readonly Dictionary<IUndoAction, Dictionary<Guid, SceneState>> _sceneStates = new Dictionary<IUndoAction, Dictionary<Guid, SceneState>>();
        private readonly Dictionary<Guid, int> _currentSceneStates = new Dictionary<Guid, int>();
        private readonly Dictionary<Guid, int> _savedSceneStates = new Dictionary<Guid, int>();
        private int _nextSceneStateID = 1;

        /// <summary>
        /// Gets a value indicating whether scene undo history has changes since the last scene save point.
        /// </summary>
        public bool HasUnsavedSceneChanges
        {
            get
            {
                foreach (var entry in _currentSceneStates)
                {
                    _savedSceneStates.TryGetValue(entry.Key, out var saved);
                    if (entry.Value != saved)
                        return true;
                }
                return false;
            }
        }

        internal EditorUndo(Editor editor)
        : base(500)
        {
            _editor = editor;

            OnOptionsChanged(editor.Options.Options);
            editor.Options.OptionsChanged += OnOptionsChanged;
            ActionDiscarded += OnActionDiscarded;
        }

        private void OnOptionsChanged(EditorOptions options)
        {
            Capacity = options.General.UndoActionsCapacity;
            SizeCapacityInBytes = options.General.UndoHistorySizeLimitMB > 0
                ? options.General.UndoHistorySizeLimitMB * 1024L * 1024L
                : -1;
        }

        private void OnActionDiscarded(IUndoAction action, HistoryStackDiscardReason reason)
        {
            _sceneStates.Remove(action);
            if (reason != HistoryStackDiscardReason.SizeLimit)
                return;

            var info = UndoActionMetadata.GetActionInfo(action);
            var operation = string.IsNullOrEmpty(info.Operation) ? action.ActionString : info.Operation;
            Editor.LogWarning(string.Format("Undo history discarded '{0}' because the configured undo history size budget was exceeded.", operation));
        }

        /// <inheritdoc />
        public override bool Enabled
        {
            get => _editor.StateMachine.CurrentState.CanUseUndoRedo;
            set => throw new AccessViolationException("Cannot change enabled state of the editor main undo.");
        }

        /// <inheritdoc />
        protected override void OnAction(IUndoAction action)
        {
            if (CheckSceneEdited(action, out var sceneIds))
            {
                var states = new Dictionary<Guid, SceneState>(sceneIds.Length);
                for (int i = 0; i < sceneIds.Length; i++)
                {
                    var sceneId = sceneIds[i];
                    _currentSceneStates.TryGetValue(sceneId, out var current);
                    var state = new SceneState
                    {
                        Before = current,
                        After = _nextSceneStateID++,
                    };
                    states[sceneId] = state;
                    _currentSceneStates[sceneId] = state.After;
                }
                _sceneStates[action] = states;
                SceneDebug.Log("UndoActionAdded", $"Action='{action.ActionString}' Scenes={string.Join(",", sceneIds)}");
            }
            base.OnAction(action);
        }

        /// <inheritdoc />
        protected override void OnUndo(IUndoAction action)
        {
            if (CheckSceneEdited(action, out var sceneIds))
            {
                if (_sceneStates.TryGetValue(action, out var states))
                {
                    foreach (var entry in states)
                        _currentSceneStates[entry.Key] = entry.Value.Before;
                }
                else
                {
                    for (int i = 0; i < sceneIds.Length; i++)
                        MarkSceneChangedOutsideUndo(sceneIds[i]);
                }
                SyncSceneEditedFlags(sceneIds);
                SceneDebug.Log("UndoApplied", $"Action='{action.ActionString}' Scenes={string.Join(",", sceneIds)}");
            }
            base.OnUndo(action);
        }

        /// <inheritdoc />
        protected override void OnRedo(IUndoAction action)
        {
            if (CheckSceneEdited(action, out var sceneIds))
            {
                if (_sceneStates.TryGetValue(action, out var states))
                {
                    foreach (var entry in states)
                        _currentSceneStates[entry.Key] = entry.Value.After;
                }
                else
                {
                    for (int i = 0; i < sceneIds.Length; i++)
                        MarkSceneChangedOutsideUndo(sceneIds[i]);
                }
                SyncSceneEditedFlags(sceneIds);
                SceneDebug.Log("RedoApplied", $"Action='{action.ActionString}' Scenes={string.Join(",", sceneIds)}");
            }
            base.OnRedo(action);
        }

        /// <summary>
        /// Marks the current scene undo state as saved.
        /// </summary>
        public void MarkScenesSaved()
        {
            foreach (var entry in _currentSceneStates)
                _savedSceneStates[entry.Key] = entry.Value;
            SyncSceneEditedFlags(_currentSceneStates.Keys);
        }

        /// <summary>
        /// Gets the current undo state identifier for a Scene.
        /// </summary>
        public int GetSceneState(Guid sceneId)
        {
            return _currentSceneStates.TryGetValue(sceneId, out var state) ? state : 0;
        }

        /// <summary>
        /// Marks a captured Scene undo state as durably saved.
        /// </summary>
        public void MarkSceneSaved(Guid sceneId, int capturedState)
        {
            _savedSceneStates[sceneId] = capturedState;
            SyncSceneEditedFlags(new[] { sceneId });
        }

        /// <summary>
        /// Marks scene state dirty for scene edits that are not tracked by undo history.
        /// </summary>
        internal void MarkSceneChangedOutsideUndo(Guid sceneId)
        {
            if (sceneId == Guid.Empty)
                return;
            _currentSceneStates[sceneId] = _nextSceneStateID++;
        }

        /// <summary>
        /// Clears the history.
        /// </summary>
        public new void Clear()
        {
            base.Clear();
            _sceneStates.Clear();
            _currentSceneStates.Clear();
            _savedSceneStates.Clear();
            _nextSceneStateID = 1;
        }

        /// <summary>
        /// Invalidates history actions that retain live objects from a Scene being unloaded.
        /// Stable-ID actions remain suspended and can replay after the Scene is loaded again.
        /// </summary>
        internal void OnSceneUnloading(Guid sceneId)
        {
            var removed = 0;
            RemoveActions(action =>
            {
                var invalidate = MustInvalidateForSceneUnload(action, sceneId);
                if (invalidate)
                    removed++;
                return invalidate;
            });
            if (removed != 0)
                SceneDebug.Error(SceneMutationErrorCode.ReplayDependencyMissing, "UndoActionsInvalidated", $"InvalidatedActions={removed} Scene={sceneId} Reason=LiveSceneObject");
        }

        internal void DiscardSceneChanges(Guid sceneId)
        {
            RemoveActions(action => ReferencesScene(action, sceneId));
            _currentSceneStates.TryGetValue(sceneId, out var current);
            _savedSceneStates[sceneId] = current;
            SyncSceneEditedFlags(new[] { sceneId });
            SceneDebug.Log("SceneChangesDiscarded", $"Scene={sceneId} HistoryInvalidated=true");
        }

        private static bool ReferencesScene(IUndoAction action, Guid sceneId)
        {
            if (action is ISceneUndoAction stableAction)
            {
                var sceneIds = stableAction.SceneIds;
                for (int i = 0; i < sceneIds.Length; i++)
                {
                    if (sceneIds[i] == sceneId)
                        return true;
                }
                return false;
            }
            if (action is MultiUndoAction multiAction)
            {
                for (int i = 0; i < multiAction.Actions.Length; i++)
                {
                    if (ReferencesScene(multiAction.Actions[i], sceneId))
                        return true;
                }
                return false;
            }
            if (action is UndoActionObject objectAction)
            {
                var target = objectAction.PrepareData().TargetInstance;
                if (target is SceneGraph.SceneGraphNode node)
                    return node.ParentScene?.Scene?.ID == sceneId;
                if (target is Actor actor)
                    return actor.Scene?.ID == sceneId;
                if (target is Script script)
                    return script.Actor?.Scene?.ID == sceneId;
            }
            return action is ISceneEditAction;
        }

        private static bool MustInvalidateForSceneUnload(IUndoAction action, Guid sceneId)
        {
            if (action is ISceneUndoAction stableAction)
            {
                var sceneIds = stableAction.SceneIds;
                for (int i = 0; i < sceneIds.Length; i++)
                {
                    if (sceneIds[i] == sceneId)
                        return !stableAction.SupportsSceneReload;
                }
                return false;
            }
            if (action is SelectionChangeAction)
                return false;
            if (action is MultiUndoAction multiAction)
            {
                for (int i = 0; i < multiAction.Actions.Length; i++)
                {
                    if (MustInvalidateForSceneUnload(multiAction.Actions[i], sceneId))
                        return true;
                }
                return false;
            }
            if (action is UndoActionObject objectAction)
            {
                var target = objectAction.PrepareData().TargetInstance;
                if (target is SceneGraph.SceneGraphNode node)
                    return node.ParentScene?.Scene?.ID == sceneId;
                if (target is Actor actor)
                    return actor.Scene?.ID == sceneId;
                if (target is Script script)
                    return script.Actor?.Scene?.ID == sceneId;
            }

            // Unknown Scene actions do not declare stable replay metadata and may retain live objects.
            return action is ISceneEditAction;
        }

        private void SyncSceneEditedFlags(IEnumerable<Guid> sceneIds)
        {
            foreach (var sceneId in sceneIds)
            {
                _currentSceneStates.TryGetValue(sceneId, out var current);
                _savedSceneStates.TryGetValue(sceneId, out var saved);
                if (Editor.Instance.Scene.GetActorNode(Level.FindScene(sceneId)) is SceneGraph.Actors.SceneNode node)
                    node.IsEdited = current != saved;
            }
        }

        /// <summary>
        /// Checks if the any scene has been edited after performing the given action.
        /// </summary>
        /// <param name="action">The action.</param>
        /// <param name="sceneIds">The identifiers of Scenes affected by the action.</param>
        private bool CheckSceneEdited(IUndoAction action, out Guid[] sceneIds)
        {
            // Note: this is automatic tracking system to check if undo action modifies scene objects

            var sceneModule = Editor.Instance.Scene;
            const bool markScenes = true;
            var suppressUndoDirtyTracking = sceneModule.SuppressUndoDirtyTracking;
            sceneModule.SuppressUndoDirtyTracking = true;
            sceneModule.BeginSceneEditCapture();

            try
            {
                return CheckSceneEdited(action, sceneModule, markScenes);
            }
            finally
            {
                sceneIds = sceneModule.EndSceneEditCapture();
                sceneModule.SuppressUndoDirtyTracking = suppressUndoDirtyTracking;
            }
        }

        private static bool CheckSceneEdited(IUndoAction action, Modules.SceneModule sceneModule, bool markScenes)
        {
            var scenesDirty = false;

            // ReSharper disable once SuspiciousTypeConversion.Global
            if (action is ISceneEditAction sceneEditAction)
            {
                scenesDirty = true;
                if (markScenes)
                    sceneEditAction.MarkSceneEdited(sceneModule);
            }
            else if (action is UndoActionObject undoActionObject)
            {
                var data = undoActionObject.PrepareData();

                if (data.TargetInstance is SceneGraph.SceneGraphNode node)
                {
                    scenesDirty = true;
                    if (markScenes)
                        sceneModule.MarkSceneEdited(node.ParentScene);
                }
                else if (data.TargetInstance is Actor actor)
                {
                    scenesDirty = true;
                    if (markScenes)
                        sceneModule.MarkSceneEdited(actor.Scene);
                }
                else if (data.TargetInstance is Script script && script.Actor != null)
                {
                    scenesDirty = true;
                    if (markScenes)
                        sceneModule.MarkSceneEdited(script.Actor.Scene);
                }
            }
            else if (action is MultiUndoAction multiUndoAction)
            {
                for (int i = 0; i < multiUndoAction.Actions.Length; i++)
                    scenesDirty |= CheckSceneEdited(multiUndoAction.Actions[i], sceneModule, markScenes);
            }

            return scenesDirty;
        }
    }
}
