// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEngine;
using FlaxEditor.Utilities;
using FlaxEngine.Utilities;

namespace FlaxEditor.States
{
    /// <summary>
    /// In this state user may edit scene and use editor normally.
    /// </summary>
    /// <seealso cref="FlaxEditor.States.EditorState" />
    [HideInEditor]
    public sealed class EditingSceneState : EditorState
    {
        internal string AutoSaveStatus;

        /// <inheritdoc />
        public override bool CanUseToolbox => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanUseUndoRedo => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanChangeScene => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanEditScene => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanEditContent => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanEnterPlayMode => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override bool CanReloadScripts => !Editor.MultiplayerPlayMode.IsReplica;

        /// <inheritdoc />
        public override string Status => Editor.MultiplayerPlayMode.IsReplica ? "Multiplayer replica (read-only)" : AutoSaveStatus;

        internal EditingSceneState(Editor editor)
        : base(editor)
        {
            UpdateFPS();
        }

        /// <inheritdoc />
        public override void OnEnter()
        {
            base.OnEnter();

            ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
        }

        /// <inheritdoc />
        public override void OnExit(State nextState)
        {
            ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;

            base.OnExit(nextState);
        }

        private void OnScriptsReloadBegin()
        {
            StateMachine.GoToState<ReloadingScriptsState>();
        }
    }
}
