// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Documents;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="Material"/> asset proxy object.
    /// </summary>
    [ContentContextMenu("New/Material/Material")]
    public class MaterialProxy : MaterialBaseProxy
    {
        /// <inheritdoc />
        public override string Name => "Material";

        /// <inheritdoc />
        public override string FileExtension => "material";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".material", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new MaterialWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x16a085);

        /// <inheritdoc />
        public override Type AssetType => typeof(Material);

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (AssetDocumentRegistry.CreateGraph(outputPath, typeof(Material).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }

    }
}
