// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A base class for <see cref="MaterialBase"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    public abstract class MaterialBaseProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            if (item is BinaryAssetItem binaryAssetItem)
            {
                var button = menu.AddButton("Create Material Instance", CreateMaterialInstanceClicked);
                button.Tag = binaryAssetItem;
            }
        }

        private void CreateMaterialInstanceClicked(ContextMenuButton button)
        {
            var binaryAssetItem = (BinaryAssetItem)button.Tag;
            CreateMaterialInstance(binaryAssetItem);
        }

        /// <summary>
        /// Creates the material instance from the given material.
        /// </summary>
        /// <param name="materialItem">The material item to use as a base material.</param>
        public static void CreateMaterialInstance(BinaryAssetItem materialItem)
        {
            var materialInstanceName = materialItem.ShortName + " Instance";
            var materialInstanceProxy = Editor.Instance.ContentDatabase.GetProxy<MaterialInstance>();
            Editor.Instance.Windows.ContentWin.NewItem(materialInstanceProxy, null, item => OnMaterialInstanceCreated(item, materialItem), materialInstanceName);
        }

        private static void OnMaterialInstanceCreated(ContentItem item, BinaryAssetItem materialItem)
        {
            var assetItem = (AssetItem)item;
            var materialInstance = FlaxEngine.Content.LoadAssetAsync<MaterialInstance>(assetItem.ObjectID);
            if (materialInstance == null || materialInstance.WaitForLoaded())
            {
                Editor.LogError("Failed to load created material instance.");
                return;
            }
            materialInstance.BaseMaterial = FlaxEngine.Content.LoadAssetAsync<MaterialBase>(materialItem.ObjectID);
            Editor.Instance.ContentDatabase.SaveAsset(materialInstance);
        }

    }
}
