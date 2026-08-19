// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Threading.Tasks;
using FlaxEditor.Content;
using FlaxEngine;

namespace FlaxEditor.Tools
{
    /// <summary>
    /// Utility for baking actor transforms (such as non-uniform scale) into separate Model assets with fresh SDF data.
    /// </summary>
    [HideInEditor]
    public static class ModelTransformBaker
    {
        /// <summary>
        /// Bakes the scale of a <see cref="StaticModel"/> actor into a new standalone <see cref="Model"/> asset and re-links the actor.
        /// </summary>
        /// <param name="actor">The static model actor to bake.</param>
        /// <param name="customOutputPath">Optional custom output path. If null, defaults to Content/SceneData/{SceneName}/Models/Bakes/{OriginalModel}_{ActorName}_{GUID}.flax.</param>
        public static void BakeScale(StaticModel actor, string customOutputPath = null)
        {
            if (actor == null)
                return;

            var srcModel = actor.Model;
            if (!srcModel || srcModel.WaitForLoaded())
            {
                Debug.LogError($"Cannot bake model for actor '{actor.Name}': Model is missing or failed to load.");
                return;
            }

            var scale = (Float3)actor.Transform.Scale;
            var scene = actor.Scene;
            string sceneName = scene != null && !string.IsNullOrEmpty(scene.Name) ? scene.Name : "Default";

            // Capture active materials and actor entries so materials are preserved in both the new asset and the actor
            var srcSlots = srcModel.MaterialSlots;
            int slotCount = srcSlots != null ? srcSlots.Length : 0;
            var activeMaterials = new MaterialBase[slotCount];
            for (int i = 0; i < slotCount; i++)
            {
                activeMaterials[i] = actor.GetMaterial(i) ?? srcSlots[i].Material;
            }
            var actorEntries = actor.Entries;

            // Determine output directory & file path
            string outputPath = customOutputPath;
            if (string.IsNullOrEmpty(outputPath))
            {
                string dirPath = StringUtils.CombinePaths(Globals.ProjectContentFolder, "SceneData", sceneName + "/Models/Bakes");
                Directory.CreateDirectory(dirPath);

                var modelItem = Editor.Instance.ContentDatabase.Find(srcModel.ID) as AssetItem;
                string originalName = modelItem != null ? modelItem.ShortName : (!string.IsNullOrEmpty(srcModel.Path) ? Path.GetFileNameWithoutExtension(srcModel.Path) : "Model");
                string safeActorName = string.Join("_", actor.Name.Split(Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
                string shortGuid = Guid.NewGuid().ToString("N").Substring(0, 6);
                string fileName = $"{originalName}_{safeActorName}_{shortGuid}.flax";
                outputPath = StringUtils.CombinePaths(dirPath, fileName);
            }

            // Perform baking and saving on background task because downloading mesh data and generating SDF cannot run on the main thread
            Task.Run(() =>
            {
                try
                {
                    // Create virtual model asset
                    var newModel = FlaxEngine.Content.CreateVirtualAsset<Model>();
                    if (newModel == null)
                    {
                        Debug.LogError("Failed to create virtual Model asset.");
                        return;
                    }

                    // Setup LODs
                    int lodCount = srcModel.LODs.Length;
                    int[] meshesCountPerLod = new int[lodCount];
                    for (int i = 0; i < lodCount; i++)
                        meshesCountPerLod[i] = srcModel.LODs[i].Meshes.Length;
                    if (newModel.SetupLODs(meshesCountPerLod))
                    {
                        FlaxEngine.Object.Destroy(newModel);
                        Debug.LogError("Failed to setup LODs for baked Model.");
                        return;
                    }

                    // Setup Material Slots with active materials from actor/source
                    if (slotCount > 0)
                    {
                        newModel.SetupMaterialSlots(slotCount);
                        for (int i = 0; i < slotCount; i++)
                        {
                            var srcSlot = srcSlots[i];
                            var dstSlot = newModel.MaterialSlots[i];
                            dstSlot.Name = srcSlot.Name;
                            dstSlot.ShadowsMode = srcSlot.ShadowsMode;
                            dstSlot.Material = activeMaterials[i];
                        }
                    }

                    // Inverse scale for normal transformation (normals transform by inverse-transpose matrix)
                    Float3 invScale = new Float3(
                        Mathf.Abs(scale.X) > 1e-6f ? 1.0f / scale.X : 1.0f,
                        Mathf.Abs(scale.Y) > 1e-6f ? 1.0f / scale.Y : 1.0f,
                        Mathf.Abs(scale.Z) > 1e-6f ? 1.0f / scale.Z : 1.0f
                    );

                    bool isFlipped = (scale.X * scale.Y * scale.Z) < 0;

                    // Extract and transform meshes
                    for (int lodIndex = 0; lodIndex < lodCount; lodIndex++)
                    {
                        var srcLod = srcModel.LODs[lodIndex];
                        var dstLod = newModel.LODs[lodIndex];
                        dstLod.ScreenSize = srcLod.ScreenSize;

                        for (int meshIndex = 0; meshIndex < srcLod.Meshes.Length; meshIndex++)
                        {
                            var srcMesh = srcLod.Meshes[meshIndex];
                            var dstMesh = dstLod.Meshes[meshIndex];
                            dstMesh.MaterialSlotIndex = srcMesh.MaterialSlotIndex;

                            var accessor = new MeshAccessor();
                            if (accessor.LoadMesh(srcMesh))
                            {
                                FlaxEngine.Object.Destroy(newModel);
                                Debug.LogError($"Failed to load mesh data for '{srcModel.Path}'.");
                                return;
                            }

                            var positions = accessor.Positions;
                            var triangles = accessor.Triangles;
                            var normals = accessor.Normals;
                            var tangents = accessor.Tangents;
                            var uvs = accessor.TexCoords;
                            var colors = accessor.Colors;

                            if (positions == null || triangles == null)
                            {
                                FlaxEngine.Object.Destroy(newModel);
                                Debug.LogError($"Mesh data missing positions or triangles in '{srcModel.Path}'.");
                                return;
                            }

                            // Scale vertex positions
                            for (int v = 0; v < positions.Length; v++)
                            {
                                positions[v] = positions[v] * scale;
                            }

                            // Transform normals
                            if (normals != null)
                            {
                                for (int v = 0; v < normals.Length; v++)
                                {
                                    normals[v] = Float3.Normalize(normals[v] * invScale);
                                }
                            }

                            // Transform tangents
                            if (tangents != null)
                            {
                                for (int v = 0; v < tangents.Length; v++)
                                {
                                    tangents[v] = Float3.Normalize(tangents[v] * scale);
                                }
                            }

                            // Flip triangle winding if scale determinant is negative (reflection)
                            if (isFlipped)
                            {
                                for (int t = 0; t < triangles.Length; t += 3)
                                {
                                    var tmp = triangles[t + 1];
                                    triangles[t + 1] = triangles[t + 2];
                                    triangles[t + 2] = tmp;
                                }
                            }

                            Color32[] colors32 = null;
                            if (colors != null && colors.Length > 0)
                            {
                                colors32 = new Color32[colors.Length];
                                for (int v = 0; v < colors.Length; v++)
                                    colors32[v] = (Color32)colors[v];
                            }

                            dstMesh.UpdateMesh(positions, triangles, normals, tangents, uvs, colors32);
                        }
                    }

                    // Save virtual model to disk (withMeshDataFromGpu must be true for virtual models)
                    if (newModel.Save(true, outputPath))
                    {
                        FlaxEngine.Object.Destroy(newModel);
                        Debug.LogError($"Failed to save baked model to '{outputPath}'.");
                        return;
                    }
                    FlaxEngine.Object.Destroy(newModel);

                    // Load the saved persistent asset
                    var savedModel = FlaxEngine.Content.Load<Model>(outputPath);
                    if (!savedModel || savedModel.WaitForLoaded())
                    {
                        Debug.LogError($"Failed to load saved model from '{outputPath}'.");
                        return;
                    }

                    // Generate SDF
                    try
                    {
                        bool sdfFailed = savedModel.GenerateSDF(1.0f, 6, true, 0.6f, true);
                        if (!sdfFailed)
                        {
                            savedModel.Save();
                        }
                    }
                    catch (Exception ex)
                    {
                        Debug.LogWarning($"Failed to generate SDF for baked model '{outputPath}': {ex.Message}");
                    }

                    // Switch back to the main thread to update the editor state and actor
                    FlaxEngine.Scripting.InvokeOnUpdate(() =>
                    {
                        // Refresh content database so the new asset is registered in the UI
                        string dirPath = Path.GetDirectoryName(outputPath);
                        var folder = Editor.Instance.ContentDatabase.Find(dirPath);
                        if (folder == null)
                        {
                            string current = dirPath;
                            while (!string.IsNullOrEmpty(current) && folder == null)
                            {
                                folder = Editor.Instance.ContentDatabase.Find(current);
                                if (folder == null)
                                    current = Path.GetDirectoryName(current);
                            }
                        }
                        if (folder != null)
                            Editor.Instance.ContentDatabase.RefreshFolder(folder, true);

                        // Apply new model to actor, restore materials, and reset scale with Undo
                        if (actor)
                        {
                            using (new UndoBlock(Editor.Instance.Undo, actor, "Bake Scale to New Model"))
                            {
                                actor.Model = savedModel;
                                if (actorEntries != null && actorEntries.Length > 0)
                                {
                                    actor.Entries = (ModelInstanceEntry[])actorEntries.Clone();
                                }
                                for (int i = 0; i < activeMaterials.Length; i++)
                                {
                                    if (activeMaterials[i] != null)
                                    {
                                        actor.SetMaterial(i, activeMaterials[i]);
                                    }
                                }
                                actor.Scale = Vector3.One;
                            }
                            Debug.Log($"Successfully baked scale for actor '{actor.Name}' into model '{outputPath}'.");
                        }
                    });
                }
                catch (Exception ex)
                {
                    Debug.LogException(ex);
                }
            });
        }
    }
}
