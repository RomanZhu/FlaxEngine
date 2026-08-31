// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="SkeletonMask"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Animation/Skeleton Mask")]
    public class SkeletonMaskProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Skeleton Mask";

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new SkeletonMaskWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x00B31C);

        /// <inheritdoc />
        public override Type AssetType => typeof(SkeletonMask);

        /// <inheritdoc />
        public override string FileExtension => "skeletonmask";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".skeletonmask", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (AuthoredAssetDocumentService.Create(outputPath, typeof(SkeletonMask).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }
    }
}
