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

            /// <summary>
            /// The content selection before the scene selection change.
            /// </summary>
            public string[] ContentBefore;

            /// <summary>
            /// The content selection after the scene selection change.
            /// </summary>
            public string[] ContentAfter;
        }

        private Action<SceneGraphNode[]> _callback;
        private Action<string[]> _contentSelectionCallback;

        /// <summary>
        /// User selection has changed
        /// </summary>
        /// <param name="before">Previously selected nodes</param>
        /// <param name="after">Newly selected nodes</param>
        /// <param name="callback">Selection change callback</param>
        /// <param name="contentBefore">Previously selected content items.</param>
        /// <param name="contentAfter">Newly selected content items.</param>
        /// <param name="contentSelectionCallback">Content selection change callback.</param>
        public SelectionChangeAction(SceneGraphNode[] before, SceneGraphNode[] after, Action<SceneGraphNode[]> callback, string[] contentBefore = null, string[] contentAfter = null, Action<string[]> contentSelectionCallback = null)
        {
            Data = new DataStorage
            {
                Before = before,
                After = after,
                ContentBefore = contentBefore ?? Array.Empty<string>(),
                ContentAfter = contentAfter ?? Array.Empty<string>(),
            };
            _callback = callback;
            _contentSelectionCallback = contentSelectionCallback;
        }

        /// <inheritdoc />
        public override string ActionString => "Selection change";

        internal bool IsSameTransition(SceneGraphNode[] before, SceneGraphNode[] after, Action<SceneGraphNode[]> callback, string[] contentBefore = null, string[] contentAfter = null, Action<string[]> contentSelectionCallback = null)
        {
            var data = Data;
            return _callback == callback &&
                   _contentSelectionCallback == contentSelectionCallback &&
                   AreSame(data.Before, before) &&
                   AreSame(data.After, after) &&
                   AreSameContentSelection(data.ContentBefore, contentBefore) &&
                   AreSameContentSelection(data.ContentAfter, contentAfter);
        }

        private static bool AreSame(SceneGraphNode[] a, SceneGraphNode[] b)
        {
            a ??= Array.Empty<SceneGraphNode>();
            b ??= Array.Empty<SceneGraphNode>();
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                if (!ReferenceEquals(a[i], b[i]))
                    return false;
            }
            return true;
        }

        private static bool AreSameContentSelection(string[] a, string[] b)
        {
            a ??= Array.Empty<string>();
            b ??= Array.Empty<string>();
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                if (!string.Equals(a[i], b[i], StringComparison.OrdinalIgnoreCase))
                    return false;
            }
            return true;
        }

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
            _contentSelectionCallback?.Invoke(data.ContentAfter);
        }

        /// <inheritdoc />
        public override void Undo()
        {
            var data = Data;
            _callback(data.Before);
            _contentSelectionCallback?.Invoke(data.ContentBefore);
        }
    }
}
