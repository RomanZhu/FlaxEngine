// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Modules;
using FlaxEditor.SceneEditing;
using FlaxEditor.SceneGraph;
using FlaxEngine;
using FlaxEngine.Utilities;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> used to delete a selection of <see cref="ActorNode"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    [Serializable]
    class DeleteActorsAction : ITryUndoAction, ISceneUndoAction
    {
        [Serialize]
        private byte[] _actorsData;

        [Serialize]
        private List<SceneGraphNode.StateData> _nodesData;

        [Serialize]
        private Guid[] _nodeParentsIDs;

        [Serialize]
        private Guid[] _nodeParentActorIDs;

        [Serialize]
        private Guid[] _sceneIDs;

        [Serialize]
        private int[] _nodeParentsOrders;

        [Serialize]
        private Guid[] _prefabIds;

        [Serialize]
        private Guid[] _prefabObjectIds;

        [Serialize]
        private bool _isInverted;

        [Serialize]
        private bool _affectsCSG;

        [Serialize]
        private bool _affectsNavigation;

        [Serialize]
        protected List<SceneGraphNode> _nodeParents;

        [NonSerialized]
        private SceneMutationResult _lastResult;

        /// <summary>
        /// Gets the most recent structured result.
        /// </summary>
        public SceneMutationResult LastResult => _lastResult;

        /// <inheritdoc />
        public Guid[] SceneIds => _sceneIDs ?? Array.Empty<Guid>();

        /// <inheritdoc />
        public bool SupportsSceneReload => true;

        /// <summary>
        /// Initializes a new instance of the <see cref="DeleteActorsAction"/> class.
        /// </summary>
        /// <param name="actor">The actor.</param>
        /// <param name="isInverted">If set to <c>true</c> action will be inverted - instead of delete it will create actors.</param>
        /// <param name="preserveOrder">If set to <c>true</c> action will be preserve actors order when performing undo.</param>
        internal DeleteActorsAction(Actor actor, bool isInverted = false, bool preserveOrder = true)
        : this(new List<SceneGraphNode>(1) { SceneGraphFactory.FindNode(actor.ID) }, isInverted, preserveOrder)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="DeleteActorsAction"/> class.
        /// </summary>
        /// <param name="node">The object.</param>
        /// <param name="isInverted">If set to <c>true</c> action will be inverted - instead of delete it will create actors.</param>
        /// <param name="preserveOrder">If set to <c>true</c> action will be preserve actors order when performing undo.</param>
        internal DeleteActorsAction(SceneGraphNode node, bool isInverted = false, bool preserveOrder = true)
        : this(new List<SceneGraphNode>(1) { node }, isInverted, preserveOrder)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="DeleteActorsAction"/> class.
        /// </summary>
        /// <param name="nodes">The objects.</param>
        /// <param name="isInverted">If set to <c>true</c> action will be inverted - instead of delete it will create actors.</param>
        /// <param name="preserveOrder">If set to <c>true</c> action will be preserve actors order when performing undo.</param>
        internal DeleteActorsAction(List<SceneGraphNode> nodes, bool isInverted = false, bool preserveOrder = true)
        {
            _isInverted = isInverted;

            // Collect nodes to delete
            var deleteNodes = new List<SceneGraphNode>(nodes.Count);
            var actors = new List<Actor>(nodes.Count);
            for (int i = 0; i < nodes.Count; i++)
            {
                var node = nodes[i];
                if (node is ActorNode actorNode)
                {
                    deleteNodes.Add(actorNode);
                    actors.Add(actorNode.Actor);
                }
                else
                {
                    deleteNodes.Add(node);
                    if (node.CanUseState)
                    {
                        if (_nodesData == null)
                            _nodesData = new List<SceneGraphNode.StateData>();
                        _nodesData.Add(node.State);
                    }
                }
            }

            // Collect parent nodes to delete
            _nodeParents = new List<SceneGraphNode>(nodes.Count);
            deleteNodes.BuildNodesParents(_nodeParents);
            OnDirtyInit();
            _nodeParentsIDs = new Guid[_nodeParents.Count];
            _nodeParentActorIDs = new Guid[_nodeParents.Count];
            _sceneIDs = new Guid[_nodeParents.Count];
            for (int i = 0; i < _nodeParentsIDs.Length; i++)
            {
                _nodeParentsIDs[i] = _nodeParents[i].ID;
                if (_nodeParents[i] is ActorNode actorNode)
                {
                    _nodeParentActorIDs[i] = actorNode.Actor.Parent?.ID ?? Guid.Empty;
                    _sceneIDs[i] = actorNode.Actor.Scene?.ID ?? Guid.Empty;
                }
            }
            if (preserveOrder)
            {
                _nodeParentsOrders = new int[_nodeParents.Count];
                for (int i = 0; i < _nodeParentsOrders.Length; i++)
                    _nodeParentsOrders[i] = _nodeParents[i].OrderInParent;
            }

            // Serialize actors
            _actorsData = Actor.ToBytes(actors.ToArray());

            // Cache actors linkage to prefab objects
            _prefabIds = new Guid[actors.Count];
            _prefabObjectIds = new Guid[actors.Count];
            for (int i = 0; i < actors.Count; i++)
            {
                _prefabIds[i] = actors[i].PrefabID;
                _prefabObjectIds[i] = actors[i].PrefabObjectID;
            }
        }

        /// <inheritdoc />
        public string ActionString => _isInverted ? "Create actors" : "Delete actors";

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public bool TryDo()
        {
            return _isInverted ? TryCreate(SceneMutationOperation.Restore) : TryDelete(SceneMutationOperation.Delete);
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            return _isInverted ? TryDelete(SceneMutationOperation.Undo) : TryCreate(SceneMutationOperation.Undo);
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _actorsData = null;
            _nodeParentsIDs = null;
            _nodeParentActorIDs = null;
            _sceneIDs = null;
            _nodeParentsOrders = null;
            _prefabIds = null;
            _prefabObjectIds = null;
            _nodeParents.Clear();
        }

        private bool TryDelete(SceneMutationOperation operation)
        {
            var transactionId = Guid.NewGuid();
            if (_nodeParents.Count == 0)
            {
                for (int i = 0; i < _nodeParentsIDs.Length; i++)
                {
                    var node = GetNode(_nodeParentsIDs[i]);
                    if (node == null)
                    {
                        _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "An Actor required by delete replay is missing.", _sceneIDs);
                        return false;
                    }
                    _nodeParents.Add(node);
                }
            }

            for (int i = 0; i < _sceneIDs.Length; i++)
            {
                if (_sceneIDs[i] != Guid.Empty && Level.FindScene(_sceneIDs[i]) == null)
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "A Scene required by delete replay is not loaded.", _sceneIDs);
                    return false;
                }
            }

            try
            {
                Delete();
                _lastResult = SceneMutationResult.Success(transactionId, operation, _sceneIDs, removedObjectIds: _nodeParentsIDs);
                return true;
            }
            catch (Exception ex)
            {
                _lastResult = SceneMutationResult.Failed(transactionId, operation, SceneMutationErrorCode.PublicationFailed, ex.Message, false, _sceneIDs);
                return false;
            }
        }

        /// <summary>
        /// Deletes the objects.
        /// </summary>
        protected virtual void Delete()
        {
            // Remove objects
            OnDirty();
            for (int i = 0; i < _nodeParents.Count; i++)
            {
                var node = _nodeParents[i];
                node.Delete();
            }
            _nodeParents.Clear();
            FlaxEngine.Scripting.FlushRemovedObjects();
        }

        /// <summary>
        /// Gets the node.
        /// </summary>
        /// <param name="id">The actor id.</param>
        /// <returns>The scene graph node.</returns>
        protected virtual SceneGraphNode GetNode(Guid id)
        {
            return SceneGraphFactory.GetNode(id);
        }

        /// <summary>
        /// Creates the removed objects (from data).
        /// </summary>
        protected virtual void Create()
        {
            TryCreate(SceneMutationOperation.Restore);
        }

        private bool TryCreate(SceneMutationOperation operation)
        {
            var transactionId = Guid.NewGuid();
            Actor[] actors = null;
            var createdStateNodes = new List<SceneGraphNode>();
            var destinationParents = new Actor[_nodeParentsIDs.Length];

            // Preflight every replay dependency before allocating any object.
            for (int i = 0; i < _nodeParentsIDs.Length; i++)
            {
                var sceneId = _sceneIDs[i];
                var scene = sceneId != Guid.Empty ? Level.FindScene(sceneId) : null;
                if (sceneId != Guid.Empty && scene == null)
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "The original Scene must be loaded before deleted Actors can be restored.", _sceneIDs);
                    return false;
                }

                var parentId = _nodeParentActorIDs[i];
                if (parentId == Guid.Empty)
                {
                    destinationParents[i] = scene;
                }
                else if (scene != null && parentId == scene.ID)
                {
                    destinationParents[i] = scene;
                }
                else
                {
                    destinationParents[i] = FlaxEngine.Object.TryFind<Actor>(ref parentId);
                }
                if (destinationParents[i] == null)
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "The original parent Actor must exist before deleted Actors can be restored.", _sceneIDs);
                    return false;
                }
            }

            if (_nodesData != null)
            {
                for (int i = 0; i < _nodesData.Count; i++)
                {
                    var state = _nodesData[i];
                    var type = TypeUtils.GetManagedType(state.TypeName);
                    if (type == null || type.GetMethod(state.CreateMethodName) == null)
                    {
                        _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.MissingType, $"Cannot restore scene graph node type '{state.TypeName}'.", _sceneIDs);
                        return false;
                    }
                }
            }

            try
            {
                var nodes = new List<SceneGraphNode>();
                actors = Actor.FromBytes(_actorsData);
                if (actors == null || actors.Length != _prefabIds.Length)
                    throw new InvalidOperationException("The serialized Actor graph could not be restored completely.");
                nodes.Capacity = Math.Max(nodes.Capacity, actors.Length);

                // Preserve prefab objects linkage.
                for (int i = 0; i < actors.Length; i++)
                {
                    Guid prefabId = _prefabIds[i];
                    if (prefabId != Guid.Empty)
                        Actor.Internal_LinkPrefab(FlaxEngine.Object.GetUnmanagedPtr(actors[i]), ref prefabId, ref _prefabObjectIds[i]);
                }

                // Explicitly restore every top-level Actor to its original Scene and parent.
                for (int i = 0; i < _nodeParentsIDs.Length; i++)
                {
                    var id = _nodeParentsIDs[i];
                    Actor root = null;
                    for (int j = 0; j < actors.Length; j++)
                    {
                        if (actors[j].ID == id)
                        {
                            root = actors[j];
                            break;
                        }
                    }
                    if (root == null)
                        throw new InvalidOperationException("A serialized top-level Actor is missing from the restored graph.");
                    root.SetParent(destinationParents[i], false);
                }

                // Restore custom node state only after all Actor dependencies exist.
                if (_nodesData != null)
                {
                    for (int i = 0; i < _nodesData.Count; i++)
                    {
                        var state = _nodesData[i];
                        var type = TypeUtils.GetManagedType(state.TypeName);
                        var method = type.GetMethod(state.CreateMethodName);
                        if (method.Invoke(null, new object[] { state }) is not SceneGraphNode node)
                            throw new InvalidOperationException($"Failed to restore scene graph node state via method {state.CreateMethodName} from type {state.TypeName}.");
                        createdStateNodes.Add(node);
                    }
                }

                for (int i = 0; i < _nodeParentsIDs.Length; i++)
                {
                    var foundNode = GetNode(_nodeParentsIDs[i]);
                    if (foundNode == null)
                        throw new InvalidOperationException("A restored Actor is missing from the Scene graph.");
                    nodes.Add(foundNode);
                    if (_nodeParentsOrders != null && foundNode is ActorNode actorNode)
                        actorNode.Actor.OrderInParent = _nodeParentsOrders[i];
                }
                nodes.BuildNodesParents(_nodeParents);
                if (_nodeParents.Count != _nodeParentsIDs.Length)
                    throw new InvalidOperationException("The restored Scene graph hierarchy does not match the serialized roots.");

                OnDirty();
                var createdIds = new Guid[actors.Length];
                for (int i = 0; i < actors.Length; i++)
                    createdIds[i] = actors[i].ID;
                _lastResult = SceneMutationResult.Success(transactionId, operation, _sceneIDs, createdIds);
                return true;
            }
            catch (Exception ex)
            {
                for (int i = createdStateNodes.Count - 1; i >= 0; i--)
                {
                    try
                    {
                        createdStateNodes[i].Delete();
                        createdStateNodes[i].Dispose();
                    }
                    catch
                    {
                        // Actor rollback below remains authoritative for Actor and Script ownership.
                    }
                }
                var rollbackCompleted = RollbackActors(actors);
                _nodeParents.Clear();
                _lastResult = SceneMutationResult.Failed(transactionId, operation, rollbackCompleted ? SceneMutationErrorCode.ConstructionFailed : SceneMutationErrorCode.RollbackFailed, ex.Message, rollbackCompleted, _sceneIDs);
                return false;
            }
        }

        private void OnDirtyInit()
        {
            for (int i = 0; i < _nodeParents.Count; i++)
            {
                if (_nodeParents[i] is ActorNode node && node.Actor is BoxBrush)
                {
                    _affectsCSG = true;
                    break;
                }
            }

            for (int i = 0; i < _nodeParents.Count; i++)
            {
                if (_nodeParents[i] is ActorNode actorNode && actorNode.AffectsNavigationWithChildren)
                {
                    _affectsNavigation = true;
                    break;
                }
            }
        }

        /// <inheritdoc />
        void ISceneEditAction.MarkSceneEdited(SceneModule sceneModule)
        {
            var marked = new HashSet<Guid>();
            for (int i = 0; i < _sceneIDs.Length; i++)
            {
                var sceneId = _sceneIDs[i];
                if (sceneId != Guid.Empty && marked.Add(sceneId))
                    sceneModule.MarkSceneEdited(Level.FindScene(sceneId));
            }
        }

        private static bool RollbackActors(Actor[] actors)
        {
            if (actors == null)
                return true;
            try
            {
                var ids = new Guid[actors.Length];
                for (int i = 0; i < actors.Length; i++)
                    ids[i] = actors[i]?.ID ?? Guid.Empty;
                for (int i = actors.Length - 1; i >= 0; i--)
                {
                    var actor = actors[i];
                    if (actor != null)
                        FlaxEngine.Object.Destroy(ref actor);
                }
                FlaxEngine.Scripting.FlushRemovedObjects();
                for (int i = 0; i < ids.Length; i++)
                {
                    var id = ids[i];
                    if (id != Guid.Empty && FlaxEngine.Object.TryFind<Actor>(ref id) != null)
                        return false;
                }
                return true;
            }
            catch (Exception rollbackException)
            {
                Editor.LogError("[SceneDebug] RollbackFailed Actor restore rollback failed. " + rollbackException.Message);
                return false;
            }
        }

        private void OnDirty()
        {
            var editor = Editor.Instance;
            if (editor.StateMachine.IsPlayMode)
                return;
            var options = editor.Options.Options;

            // Auto CSG mesh rebuild
            if (_affectsCSG && options.General.AutoRebuildCSG)
            {
                for (var i = 0; i < _nodeParents.Count; i++)
                {
                    if (_nodeParents[i] is ActorNode node && node.Actor is BoxBrush)
                        node.Actor.Scene.BuildCSG(options.General.AutoRebuildCSGTimeoutMs);
                }
            }

            // Auto NavMesh rebuild
            if (_affectsNavigation && options.General.AutoRebuildNavMesh)
            {
                for (var i = 0; i < _nodeParents.Count; i++)
                {
                    if (_nodeParents[i] is ActorNode node && node.Actor && node.Actor.Scene && node.AffectsNavigationWithChildren)
                    {
                        var bounds = node.Actor.BoxWithChildren;
                        Navigation.BuildNavMesh(bounds, options.General.AutoRebuildNavMeshTimeoutMs);
                    }
                }
            }
        }
    }
}
