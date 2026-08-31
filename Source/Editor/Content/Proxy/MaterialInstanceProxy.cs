// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Thumbnails;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="MaterialInstance"/> asset proxy object.
    /// </summary>
    [ContentContextMenu("New/Material/Material Instance")]
    public class MaterialInstanceProxy : MaterialBaseProxy
    {
        /// <inheritdoc />
        public override string Name => "Material Instance";

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new MaterialInstanceWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x2c3e50);

        /// <inheritdoc />
        public override Type AssetType => typeof(MaterialInstance);

        /// <inheritdoc />
        public override string FileExtension => "materialinstance";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".materialinstance", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            CanonicalGraphDocuments.EnsureCanAuthor(typeof(MaterialInstance).FullName, outputPath);
            if (AssetDatabaseFacade.CreateAuthoredDocument(outputPath, typeof(MaterialInstance).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }

        /// <inheritdoc />
        public override bool CanDrawThumbnail(ThumbnailRequest request)
        {
            return _preview.HasLoadedAssets && ThumbnailsModule.HasMinimumQuality((MaterialInstance)request.Asset);
        }
    }
}
