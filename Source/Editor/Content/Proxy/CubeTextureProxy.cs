// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="CubeTexture"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    public class CubeTextureProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Cube Texture";

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new CubeTextureWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x3498db);

        /// <inheritdoc />
        public override Type AssetType => typeof(CubeTexture);

    }
}
