// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Modules;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.Utilities;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> used to add/remove <see cref="Script"/> from the <see cref="Actor"/>.
    /// </summary>
    /// <seealso cref="IUndoAction" />
    [Serializable]
    sealed class AddRemoveScript : ITryUndoAction, ISceneUndoAction
    {
        [Serialize]
        private bool _isAdd;

        [Serialize]
        private Guid _scriptId;

        [Serialize]
        private Guid _prefabId;

        [Serialize]
        private Guid _prefabObjectId;

        [Serialize]
        private string _scriptTypeName;

        [Serialize]
        private string _scriptData;

        [Serialize]
        private Guid _parentId;

        [Serialize]
        private Guid _sceneId;

        [Serialize]
        private int _orderInParent;

        [Serialize]
        private bool _enabled;

        internal AddRemoveScript(bool isAdd, Script script)
        {
            _isAdd = isAdd;
            _scriptId = script.ID;
            _scriptTypeName = script.TypeName;
            _prefabId = script.PrefabID;
            _prefabObjectId = script.PrefabObjectID;
            try
            {
                _scriptData = FlaxEngine.Json.JsonSerializer.Serialize(script);
            }
            catch (Exception ex)
            {
                _scriptData = null;
                Debug.LogError("Failed to serialize script data for Undo due to exception");
                Debug.LogException(ex);
            }
            _parentId = script.Actor.ID;
            _sceneId = script.Actor.Scene?.ID ?? Guid.Empty;
            _orderInParent = script.OrderInParent;
            _enabled = script.Enabled;
        }

        internal AddRemoveScript(bool isAdd, Actor parentActor, ScriptType scriptType)
        {
            _isAdd = isAdd;
            _scriptId = Guid.NewGuid();
            _scriptTypeName = scriptType.TypeName;
            _scriptData = null;
            _parentId = parentActor.ID;
            _sceneId = parentActor.Scene?.ID ?? Guid.Empty;
            _orderInParent = -1;
            _enabled = true;
        }

        public int GetOrderInParent()
        {
            return _orderInParent;
        }

        /// <summary>
        /// Creates a new added script undo action.
        /// </summary>
        /// <param name="script">The new script.</param>
        /// <returns>The action.</returns>
        public static AddRemoveScript Added(Script script)
        {
            if (script == null)
                throw new ArgumentNullException(nameof(script));
            return new AddRemoveScript(true, script);
        }

        /// <summary>
        /// Creates a new add script undo action.
        /// </summary>
        /// <param name="parentActor">The parent actor.</param>
        /// <param name="scriptType">The script type.</param>
        /// <returns>The action.</returns>
        public static AddRemoveScript Add(Actor parentActor, ScriptType scriptType)
        {
            if (parentActor == null)
                throw new ArgumentNullException(nameof(parentActor));
            if (!scriptType)
                throw new ArgumentNullException(nameof(scriptType));
            return new AddRemoveScript(true, parentActor, scriptType);
        }

        /// <summary>
        /// Creates a new remove script undo action.
        /// </summary>
        /// <param name="script">The script.</param>
        /// <returns>The action.</returns>
        public static AddRemoveScript Remove(Script script)
        {
            if (script == null)
                throw new ArgumentNullException(nameof(script));
            return new AddRemoveScript(false, script);
        }

        /// <inheritdoc />
        public string ActionString => _isAdd ? "Add script" : "Remove script";

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
            return _isAdd ? TryAdd() : TryRemove();
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            return _isAdd ? TryRemove() : TryAdd();
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _scriptTypeName = null;
            _scriptData = null;
        }

        private bool TryRemove()
        {
            if (_sceneId != Guid.Empty && Level.FindScene(_sceneId) == null)
                return false;
            // Remove script (it could be removed by sth else, just check it)
            var script = Object.Find<Script>(ref _scriptId);
            if (!script)
                return false;
            Object.Destroy(ref script);
            FlaxEngine.Scripting.FlushRemovedObjects();
            return Object.Find<Script>(ref _scriptId) == null;
        }

        private bool TryAdd()
        {
            if (_sceneId != Guid.Empty && Level.FindScene(_sceneId) == null)
                return false;
            // Restore script
            var parentActor = Object.Find<Actor>(ref _parentId);
            if (parentActor == null)
                return false;
            var type = TypeUtils.GetType(_scriptTypeName);
            if (!type)
                return false;
            var script = type.CreateInstance() as Script;
            if (script == null)
                return false;
            try
            {
                Object.Internal_ChangeID(Object.GetUnmanagedPtr(script), ref _scriptId);
                if (_scriptData != null)
                    FlaxEngine.Json.JsonSerializer.Deserialize(script, _scriptData);
                script.Enabled = _enabled;
                script.Parent = parentActor;
                if (_orderInParent != -1)
                    script.OrderInParent = _orderInParent;
                _orderInParent = script.OrderInParent;
                if (_prefabObjectId != Guid.Empty)
                    SceneObject.Internal_LinkPrefab(Object.GetUnmanagedPtr(script), ref _prefabId, ref _prefabObjectId);
                return script.Actor == parentActor;
            }
            catch
            {
                Object.Destroy(ref script);
                FlaxEngine.Scripting.FlushRemovedObjects();
                return false;
            }
        }

        /// <inheritdoc />
        void ISceneEditAction.MarkSceneEdited(SceneModule sceneModule)
        {
            if (_sceneId != Guid.Empty)
                sceneModule.MarkSceneEdited(Level.FindScene(_sceneId));
        }
    }
}
