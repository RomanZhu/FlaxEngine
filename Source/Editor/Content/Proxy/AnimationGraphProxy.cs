// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="AnimationGraph"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Animation/Animation Graph")]
    public class AnimationGraphProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Animation Graph";

        /// <inheritdoc />
        public override string FileExtension => CanonicalGraphDocuments.UseTextGraphAssets ? "animgraph" : Extension;

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".animgraph", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new AnimationGraphWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x00B371);

        /// <inheritdoc />
        public override Type AssetType => typeof(AnimationGraph);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (CanonicalGraphDocuments.UseTextGraphAssets)
            {
                if (AssetDatabaseFacade.CreateGraphDocument(outputPath, typeof(AnimationGraph).FullName) == Guid.Empty)
                    throw new Exception("Failed to create new asset.");
                return;
            }
            if (Editor.CreateAsset("AnimationGraph", outputPath))
                throw new Exception("Failed to create new asset.");
        }
    }
}
