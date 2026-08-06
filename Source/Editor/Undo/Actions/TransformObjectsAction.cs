// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Modules;
using FlaxEditor.SceneGraph;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> used to transform a selection of <see cref="SceneGraphNode"/>.
    /// The same logic could be achieved using <see cref="UndoMultiBlock"/> but it would be slower.
    /// Since we use this kind of action very ofter (for <see cref="FlaxEditor.Gizmo.TransformGizmo"/> operations) it's better to provide faster implementation.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    [Serializable]
    public sealed class TransformObjectsAction : UndoActionBase<TransformObjectsAction.DataStorage>, ISceneEditAction
    {
        /// <summary>
        /// The undo data.
        /// </summary>
        [Serializable]
        public struct DataStorage
        {
            /// <summary>
            /// The scene of the selected objects.
            /// </summary>
            public Scene Scene;

            /// <summary>
            /// The selection pool.
            /// </summary>
            public SceneGraphNode[] Selection;

            /// <summary>
            /// Stable selection identities used to reacquire nodes after a
            /// duplicate action recreates them during redo.
            /// </summary>
            public Guid[] SelectionIds;

            /// <summary>
            /// The 'before' state.
            /// </summary>
            public Transform[] Before;

            /// <summary>
            /// The 'after' state.
            /// </summary>
            public Transform[] After;

            /// <summary>
            /// The cached bounding box that contains all selected items in 'before' state.
            /// </summary>
            public BoundingBox BeforeBounds;

            /// <summary>
            /// The cached bounding box that contains all selected items in 'after' state.
            /// </summary>
            public BoundingBox AfterBounds;

            /// <summary>
            /// True if navigation system has been modified during editing the selected objects (navmesh auto-rebuild is required).
            /// </summary>
            public bool NavigationDirty;
        }

        internal TransformObjectsAction(List<SceneGraphNode> selection, List<Transform> before, ref BoundingBox boundsBefore, bool navigationDirty)
        {
            var after = Utilities.Utils.GetTransformsAndBounds(selection, out var afterBounds);

            // TODO: support moving objects from more than one scene
            var scene = selection.Count != 0 ? selection[0].ParentScene?.Scene : null;

            var data = new DataStorage
            {
                Scene = scene,
                // Keep the legacy field available for old history entries,
                // but do not retain live node references for new actions.
                Selection = null,
                SelectionIds = GetSelectionIds(selection),
                After = after,
                Before = before.ToArray(),
                BeforeBounds = boundsBefore,
                AfterBounds = afterBounds,
                NavigationDirty = navigationDirty,
            };
            Data = data;

            InvalidateBounds(ref data);
        }

        /// <inheritdoc />
        public override string ActionString => "Transform object(s)";

        /// <inheritdoc />
        public override void Do()
        {
            var data = Data;
            var selection = ResolveSelection(ref data);
            var count = Math.Min(selection.Length, data.After?.Length ?? 0);
            for (int i = 0; i < count; i++)
            {
                var node = selection[i];
                if (IsNodeUsable(node))
                    node.Transform = data.After[i];
            }
            InvalidateBounds(ref data);
        }

        /// <inheritdoc />
        public override void Undo()
        {
            var data = Data;
            var selection = ResolveSelection(ref data);
            var count = Math.Min(selection.Length, data.Before?.Length ?? 0);
            for (int i = 0; i < count; i++)
            {
                var node = selection[i];
                if (IsNodeUsable(node))
                    node.Transform = data.Before[i];
            }
            InvalidateBounds(ref data);
        }

        private static bool IsNodeUsable(SceneGraphNode node)
        {
            if (node == null)
                return false;
            try
            {
                return node.IsActive;
            }
            catch
            {
                return false;
            }
        }

        private static Guid[] GetSelectionIds(IReadOnlyList<SceneGraphNode> selection)
        {
            var result = new Guid[selection?.Count ?? 0];
            for (int i = 0; i < result.Length; i++)
                result[i] = selection[i]?.ID ?? Guid.Empty;
            return result;
        }

        private static SceneGraphNode[] ResolveSelection(ref DataStorage data)
        {
            var ids = data.SelectionIds;
            if (ids == null || ids.Length == 0)
                return data.Selection ?? Array.Empty<SceneGraphNode>();

            var selection = new SceneGraphNode[ids.Length];
            for (int i = 0; i < ids.Length; i++)
            {
                var node = ids[i] != Guid.Empty ? SceneGraphFactory.FindNode(ids[i]) : null;
                if (node == null && ids[i] != Guid.Empty)
                    node = SceneGraphFactory.GetNode(ids[i]);
                if (node == null && data.Selection != null && i < data.Selection.Length && IsNodeUsable(data.Selection[i]))
                    node = data.Selection[i];
                selection[i] = node;
            }
            data.Selection = selection;
            return selection;
        }

        private void InvalidateBounds(ref DataStorage data)
        {
            if (!data.NavigationDirty)
                return;

            var editor = Editor.Instance;
            bool isPlayMode = editor.StateMachine.IsPlayMode;
            var options = editor.Options.Options;

            // Auto NavMesh rebuild
            if (!isPlayMode && options.General.AutoRebuildNavMesh && data.Scene != null)
            {
                // Handle simple case where objects were moved just a little and use one navmesh build request to improve performance
                if (data.BeforeBounds.Intersects(ref data.AfterBounds))
                {
                    Navigation.BuildNavMesh(BoundingBox.Merge(data.BeforeBounds, data.AfterBounds), options.General.AutoRebuildNavMeshTimeoutMs);
                }
                else
                {
                    Navigation.BuildNavMesh(data.BeforeBounds, options.General.AutoRebuildNavMeshTimeoutMs);
                    Navigation.BuildNavMesh(data.AfterBounds, options.General.AutoRebuildNavMeshTimeoutMs);
                }
            }
        }

        void ISceneEditAction.MarkSceneEdited(SceneModule sceneModule)
        {
            var data = Data;
            var selection = ResolveSelection(ref data);
            for (int i = 0; i < selection.Length; i++)
            {
                var node = selection[i];
                if (node != null)
                    sceneModule.MarkSceneEdited(node.ParentScene);
            }
        }
    }
}
