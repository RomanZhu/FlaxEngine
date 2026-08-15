// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Modules;

namespace FlaxEditor
{
    /// <summary>
    /// Interface for undo action that can modify scene data (actors, scripts, etc.)
    /// </summary>
    public interface ISceneEditAction
    {
        /// <summary>
        /// Marks the scenes edited.
        /// </summary>
        /// <param name="sceneModule">The scene module.</param>
        void MarkSceneEdited(SceneModule sceneModule);
    }

    /// <summary>
    /// Stable Scene dependency metadata for undo actions that can survive Scene unload and resolve objects on replay.
    /// </summary>
    public interface ISceneUndoAction : ISceneEditAction
    {
        /// <summary>
        /// Gets the Scenes read or written by this action.
        /// </summary>
        Guid[] SceneIds { get; }

        /// <summary>
        /// Gets a value indicating whether the action can remain suspended while a required Scene is unloaded.
        /// </summary>
        bool SupportsSceneReload { get; }
    }
}
