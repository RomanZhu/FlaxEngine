// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Documents;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="AnimationGraphFunction"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Animation/Animation Graph Function")]
    public class AnimationGraphFunctionProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Animation Graph Function";

        /// <inheritdoc />
        public override string FileExtension => "animgraphfunction";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".animgraphfunction", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new AnimationGraphFunctionWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x991597);

        /// <inheritdoc />
        public override Type AssetType => typeof(AnimationGraphFunction);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (AssetDocumentRegistry.CreateGraph(outputPath, typeof(AnimationGraphFunction).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }
    }
}
