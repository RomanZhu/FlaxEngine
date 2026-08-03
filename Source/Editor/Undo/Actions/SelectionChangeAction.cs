// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.SceneGraph;

namespace FlaxEditor
{
    /// <summary>
    /// Objects selection change action.
    /// </summary>
    /// <seealso cref="IUndoAction" />
    [Serializable]
    public class SelectionChangeAction : UndoActionBase<SelectionChangeAction.DataStorage>, IUndoActionMetadata
    {
        /// <summary>
        /// The undo data.
        /// </summary>
        [Serializable]
        public struct DataStorage
        {
            /// <summary>
            /// The 'before' selection.
            /// </summary>
            public SceneGraphNode[] Before;

            /// <summary>
            /// The 'after' selection.
            /// </summary>
            public SceneGraphNode[] After;
        }

        private Action<SceneGraphNode[]> _callback;

        /// <summary>
        /// User selection has changed
        /// </summary>
        /// <param name="before">Previously selected nodes</param>
        /// <param name="after">Newly selected nodes</param>
        /// <param name="callback">Selection change callback</param>
        public SelectionChangeAction(SceneGraphNode[] before, SceneGraphNode[] after, Action<SceneGraphNode[]> callback)
        {
            Data = new DataStorage
            {
                Before = before,
                After = after,
            };
            _callback = callback;
        }

        /// <inheritdoc />
        public override string ActionString => "Selection change";

        /// <inheritdoc />
        public UndoActionInfo ActionInfo
        {
            get
            {
                var data = Data;
                var after = data.After ?? Array.Empty<SceneGraphNode>();
                var target = after.Length == 1 ? after[0] : null;
                return new UndoActionInfo
                {
                    Operation = ActionString,
                    TargetType = target != null ? UndoActionTargetType.SceneObject : UndoActionTargetType.Multiple,
                    TargetName = target != null ? target.Name : "Scene Selection",
                    TargetId = target?.ID ?? Guid.Empty,
                    TargetObjectId = target?.ID.ToString("N"),
                    Flags = UndoActionFlags.SelectionOnly,
                    SizeInBytes = 0,
                };
            }
        }

        /// <inheritdoc />
        public override void Do()
        {
            var data = Data;
            _callback(data.After);
        }

        /// <inheritdoc />
        public override void Undo()
        {
            var data = Data;
            _callback(data.Before);
        }
    }
}
