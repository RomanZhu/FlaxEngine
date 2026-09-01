// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Content.Documents;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="BehaviorTree"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/AI/Behavior Tree")]
    public class BehaviorTreeProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Behavior Tree";

        /// <inheritdoc />
        public override string FileExtension => "behaviortree";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = Path.GetExtension(path);
            return string.Equals(extension, ".behaviortree", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new BehaviorTreeWindow(editor, item as BinaryAssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x3256A8);

        /// <inheritdoc />
        public override Type AssetType => typeof(BehaviorTree);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (AssetDocumentRegistry.CreateGraph(outputPath, typeof(BehaviorTree).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }

    }
}
