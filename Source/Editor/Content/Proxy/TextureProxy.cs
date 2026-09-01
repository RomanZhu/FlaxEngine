// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="Texture"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    public sealed class TextureProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Texture";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".png", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".tga", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".exr", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new TextureWindow(editor, (AssetItem)item);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x25B84C);

        /// <inheritdoc />
        public override Type AssetType => typeof(Texture);

    }
}
