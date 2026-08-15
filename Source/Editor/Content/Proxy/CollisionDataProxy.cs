// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using FlaxEditor.Content.Create;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Implementation of <see cref="BinaryAssetItem"/> for <see cref="CollisionData"/> assets.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetItem" />
    class CollisionDataItem : BinaryAssetItem
    {
        /// <inheritdoc />
        public CollisionDataItem(string path, ref Guid id, string typeName, Type type)
        : base(path, ref id, typeName, type, ContentItemSearchFilter.Other)
        {
        }

        /// <inheritdoc />
        public override bool OnEditorDrag(object context)
        {
            return true;
        }

        /// <inheritdoc />
        public override Actor OnEditorDrop(object context)
        {
            return new MeshCollider { CollisionData = FlaxEngine.Content.LoadAsync<CollisionData>(ID) };
        }
    }

    /// <summary>
    /// A <see cref="CollisionData"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Physics/Collision Data")]
    class CollisionDataProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Collision Data";

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new CollisionDataWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            return new CollisionDataItem(path, ref id, typeName, AssetType);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x2c3e50);

        /// <inheritdoc />
        public override Type AssetType => typeof(CollisionData);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            menu.AddButton("Recook all collision data", RecookAllCollisionData);
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (Editor.CreateAsset("CollisionData", outputPath))
                throw new Exception("Failed to create new asset.");
        }

        private sealed class RecookCollisionDataEntry : CreateFileEntry
        {
            public CollisionDataType Type;
            public ModelBase Model;
            public int ModelLodIndex;
            public uint MaterialSlotsMask;
            public ConvexMeshGenerationFlags ConvexFlags;
            public int ConvexVertexLimit;

            public RecookCollisionDataEntry(string resultUrl)
            : base("CollisionData", resultUrl)
            {
            }

            public override bool CanBeCreated => Type != CollisionDataType.None && Model;

            public override bool Create()
            {
                if (!CanBeCreated)
                    return true;
                return Editor.CookMeshCollision(ResultUrl, Type, Model, ModelLodIndex, MaterialSlotsMask, ConvexFlags, ConvexVertexLimit);
            }
        }

        private static void CollectCollisionData(ContentFolder folder, List<BinaryAssetItem> result)
        {
            if (folder == null)
                return;

            foreach (var child in folder.Children)
            {
                if (child is BinaryAssetItem asset && asset.IsOfType<CollisionData>())
                    result.Add(asset);
                else if (child is ContentFolder childFolder)
                    CollectCollisionData(childFolder, result);
            }
        }

        private static ModelBase ResolveCollisionModel(BinaryAssetItem item, Guid modelId)
        {
            if (modelId != Guid.Empty)
                return FlaxEngine.Content.LoadAsync<ModelBase>(modelId);

            const string suffix = " Collision.flax";
            if (!item.Path.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                return null;

            var modelPath = item.Path.Substring(0, item.Path.Length - suffix.Length) + ".flax";
            return FlaxEngine.Content.LoadAsync<ModelBase>(modelPath);
        }

        internal static void RecookAllCollisionData()
        {
            var items = new List<BinaryAssetItem>();
            foreach (var project in Editor.Instance.ContentDatabase.Projects)
                CollectCollisionData(project.Content?.Folder, items);

            int queued = 0;
            int skipped = 0;
            foreach (var item in items)
            {
                if (!Editor.GetCollisionDataOptions(item.Path, out var type, out var modelId, out var modelLodIndex, out var materialSlotsMask, out var convexFlags, out var convexVertexLimit))
                {
                    skipped++;
                    Editor.LogWarning("Failed to read collision data options for " + item.Path);
                    continue;
                }
                if (type == CollisionDataType.None)
                {
                    skipped++;
                    continue;
                }

                var model = ResolveCollisionModel(item, modelId);
                if (!model)
                {
                    skipped++;
                    Editor.LogWarning("Missing source model for collision data " + item.Path);
                    continue;
                }

                Editor.Instance.ContentImporting.Create(new RecookCollisionDataEntry(item.Path)
                {
                    Type = type,
                    Model = model,
                    ModelLodIndex = modelLodIndex,
                    MaterialSlotsMask = materialSlotsMask,
                    ConvexFlags = convexFlags,
                    ConvexVertexLimit = convexVertexLimit,
                });
                queued++;
            }

            Editor.Instance.UI.AddStatusMessage(queued != 0 ? string.Format("Queued recook for {0} collision data asset{1}.", queued, queued == 1 ? string.Empty : "s") : "No collision data assets queued for recook.");
            if (skipped != 0)
                Editor.LogWarning(string.Format("Skipped {0} collision data asset{1} during recook.", skipped, skipped == 1 ? string.Empty : "s"));
        }

        private bool TryUseCollisionData(Model model, BinaryAssetItem assetItem, Action<CollisionData> created, bool alwaysDeferCallback, CollisionDataType type)
        {
            var collisionData = FlaxEngine.Content.Load<CollisionData>(assetItem.ID);
            if (collisionData)
            {
                var options = collisionData.Options;
                if ((options.Model == model.ID || options.Model == Guid.Empty) && options.Type == type)
                {
                    Editor.Instance.Windows.ContentWin.Select(assetItem);
                    if (created != null && alwaysDeferCallback)
                        FlaxEngine.Scripting.InvokeOnUpdate(() => created(collisionData));
                    else if (created != null)
                        created(collisionData);
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// Create collision data from model.
        /// </summary>
        /// <param name="model">The associated model.</param>
        /// <param name="created">The action to call once the collision data gets created (or reused from existing).</param>
        /// <param name="withRenaming">True if start initial item renaming by user, or tru to skip it.</param>
        /// <param name="alwaysDeferCallback">True if always call <paramref name="created"/> callback on the next engine update, otherwise callback might be called within this function if collision data already exists.</param>
        /// <param name="type">Type of the collider to create.</param>
        public void CreateCollisionDataFromModel(Model model, Action<CollisionData> created = null, bool withRenaming = true, bool alwaysDeferCallback = true, CollisionDataType type = CollisionDataType.ConvexMesh)
        {
            // Check if there already is collision data for that model to reuse
            var modelItem = (AssetItem)Editor.Instance.ContentDatabase.Find(model.ID);
            if (modelItem?.ParentFolder != null)
            {
                foreach (var child in modelItem.ParentFolder.Children)
                {
                    // Check if there is collision that was made with this model
                    if (child is BinaryAssetItem b && b.IsOfType<CollisionData>())
                    {
                        if (TryUseCollisionData(model, b, created, alwaysDeferCallback, type))
                            return;
                    }

                    // Check if there is an auto-imported collision
                    if (child is ContentFolder childFolder && childFolder.ShortName == modelItem.ShortName)
                    {
                        foreach (var childFolderChild in childFolder.Children)
                        {
                            if (childFolderChild is BinaryAssetItem c && c.IsOfType<CollisionData>())
                            {
                                if (TryUseCollisionData(model, c, created, alwaysDeferCallback, type))
                                    return;
                            }
                        }
                    }
                }
            }

            // Create new item so user can name it and then generate collision for it in async
            Action<ContentItem> create = contentItem =>
            {
                var assetItem = (AssetItem)contentItem;
                var collisionData = FlaxEngine.Content.LoadAsync<CollisionData>(assetItem.ID);
                if (collisionData == null || collisionData.WaitForLoaded())
                {
                    Editor.LogError("Failed to load created collision data.");
                    return;
                }
                Task.Run(() =>
                {
                    Editor.Instance.ContentDatabase.SaveAsset(assetItem.Path, () => Editor.CookMeshCollision(assetItem.Path, type, model));
                    if (created != null)
                        FlaxEngine.Scripting.InvokeOnUpdate(() => created(collisionData));
                });
            };
            var initialName = (modelItem?.ShortName ?? Path.GetFileNameWithoutExtension(model.Path)) + " Collision";
            Editor.Instance.Windows.ContentWin.NewItem(this, null, create, initialName, withRenaming);
        }
    }
}
