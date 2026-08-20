// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Actions;
using FlaxEditor.Modules;
using FlaxEditor.SceneGraph;
using FlaxEditor.Tools.CSG;
using FlaxEngine;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Windows.Assets
{
    public sealed partial class PrefabWindow
    {
        /// <summary>
        /// Implementation of <see cref="IUndoAction"/> used to change the root actor of the prefab.
        /// </summary>
        /// <seealso cref="FlaxEditor.IUndoAction" />
        public class SetRootAction : IUndoAction
        {
            private PrefabWindow _window;
            private readonly Guid _before;
            private readonly Guid _after;

            /// <summary>
            /// Initializes a new instance of the <see cref="SetRootAction"/> class.
            /// </summary>
            /// <param name="window">The window reference.</param>
            /// <param name="before">The root before.</param>
            /// <param name="after">The root after.</param>
            internal SetRootAction(PrefabWindow window, Actor before, Actor after)
            {
                _window = window;
                _before = before.ID;
                _after = after.ID;
            }

            private void Set(Guid oldRootId, Guid newRootId)
            {
                var oldRoot = Object.Find<Actor>(ref oldRootId);
                var newRoot = Object.Find<Actor>(ref newRootId);

                _window.Graph.MainActor = null;
                _window.Viewport.Instance = null;

                if (SceneGraphFactory.Nodes.TryGetValue(oldRootId, out var oldRootNode))
                    oldRootNode.Dispose();
                if (SceneGraphFactory.Nodes.TryGetValue(newRootId, out var newRootNode))
                    newRootNode.Dispose();

                newRoot.Parent = null;
                oldRoot.Parent = newRoot;

                _window.Graph.MainActor = newRoot;
                _window.Viewport.Instance = newRoot;
            }

            /// <inheritdoc />
            public string ActionString => "Set root";

            /// <inheritdoc />
            public void Do()
            {
                Set(_before, _after);
            }

            /// <inheritdoc />
            public void Undo()
            {
                Set(_after, _before);
            }

            /// <inheritdoc />
            public void Dispose()
            {
                _window = null;
            }
        }

        /// <summary>
        /// Changes the root object of the prefab.
        /// </summary>
        private void SetRoot()
        {
            var oldRoot = Graph.MainActor;
            var newRoot = ((ActorNode)Selection[0]).Actor;

            Deselect();

            var action = new SetRootAction(this, oldRoot, newRoot);
            action.Do();
            Undo.AddAction(action);
        }

        /// <summary>
        /// Cuts selected objects.
        /// </summary>
        public void Cut()
        {
            if (TryCopy())
                Delete();
        }

        /// <summary>
        /// Copies selected objects to system clipboard.
        /// </summary>
        public void Copy()
        {
            TryCopy();
        }

        private bool TryCopy()
        {
            // Peek things that can be copied (copy all actors)
            var objects = Selection.Where(x => x.CanCopyPaste).ToList().BuildAllNodes().Where(x => x.CanCopyPaste && x is ActorNode).ToList();
            if (objects.Count == 0)
                return false;

            // Serialize actors
            var actors = objects.ConvertAll(x => ((ActorNode)x).Actor);
            var data = Actor.ToBytes(actors.ToArray());
            if (data == null || data.Length == 0)
            {
                Editor.LogError("Failed to copy actors data.");
                return false;
            }
            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (objectIds == null || objectIds.Length == 0)
                return false;

            // Copy data
            Clipboard.RawData = data;
            return true;
        }

        /// <summary>
        /// Pastes objects from the system clipboard.
        /// </summary>
        public void Paste()
        {
            Paste(null);
        }

        /// <summary>
        /// Pastes the copied objects. Supports undo/redo.
        /// </summary>
        /// <param name="pasteTargetActor">The target actor to paste copied data.</param>
        public void Paste(Actor pasteTargetActor)
        {
            // Get clipboard data
            var data = Clipboard.RawData;

            // Set paste target if only one actor is selected and no target provided
            if (pasteTargetActor == null && Selection.Count == 1 && Selection[0] is ActorNode actorNode)
            {
                pasteTargetActor = actorNode.Actor;
            }

            // Create paste action
            var pasteAction = CustomPasteActorsAction.CustomPaste(this, data, pasteTargetActor?.ID ?? Guid.Empty);
            if (pasteAction != null)
            {
                OnPasteAction(pasteAction);
            }

            // Scroll to new selected node
            ScrollToSelectedNode();
        }

        /// <summary>
        /// Duplicates selected objects.
        /// </summary>
        public void Duplicate()
        {
            if (TryDuplicateForTransform(out _, out var undoAction))
            {
                Undo.AddAction(undoAction);
                ScrollToSelectedNode();
            }
        }

        /// <summary>
        /// Duplicates the selected objects as part of a transform transaction.
        /// The duplicate action is already performed but is not added to the
        /// undo stack until the transaction commits.
        /// </summary>
        /// <param name="createdObjects">The created top-level objects.</param>
        /// <param name="undoAction">The already-performed duplicate action.</param>
        /// <returns>True if duplication produced objects.</returns>
        internal bool TryDuplicateForTransform(out List<SceneGraphNode> createdObjects, out IUndoAction undoAction)
        {
            createdObjects = new List<SceneGraphNode>();
            undoAction = null;

            // Peek things that can be copied (copy all actors)
            var objects = Selection.Where(x => x.CanDuplicate && x != Graph.Main).ToList().BuildAllNodes().Where(x => x.CanDuplicate && x is ActorNode).ToList();
            if (objects.Count == 0)
                return false;

            // Serialize actors
            var actors = objects.ConvertAll(x => ((ActorNode)x).Actor);
            var data = Actor.ToBytes(actors.ToArray());
            if (data == null)
            {
                Editor.LogError("Failed to copy actors data.");
                return false;
            }

            // Create paste action (with selecting spawned objects)
            var pasteAction = CustomPasteActorsAction.CustomDuplicate(this, data, Guid.Empty);
            if (pasteAction != null)
            {
                if (!TryApplyPasteAction(pasteAction, out createdObjects, out undoAction))
                    return false;
            }

            return createdObjects.Count != 0;
        }

        private void OnPasteAction(PasteActorsAction pasteAction)
        {
            if (TryApplyPasteAction(pasteAction, out _, out var undoAction))
                Undo.AddAction(undoAction);
        }

        private bool TryApplyPasteAction(PasteActorsAction pasteAction, out List<SceneGraphNode> createdObjects, out IUndoAction undoAction)
        {
            createdObjects = new List<SceneGraphNode>();
            undoAction = null;
            List<ActorNode> nodeParents;
            try
            {
                if (!pasteAction.TryDo(out _, out nodeParents))
                {
                    Editor.LogError($"[SceneDebug] {pasteAction.LastResult?.ErrorCode} Prefab paste rejected. {pasteAction.LastResult?.Message}");
                    pasteAction.Dispose();
                    return false;
                }
            }
            catch (Exception ex)
            {
                try
                {
                    pasteAction.Undo();
                }
                catch (Exception rollbackException)
                {
                    Editor.LogError("Paste rollback failed. " + rollbackException.Message);
                }
                finally
                {
                    pasteAction.Dispose();
                }
                throw new InvalidOperationException("Pasting actors failed.", ex);
            }
            if (nodeParents == null || nodeParents.Count == 0)
            {
                try
                {
                    pasteAction.Undo();
                }
                catch (Exception rollbackException)
                {
                    Editor.LogError("Empty paste rollback failed. " + rollbackException.Message);
                }
                finally
                {
                    pasteAction.Dispose();
                }
                return false;
            }

            // Select spawned objects
            var selectAction = new SelectionChangeAction(Selection.ToArray(), nodeParents.Cast<SceneGraphNode>().ToArray(), OnSelectionUndo);
            try
            {
                selectAction.Do();
            }
            catch
            {
                try
                {
                    selectAction.Undo();
                }
                catch (Exception rollbackException)
                {
                    Editor.LogError("Paste selection rollback failed. " + rollbackException.Message);
                }
                try
                {
                    pasteAction.Undo();
                }
                catch (Exception rollbackException)
                {
                    Editor.LogError("Paste rollback failed. " + rollbackException.Message);
                }
                pasteAction.Dispose();
                throw;
            }

            createdObjects.AddRange(nodeParents.Cast<SceneGraphNode>());
            undoAction = new MultiUndoAction(pasteAction, selectAction);
            try
            {
                OnSelectionChanges();
            }
            catch
            {
                try
                {
                    undoAction.Undo();
                }
                catch (Exception rollbackException)
                {
                    Editor.LogError("Paste action rollback failed. " + rollbackException.Message);
                }
                undoAction.Dispose();
                undoAction = null;
                createdObjects.Clear();
                throw;
            }
            return true;
        }

        private class CustomDeleteActorsAction : DeleteActorsAction
        {
            public CustomDeleteActorsAction(List<SceneGraphNode> nodes, bool isInverted = false)
            : base(nodes, isInverted)
            {
            }

            /// <inheritdoc />
            protected override void Delete()
            {
                var nodes = _nodeParents.ToArray();

                // Unlink nodes from parents (actors spawned for prefab editing are not in a gameplay and may not send some important events)
                for (int i = 0; i < nodes.Length; i++)
                {
                    if (nodes[i] is ActorNode actorNode)
                        actorNode.Actor.Parent = null;
                }

                base.Delete();

                // Remove nodes (actors in prefab are not in a gameplay and some events from the engine may not be send eg. ActorDeleted)
                for (int i = 0; i < nodes.Length; i++)
                {
                    nodes[i].Dispose();
                }
            }

            /// <inheritdoc />
            protected override SceneGraphNode GetNode(Guid id)
            {
                return SceneGraphFactory.GetNode(id);
            }
        }

        private class CustomPasteActorsAction : PasteActorsAction
        {
            private PrefabWindow _window;

            private CustomPasteActorsAction(PrefabWindow window, byte[] data, Guid[] objectIds, ref Guid pasteParent, string name)
            : base(data, objectIds, ref pasteParent, name)
            {
                _window = window;
            }

            internal static CustomPasteActorsAction CustomPaste(PrefabWindow window, byte[] data, Guid pasteParent)
            {
                var objectIds = Actor.TryGetSerializedObjectsIds(data);
                if (objectIds == null || objectIds.Length == 0)
                    return null;

                return new CustomPasteActorsAction(window, data, objectIds, ref pasteParent, "Paste actors");
            }

            internal static CustomPasteActorsAction CustomDuplicate(PrefabWindow window, byte[] data, Guid pasteParent)
            {
                var objectIds = Actor.TryGetSerializedObjectsIds(data);
                if (objectIds == null || objectIds.Length == 0)
                    return null;

                return new CustomPasteActorsAction(window, data, objectIds, ref pasteParent, "Duplicate actors");
            }

            /// <inheritdoc />
            protected override void LinkBrokenParentReference(ActorNode actorNode)
            {
                // Link to prefab root
                actorNode.Actor.SetParent(_window.Graph.MainActor, false);
            }

            /// <inheritdoc />
            protected override void CheckBrokenParentReference(ActorNode actorNode)
            {
                if (actorNode.Actor.Scene != null || actorNode.Root != _window.Graph.Root)
                    LinkBrokenParentReference(actorNode);
            }

            /// <inheritdoc />
            public override void Undo()
            {
                TryUndo();
            }

            /// <inheritdoc />
            public override bool TryUndo()
            {
                var nodes = _nodeParents.ToArray();

                for (int i = 0; i < nodes.Length; i++)
                {
                    if (SceneGraphFactory.FindNode(nodes[i]) == null)
                        return false;
                }

                for (int i = 0; i < nodes.Length; i++)
                {
                    var node = SceneGraphFactory.FindNode(_nodeParents[i]);
                    if (node != null)
                    {
                        // Unlink nodes from parents (actors spawned for prefab editing are not in a gameplay and may not send some important events)
                        if (node is ActorNode actorNode)
                            actorNode.Actor.Parent = null;

                        // Remove objects
                        node.Delete();

                        // Remove nodes (actors in prefab are not in a gameplay and some events from the engine may not be send eg. ActorDeleted)
                        node.Dispose();
                    }
                }

                _nodeParents.Clear();
                return true;
            }

            /// <inheritdoc />
            protected override SceneGraphNode GetNode(Guid id)
            {
                return SceneGraphFactory.GetNode(id);
            }

            /// <inheritdoc />
            public override void Dispose()
            {
                base.Dispose();

                _window = null;
            }
        }

        /// <summary>
        /// Deletes selected objects.
        /// </summary>
        public void Delete()
        {
            // Peek things that can be removed
            var objects = Selection.Where(x => x != null && x.CanDelete && x != Graph.Main).ToList().BuildAllNodes().Where(x => x != null && x.CanDelete).ToList();
            if (objects.Count == 0)
                return;

            // Change selection
            var action1 = new SelectionChangeAction(Selection.ToArray(), new SceneGraphNode[0], OnSelectionUndo);

            // Delete objects
            var action2 = new CustomDeleteActorsAction(objects);

            // Merge actions and perform them
            var action = new MultiUndoAction(new IUndoAction[]
            {
                action1,
                action2
            }, action2.ActionString);
            if (!action.TryDo())
            {
                action.Dispose();
                Editor.LogError($"[SceneDebug] {action2.LastResult?.ErrorCode} Prefab delete failed. {action2.LastResult?.Message}");
                return;
            }
            Undo.AddAction(action);

            _treePanel.PerformLayout();
            _treePanel.PerformLayout();
        }

        /// <summary>
        /// Groups selected actors.
        /// </summary>
        public void MakeSelectionGroup()
        {
            CreateParentForSelectedActors();
        }

        /// <summary>
        /// Creates parent actor for selected actors and links them to it.
        /// </summary>
        public void CreateParentForSelectedActors()
        {
            var nodes = Selection.Where(x => x is ActorNode).Cast<ActorNode>().ToList().BuildNodesParents();
            if (nodes.Count == 0)
            {
                if (Graph.MainActor != null)
                {
                    var emptyGroup = new GroupActor { Name = "Group" };
                    Spawn(emptyGroup, Graph.MainActor);
                }
                return;
            }

            var actors = nodes.Select(x => x.Actor).Where(x => x != null).ToList();
            if (actors.Count == 0)
                return;

            var commonParent = SceneEditingModule.FindLowestCommonActorParent(actors) ?? Graph.MainActor;

            // Calculate center
            var bounds = BoundingBox.Empty;
            Vector3 center = Vector3.Zero;
            for (int i = 0; i < actors.Count; i++)
            {
                bounds = BoundingBox.Merge(bounds, actors[i].EditorBoxChildren);
                center += actors[i].Position;
            }
            center = bounds != BoundingBox.Empty ? bounds.Center : center / actors.Count;

            // Calculate order
            int groupOrder = int.MaxValue;
            for (int i = 0; i < actors.Count; i++)
            {
                var child = actors[i];
                while (child.Parent != null && child.Parent != commonParent)
                    child = child.Parent;
                if (child.Parent == commonParent)
                    groupOrder = Math.Min(groupOrder, child.OrderInParent);
            }
            if (groupOrder == int.MaxValue)
                groupOrder = -1;

            var plan = CSGGroupingPolicy.Classify(actors);
            var selectionBefore = Selection.ToArray();

            if (plan.WrapLooseBrushesInStack)
            {
                var looseBrushes = actors.Where(x => x is BoxBrush).ToList();
                var stacks = actors.OfType<CSGStack>().ToList();

                var model = new CSGModel
                {
                    Name = "CSG Model",
                    Position = center,
                };

                var looseBounds = BoundingBox.Empty;
                Vector3 looseCenter = Vector3.Zero;
                foreach (var brush in looseBrushes)
                {
                    looseBounds = BoundingBox.Merge(looseBounds, brush.EditorBoxChildren);
                    looseCenter += brush.Position;
                }
                looseCenter = looseBounds != BoundingBox.Empty ? looseBounds.Center : looseCenter / looseBrushes.Count;

                var autoStack = new CSGStack
                {
                    Name = "CSG Stack",
                    Position = looseCenter,
                };

                CustomDeleteActorsAction createModel = null;
                CustomDeleteActorsAction createAutoStack = null;
                ParentActorsAction parentLooseBrushes = null;
                ParentActorsAction parentStacks = null;
                try
                {
                    model.Parent = commonParent;
                    if (groupOrder != -1)
                        model.OrderInParent = groupOrder;
                    var modelNode = SceneGraphFactory.FindNode(model.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(model);
                    if (modelNode == null)
                        throw new InvalidOperationException("Failed to publish the CSG Model in the Scene graph.");
                    var commonParentNode = SceneGraphFactory.FindNode(commonParent.ID) as ActorNode;
                    modelNode.ParentNode = commonParentNode;
                    modelNode.PostSpawn();
                    createModel = new CustomDeleteActorsAction(new List<SceneGraphNode> { modelNode }, true);

                    autoStack.Parent = model;
                    var autoStackNode = SceneGraphFactory.FindNode(autoStack.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(autoStack);
                    if (autoStackNode == null)
                        throw new InvalidOperationException("Failed to publish the auto-generated CSG Stack in the Scene graph.");
                    autoStackNode.ParentNode = modelNode;
                    autoStackNode.PostSpawn();
                    createAutoStack = new CustomDeleteActorsAction(new List<SceneGraphNode> { autoStackNode }, true);

                    parentLooseBrushes = new ParentActorsAction(looseBrushes.Cast<SceneObject>().ToArray(), autoStack, -1, true);
                    if (!parentLooseBrushes.TryDo())
                        throw new InvalidOperationException("Failed to attach loose brushes to the CSG Stack. " + parentLooseBrushes.LastResult?.Message);

                    parentStacks = new ParentActorsAction(stacks.Cast<SceneObject>().ToArray(), model, -1, true);
                    if (!parentStacks.TryDo())
                        throw new InvalidOperationException("Failed to attach CSG Stacks to the CSG Model. " + parentStacks.LastResult?.Message);

                    var selectModel = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { modelNode }, OnSelectionUndo);
                    selectModel.Do();
                    Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createModel, createAutoStack, parentLooseBrushes, parentStacks, selectModel }, "Group actors"));
                    modelNode.TreeNode.StartRenaming(this, _treePanel);
                    return;
                }
                catch
                {
                    parentStacks?.TryUndo();
                    parentLooseBrushes?.TryUndo();
                    createAutoStack?.TryUndo();
                    createModel?.TryUndo();
                    Selection.Clear();
                    Selection.AddRange(selectionBefore.Where(x => x != null));
                    OnSelectionChanges();
                    if (autoStack)
                        FlaxEngine.Object.Destroy(ref autoStack);
                    if (model)
                        FlaxEngine.Object.Destroy(ref model);
                    FlaxEngine.Scripting.FlushRemovedObjects();
                    throw;
                }
            }

            GroupActor group = plan.Kind switch
            {
                CSGGroupingKind.CSGStack => new CSGStack { Name = "CSG Stack", Position = center },
                CSGGroupingKind.CSGModel => new CSGModel { Name = "CSG Model", Position = center },
                _ => new GroupActor { Name = "Group", Position = center },
            };

            CustomDeleteActorsAction createGroup = null;
            ParentActorsAction parentActors = null;
            try
            {
                group.Parent = commonParent;
                if (groupOrder != -1)
                    group.OrderInParent = groupOrder;
                var groupNode = SceneGraphFactory.FindNode(group.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(group);
                if (groupNode == null)
                    throw new InvalidOperationException("Failed to publish the group in the Scene graph.");
                var commonParentNode = SceneGraphFactory.FindNode(commonParent.ID) as ActorNode;
                groupNode.ParentNode = commonParentNode;
                groupNode.PostSpawn();
                createGroup = new CustomDeleteActorsAction(new List<SceneGraphNode> { groupNode }, true);

                parentActors = new ParentActorsAction(actors.Cast<SceneObject>().ToArray(), group, -1, true);
                if (!parentActors.TryDo())
                    throw new InvalidOperationException("Failed to attach Actors to the group. " + parentActors.LastResult?.Message);

                var selectGroup = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { groupNode }, OnSelectionUndo);
                selectGroup.Do();
                Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createGroup, parentActors, selectGroup }, "Group actors"));
                groupNode.TreeNode.StartRenaming(this, _treePanel);
            }
            catch
            {
                parentActors?.TryUndo();
                createGroup?.TryUndo();
                Selection.Clear();
                Selection.AddRange(selectionBefore.Where(x => x != null));
                OnSelectionChanges();
                if (group)
                    FlaxEngine.Object.Destroy(ref group);
                FlaxEngine.Scripting.FlushRemovedObjects();
                throw;
            }
        }

        /// <summary>
        /// Wraps selected actors in a CSG Stack.
        /// </summary>
        public void WrapSelectedInCSGStack()
        {
            WrapSelectedInCSG(typeof(CSGStack), "CSG Stack");
        }

        /// <summary>
        /// Wraps selected actors in a CSG Model.
        /// </summary>
        public void WrapSelectedInCSGModel()
        {
            WrapSelectedInCSG(typeof(CSGModel), "CSG Model");
        }

        private void WrapSelectedInCSG(Type wrapperType, string defaultName)
        {
            var nodes = Selection.Where(x => x is ActorNode).Cast<ActorNode>().ToList().BuildNodesParents();
            if (nodes.Count == 0)
                return;

            var actors = nodes.Select(x => x.Actor).Where(x => x != null).ToList();
            if (actors.Count == 0)
                return;

            var commonParent = SceneEditingModule.FindLowestCommonActorParent(actors) ?? Graph.MainActor;

            var bounds = BoundingBox.Empty;
            Vector3 center = Vector3.Zero;
            for (int i = 0; i < actors.Count; i++)
            {
                bounds = BoundingBox.Merge(bounds, actors[i].EditorBoxChildren);
                center += actors[i].Position;
            }
            center = bounds != BoundingBox.Empty ? bounds.Center : center / actors.Count;

            int groupOrder = int.MaxValue;
            for (int i = 0; i < actors.Count; i++)
            {
                var child = actors[i];
                while (child.Parent != null && child.Parent != commonParent)
                    child = child.Parent;
                if (child.Parent == commonParent)
                    groupOrder = Math.Min(groupOrder, child.OrderInParent);
            }
            if (groupOrder == int.MaxValue)
                groupOrder = -1;

            var wrapper = (GroupActor)FlaxEngine.Object.New(wrapperType);
            wrapper.Name = defaultName;
            wrapper.Position = center;

            var selectionBefore = Selection.ToArray();
            CustomDeleteActorsAction createWrapper = null;
            ParentActorsAction parentActors = null;
            try
            {
                wrapper.Parent = commonParent;
                if (groupOrder != -1)
                    wrapper.OrderInParent = groupOrder;
                var wrapperNode = SceneGraphFactory.FindNode(wrapper.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(wrapper);
                if (wrapperNode == null)
                    throw new InvalidOperationException("Failed to publish wrapper in the Scene graph.");
                var commonParentNode = SceneGraphFactory.FindNode(commonParent.ID) as ActorNode;
                wrapperNode.ParentNode = commonParentNode;
                wrapperNode.PostSpawn();
                createWrapper = new CustomDeleteActorsAction(new List<SceneGraphNode> { wrapperNode }, true);

                parentActors = new ParentActorsAction(actors.Cast<SceneObject>().ToArray(), wrapper, -1, true);
                if (!parentActors.TryDo())
                    throw new InvalidOperationException("Failed to attach Actors to the wrapper. " + parentActors.LastResult?.Message);

                var selectWrapper = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { wrapperNode }, OnSelectionUndo);
                selectWrapper.Do();
                Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createWrapper, parentActors, selectWrapper }, $"Wrap in {defaultName}"));
                wrapperNode.TreeNode.StartRenaming(this, _treePanel);
            }
            catch
            {
                parentActors?.TryUndo();
                createWrapper?.TryUndo();
                Selection.Clear();
                Selection.AddRange(selectionBefore.Where(x => x != null));
                OnSelectionChanges();
                if (wrapper)
                    FlaxEngine.Object.Destroy(ref wrapper);
                FlaxEngine.Scripting.FlushRemovedObjects();
                throw;
            }
        }
    }
}
