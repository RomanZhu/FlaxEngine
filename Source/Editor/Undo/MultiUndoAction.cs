// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> that contains one or more child actions performed at once. Allows to merge different actions.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    [Serializable]
    [HideInEditor]
    public class MultiUndoAction : ITryUndoAction, IUndoActionMetadata
    {
        /// <summary>
        /// The child actions.
        /// </summary>
        [Serialize]
        public readonly IUndoAction[] Actions;

        /// <summary>
        /// Initializes a new instance of the <see cref="MultiUndoAction"/> class.
        /// </summary>
        /// <param name="actions">The actions to include within this multi action.</param>
        public MultiUndoAction(params IUndoAction[] actions)
        {
            Actions = actions?.ToArray() ?? throw new ArgumentNullException();
            if (Actions.Length == 0)
                throw new ArgumentException("Empty actions collection.");
            ActionString = Actions[0].ActionString;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="MultiUndoAction"/> class.
        /// </summary>
        /// <param name="actions">The actions to include within this multi action.</param>
        /// <param name="actionString">The action string.</param>
        public MultiUndoAction(IEnumerable<IUndoAction> actions, string actionString = null)
        {
            Actions = actions?.ToArray() ?? throw new ArgumentNullException();
            if (Actions.Length == 0)
                throw new ArgumentException("Empty actions collection.");
            ActionString = actionString ?? Actions[0].ActionString;
        }

        /// <inheritdoc />
        public string ActionString { get; }

        /// <inheritdoc />
        public UndoActionInfo ActionInfo
        {
            get
            {
                var result = UndoActionMetadata.GetActionInfo(Actions[0]).Clone();
                result.Operation = ActionString;
                if (Actions.Length > 1)
                    result.TargetType = UndoActionTargetType.Multiple;

                long sizeInBytes = 0;
                bool sizeKnown = true;
                for (int i = 0; i < Actions.Length; i++)
                {
                    var info = UndoActionMetadata.GetActionInfo(Actions[i]);
                    result.Flags |= info.Flags;
                    if (info.SizeInBytes >= 0)
                        sizeInBytes += info.SizeInBytes;
                    else
                        sizeKnown = false;
                }
                if (sizeKnown)
                    result.SizeInBytes = sizeInBytes;

                return result;
            }
        }

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryDo()
        {
            for (int i = 0; i < Actions.Length; i++)
            {
                if (TryDo(Actions[i]))
                    continue;
                for (int rollback = i - 1; rollback >= 0; rollback--)
                {
                    if (!TryUndo(Actions[rollback]))
                        Editor.LogError("Failed to roll back a partially applied multi-action: " + ActionString);
                }
                return false;
            }
            return true;
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            for (int i = Actions.Length - 1; i >= 0; i--)
            {
                if (TryUndo(Actions[i]))
                    continue;
                for (int rollback = i + 1; rollback < Actions.Length; rollback++)
                {
                    if (!TryDo(Actions[rollback]))
                        Editor.LogError("Failed to roll back a partially undone multi-action: " + ActionString);
                }
                return false;
            }
            return true;
        }

        private static bool TryDo(IUndoAction action)
        {
            if (action is ITryUndoAction tryAction)
                return tryAction.TryDo();
            action.Do();
            return true;
        }

        private static bool TryUndo(IUndoAction action)
        {
            if (action is ITryUndoAction tryAction)
                return tryAction.TryUndo();
            action.Undo();
            return true;
        }

        /// <inheritdoc />
        public void Dispose()
        {
            for (int i = 0; i < Actions.Length; i++)
            {
                Actions[i].Dispose();
            }
        }
    }
}
