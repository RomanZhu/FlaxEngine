// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Actions;
using FlaxEditor.History;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.CSG.Rebuild;
using FlaxEngine;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Editing scenes module. Manages scene objects selection and editing modes.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class SceneEditingModule : EditorModule
    {
        private int _lastSelectionStatus = 0;

        /// <summary>
        /// The selected objects.
        /// </summary>
        public readonly List<SceneGraphNode> Selection = new List<SceneGraphNode>(64);

        /// <summary>
        /// Gets the amount of the selected objects.
        /// </summary>
        public int SelectionCount => Selection.Count;

        /// <summary>
        /// Gets a value indicating whether any object is selected.
        /// </summary>
        public bool HasSthSelected => Selection.Count > 0;

        /// <summary>
        /// Occurs when selected objects collection gets changed.
        /// </summary>
        public event Action SelectionChanged;

        /// <summary>
        /// Occurs before spawning actor to game action.
        /// </summary>
        public event Action SpawnBegin;

        /// <summary>
        /// Occurs after spawning actor to game action.
        /// </summary>
        public event Action SpawnEnd;

        /// <summary>
        /// Occurs before selection delete action.
        /// </summary>
        public event Action SelectionDeleteBegin;

        /// <summary>
        /// Occurs after selection delete action.
        /// </summary>
        public event Action SelectionDeleteEnd;

        internal SceneEditingModule(Editor editor)
        : base(editor)
        {
        }

        private void BulkScenesSelectUpdate(bool select = true)
        {
            // Blank list deselects all
            Select(select ? Editor.Scene.Root.ChildNodes : new List<SceneGraphNode>());
        }

        /// <summary>
        /// Selects all scenes.
        /// </summary>
        public void SelectAllScenes()
        {
            BulkScenesSelectUpdate(true);
        }

        /// <summary>
        /// Deselects all scenes.
        /// </summary>
        public void DeselectAllScenes()
        {
            BulkScenesSelectUpdate(false);
        }

        /// <summary>
        /// Selects the specified actor (finds it's scene graph node).
        /// </summary>
        /// <param name="actor">The actor.</param>
        public void Select(Actor actor)
        {
            var node = Editor.Scene.GetActorNode(actor);
            if (node != null)
                Select(node);
        }

        /// <summary>
        /// Selects the specified collection of objects.
        /// </summary>
        /// <param name="selection">The selection.</param>
        /// <param name="additive">if set to <c>true</c> will use additive mode, otherwise will clear previous selection.</param>
        /// <param name="recordUndo">True if record the selection change in edit and navigation history.</param>
        public void Select(List<SceneGraphNode> selection, bool additive = false, bool recordUndo = true)
        {
            if (selection == null)
            {
                Deselect();
                return;
            }

            // Prevent from selecting null nodes
            selection.RemoveAll(x => x == null);

            // Check if won't change
            if (!additive && Selection.Count == selection.Count && Selection.SequenceEqual(selection))
                return;

            var before = Selection.ToArray();
            if (!additive)
                Selection.Clear();
            Selection.AddRange(selection);

            SelectionChange(before, recordUndo);
        }

        /// <summary>
        /// Selects the specified collection of objects.
        /// </summary>
        /// <param name="selection">The selection.</param>
        /// <param name="additive">if set to <c>true</c> will use additive mode, otherwise will clear previous selection.</param>
        /// <param name="recordUndo">True if record the selection change in edit and navigation history.</param>
        public void Select(SceneGraphNode[] selection, bool additive = false, bool recordUndo = true)
        {
            if (selection == null)
                throw new ArgumentNullException();

            Select(selection.ToList(), additive, recordUndo);
        }

        /// <summary>
        /// Selects the specified object.
        /// </summary>
        /// <param name="selection">The selection.</param>
        /// <param name="additive">if set to <c>true</c> will use additive mode, otherwise will clear previous selection.</param>
        public void Select(SceneGraphNode selection, bool additive = false)
        {
            if (selection == null)
                throw new ArgumentNullException();

            // Check if won't change
            if (!additive && Selection.Count == 1 && Selection[0] == selection)
                return;
            if (additive && Selection.Contains(selection))
                return;

            var before = Selection.ToArray();
            if (!additive)
                Selection.Clear();
            Selection.Add(selection);

            SelectionChange(before);
        }

        /// <summary>
        /// Deselects given object.
        /// </summary>
        public void Deselect(SceneGraphNode node)
        {
            if (!Selection.Contains(node))
                return;

            var before = Selection.ToArray();
            Selection.Remove(node);

            SelectionChange(before);
        }

        /// <summary>
        /// Clears selected objects collection.
        /// </summary>
        /// <param name="recordUndo">True if record the selection change in edit and navigation history.</param>
        public void Deselect(bool recordUndo = true)
        {
            // Check if won't change
            if (Selection.Count == 0)
                return;

            var before = Selection.ToArray();
            Selection.Clear();

            SelectionChange(before, recordUndo);
        }

        private void SelectionChange(SceneGraphNode[] before, bool recordUndo = true)
        {
            var after = Selection.ToArray();
            for (int i = after.Length - 1; i >= 0; i--)
            {
                var scene = after[i]?.ParentScene?.Scene;
                if (scene != null)
                {
                    Editor.Scene.SetActiveScene(scene);
                    break;
                }
            }
            var contentSelectionBefore = Array.Empty<string>();
            var contentSelectionAfter = Array.Empty<string>();
            Action<string[]> contentSelectionCallback = null;
            var contentWindow = Editor.Windows?.ContentWin;
            if (recordUndo && !Editor.Undo.IsPerformingUndoRedo && contentWindow != null && after.Length != 0)
            {
                contentSelectionBefore = contentWindow.GetSelectionPathsForSceneUndo();
                if (contentSelectionBefore.Length != 0)
                {
                    contentSelectionCallback = contentWindow.RestoreSelectionFromSceneUndo;
                }
            }
            if (recordUndo && !Editor.Undo.IsPerformingUndoRedo)
            {
                var previousSelectionAction = Editor.Undo.UndoOperationsStack.PeekHistory() as SelectionChangeAction;
                if (previousSelectionAction == null || !previousSelectionAction.IsSameTransition(before, after, OnSelectionUndo, contentSelectionBefore, contentSelectionAfter, contentSelectionCallback))
                    Editor.Undo.AddAction(new SelectionChangeAction(before, after, OnSelectionUndo, contentSelectionBefore, contentSelectionAfter, contentSelectionCallback));
                Editor.NavigationHistory.AddAction(new SelectionNavigationAction(this, before, after, OnSelectionUndo, contentSelectionBefore, contentSelectionAfter, contentSelectionCallback));
            }

            OnSelectionChanged();

            // Display amount of selected actors on the status bar
            Editor.UI.RemoveStatusMessage(_lastSelectionStatus);
            var objects = Selection.Count;
            var actors = CountActors(Selection);
            _lastSelectionStatus = Editor.UI.AddStatusMessage($"Selected {objects} {(objects > 1 ? "objects" : "object")} (total {actors} {(actors > 1 ? "actors" : "actor")})");
        }

        private int CountActors(List<SceneGraphNode> nodes)
        {
            int result = 0;
            foreach (var node in nodes)
                result += 1 + CountActors(node.ChildNodes);
            return result;
        }

        private void OnSelectionUndo(SceneGraphNode[] toSelect)
        {
            Selection.Clear();
            if (toSelect != null)
            {
                for (int i = 0; i < toSelect.Length; i++)
                {
                    if (toSelect[i] != null)
                        Selection.Add(toSelect[i]);
                    else
                        Editor.LogWarning("Null scene graph node to select");
                }
            }

            OnSelectionChanged();
        }

        private void OnDirty(ActorNode node, bool requestCSGRebuild = true)
        {
            var options = Editor.Options.Options;
            var isPlayMode = Editor.StateMachine.IsPlayMode;
            var actor = node.Actor;

            // Auto CSG mesh rebuild
            if (requestCSGRebuild && !isPlayMode && actor is BoxBrush)
            {
                var target = CSGRebuildScheduler.ResolveTarget(actor);
                if (target != null)
                    CSGRebuildScheduler.Shared.RequestExternal(target);
            }

            // Auto NavMesh rebuild
            if (!isPlayMode && options.General.AutoRebuildNavMesh && actor.Scene && node.AffectsNavigationWithChildren)
            {
                var bounds = actor.BoxWithChildren;
                Navigation.BuildNavMesh(bounds, options.General.AutoRebuildNavMeshTimeoutMs);
            }
        }

        private static bool SelectActorsUsingAsset(Guid assetId, ref Guid id, Dictionary<Guid, bool> scannedAssets)
        {
            // Check for asset match or try to use cache
            if (assetId == id)
                return true;
            if (scannedAssets.TryGetValue(id, out var result))
                return result;
            if (id == Guid.Empty || !FlaxEngine.Content.GetAssetInfo(id, out var assetInfo))
                return false;
            scannedAssets.Add(id, false);

            // Skip scene assets
            if (assetInfo.TypeName == "FlaxEngine.SceneAsset")
                return false;

            // Recursive check if this asset contains direct or indirect reference to the given asset
            var asset = FlaxEngine.Content.Load<Asset>(assetInfo.ID, 1000);
            if (asset)
            {
                var references = asset.GetReferences();
                for (var i = 0; i < references.Length; i++)
                {
                    if (SelectActorsUsingAsset(assetId, ref references[i], scannedAssets))
                    {
                        scannedAssets[id] = true;
                        return true;
                    }
                }
            }

            return false;
        }

        private static void SelectActorsUsingAsset(Guid assetId, SceneGraphNode node, List<SceneGraphNode> selection, Dictionary<Guid, bool> scannedAssets)
        {
            if (node is ActorNode actorNode && actorNode.Actor)
            {
                // To detect if this actor uses the given asset simply serialize it to json and check used asset ids
                // TODO: check scripts too
                var json = actorNode.Actor.ToJson();
                JsonAssetBase.GetReferences(json, out var ids);
                for (var i = 0; i < ids.Length; i++)
                {
                    if (SelectActorsUsingAsset(assetId, ref ids[i], scannedAssets))
                    {
                        selection.Add(actorNode);
                        break;
                    }
                }
            }

            // Recursive check for children
            for (int i = 0; i < node.ChildNodes.Count; i++)
                SelectActorsUsingAsset(assetId, node.ChildNodes[i], selection, scannedAssets);
        }

        /// <summary>
        /// Selects the actors using the given asset.
        /// </summary>
        /// <param name="assetId">The asset ID.</param>
        /// <param name="additive">if set to <c>true</c> will use additive mode, otherwise will clear previous selection.</param>
        public void SelectActorsUsingAsset(Guid assetId, bool additive = false)
        {
            // TODO: make it async action with progress
            Profiler.BeginEvent("SelectActorsUsingAsset");
            var selection = new List<SceneGraphNode>();
            var scannedAssets = new Dictionary<Guid, bool>();
            SelectActorsUsingAsset(assetId, Editor.Scene.Root, selection, scannedAssets);
            Profiler.EndEvent();

            Select(selection, additive);
        }

        /// <summary>
        /// Spawns the specified actor to the game (with undo).
        /// </summary>
        /// <param name="actor">The actor.</param>
        /// <param name="parent">The parent actor. Set null as default.</param>
        /// <param name="orderInParent">The order under the parent to put the spawned actor.</param>
        /// <param name="autoSelect">True if automatically select the spawned actor, otherwise false.</param>
        public void Spawn(Actor actor, Actor parent = null, int orderInParent = -1, bool autoSelect = true)
        {
            bool isPlayMode = Editor.StateMachine.IsPlayMode;

            if (actor == null)
                throw new ArgumentNullException(nameof(actor));
            parent ??= Editor.Scene.ActiveScene;
            if (parent == null || parent.Scene == null || Level.FindScene(parent.Scene.ID) != parent.Scene)
                throw new InvalidOperationException("Cannot spawn Actor without a loaded explicit destination Scene and parent.");

            SpawnBegin?.Invoke();
            var before = Selection.ToArray();
            try
            {
                // During play in editor mode spawned actors should be dynamic (user can move them)
                if (isPlayMode)
                    actor.StaticFlags = StaticFlags.None;

                Level.SpawnActor(actor, parent);
                if (orderInParent != -1)
                    actor.OrderInParent = orderInParent;

                var actorNode = Editor.Instance.Scene.GetActorNode(actor);
                if (actorNode == null)
                    throw new InvalidOperationException("Failed to create scene node for the spawned actor.");
                actorNode.PostSpawn();

                IUndoAction action = new DeleteActorsAction(actorNode, true);
                if (autoSelect)
                {
                    Selection.Clear();
                    Selection.Add(actorNode);
                    OnSelectionChanged();
                    action = new MultiUndoAction(action, new SelectionChangeAction(before, Selection.ToArray(), OnSelectionUndo));
                }
                OnDirty(actorNode);
                Undo.AddAction(action);
            }
            catch
            {
                Selection.Clear();
                Selection.AddRange(before);
                OnSelectionChanged();
                if (actor)
                    FlaxEngine.Object.Destroy(ref actor);
                FlaxEngine.Scripting.FlushRemovedObjects();
                throw;
            }
            finally
            {
                SpawnEnd?.Invoke();
            }

        }

        /// <summary>
        /// Spawns and selects a CSG actor as an already-performed action owned by an authoring transaction.
        /// The caller must either add the returned action to undo history or undo and dispose it.
        /// </summary>
        /// <param name="actor">The initialized CSG actor.</param>
        /// <param name="actorNode">The spawned scene graph node.</param>
        /// <param name="undoAction">The already-performed create and selection action.</param>
        /// <returns>True when the actor was spawned and selected.</returns>
        internal bool TrySpawnForCSGTransaction(Actor actor, out ActorNode actorNode, out IUndoAction undoAction)
        {
            actorNode = null;
            undoAction = null;
            if (actor == null || Level.IsAnySceneLoaded == false || Editor.StateMachine.IsPlayMode)
                return false;

            var before = Selection.ToArray();
            SpawnBegin?.Invoke();
            try
            {
                var parent = actor.Parent ?? Editor.Scene.ActiveScene;
                if (parent == null)
                    throw new InvalidOperationException("Cannot spawn the CSG actor without an explicit active destination Scene.");
                actor.Name = Utilities.Utils.IncrementNameNumber(actor.Name, x => parent.GetChild(x) == null);
                Level.SpawnActor(actor, parent);
                actorNode = Editor.Scene.GetActorNode(actor);
                if (actorNode == null)
                    throw new InvalidOperationException("Failed to create scene node for the CSG actor.");
                actorNode.PostSpawn();

                var createAction = new DeleteActorsAction(actorNode, true);
                Selection.Clear();
                Selection.Add(actorNode);
                OnSelectionChanged();
                var selectionAction = new SelectionChangeAction(before, Selection.ToArray(), OnSelectionUndo);
                undoAction = new MultiUndoAction(new IUndoAction[] { createAction, selectionAction }, "Create CSG Box");

                OnDirty(actorNode, false);
                return true;
            }
            catch
            {
                try
                {
                    Selection.Clear();
                    Selection.AddRange(before);
                    OnSelectionChanged();
                }
                catch (Exception ex)
                {
                    Editor.LogError("CSG selection rollback failed. " + ex.Message);
                }
                if (actorNode != null)
                {
                    try
                    {
                        actorNode.Delete();
                        FlaxEngine.Scripting.FlushRemovedObjects();
                    }
                    catch (Exception ex)
                    {
                        Editor.LogError("CSG actor spawn rollback failed. " + ex.Message);
                    }
                }
                actorNode = null;
                undoAction?.Dispose();
                undoAction = null;
                throw;
            }
            finally
            {
                SpawnEnd?.Invoke();
            }
        }

        /// <summary>
        /// Converts the selected actor to another type.
        /// </summary>
        /// <param name="to">The type to convert in.</param>
        public void Convert(Type to)
        {
            if (!HasSthSelected || Selection[0] is not ActorNode oldNode)
                return;
            Convert(oldNode, to);
        }

        /// <summary>
        /// Converts the specified actor to another type.
        /// </summary>
        /// <param name="oldNode">The actor node to convert.</param>
        /// <param name="to">The target actor type.</param>
        public void Convert(ActorNode oldNode, Type to)
        {
            if (oldNode == null || !oldNode.Actor || oldNode.Actor.GetType() == to)
                return;
            if (to == null || !typeof(Actor).IsAssignableFrom(to) || to.IsAbstract)
                throw new ArgumentException("The conversion target must be a concrete Actor type.", nameof(to));

            var old = oldNode.Actor;
            var parent = old.Parent;
            var scene = old.Scene;
            if (parent == null || scene == null || Level.FindScene(scene.ID) != scene)
                throw new InvalidOperationException("Cannot convert an Actor without a loaded explicit Scene and parent.");

            var actor = (Actor)FlaxEngine.Object.New(to);
            if (actor == null)
                throw new InvalidOperationException("Failed to construct the target Actor type.");
            var orderInParent = old.OrderInParent;
            var selectionBefore = Selection.ToArray();
            var scripts = old.Scripts;
            var children = old.Children;
            var deleteOld = new DeleteActorsAction(oldNode.BuildAllNodes().Where(x => x.CanDelete).ToList());
            DeleteActorsAction createNew = null;
            var oldDeleted = false;
            ActorNode actorNode = null;
            SelectionDeleteBegin?.Invoke();
            SpawnBegin?.Invoke();
            try
            {
                // Stage the replacement while the complete original graph remains recoverable.
                actor.Transform = old.Transform;
                actor.StaticFlags = old.StaticFlags;
                actor.HideFlags = old.HideFlags;
                actor.Layer = old.Layer;
                actor.Tags = old.Tags;
                actor.Name = old.Name;
                actor.IsActive = old.IsActive;

                Level.SpawnActor(actor, parent);
                actor.OrderInParent = orderInParent;
                if (Editor.StateMachine.IsPlayMode)
                    actor.StaticFlags = StaticFlags.None;

                for (var i = scripts.Length - 1; i >= 0; i--)
                    scripts[i].Actor = actor;
                for (var i = children.Length - 1; i >= 0; i--)
                    children[i].Parent = actor;

                actorNode = Editor.Instance.Scene.GetActorNode(actor);
                if (actorNode == null)
                    throw new InvalidOperationException("Failed to publish the replacement Actor in the Scene graph.");
                actorNode.PostConvert(oldNode);
                actorNode.PostSpawn();

                // Commit deletion only after the replacement graph is complete.
                if (!deleteOld.TryDo())
                    throw new InvalidOperationException("Failed to remove the original Actor. " + deleteOld.LastResult?.Message);
                oldDeleted = true;
                createNew = new DeleteActorsAction(actorNode.BuildAllNodes().Where(x => x.CanDelete).ToList(), true);

                var clearSelection = new SelectionChangeAction(selectionBefore, Array.Empty<SceneGraphNode>(), OnSelectionUndo);
                var selectNew = new SelectionChangeAction(Array.Empty<SceneGraphNode>(), new SceneGraphNode[] { actorNode }, OnSelectionUndo);
                clearSelection.Do();
                selectNew.Do();

                Undo.AddAction(new MultiUndoAction(new IUndoAction[]
                {
                    clearSelection,
                    deleteOld,
                    createNew,
                    selectNew,
                }, "Convert actor"));
                OnDirty(actorNode);
            }
            catch
            {
                // Reverse staged publication before restoring the original serialized graph.
                if (oldDeleted)
                {
                    if (createNew != null)
                        createNew.TryUndo();
                    else if (actor)
                        FlaxEngine.Object.Destroy(ref actor);
                    FlaxEngine.Scripting.FlushRemovedObjects();
                    deleteOld.TryUndo();
                }
                else
                {
                    for (var i = scripts.Length - 1; i >= 0; i--)
                    {
                        if (scripts[i] != null)
                            scripts[i].Actor = old;
                    }
                    for (var i = children.Length - 1; i >= 0; i--)
                    {
                        if (children[i] != null)
                            children[i].Parent = old;
                    }
                    if (actor)
                        FlaxEngine.Object.Destroy(ref actor);
                    FlaxEngine.Scripting.FlushRemovedObjects();
                }

                Selection.Clear();
                for (int i = 0; i < selectionBefore.Length; i++)
                {
                    var node = SceneGraphFactory.FindNode(selectionBefore[i]?.ID ?? Guid.Empty);
                    if (node != null)
                        Selection.Add(node);
                }
                OnSelectionChanged();
                throw;
            }
            finally
            {
                SelectionDeleteEnd?.Invoke();
                SpawnEnd?.Invoke();
            }
        }

        /// <summary>
        /// Deletes the selected objects. Supports undo/redo.
        /// </summary>
        public void Delete()
        {
            // Peek things that can be removed
            var objects = Selection.Where(x => x.CanDelete).ToList().BuildAllNodes().Where(x => x.CanDelete).ToList();
            if (objects.Count == 0)
                return;
            var isSceneTreeFocus = Editor.Windows.SceneWin.ContainsFocus;

            SelectionDeleteBegin?.Invoke();

            // Change selection
            var action1 = new SelectionChangeAction(Selection.ToArray(), new SceneGraphNode[0], OnSelectionUndo);

            // Delete objects
            var action2 = new DeleteActorsAction(objects);

            // Merge two actions and perform them
            var action = new MultiUndoAction(new IUndoAction[]
            {
                action1,
                action2
            }, action2.ActionString);
            if (!action.TryDo())
            {
                action.Dispose();
                Editor.LogError($"[SceneDebug] Delete failed. {action2.LastResult?.ErrorCode} {action2.LastResult?.Message}");
                Editor.UI.AddStatusMessage("Cannot delete Actors: " + (action2.LastResult?.Message ?? "the mutation failed."));
                SelectionDeleteEnd?.Invoke();
                return;
            }
            Undo.AddAction(action);

            SelectionDeleteEnd?.Invoke();

            if (isSceneTreeFocus)
            {
                Editor.Windows.SceneWin.Focus();
            }

            // fix scene window layout
            Editor.Windows.SceneWin.PerformLayout();
            Editor.Windows.SceneWin.PerformLayout();
        }

        /// <summary>
        /// Copies the selected objects.
        /// </summary>
        public void Copy()
        {
            TryCopy();
        }

        /// <summary>
        /// Attempts to create a complete Actor clipboard payload.
        /// </summary>
        /// <returns>True when a validated non-empty payload was published.</returns>
        public bool TryCopy()
        {
            // Peek things that can be copied (copy all actors)
            var objects = Selection.Where(x => x.CanCopyPaste).ToList().BuildAllNodes().Where(x => x.CanCopyPaste && x is ActorNode).ToList();
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

            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (objectIds == null || objectIds.Length == 0)
            {
                Editor.LogError("[SceneDebug] InvalidPayload Actor copy rejected because serialization produced an invalid payload.");
                return false;
            }

            // Copy data
            Clipboard.RawData = data;
            return true;
        }


        /// <summary>
        /// Pastes the copied objects. Supports undo/redo.
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
            if (pasteTargetActor == null && SelectionCount == 1 && Selection[0] is ActorNode actorNode)
            {
                pasteTargetActor = actorNode.Actor;
            }

            var destinationScene = pasteTargetActor?.Scene ?? Editor.Scene.ActiveScene;
            if (destinationScene == null)
            {
                Editor.LogError("[SceneDebug] MissingDestination Paste rejected because there is no unambiguous active destination Scene.");
                Editor.UI.AddStatusMessage("Cannot paste Actors: select a destination in a loaded Scene.");
                return;
            }

            // Create paste action
            var pasteAction = PasteActorsAction.Paste(data, destinationScene, pasteTargetActor);
            if (pasteAction != null)
            {
                if (!pasteAction.TryDo(out _, out var nodeParents))
                {
                    var result = pasteAction.LastResult;
                    Editor.LogError($"[SceneDebug] {result?.ErrorCode} Paste {result?.Status} Transaction={result?.TransactionId} {result?.Message}");
                    Editor.UI.AddStatusMessage("Cannot paste Actors: " + (result?.Message ?? "invalid clipboard payload."));
                    pasteAction.Dispose();
                    return;
                }

                // Select spawned objects (parents only)
                var selectAction = new SelectionChangeAction(Selection.ToArray(), nodeParents.Cast<SceneGraphNode>().ToArray(), OnSelectionUndo);
                try
                {
                    selectAction.Do();
                }
                catch (Exception ex)
                {
                    if (!pasteAction.TryUndo())
                        Editor.LogError("[SceneDebug] RollbackFailed Paste selection publication rollback failed.");
                    pasteAction.Dispose();
                    Editor.LogError("[SceneDebug] PublicationFailed Paste selection failed. " + ex.Message);
                    Editor.UI.AddStatusMessage("Cannot paste Actors: selection publication failed.");
                    return;
                }

                // Build single compound undo action that pastes the actors and selects the created objects (parents only)
                Undo.AddAction(new MultiUndoAction(pasteAction, selectAction));
                OnSelectionChanged();
            }
            else
            {
                Editor.LogError("[SceneDebug] InvalidPayload Paste rejected because the clipboard does not contain a valid Actor graph.");
                Editor.UI.AddStatusMessage("Cannot paste Actors: the clipboard payload is invalid.");
                return;
            }

            // Scroll to new selected node while pasting
            Editor.Windows.SceneWin.ScrollToSelectedNode();
        }

        /// <summary>
        /// Cuts the selected objects. Supports undo/redo.
        /// </summary>
        public void Cut()
        {
            if (TryCopy())
            {
                Delete();
            }
            else
            {
                Editor.UI.AddStatusMessage("Cannot cut Actors: the clipboard payload could not be created.");
            }
        }

        /// <summary>
        /// Converts or wraps the current selection into a group, depending on the selected actor types.
        /// </summary>
        public void MakeSelectionGroup()
        {
            if (Selection.Count == 1 && Selection[0] is ActorNode actorNode && actorNode.Actor)
            {
                var actorType = actorNode.Actor.GetType();
                if (actorType == typeof(GroupActor))
                    return;
                if (actorType == typeof(EmptyActor))
                {
                    Convert(actorNode, typeof(GroupActor));
                    return;
                }
            }

            CreateParentForSelectedActors();
        }

        /// <summary>
        /// Create parent for selected actors.
        /// </summary>
        public void CreateParentForSelectedActors()
        {
            if (!Level.IsAnySceneLoaded)
                return;

            var nodes = Selection.Where(x => x is ActorNode and not SceneNode).Cast<ActorNode>().ToList().BuildNodesParents();
            if (nodes.Count == 0)
            {
                var destinationScene = Editor.Scene.ActiveScene;
                if (destinationScene == null)
                {
                    Editor.UI.AddStatusMessage("Cannot create a group: select an active destination Scene.");
                    return;
                }
                var emptyGroup = new GroupActor
                {
                    Name = "Group",
                    Position = Editor.Windows.EditWin.Viewport.GetWorldPointUnderCursor(),
                };
                Spawn(emptyGroup, destinationScene, -1, false);
                SelectAndRenameGroup(emptyGroup);
                return;
            }

            var actors = nodes.Select(x => x.Actor).ToList();
            var commonParent = FindLowestCommonActorParent(actors);
            if (commonParent == null)
                return;

            var bounds = BoundingBox.Empty;
            Vector3 center = Vector3.Zero;
            foreach (var actor in actors)
            {
                bounds = BoundingBox.Merge(bounds, actor.EditorBoxChildren);
                center += actor.Position;
            }
            center = bounds != BoundingBox.Empty ? bounds.Center : center / actors.Count;

            int groupOrder = int.MaxValue;
            for (int i = 0; i < actors.Count; i++)
            {
                var child = actors[i];
                while (child.Parent != commonParent)
                    child = child.Parent;
                groupOrder = Math.Min(groupOrder, child.OrderInParent);
            }

            // Categorize actors for semantic CSG promotion hierarchy:
            // Brushes -> CSGStack -> CSGModel -> GroupActor
            var plan = Tools.CSG.CSGGroupingPolicy.Classify(actors);
            var selectionBefore = Selection.ToArray();

            // Promotion rule: Stack + Brush -> CSGModel, with loose brushes auto-wrapped in a CSGStack
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

                DeleteActorsAction createModel = null;
                DeleteActorsAction createAutoStack = null;
                ParentActorsAction parentLooseBrushes = null;
                ParentActorsAction parentStacks = null;
                try
                {
                    Level.SpawnActor(model, commonParent);
                    model.OrderInParent = groupOrder;
                    var modelNode = Editor.Scene.GetActorNode(model);
                    if (modelNode == null)
                        throw new InvalidOperationException("Failed to publish the CSG Model in the Scene graph.");
                    modelNode.PostSpawn();
                    createModel = new DeleteActorsAction(modelNode, true);

                    Level.SpawnActor(autoStack, model);
                    var autoStackNode = Editor.Scene.GetActorNode(autoStack);
                    if (autoStackNode == null)
                        throw new InvalidOperationException("Failed to publish the auto-generated CSG Stack in the Scene graph.");
                    autoStackNode.PostSpawn();
                    createAutoStack = new DeleteActorsAction(autoStackNode, true);

                    parentLooseBrushes = new ParentActorsAction(looseBrushes.Cast<SceneObject>().ToArray(), autoStack, -1, true);
                    if (!parentLooseBrushes.TryDo())
                        throw new InvalidOperationException("Failed to attach loose brushes to the CSG Stack. " + parentLooseBrushes.LastResult?.Message);

                    parentStacks = new ParentActorsAction(stacks.Cast<SceneObject>().ToArray(), model, -1, true);
                    if (!parentStacks.TryDo())
                        throw new InvalidOperationException("Failed to attach CSG Stacks to the CSG Model. " + parentStacks.LastResult?.Message);

                    var selectModel = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { modelNode }, OnSelectionUndo);
                    selectModel.Do();
                    Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createModel, createAutoStack, parentLooseBrushes, parentStacks, selectModel }, "Group actors"));
                    modelNode.TreeNode.StartRenaming(Editor.Windows.SceneWin, Editor.Windows.SceneWin.SceneTreePanel);
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
                    OnSelectionChanged();
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
                Tools.CSG.CSGGroupingKind.CSGStack => new CSGStack { Name = "CSG Stack", Position = center },
                Tools.CSG.CSGGroupingKind.CSGModel => new CSGModel { Name = "CSG Model", Position = center },
                _ => new GroupActor { Name = "Group", Position = center },
            };

            DeleteActorsAction createGroup = null;
            ParentActorsAction parentActors = null;
            try
            {
                Level.SpawnActor(group, commonParent);
                group.OrderInParent = groupOrder;
                var groupNode = Editor.Scene.GetActorNode(group);
                if (groupNode == null)
                    throw new InvalidOperationException("Failed to publish the group in the Scene graph.");
                groupNode.PostSpawn();
                createGroup = new DeleteActorsAction(groupNode, true);

                parentActors = new ParentActorsAction(actors.Cast<SceneObject>().ToArray(), group, -1, true);
                if (!parentActors.TryDo())
                    throw new InvalidOperationException("Failed to attach Actors to the group. " + parentActors.LastResult?.Message);

                var selectGroup = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { groupNode }, OnSelectionUndo);
                selectGroup.Do();
                Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createGroup, parentActors, selectGroup }, "Group actors"));
                groupNode.TreeNode.StartRenaming(Editor.Windows.SceneWin, Editor.Windows.SceneWin.SceneTreePanel);
            }
            catch
            {
                parentActors?.TryUndo();
                createGroup?.TryUndo();
                Selection.Clear();
                Selection.AddRange(selectionBefore.Where(x => x != null));
                OnSelectionChanged();
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
            if (!Level.IsAnySceneLoaded)
                return;

            var nodes = Selection.Where(x => x is ActorNode and not SceneNode).Cast<ActorNode>().ToList().BuildNodesParents();
            if (nodes.Count == 0)
                return;

            var actors = nodes.Select(x => x.Actor).ToList();
            var commonParent = FindLowestCommonActorParent(actors);
            if (commonParent == null)
                return;

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
                while (child.Parent != commonParent)
                    child = child.Parent;
                groupOrder = Math.Min(groupOrder, child.OrderInParent);
            }

            var wrapper = (GroupActor)FlaxEngine.Object.New(wrapperType);
            wrapper.Name = defaultName;
            wrapper.Position = center;

            var selectionBefore = Selection.ToArray();
            DeleteActorsAction createWrapper = null;
            ParentActorsAction parentActors = null;
            try
            {
                Level.SpawnActor(wrapper, commonParent);
                wrapper.OrderInParent = groupOrder;
                var wrapperNode = Editor.Scene.GetActorNode(wrapper);
                if (wrapperNode == null)
                    throw new InvalidOperationException("Failed to publish wrapper in the Scene graph.");
                wrapperNode.PostSpawn();
                createWrapper = new DeleteActorsAction(wrapperNode, true);

                parentActors = new ParentActorsAction(actors.Cast<SceneObject>().ToArray(), wrapper, -1, true);
                if (!parentActors.TryDo())
                    throw new InvalidOperationException("Failed to attach Actors to the wrapper. " + parentActors.LastResult?.Message);

                var selectWrapper = new SelectionChangeAction(selectionBefore, new SceneGraphNode[] { wrapperNode }, OnSelectionUndo);
                selectWrapper.Do();
                Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createWrapper, parentActors, selectWrapper }, $"Wrap in {defaultName}"));
                wrapperNode.TreeNode.StartRenaming(Editor.Windows.SceneWin, Editor.Windows.SceneWin.SceneTreePanel);
            }
            catch
            {
                parentActors?.TryUndo();
                createWrapper?.TryUndo();
                Selection.Clear();
                Selection.AddRange(selectionBefore.Where(x => x != null));
                OnSelectionChanged();
                if (wrapper)
                    FlaxEngine.Object.Destroy(ref wrapper);
                FlaxEngine.Scripting.FlushRemovedObjects();
                throw;
            }
        }

        private void SelectAndRenameGroup(GroupActor group)
        {
            Select(group);
            var node = Editor.Scene.GetActorNode(group);
            if (node != null)
                node.TreeNode.StartRenaming(Editor.Windows.SceneWin, Editor.Windows.SceneWin.SceneTreePanel);
        }

        public static Actor FindLowestCommonActorParent(List<Actor> actors)
        {
            for (var candidate = actors[0].Parent; candidate != null; candidate = candidate.Parent)
            {
                bool containsAll = true;
                for (int i = 1; i < actors.Count && containsAll; i++)
                {
                    var parent = actors[i].Parent;
                    while (parent != null && parent != candidate)
                        parent = parent.Parent;
                    containsAll = parent == candidate;
                }
                if (containsAll)
                    return candidate;
            }
            return null;
        }

        /// <summary>
        /// Duplicates the selected objects. Supports undo/redo.
        /// </summary>
        public void Duplicate()
        {
            if (TryDuplicateForTransform(out _, out var undoAction))
            {
                Undo.AddAction(undoAction);
                // Scroll to new selected node while duplicating.
                Editor.Windows.SceneWin.ScrollToSelectedNode();
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
            var nodes = Selection.Where(x => x.CanDuplicate).ToList().BuildAllNodes();
            if (nodes.Count == 0)
                return false;
            var actors = new List<Actor>();
            var newSelection = new List<SceneGraphNode>();
            List<IUndoAction> customUndoActions = null;
            try
            {
                foreach (var node in nodes)
                {
                    if (node.CanDuplicate)
                    {
                        if (node is ActorNode actorNode)
                        {
                            actors.Add(actorNode.Actor);
                        }
                        else
                        {
                            var customDuplicatedObject = node.Duplicate(out var customUndoAction);
                            if (customDuplicatedObject != null)
                                newSelection.Add(customDuplicatedObject);
                            if (customUndoAction != null)
                            {
                                if (customUndoActions == null)
                                    customUndoActions = new List<IUndoAction>();
                                customUndoActions.Add(customUndoAction);
                            }
                        }
                    }
                }
            }
            catch
            {
                RollbackDuplicateActions(customUndoActions);
                throw;
            }
            if (actors.Count == 0)
            {
                // Duplicate custom scene graph nodes only without actors
                if (newSelection.Count != 0)
                {
                    // Select spawned objects (parents only)
                    var selectAction = new SelectionChangeAction(Selection.ToArray(), newSelection.ToArray(), OnSelectionUndo);
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
                        catch (Exception ex)
                        {
                            Editor.LogError("Custom duplicate selection rollback failed. " + ex.Message);
                        }
                        RollbackDuplicateActions(customUndoActions);
                        throw;
                    }

                    // Build a single compound undo action that pastes the actors, pastes custom stuff (scene graph extension) and selects the created objects (parents only)
                    var customUndoActionsCount = customUndoActions?.Count ?? 0;
                    var undoActions = new IUndoAction[1 + customUndoActionsCount];
                    for (int i = 0; i < customUndoActionsCount; i++)
                        undoActions[i] = customUndoActions[i];
                    undoActions[undoActions.Length - 1] = selectAction;

                    undoAction = new MultiUndoAction(undoActions);
                    createdObjects.AddRange(newSelection);
                    try
                    {
                        OnSelectionChanged();
                    }
                    catch
                    {
                        try
                        {
                            undoAction.Undo();
                        }
                        catch (Exception ex)
                        {
                            Editor.LogError("Custom duplicate action rollback failed. " + ex.Message);
                        }
                        undoAction.Dispose();
                        undoAction = null;
                        createdObjects.Clear();
                        throw;
                    }
                }
                else
                {
                    RollbackDuplicateActions(customUndoActions);
                }
                return createdObjects.Count != 0;
            }

            // Serialize actors
            var data = Actor.ToBytes(actors.ToArray());
            if (data == null)
            {
                Editor.LogError("Failed to copy actors data.");
                RollbackDuplicateActions(customUndoActions);
                return false;
            }

            // Create paste action (with selecting spawned objects)
            var pasteAction = PasteActorsAction.Duplicate(data, Guid.Empty);
            if (pasteAction != null)
            {
                List<ActorNode> nodeParents;
                try
                {
                    pasteAction.Do(out _, out nodeParents);
                }
                catch (Exception ex)
                {
                    try
                    {
                        pasteAction.Undo();
                    }
                    catch (Exception rollbackException)
                    {
                        Editor.LogError("Actor duplicate rollback failed. " + rollbackException.Message);
                    }
                    finally
                    {
                        pasteAction.Dispose();
                    }
                    RollbackDuplicateActions(customUndoActions);
                    throw new InvalidOperationException("Actor duplication failed.", ex);
                }
                if (nodeParents == null || nodeParents.Count == 0)
                {
                    try
                    {
                        pasteAction.Undo();
                    }
                    catch (Exception ex)
                    {
                        Editor.LogError("Empty actor duplicate rollback failed. " + ex.Message);
                    }
                    finally
                    {
                        pasteAction.Dispose();
                    }
                    RollbackDuplicateActions(customUndoActions);
                    return false;
                }

                // Select spawned objects (parents only)
                newSelection.Clear();
                newSelection.AddRange(nodeParents);
                var selectAction = new SelectionChangeAction(Selection.ToArray(), newSelection.ToArray(), OnSelectionUndo);
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
                    catch (Exception ex)
                    {
                        Editor.LogError("Actor duplicate selection rollback failed. " + ex.Message);
                    }
                    try
                    {
                        pasteAction.Undo();
                    }
                    catch (Exception ex)
                    {
                        Editor.LogError("Actor duplicate rollback failed. " + ex.Message);
                    }
                    pasteAction.Dispose();
                    RollbackDuplicateActions(customUndoActions);
                    throw;
                }

                // Build a single compound undo action that pastes the actors, pastes custom stuff (scene graph extension) and selects the created objects (parents only)
                var customUndoActionsCount = customUndoActions?.Count ?? 0;
                var undoActions = new IUndoAction[2 + customUndoActionsCount];
                undoActions[0] = pasteAction;
                for (int i = 0; i < customUndoActionsCount; i++)
                    undoActions[i + 1] = customUndoActions[i];
                undoActions[undoActions.Length - 1] = selectAction;

                undoAction = new MultiUndoAction(undoActions);
                createdObjects.AddRange(newSelection);
                try
                {
                    OnSelectionChanged();
                }
                catch
                {
                    try
                    {
                        undoAction.Undo();
                    }
                    catch (Exception ex)
                    {
                        Editor.LogError("Actor duplicate action rollback failed. " + ex.Message);
                    }
                    undoAction.Dispose();
                    undoAction = null;
                    createdObjects.Clear();
                    throw;
                }
            }
            else
            {
                RollbackDuplicateActions(customUndoActions);
            }

            return createdObjects.Count != 0;
        }

        private static void RollbackDuplicateActions(List<IUndoAction> actions)
        {
            if (actions == null)
                return;
            for (int i = actions.Count - 1; i >= 0; i--)
            {
                var action = actions[i];
                try
                {
                    action?.Undo();
                }
                catch (Exception ex)
                {
                    Editor.LogError("Custom duplicate rollback failed. " + ex.Message);
                }
                finally
                {
                    action?.Dispose();
                }
            }
        }

        /// <summary>
        /// Called when selection gets changed. Invokes the other events and updates editor. Call it when you manually modify selected objects collection.
        /// </summary>
        public void OnSelectionChanged()
        {
            SelectionChanged?.Invoke();
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            // Deselect actors on remove (and actor child nodes)
            Editor.Scene.ActorRemoved += Deselect;
            Editor.Scene.Root.ActorChildNodesDispose += OnActorChildNodesDispose;
        }

        private void OnActorChildNodesDispose(ActorNode node)
        {
            if (Selection.Count == 0)
                return;

            // TODO: cache if selection contains any actor child node and skip this loop if no need to iterate
            // TODO: or build a hash set with selected nodes for quick O(1) checks (cached until selection changes)

            // Deselect child nodes
            for (int i = 0; i < node.ChildNodes.Count; i++)
            {
                if (Selection.Contains(node.ChildNodes[i]))
                {
                    Deselect();
                    return;
                }
            }
        }
    }
}
