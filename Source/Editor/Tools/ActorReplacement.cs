// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Actions;
using FlaxEditor.SceneGraph;
using FlaxEngine;

namespace FlaxEditor.Tools
{
    /// <summary>
    /// Configuration options for replacing actors with a prefab.
    /// </summary>
    [HideInEditor]
    public class ReplaceOptions
    {
        /// <summary>
        /// True to apply the position of the original actor to the replacement prefab instance.
        /// </summary>
        [EditorOrder(10), EditorDisplay("Transform", "Apply Position"), Tooltip("Apply world/local position from the original actor to the prefab instance.")]
        public bool ApplyPosition = true;

        /// <summary>
        /// True to apply the rotation of the original actor to the replacement prefab instance.
        /// </summary>
        [EditorOrder(20), EditorDisplay("Transform", "Apply Rotation"), Tooltip("Apply rotation from the original actor to the prefab instance.")]
        public bool ApplyRotation = true;

        /// <summary>
        /// True to apply the scale of the original actor to the replacement prefab instance.
        /// </summary>
        [EditorOrder(30), EditorDisplay("Transform", "Apply Scale"), Tooltip("Apply scale from the original actor to the prefab instance.")]
        public bool ApplyScale = true;

        /// <summary>
        /// True to transfer material references and overrides from the original actor hierarchy to matching prefab hierarchy.
        /// </summary>
        [EditorOrder(40), EditorDisplay("Materials", "Transfer Materials"), Tooltip("Transfer material references and overrides if the prefab hierarchy matches the original actor.")]
        public bool TransferMaterials = true;

        /// <summary>
        /// True to transfer attached scripts/components from the original actor and matching child actors to the new prefab instance.
        /// </summary>
        [EditorOrder(50), EditorDisplay("Scripts", "Transfer Scripts"), Tooltip("Transfer attached scripts/components from the original actor and matching child actors to the new prefab instance.")]
        public bool TransferScripts = true;

        /// <summary>
        /// True to keep original actor's child actors and re-parent them under the new prefab instance.
        /// </summary>
        [EditorOrder(60), EditorDisplay("Hierarchy", "Keep Children"), Tooltip("Keep original actor's child actors and re-parent them under the new prefab instance.")]
        public bool KeepChildren = false;

        /// <summary>
        /// True to keep the original actor's name instead of using the prefab root name.
        /// </summary>
        [EditorOrder(70), EditorDisplay("Naming", "Keep Name"), Tooltip("Keep the original actor name instead of using the prefab root name.")]
        public bool KeepName = false;
    }

    /// <summary>
    /// Utility for replacing scene actors with a prefab instance.
    /// </summary>
    [HideInEditor]
    public static class ActorReplacement
    {
        private class ModelMaterialData
        {
            public string Path;
            public string Name;
            public MaterialBase[] Materials;
        }

        private class ScriptTransferData
        {
            public string Path;
            public string Name;
            public List<Script> Scripts;
        }

