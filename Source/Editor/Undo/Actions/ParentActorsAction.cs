// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Modules;
using FlaxEditor.SceneEditing;
using FlaxEngine;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> used to change parent for <see cref="Actor"/> or <see cref="Script"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    [Serializable]
    class ParentActorsAction : ITryUndoAction, ISceneUndoAction
    {
        private struct Item
        {
            public Guid ID;
            public Guid Parent;
            public int OrderInParent;
            public Transform LocalTransform;
        }

        [Serialize]
        private bool _worldPositionsStays;

        [Serialize]
        private Guid _newParent;

        [Serialize]
        private int _newOrder;

        [Serialize]
        private Item[] _items;

        [Serialize]
        private Guid[] _idsForPrefab;

        [Serialize]
        private Guid[] _prefabIds;

        [Serialize]
        private Guid[] _prefabObjectIds;

        [Serialize]
        private Guid[] _sceneIDs;

        [NonSerialized]
        private SceneMutationResult _lastResult;

        public SceneMutationResult LastResult => _lastResult;

        public Guid[] SceneIds => _sceneIDs ?? Array.Empty<Guid>();

        public bool SupportsSceneReload => true;

        public ParentActorsAction(SceneObject[] objects, Actor newParent, int newOrder, bool worldPositionsStays = true)
        {
            // Sort source objects to provide deterministic behavior
            Array.Sort(objects, SortObjects);

            // Cache initial state for undo
            _worldPositionsStays = worldPositionsStays;
            _newParent = newParent.ID;
            _newOrder = newOrder;
            _items = new Item[objects.Length];
            var sceneIds = new HashSet<Guid>();
            if (newParent.Scene != null)
                sceneIds.Add(newParent.Scene.ID);
            for (int i = 0; i < objects.Length; i++)
            {
                var obj = objects[i];
                if (obj.Parent?.Scene != null)
                    sceneIds.Add(obj.Parent.Scene.ID);
                _items[i] = new Item
                {
                    ID = obj.ID,
                    Parent = obj.Parent?.ID ?? Guid.Empty,
                    OrderInParent = obj.OrderInParent,
                    LocalTransform = obj is Actor actor ? actor.LocalTransform : Transform.Identity,
                };
            }
            _sceneIDs = new Guid[sceneIds.Count];
            sceneIds.CopyTo(_sceneIDs);

            // Collect all objects that have prefab links so they can be restored on undo
            var prefabs = new List<SceneObject>();
            for (int i = 0; i < objects.Length; i++)
                GetAllPrefabs(prefabs, objects[i]);
            if (prefabs.Count != 0)
            {
                // Cache ids of all objects
                _idsForPrefab = new Guid[prefabs.Count];
                _prefabIds = new Guid[prefabs.Count];
                _prefabObjectIds = new Guid[prefabs.Count];
                for (int i = 0; i < prefabs.Count; i++)
                {
                    var obj = prefabs[i];
                    _idsForPrefab[i] = obj.ID;
                    _prefabIds[i] = obj.PrefabID;
                    _prefabObjectIds[i] = obj.PrefabObjectID;
                }
            }
        }

        private static int SortObjects(SceneObject a, SceneObject b)
        {
            // By parent
            var aParent = Object.GetUnmanagedPtr(a.Parent);
            var bParent = Object.GetUnmanagedPtr(b.Parent);
            if (aParent == bParent)
            {
                // By index in parent
                var aOrder = a.OrderInParent;
                var bOrder = b.OrderInParent;
                return aOrder.CompareTo(bOrder);
            }
            return aParent.CompareTo(bParent);
        }

        private static void GetAllPrefabs(List<SceneObject> result, SceneObject obj)
        {
            if (result.Contains(obj))
                return;
            if (obj.HasPrefabLink)
                result.Add(obj);
            if (obj is Actor actor)
            {
                for (int i = 0; i < actor.ScriptsCount; i++)
                    GetAllPrefabs(result, actor.GetScript(i));
                for (int i = 0; i < actor.ChildrenCount; i++)
                    GetAllPrefabs(result, actor.GetChild(i));
            }
        }

        public string ActionString => "Change parent";

        public void Do()
        {
            TryDo();
        }

        public bool TryDo()
        {
            var transactionId = Guid.NewGuid();
            if (!ValidateScenes())
            {
                _lastResult = SceneMutationResult.Rejected(transactionId, SceneMutationOperation.Reparent, SceneMutationErrorCode.ReplayDependencyMissing, "A Scene required by reparent replay is not loaded.", _sceneIDs);
                return false;
            }
            var newParent = Object.Find<Actor>(ref _newParent);
            if (newParent == null || !TryResolveObjects(out var objects))
            {
                _lastResult = SceneMutationResult.Rejected(transactionId, SceneMutationOperation.Reparent, SceneMutationErrorCode.ReplayDependencyMissing, "An Actor required by reparent replay is missing.", _sceneIDs);
                return false;
            }
            for (int i = 0; i < objects.Length; i++)
            {
                if (objects[i] is not Actor actor)
                    continue;
                for (var parent = newParent; parent != null; parent = parent.Parent)
                {
                    if (parent == actor)
                    {
                        _lastResult = SceneMutationResult.Rejected(transactionId, SceneMutationOperation.Reparent, SceneMutationErrorCode.MissingDestination, "The requested parent would create a hierarchy cycle.", _sceneIDs);
                        return false;
                    }
                }
            }

            var previous = CaptureStates(objects);
            try
            {
                var order = _newOrder;
                for (int i = 0; i < objects.Length; i++)
                {
                    var obj = objects[i];
                    if (obj is Actor actor)
                        actor.SetParent(newParent, _worldPositionsStays, true);
                    else
                        obj.Parent = newParent;
                    if (order != -1)
                        obj.OrderInParent = order++;
                }
                _lastResult = SceneMutationResult.Success(transactionId, SceneMutationOperation.Reparent, _sceneIDs);
                return true;
            }
            catch (Exception ex)
            {
                var rolledBack = RestoreStates(previous, objects);
                _lastResult = SceneMutationResult.Failed(transactionId, SceneMutationOperation.Reparent, rolledBack ? SceneMutationErrorCode.PublicationFailed : SceneMutationErrorCode.RollbackFailed, ex.Message, rolledBack, _sceneIDs);
                return false;
            }
        }

        public void Undo()
        {
            TryUndo();
        }

        public bool TryUndo()
        {
            var transactionId = Guid.NewGuid();
            if (!ValidateScenes() || !TryResolveObjects(out var objects) || !TryResolveParents(_items))
            {
                _lastResult = SceneMutationResult.Rejected(transactionId, SceneMutationOperation.Undo, SceneMutationErrorCode.ReplayDependencyMissing, "The original Scene hierarchy required by reparent undo is unavailable.", _sceneIDs);
                return false;
            }

            var previous = CaptureStates(objects);
            try
            {
                if (!RestoreStates(_items, objects))
                    throw new InvalidOperationException("Failed to restore the original Actor parents.");

                // Restore prefab links (if any was in use).
                if (_idsForPrefab != null)
                {
                    for (int i = 0; i < _idsForPrefab.Length; i++)
                    {
                        var obj = Object.Find<SceneObject>(ref _idsForPrefab[i]);
                        if (obj == null)
                            throw new InvalidOperationException("A Prefab-linked object required by reparent undo is missing.");
                        if (_prefabIds[i] != Guid.Empty)
                            SceneObject.Internal_LinkPrefab(Object.GetUnmanagedPtr(obj), ref _prefabIds[i], ref _prefabObjectIds[i]);
                    }
                }
                _lastResult = SceneMutationResult.Success(transactionId, SceneMutationOperation.Undo, _sceneIDs);
                return true;
            }
            catch (Exception ex)
            {
                var rolledBack = RestoreStates(previous, objects);
                _lastResult = SceneMutationResult.Failed(transactionId, SceneMutationOperation.Undo, rolledBack ? SceneMutationErrorCode.PublicationFailed : SceneMutationErrorCode.RollbackFailed, ex.Message, rolledBack, _sceneIDs);
                return false;
            }
        }

        private bool ValidateScenes()
        {
            for (int i = 0; i < _sceneIDs.Length; i++)
            {
                if (_sceneIDs[i] != Guid.Empty && Level.FindScene(_sceneIDs[i]) == null)
                    return false;
            }
            return true;
        }

        private bool TryResolveObjects(out SceneObject[] objects)
        {
            objects = new SceneObject[_items.Length];
            for (int i = 0; i < _items.Length; i++)
            {
                var id = _items[i].ID;
                objects[i] = Object.Find<SceneObject>(ref id);
                if (objects[i] == null)
                    return false;
            }
            return true;
        }

        private static bool TryResolveParents(Item[] states)
        {
            for (int i = 0; i < states.Length; i++)
            {
                var parentId = states[i].Parent;
                if (parentId != Guid.Empty && Object.Find<Actor>(ref parentId) == null)
                    return false;
            }
            return true;
        }

        private static Item[] CaptureStates(SceneObject[] objects)
        {
            var result = new Item[objects.Length];
            for (int i = 0; i < objects.Length; i++)
            {
                var obj = objects[i];
                result[i] = new Item
                {
                    ID = obj.ID,
                    Parent = obj.Parent?.ID ?? Guid.Empty,
                    OrderInParent = obj.OrderInParent,
                    LocalTransform = obj is Actor actor ? actor.LocalTransform : Transform.Identity,
                };
            }
            return result;
        }

        private static bool RestoreStates(Item[] states, SceneObject[] objects)
        {
            try
            {
                for (int i = 0; i < states.Length; i++)
                {
                    var parentId = states[i].Parent;
                    var parent = parentId != Guid.Empty ? Object.Find<Actor>(ref parentId) : null;
                    if (parentId != Guid.Empty && parent == null)
                        return false;
                    objects[i].Parent = parent;
                    if (objects[i] is Actor actor)
                        actor.LocalTransform = states[i].LocalTransform;
                }
                for (int pass = 0; pass < states.Length; pass++)
                for (int i = 0; i < states.Length; i++)
                    objects[i].OrderInParent = states[i].OrderInParent;
                return true;
            }
            catch
            {
                return false;
            }
        }

        void ISceneEditAction.MarkSceneEdited(SceneModule sceneModule)
        {
            for (int i = 0; i < _sceneIDs.Length; i++)
                sceneModule.MarkSceneEdited(Level.FindScene(_sceneIDs[i]));
        }

        public void Dispose()
        {
            _items = null;
            _idsForPrefab = null;
            _prefabIds = null;
            _prefabObjectIds = null;
            _sceneIDs = null;
        }
    }
}
