// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Modules;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEditor.Tools.CSG.Transactions;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Restores complete before/after states for one or more edited box brushes.
    /// </summary>
    [Serializable]
    public sealed class EditBoxBrushAction : UndoActionBase<EditBoxBrushAction.DataStorage>, ISceneEditAction
    {
        /// <summary>The serialized undo data.</summary>
        [Serializable]
        public struct DataStorage
        {
            /// <summary>The state before editing.</summary>
            public CSGBoxBrushState[] Before;
            /// <summary>The state after editing.</summary>
            public CSGBoxBrushState[] After;
        }

        /// <summary>
        /// Initializes a box-brush edit action. The edit has already been applied.
        /// </summary>
        public EditBoxBrushAction(CSGBoxBrushState[] before, CSGBoxBrushState[] after)
        {
            if (before == null || after == null || before.Length == 0 || before.Length != after.Length)
                throw new ArgumentException("Box brush undo states must be non-empty and paired.");
            Data = new DataStorage
            {
                Before = before,
                After = after,
            };
        }

        /// <inheritdoc />
        public override string ActionString => "Edit CSG brush";

        /// <inheritdoc />
        public override void Do()
        {
            Apply(Data.After);
        }

        /// <inheritdoc />
        public override void Undo()
        {
            Apply(Data.Before);
        }

        /// <inheritdoc />
        public void MarkSceneEdited(SceneModule sceneModule)
        {
            var states = Data.After;
            var scenes = new HashSet<Guid>();
            for (int i = 0; i < states.Length; i++)
            {
                var brush = states[i].Resolve();
                if (brush?.Scene != null && scenes.Add(brush.Scene.ID))
                    sceneModule.MarkSceneEdited(brush.Scene);
            }
        }

        private static void Apply(CSGBoxBrushState[] states)
        {
            var scenes = new HashSet<Guid>();
            for (int i = 0; i < states.Length; i++)
            {
                states[i].Apply();
                var brush = states[i].Resolve();
                if (brush?.Scene != null && scenes.Add(brush.Scene.ID))
                    CSGRebuildScheduler.Shared.RequestFinal(brush.Scene);
            }
        }
    }
}
