// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.History;

namespace FlaxEditor
{
    /// <summary>
    /// Interface for <see cref="Undo"/> actions.
    /// </summary>
    /// <seealso cref="FlaxEditor.History.IHistoryAction" />
    public interface IUndoAction : IHistoryAction
    {
        /// <summary>
        /// Performs this action.
        /// </summary>
        void Do();

        /// <summary>
        /// Undoes this action.
        /// </summary>
        void Undo();
    }

    /// <summary>
    /// Optional undo action contract for operations that can fail without changing state.
    /// The undo system transfers these actions between history stacks only after success.
    /// </summary>
    public interface ITryUndoAction : IUndoAction
    {
        /// <summary>
        /// Attempts to perform the action.
        /// </summary>
        /// <returns>True on success, otherwise false.</returns>
        bool TryDo();

        /// <summary>
        /// Attempts to undo the action.
        /// </summary>
        /// <returns>True on success, otherwise false.</returns>
        bool TryUndo();
    }
}
