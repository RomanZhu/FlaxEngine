// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.History;
using FlaxEditor.Options;
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
        private readonly Dictionary<IUndoAction, SceneState> _sceneStates = new Dictionary<IUndoAction, SceneState>();
        private int _nextSceneStateID = 1;
        private int _currentSceneStateID;
        private int _savedSceneStateID;

        /// <summary>
        /// Gets a value indicating whether scene undo history has changes since the last scene save point.
        /// </summary>
        public bool HasUnsavedSceneChanges => _currentSceneStateID != _savedSceneStateID;

        internal EditorUndo(Editor editor)
        : base(500)
        {
            _editor = editor;

            editor.Options.OptionsChanged += OnOptionsChanged;
        }

        private void OnOptionsChanged(EditorOptions options)
        {
            Capacity = options.General.UndoActionsCapacity;
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
            if (CheckSceneEdited(action))
            {
                var state = new SceneState
                {
                    Before = _currentSceneStateID,
                    After = _nextSceneStateID++,
                };
                _sceneStates[action] = state;
                _currentSceneStateID = state.After;
            }
            base.OnAction(action);
        }

        /// <inheritdoc />
        protected override void OnUndo(IUndoAction action)
        {
            if (CheckSceneEdited(action))
            {
                if (_sceneStates.TryGetValue(action, out var state))
                    _currentSceneStateID = state.Before;
                else
                    MarkSceneChangedOutsideUndo();
                SyncSceneEditedFlags();
            }
            base.OnUndo(action);
        }

        /// <inheritdoc />
        protected override void OnRedo(IUndoAction action)
        {
            if (CheckSceneEdited(action))
            {
                if (_sceneStates.TryGetValue(action, out var state))
                    _currentSceneStateID = state.After;
                else
                    MarkSceneChangedOutsideUndo();
                SyncSceneEditedFlags();
            }
            base.OnRedo(action);
        }

        /// <summary>
        /// Marks the current scene undo state as saved.
        /// </summary>
        public void MarkScenesSaved()
        {
            _savedSceneStateID = _currentSceneStateID;
        }

        /// <summary>
        /// Marks scene state dirty for scene edits that are not tracked by undo history.
        /// </summary>
        internal void MarkSceneChangedOutsideUndo()
        {
            _currentSceneStateID = _nextSceneStateID++;
        }

        /// <summary>
        /// Clears the history.
        /// </summary>
        public new void Clear()
        {
            base.Clear();
            _sceneStates.Clear();
            _nextSceneStateID = 1;
            _currentSceneStateID = 0;
            _savedSceneStateID = 0;
        }

        private void SyncSceneEditedFlags()
        {
            if (!HasUnsavedSceneChanges)
                Editor.Instance.Scene.ClearEditedScenes();
        }

        /// <summary>
        /// Checks if the any scene has been edited after performing the given action.
        /// </summary>
        /// <param name="action">The action.</param>
        private bool CheckSceneEdited(IUndoAction action)
        {
            // Note: this is automatic tracking system to check if undo action modifies scene objects

            var sceneModule = Editor.Instance.Scene;
            var scenesDirty = false;
            var markScenes = !sceneModule.IsEverySceneEdited();
            var suppressUndoDirtyTracking = sceneModule.SuppressUndoDirtyTracking;
            sceneModule.SuppressUndoDirtyTracking = true;

            try
            {
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
                    // Process child actions
                    for (int i = 0; i < multiUndoAction.Actions.Length; i++)
                    {
                        scenesDirty |= CheckSceneEdited(multiUndoAction.Actions[i]);
                    }
                }
            }
            finally
            {
                sceneModule.SuppressUndoDirtyTracking = suppressUndoDirtyTracking;
            }

            return scenesDirty;
        }
    }
}
