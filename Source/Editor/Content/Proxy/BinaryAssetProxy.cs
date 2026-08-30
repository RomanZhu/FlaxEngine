// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Base class for all binary asset proxy objects used to manage <see cref="BinaryAssetItem"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.AssetProxy" />
    public abstract class BinaryAssetProxy : AssetProxy
    {
        /// <summary>
        /// The binary asset files extension.
        /// </summary>
        public static readonly string Extension = "flax";

        /// <inheritdoc />
        public override bool IsProxyFor(ContentItem item)
        {
            return item is BinaryAssetItem binaryAssetItem && TypeName == binaryAssetItem.TypeName;
        }

        /// <inheritdoc />
        public override string FileExtension => Extension;

        /// <inheritdoc />
        public override string TypeName => AssetType.FullName;

        /// <inheritdoc />
        public override bool IsProxyFor<T>()
        {
            return typeof(T) == AssetType;
        }

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            if (item is not BinaryAssetItem assetItem ||
                !string.Equals(Path.GetExtension(item.Path), ".flax", StringComparison.OrdinalIgnoreCase) ||
                !CanConvertToText(assetItem))
                return;

            menu.AddButton("Convert to text form", () => ConvertToText(assetItem));
        }

        private static bool CanConvertToText(BinaryAssetItem item)
        {
            return ConvertedTypePolicy.IsConvertedGraphType(item.TypeName) || item.Type == typeof(Shader);
        }

        private static void ConvertToText(BinaryAssetItem item)
        {
            if (MessageBox.Show(
                    $"Convert '{item.ShortName}' to its text form? The legacy .flax file will be replaced while preserving the asset ID.",
                    "Convert asset to text", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK)
                return;

            var sourcePath = item.Path;
            if (!AssetDatabaseFacade.MigrateLegacyAsset(sourcePath))
            {
                Editor.Log($"Converted asset to text form: {sourcePath}");
                return;
            }

            var message = $"Failed to convert asset to text form: {sourcePath}";
            var diagnostics = AssetDatabaseFacade.GetDiagnostics();
            for (int i = 0; i < diagnostics.Length; i++)
            {
                if (!string.IsNullOrEmpty(diagnostics[i].Message))
                {
                    message += "\n" + diagnostics[i].Message;
                    break;
                }
            }
            Editor.LogError(message);
            MessageBox.Show(message, "Asset conversion failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }

        /// <summary>
        /// Gets the type of the asset.
        /// </summary>
        public abstract Type AssetType { get; }

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            var type = TypeUtils.GetType(typeName).Type;

            if (typeof(TextureBase).IsAssignableFrom(type))
                return new TextureAssetItem(path, ref id, typeName, type);
            if (typeof(Model).IsAssignableFrom(type))
                return new ModelItem(path, ref id, typeName, type);
            if (typeof(SkinnedModel).IsAssignableFrom(type))
                return new SkinnedModeItem(path, ref id, typeName, type);

            ContentItemSearchFilter searchFilter;
            if (typeof(MaterialBase).IsAssignableFrom(type))
                searchFilter = ContentItemSearchFilter.Material;
            else if (typeof(Prefab).IsAssignableFrom(type))
                searchFilter = ContentItemSearchFilter.Prefab;
            else if (typeof(SceneAsset).IsAssignableFrom(type))
                searchFilter = ContentItemSearchFilter.Scene;
            else if (typeof(Animation).IsAssignableFrom(type))
                searchFilter = ContentItemSearchFilter.Animation;
            else if (typeof(ParticleEmitter).IsAssignableFrom(type))
                searchFilter = ContentItemSearchFilter.Particles;
            else
                searchFilter = ContentItemSearchFilter.Other;
            return new BinaryAssetItem(path, ref id, typeName, type, searchFilter);
        }
    }
}