        /// <summary>
        /// Replaces the specified actor nodes with instances of the given prefab according to the provided options.
        /// </summary>
        /// <param name="targetNodes">The actor nodes to replace.</param>
        /// <param name="replacementPrefab">The prefab asset to instantiate.</param>
        /// <param name="options">The replacement options.</param>
        /// <returns>True if replacement succeeded for at least one actor, false otherwise.</returns>
        public static bool Replace(List<ActorNode> targetNodes, Prefab replacementPrefab, ReplaceOptions options)
        {
            if (targetNodes == null || targetNodes.Count == 0 || replacementPrefab == null || options == null)
                return false;

            if (replacementPrefab.WaitForLoaded())
            {
                Debug.LogError($"Failed to load replacement prefab: {replacementPrefab.Path}");
                return false;
            }

            var editor = Editor.Instance;
            var selectionBefore = editor.SceneEditing.Selection.ToArray();
            Action<SceneGraphNode[]> selectionCallback = selection => editor.SceneEditing.Select(selection);
            
            // Capture selectionBefore while all original nodes and actors are alive and intact
            var clearSelection = new SelectionChangeAction(selectionBefore, Array.Empty<SceneGraphNode>(), selectionCallback);

            var newCreatedNodes = new List<SceneGraphNode>();
            var undoActions = new List<IUndoAction>();

            try
            {
                foreach (var oldNode in targetNodes)
                {
                    if (oldNode == null || !oldNode.Actor || oldNode.Actor is Scene)
                        continue;

                    var oldActor = oldNode.Actor;
                    var parent = oldActor.Parent;
                    var scene = oldActor.Scene;
                    if (parent == null || scene == null || Level.FindScene(scene.ID) != scene)
                        continue;

                    int orderInParent = oldActor.OrderInParent;
                    var oldTransform = oldActor.Transform;
                    var layer = oldActor.Layer;
                    var tags = oldActor.Tags;
                    var staticFlags = oldActor.StaticFlags;
                    var name = oldActor.Name;
                    var isActive = oldActor.IsActive;

                    // Capture materials before modifying actor
                    List<ModelMaterialData> cachedMaterials = null;
                    if (options.TransferMaterials)
                    {
                        cachedMaterials = CaptureMaterials(oldActor);
                    }

                    // Capture scripts before modifying actor
                    List<ScriptTransferData> cachedScripts = null;
                    if (options.TransferScripts)
                    {
                        cachedScripts = CaptureScripts(oldActor);
                    }

                    // Capture children if requested
                    var children = options.KeepChildren ? oldActor.Children : null;

                    // Prepare delete undo action while old hierarchy is completely intact
                    var deleteOld = new DeleteActorsAction(oldNode.BuildAllNodes().Where(x => x != null && x.CanDelete).ToList());

                    // Spawn replacement prefab instance under the parent
                    var newActor = PrefabManager.SpawnPrefab(replacementPrefab, parent);
                    if (newActor == null)
                    {
                        Debug.LogError($"Failed to spawn prefab instance for '{replacementPrefab.Path}'.");
                        continue;
                    }

                    // Apply transform
                    var newTransform = newActor.Transform;
                    if (options.ApplyPosition)
                        newTransform.Translation = oldTransform.Translation;
                    if (options.ApplyRotation)
                        newTransform.Orientation = oldTransform.Orientation;
                    if (options.ApplyScale)
                        newTransform.Scale = oldTransform.Scale;

                    newActor.Transform = newTransform;
                    newActor.OrderInParent = orderInParent;
                    newActor.Layer = layer;
                    newActor.Tags = tags;
                    newActor.StaticFlags = staticFlags;
                    newActor.IsActive = isActive;

                    if (options.KeepName && !string.IsNullOrEmpty(name))
                        newActor.Name = name;

                    // Apply materials
                    if (options.TransferMaterials && cachedMaterials != null)
                    {
                        ApplyCachedMaterials(cachedMaterials, newActor);
                    }

                    // Transfer scripts (hierarchy-aware) while live
                    if (options.TransferScripts && cachedScripts != null)
                    {
                        ApplyTransferredScripts(cachedScripts, newActor);
                    }

                    // Keep children
                    if (options.KeepChildren && children != null)
                    {
                        for (int i = children.Length - 1; i >= 0; i--)
                        {
                            if (children[i])
                                children[i].Parent = newActor;
                        }
                    }

                    var newActorNode = editor.Scene.GetActorNode(newActor);
                    if (newActorNode != null)
                    {
                        newActorNode.PostSpawn();
                        newCreatedNodes.Add(newActorNode);

                        // Execute deletion of old actor now that graph is transferred
                        if (!deleteOld.TryDo())
                        {
                            Debug.LogError($"Failed to remove old actor '{name}'.");
                            continue;
                        }

                        var createNew = new DeleteActorsAction(newActorNode.BuildAllNodes().Where(x => x != null && x.CanDelete).ToList(), true);
                        undoActions.Add(deleteOld);
                        undoActions.Add(createNew);
                    }
                }

                if (undoActions.Count > 0)
                {
                    clearSelection.Do();
                    var selectNew = new SelectionChangeAction(Array.Empty<SceneGraphNode>(), newCreatedNodes.ToArray(), selectionCallback);
                    selectNew.Do();

                    var multiUndo = new List<IUndoAction> { clearSelection };
                    multiUndo.AddRange(undoActions);
                    multiUndo.Add(selectNew);

                    editor.Undo.AddAction(new MultiUndoAction(multiUndo, "Replace with Prefab"));
                    foreach (var node in newCreatedNodes)
                    {
                        if (node is ActorNode an && an.Actor && an.Actor.Scene)
                        {
                            editor.Scene.MarkSceneEdited(an.Actor.Scene);
                        }
                    }
                    return true;
                }

                return false;
            }
            catch (Exception ex)
            {
                Debug.LogException(ex);
                return false;
            }
        }

        internal static void TransferMaterials(Actor oldActor, Actor newActor)
        {
            if (oldActor == null || newActor == null)
                return;

            var cached = CaptureMaterials(oldActor);
            ApplyCachedMaterials(cached, newActor);
        }

        private static List<ModelMaterialData> CaptureMaterials(Actor actor)
        {
            var list = new List<ModelMaterialData>();
            if (actor == null)
                return list;

            CollectModelMaterials(actor, "", list);
            return list;
        }

        private static void CollectModelMaterials(Actor actor, string currentPath, List<ModelMaterialData> result)
        {
            if (actor == null)
                return;

            string path = string.IsNullOrEmpty(currentPath) ? (actor.Name ?? "") : currentPath + "/" + (actor.Name ?? "");
            if (actor is ModelInstanceActor modelActor)
            {
                var slots = modelActor.MaterialSlots;
                int count = slots != null ? slots.Length : 0;
                var materials = new MaterialBase[count];
                for (int i = 0; i < count; i++)
                {
                    materials[i] = modelActor.GetMaterial(i);
                }
                result.Add(new ModelMaterialData
                {
                    Path = path,
                    Name = actor.Name ?? "",
                    Materials = materials,
                });
            }

            for (int i = 0; i < actor.ChildrenCount; i++)
            {
                CollectModelMaterials(actor.GetChild(i), path, result);
            }
        }

