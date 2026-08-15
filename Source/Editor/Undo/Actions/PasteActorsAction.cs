// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using FlaxEditor.Modules;
using FlaxEditor.SceneEditing;
using FlaxEditor.SceneGraph;
using FlaxEngine;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Implementation of <see cref="IUndoAction"/> used to paste a set of <see cref="ActorNode"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.IUndoAction" />
    [Serializable]
    class PasteActorsAction : ITryUndoAction, ISceneUndoAction
    {
        [Serialize]
        private Dictionary<Guid, Guid> _idsMapping;

        [Serialize]
        private byte[] _data;

        [Serialize]
        private Guid _pasteParent;

        [Serialize]
        private bool _insertAfterSource;

        [Serialize]
        private Guid _destinationScene;

        [Serialize]
        private Guid[] _affectedScenes;

        [NonSerialized]
        private SceneMutationResult _lastResult;

        [NonSerialized]
        private SceneMutationPlan _plan;

        /// <summary>
        /// The node parents.
        /// </summary>
        [Serialize]
        protected List<Guid> _nodeParents;

        /// <summary>
        /// Initializes a new instance of the <see cref="PasteActorsAction"/> class.
        /// </summary>
        /// <param name="data">The data.</param>
        /// <param name="objectIds">The object ids.</param>
        /// <param name="pasteParent">The paste parent object id.</param>
        /// <param name="name">The action name.</param>
        protected PasteActorsAction(byte[] data, Guid[] objectIds, ref Guid pasteParent, string name)
        : this(data, objectIds, Guid.Empty, pasteParent, name)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="PasteActorsAction"/> class with an explicit destination.
        /// </summary>
        protected PasteActorsAction(byte[] data, Guid[] objectIds, Guid destinationScene, Guid pasteParent, string name)
        {
            ActionString = name;

            _pasteParent = pasteParent;
            _destinationScene = destinationScene;
            _affectedScenes = destinationScene == Guid.Empty ? Array.Empty<Guid>() : new[] { destinationScene };
            _idsMapping = new Dictionary<Guid, Guid>(objectIds.Length);
            for (int i = 0; i < objectIds.Length; i++)
            {
                _idsMapping[objectIds[i]] = Guid.NewGuid();
            }

            _nodeParents = new List<Guid>(objectIds.Length);
            _data = data;
            _plan = new SceneMutationPlan(name == "Duplicate actors" ? SceneMutationOperation.Duplicate : SceneMutationOperation.Paste, destinationScene, pasteParent, data != null && data.Length >= sizeof(int) ? BitConverter.ToInt32(data, 0) : 0, objectIds);
        }

        internal static PasteActorsAction Paste(byte[] data, Scene destinationScene, Actor destinationParent)
        {
            if (data == null || data.Length == 0 || destinationScene == null)
                return null;
            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (objectIds == null || objectIds.Length == 0)
                return null;

            return new PasteActorsAction(data, objectIds, destinationScene.ID, destinationParent?.ID ?? destinationScene.ID, "Paste actors");
        }

        internal static PasteActorsAction Duplicate(byte[] data, Guid pasteParent)
        {
            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (objectIds == null || objectIds.Length == 0)
                return null;

            return new PasteActorsAction(data, objectIds, ref pasteParent, "Duplicate actors")
            {
                _insertAfterSource = pasteParent == Guid.Empty,
            };
        }

        private struct DuplicatePlacement
        {
            public Actor Source;
            public Actor Clone;
        }

        private static int SortDuplicatePlacement(DuplicatePlacement a, DuplicatePlacement b)
        {
            var parentA = Object.GetUnmanagedPtr(a.Source.Parent);
            var parentB = Object.GetUnmanagedPtr(b.Source.Parent);
            if (parentA == parentB)
                return a.Source.OrderInParent.CompareTo(b.Source.OrderInParent);
            return parentA.CompareTo(parentB);
        }

        /// <summary>
        /// Links the broken parent reference (missing parent). By default links the actor to the first scene.
        /// </summary>
        /// <param name="actorNode">The actor node.</param>
        protected virtual void LinkBrokenParentReference(ActorNode actorNode)
        {
            throw new SceneMutationFailureException(SceneMutationErrorCode.MissingDestination, "The Actor payload has no explicit destination parent.");
        }

        /// <summary>
        /// Checks if actor has a broken parent reference. For example, it's linked to the parent that is indie prefab editor while it should be pasted into scene.
        /// </summary>
        /// <param name="actorNode">The actor node.</param>
        protected virtual void CheckBrokenParentReference(ActorNode actorNode)
        {
            // Ensure pasted object ends up on a scene
            if (actorNode.Actor.Scene == null)
                LinkBrokenParentReference(actorNode);
        }

        /// <inheritdoc />
        public string ActionString { get; }

        /// <summary>
        /// Gets the immutable mutation plan.
        /// </summary>
        public SceneMutationPlan Plan => _plan;

        /// <summary>
        /// Gets the most recent structured result.
        /// </summary>
        public SceneMutationResult LastResult => _lastResult;

        /// <inheritdoc />
        public Guid[] SceneIds => _affectedScenes ?? Array.Empty<Guid>();

        /// <inheritdoc />
        public bool SupportsSceneReload => true;

        /// <summary>
        /// Performs the paste/duplicate action and outputs created objects nodes.
        /// </summary>
        /// <param name="nodes">The nodes.</param>
        /// <param name="nodeParents">The node parents.</param>
        public virtual void Do(out List<ActorNode> nodes, out List<ActorNode> nodeParents)
        {
            TryDo(out nodes, out nodeParents);
        }

        /// <summary>
        /// Attempts to perform the paste without publishing partial state.
        /// </summary>
        public virtual bool TryDo(out List<ActorNode> nodes, out List<ActorNode> nodeParents)
        {
            nodes = null;
            nodeParents = null;
            var operation = _plan?.Operation ?? SceneMutationOperation.Paste;
            var transactionId = _plan?.TransactionId ?? Guid.NewGuid();
            var affectedScenes = _destinationScene == Guid.Empty ? Array.Empty<Guid>() : new[] { _destinationScene };
            Actor[] actors = null;
            SceneDebug.Log("MutationPreflight", $"Transaction={transactionId} Operation={operation} DestinationScene={_destinationScene} DestinationParent={_pasteParent}");

            if (_data == null || _data.Length == 0 || _idsMapping == null || _idsMapping.Count == 0)
            {
                _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.InvalidPayload, "The Actor clipboard payload is empty or invalid.", affectedScenes);
                return false;
            }

            Scene destinationScene = null;
            Actor destinationParent = null;
            if (_destinationScene != Guid.Empty)
            {
                destinationScene = Level.FindScene(_destinationScene);
                if (destinationScene == null)
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.DestinationUnloaded, "The destination Scene is not loaded.", affectedScenes);
                    return false;
                }
                if (IsReadOnly(destinationScene))
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.DestinationReadOnly, "The destination Scene file is read-only.", affectedScenes);
                    return false;
                }

                if (_pasteParent == destinationScene.ID)
                {
                    destinationParent = destinationScene;
                }
                else
                {
                    var pasteParent = _pasteParent;
                    destinationParent = Object.TryFind<Actor>(ref pasteParent);
                    if (destinationParent == null || destinationParent.Scene != destinationScene)
                    {
                        _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.MissingDestination, "The destination parent is missing or belongs to another Scene.", affectedScenes);
                        return false;
                    }
                }
            }

            try
            {
                if (SceneMutationFaults.ShouldFail(transactionId, "Preflight"))
                    throw new SceneMutationInjectedFaultException("Preflight");

                var actorIds = Actor.FromBytesToIds(_data, _idsMapping, destinationParent?.ID ?? Guid.Empty);
                if (actorIds == null || actorIds.Length == 0)
                    throw new SceneMutationFailureException(SceneMutationErrorCode.ConstructionFailed, "The Actor payload did not construct any Actors.");
                actors = new Actor[actorIds.Length];
                bool actorResolutionFailed = false;
                for (int i = 0; i < actorIds.Length; i++)
                {
                    var actorId = actorIds[i];
                    actors[i] = Object.TryFind<Actor>(ref actorId);
                    actorResolutionFailed |= actors[i] == null;
                }
                if (actorResolutionFailed)
                    throw new SceneMutationFailureException(SceneMutationErrorCode.ConstructionFailed, "A constructed Actor could not be resolved by its stable identifier.");
                if (SceneMutationFaults.ShouldFail(transactionId, "Construction"))
                    throw new SceneMutationInjectedFaultException("Construction");
                SceneDebug.Log("MutationStaged", $"Transaction={transactionId} Actors={actors.Length}");

                nodes = new List<ActorNode>(actors.Length);
                if (destinationParent != null)
                {
                    var actorsSet = new HashSet<Actor>(actors);
                    var roots = new List<Actor>();
                    for (int i = 0; i < actors.Length; i++)
                    {
                        var actor = actors[i];
                        if (actor != null && (actor.Parent == null || !actorsSet.Contains(actor.Parent)))
                            roots.Add(actor);
                    }
                    if (roots.Count == 0)
                        throw new SceneMutationFailureException(SceneMutationErrorCode.InvalidPayload, "The Actor payload has no valid top-level Actors.");

                    for (int i = 0; i < roots.Count; i++)
                        roots[i].SetParent(destinationParent, false);
                    if (SceneMutationFaults.ShouldFail(transactionId, "Attachment"))
                        throw new SceneMutationInjectedFaultException("Attachment");
                    SceneDebug.Log("MutationAttached", $"Transaction={transactionId} Roots={roots.Count} Scene={_destinationScene}");

                    for (int i = 0; i < actors.Length; i++)
                    {
                        if (GetNode(actors[i].ID) is ActorNode actorNode)
                            nodes.Add(actorNode);
                    }
                    nodeParents = new List<ActorNode>(roots.Count);
                    for (int i = 0; i < roots.Count; i++)
                    {
                        if (GetNode(roots[i].ID) is not ActorNode rootNode)
                            throw new SceneMutationFailureException(SceneMutationErrorCode.PublicationFailed, "A pasted top-level Actor is missing from the Scene graph.");
                        nodeParents.Add(rootNode);
                    }
                }
                else
                {
                    // Prefab editing uses a local graph and supplies its destination policy via the virtual hooks.
                    for (int i = 0; i < actors.Length; i++)
                    {
                        var actor = actors[i];
                        var node = GetNode(actor.ID);
                        if (node is ActorNode actorNode)
                        {
                            if (actor.Parent == null)
                                LinkBrokenParentReference(actorNode);
                            nodes.Add(actorNode);
                        }
                    }
                    nodeParents = nodes.BuildNodesParents();
                    foreach (var node in nodeParents)
                        CheckBrokenParentReference(node);
                }

                if (nodeParents.Count == 0)
                    throw new SceneMutationFailureException(SceneMutationErrorCode.PostconditionFailed, "The non-empty Actor payload produced no attached top-level Actors.");

                if (_insertAfterSource)
                    InsertAfterSourceActors(nodeParents);

                // Store previously looked up names and the results
                Dictionary<string, bool> foundNamesResults = new();
                for (int i = 0; i < nodeParents.Count; i++)
                {
                    var node = nodeParents[i];
                    var actor = node.Actor;
                    var parent = actor != null ? actor.Parent : null;
                    if (parent != null)
                    {
                        bool IsNameValid(string name)
                        {
                            if (!foundNamesResults.TryGetValue(name, out bool found))
                            {
                                found = parent.GetChild(name) != null;
                                foundNamesResults.Add(name, found);
                            }
                            return !found;
                        }

                        // Fix name collisions
                        var name = actor.Name;
                        var children = parent.Children;
                        for (int j = 0; j < children.Length; j++)
                        {
                            var child = children[j];
                            if (child != actor && child.Name == name)
                            {
                                string newName = Utilities.Utils.IncrementNameNumber(name, IsNameValid);
                                foundNamesResults[newName] = true;
                                actor.Name = newName;
                                // Multiple actors may have the same name, continue
                            }
                        }
                    }
                }

                for (int i = 0; i < nodeParents.Count; i++)
                    nodeParents[i].PostPaste();
                if (SceneMutationFaults.ShouldFail(transactionId, "Publication"))
                    throw new SceneMutationInjectedFaultException("Publication");

                _nodeParents.Clear();
                _nodeParents.Capacity = Mathf.Max(_nodeParents.Capacity, nodeParents.Count);
                for (int i = 0; i < nodeParents.Count; i++)
                    _nodeParents.Add(nodeParents[i].ID);

                var affectedSceneSet = new HashSet<Guid>();
                for (int i = 0; i < nodeParents.Count; i++)
                {
                    var sceneId = nodeParents[i].ParentScene?.Scene?.ID ?? Guid.Empty;
                    if (sceneId != Guid.Empty)
                        affectedSceneSet.Add(sceneId);
                }
                if (affectedSceneSet.Count != 0)
                {
                    _affectedScenes = new Guid[affectedSceneSet.Count];
                    affectedSceneSet.CopyTo(_affectedScenes);
                }

                var createdIds = new Guid[actors.Length];
                for (int i = 0; i < actors.Length; i++)
                    createdIds[i] = actors[i].ID;
                _lastResult = SceneMutationResult.Success(transactionId, operation, _affectedScenes, createdIds);
                SceneDebug.Log("MutationCommitted", $"Transaction={transactionId} Operation={operation} Created={createdIds.Length}");
                return true;
            }
            catch (Exception ex)
            {
                var rollbackCompleted = RollbackActors(actors);
                var failure = ex as SceneMutationFailureException;
                var code = failure?.ErrorCode ?? (rollbackCompleted ? SceneMutationErrorCode.ConstructionFailed : SceneMutationErrorCode.RollbackFailed);
                _lastResult = SceneMutationResult.Failed(transactionId, operation, code, ex.Message, rollbackCompleted, affectedScenes);
                SceneDebug.Error(code, rollbackCompleted ? "MutationRolledBack" : "MutationRollbackFailed", $"Transaction={transactionId} Operation={operation} Message='{ex.Message}'");
                nodes = null;
                nodeParents = null;
                return false;
            }
        }

        private void InsertAfterSourceActors(List<ActorNode> nodeParents)
        {
            var placements = new List<DuplicatePlacement>(nodeParents.Count);
            for (int i = 0; i < nodeParents.Count; i++)
            {
                var clone = nodeParents[i].Actor;
                if (clone == null)
                    continue;

                var cloneId = clone.ID;
                Guid sourceId = Guid.Empty;
                foreach (var idsMapping in _idsMapping)
                {
                    if (idsMapping.Value == cloneId)
                    {
                        sourceId = idsMapping.Key;
                        break;
                    }
                }
                if (sourceId == Guid.Empty)
                    continue;

                var source = Object.Find<Actor>(ref sourceId);
                if (source == null || source.Parent == null || clone.Parent != source.Parent)
                    continue;

                placements.Add(new DuplicatePlacement
                {
                    Source = source,
                    Clone = clone,
                });
            }

            placements.Sort(SortDuplicatePlacement);
            for (int i = 0; i < placements.Count; i++)
            {
                var placement = placements[i];
                placement.Clone.OrderInParent = placement.Source.OrderInParent + 1;
            }
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

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public virtual bool TryDo()
        {
            return TryDo(out _, out _);
        }

        /// <inheritdoc />
        public virtual void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public virtual bool TryUndo()
        {
            var transactionId = Guid.NewGuid();
            var operation = SceneMutationOperation.Undo;
            if (_destinationScene != Guid.Empty && Level.FindScene(_destinationScene) == null)
            {
                _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "The destination Scene must be loaded before this action can be undone.", new[] { _destinationScene });
                return false;
            }

            var nodes = new SceneGraphNode[_nodeParents.Count];
            for (int i = 0; i < _nodeParents.Count; i++)
            {
                nodes[i] = GetNode(_nodeParents[i]);
                if (nodes[i] == null)
                {
                    _lastResult = SceneMutationResult.Rejected(transactionId, operation, SceneMutationErrorCode.ReplayDependencyMissing, "A pasted Actor required by undo is missing.", _destinationScene == Guid.Empty ? null : new[] { _destinationScene });
                    return false;
                }
            }
            for (int i = 0; i < nodes.Length; i++)
                nodes[i].Delete();
            FlaxEngine.Scripting.FlushRemovedObjects();
            for (int i = 0; i < nodes.Length; i++)
            {
                var id = nodes[i].ID;
                if (Object.TryFind<Actor>(ref id) != null)
                {
                    _lastResult = SceneMutationResult.Failed(transactionId, operation, SceneMutationErrorCode.PostconditionFailed, "A pasted Actor remained alive after undo.", false, _destinationScene == Guid.Empty ? null : new[] { _destinationScene });
                    return false;
                }
            }
            _nodeParents.Clear();
            _lastResult = SceneMutationResult.Success(transactionId, operation, _destinationScene == Guid.Empty ? null : new[] { _destinationScene });
            return true;
        }

        /// <inheritdoc />
        void ISceneEditAction.MarkSceneEdited(SceneModule sceneModule)
        {
            for (int i = 0; i < _affectedScenes.Length; i++)
                sceneModule.MarkSceneEdited(Level.FindScene(_affectedScenes[i]));
        }

        /// <inheritdoc />
        public virtual void Dispose()
        {
            _nodeParents?.Clear();
            _idsMapping?.Clear();
            _affectedScenes = null;
            _data = null;
        }

        private static bool IsReadOnly(Scene scene)
        {
            try
            {
                return !string.IsNullOrEmpty(scene.Path) && File.Exists(scene.Path) && (File.GetAttributes(scene.Path) & FileAttributes.ReadOnly) != 0;
            }
            catch
            {
                return true;
            }
        }

        private static bool RollbackActors(Actor[] actors)
        {
            if (actors == null)
                return true;
            try
            {
                for (int i = actors.Length - 1; i >= 0; i--)
                {
                    var actor = actors[i];
                    if (actor != null)
                        Object.Destroy(ref actor);
                }
                FlaxEngine.Scripting.FlushRemovedObjects();
                for (int i = 0; i < actors.Length; i++)
                {
                    var id = actors[i]?.ID ?? Guid.Empty;
                    if (id != Guid.Empty && Object.TryFind<Actor>(ref id) != null)
                        return false;
                }
                return true;
            }
            catch (Exception rollbackException)
            {
                SceneDebug.Error(SceneMutationErrorCode.RollbackFailed, "MutationRollbackFailed", $"Message='{rollbackException.Message}'");
                return false;
            }
        }

        private class SceneMutationFailureException : Exception
        {
            public readonly SceneMutationErrorCode ErrorCode;

            public SceneMutationFailureException(SceneMutationErrorCode errorCode, string message)
            : base(message)
            {
                ErrorCode = errorCode;
            }
        }

        private sealed class SceneMutationInjectedFaultException : SceneMutationFailureException
        {
            public SceneMutationInjectedFaultException(string stage)
            : base(SceneMutationErrorCode.PublicationFailed, "Injected scene mutation fault after " + stage + ".")
            {
            }
        }
    }
}
