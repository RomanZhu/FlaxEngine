// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Modules;
using FlaxEngine;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Change <see cref="Script"/> order or enable/disable undo action.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    /// <seealso cref="FlaxEditor.ISceneEditAction" />
    [Serializable]
    class ChangeScriptAction : ITryUndoAction, ISceneUndoAction
    {
        [Serialize]
        private Guid _scriptId;

        [Serialize]
        private Guid _sceneId;

        [Serialize]
        private bool _enableA;

        [Serialize]
        private int _orderA;

        [Serialize]
        private bool _enableB;

        [Serialize]
        private int _orderB;

        private ChangeScriptAction(Script script, bool enable, int order)
        {
            _scriptId = script.ID;
            _sceneId = script.Scene?.ID ?? Guid.Empty;
            _enableA = script.Enabled;
            _orderA = script.OrderInParent;
            _enableB = enable;
            _orderB = order;
        }

        /// <summary>
        /// Creates new undo action that changes script order in parent actor scripts collection.
        /// </summary>
        /// <param name="script">The script to reorder.</param>
        /// <param name="newOrder">New index.</param>
        /// <returns>The action (not performed yet).</returns>
        public static ChangeScriptAction ChangeOrder(Script script, int newOrder)
        {
            return new ChangeScriptAction(script, script.Enabled, newOrder);
        }

        /// <summary>
        /// Creates new undo action that enables/disables script.
        /// </summary>
        /// <param name="script">The script to enable or disable.</param>
        /// <param name="newEnabled">New enable state.</param>
        /// <returns>The action (not performed yet).</returns>
        public static ChangeScriptAction ChangeEnabled(Script script, bool newEnabled)
        {
            return new ChangeScriptAction(script, newEnabled, script.OrderInParent);
        }

        /// <inheritdoc />
        public string ActionString => "Edit script";

        /// <inheritdoc />
        public Guid[] SceneIds => _sceneId != Guid.Empty ? new[] { _sceneId } : Array.Empty<Guid>();

        /// <inheritdoc />
        public bool SupportsSceneReload => true;

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public bool TryDo()
        {
            return Apply(_enableB, _orderB);
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            return Apply(_enableA, _orderA);
        }

        private bool Apply(bool enabled, int order)
        {
            if (_sceneId != Guid.Empty && Level.FindScene(_sceneId) == null)
                return false;
            var script = FlaxEngine.Object.Find<Script>(ref _scriptId);
            if (script == null)
                return false;
            var previousEnabled = script.Enabled;
            var previousOrder = script.OrderInParent;
            try
            {
                script.Enabled = enabled;
                script.OrderInParent = order;
                return script.Enabled == enabled && script.OrderInParent == order;
            }
            catch
            {
                script.Enabled = previousEnabled;
                script.OrderInParent = previousOrder;
                return false;
            }
        }

        /// <inheritdoc />
        public void Dispose()
        {
        }

        /// <inheritdoc />
        public void MarkSceneEdited(SceneModule sceneModule)
        {
            if (_sceneId != Guid.Empty)
                sceneModule.MarkSceneEdited(Level.FindScene(_sceneId));
        }
    }
}