        private static void ApplyCachedMaterials(List<ModelMaterialData> cached, Actor newActor)
        {
            if (cached == null || cached.Count == 0 || newActor == null)
                return;

            var newModels = new List<(string Path, ModelInstanceActor Actor)>();
            CollectNewModels(newActor, "", newModels);

            foreach (var (newPath, newModel) in newModels)
            {
                ModelMaterialData match = cached.FirstOrDefault(x => string.Equals(x.Path, newPath, StringComparison.OrdinalIgnoreCase));
                if (match == null)
                {
                    match = cached.FirstOrDefault(x => string.Equals(x.Name, newModel.Name, StringComparison.OrdinalIgnoreCase));
                }
                if (match == null && cached.Count == 1 && newModels.Count == 1)
                {
                    match = cached[0];
                }

                if (match != null && match.Materials != null)
                {
                    var dstSlots = newModel.MaterialSlots;
                    int count = Math.Min(match.Materials.Length, dstSlots != null ? dstSlots.Length : 0);
                    for (int i = 0; i < count; i++)
                    {
                        if (match.Materials[i] != null)
                        {
                            newModel.SetMaterial(i, match.Materials[i]);
                        }
                    }
                }
            }
        }

        private static void CollectNewModels(Actor actor, string currentPath, List<(string Path, ModelInstanceActor Actor)> result)
        {
            if (actor == null)
                return;

            string path = string.IsNullOrEmpty(currentPath) ? (actor.Name ?? "") : currentPath + "/" + (actor.Name ?? "");
            if (actor is ModelInstanceActor modelActor)
            {
                result.Add((path, modelActor));
            }

            for (int i = 0; i < actor.ChildrenCount; i++)
            {
                CollectNewModels(actor.GetChild(i), path, result);
            }
        }

        private static List<ScriptTransferData> CaptureScripts(Actor actor)
        {
            var list = new List<ScriptTransferData>();
            if (actor == null)
                return list;

            CollectActorScripts(actor, "", list);
            return list;
        }

        private static void CollectActorScripts(Actor actor, string currentPath, List<ScriptTransferData> result)
        {
            if (actor == null)
                return;

            string path = string.IsNullOrEmpty(currentPath) ? (actor.Name ?? "") : currentPath + "/" + (actor.Name ?? "");
            var scripts = actor.Scripts;
            if (scripts != null && scripts.Length > 0)
            {
                var validScripts = new List<Script>(scripts.Length);
                for (int i = 0; i < scripts.Length; i++)
                {
                    if (scripts[i])
                        validScripts.Add(scripts[i]);
                }
                if (validScripts.Count > 0)
                {
                    result.Add(new ScriptTransferData
                    {
                        Path = path,
                        Name = actor.Name ?? "",
                        Scripts = validScripts,
                    });
                }
            }

            for (int i = 0; i < actor.ChildrenCount; i++)
            {
                CollectActorScripts(actor.GetChild(i), path, result);
            }
        }

        private static void ApplyTransferredScripts(List<ScriptTransferData> capturedScripts, Actor newActor)
        {
            if (capturedScripts == null || capturedScripts.Count == 0 || newActor == null)
                return;

            var newActors = new List<(string Path, Actor Actor)>();
            CollectAllActors(newActor, "", newActors);

            foreach (var scriptData in capturedScripts)
            {
                if (scriptData.Scripts == null || scriptData.Scripts.Count == 0)
                    continue;

                // Try exact relative path match
                Actor targetActor = newActors.FirstOrDefault(x => string.Equals(x.Path, scriptData.Path, StringComparison.OrdinalIgnoreCase)).Actor;
                if (targetActor == null)
                {
                    // Try matching by actor name
                    targetActor = newActors.FirstOrDefault(x => string.Equals(x.Actor?.Name, scriptData.Name, StringComparison.OrdinalIgnoreCase)).Actor;
                }
                if (targetActor == null)
                {
                    // Fallback to root newActor
                    targetActor = newActor;
                }

                if (targetActor != null)
                {
                    for (int i = scriptData.Scripts.Count - 1; i >= 0; i--)
                    {
                        var script = scriptData.Scripts[i];
                        if (script)
                        {
                            script.Actor = targetActor;
                        }
                    }
                }
            }
        }

        private static void CollectAllActors(Actor actor, string currentPath, List<(string Path, Actor Actor)> result)
        {
            if (actor == null)
                return;

            string path = string.IsNullOrEmpty(currentPath) ? (actor.Name ?? "") : currentPath + "/" + (actor.Name ?? "");
            result.Add((path, actor));

            for (int i = 0; i < actor.ChildrenCount; i++)
            {
                CollectAllActors(actor.GetChild(i), path, result);
            }
        }
    }
}
