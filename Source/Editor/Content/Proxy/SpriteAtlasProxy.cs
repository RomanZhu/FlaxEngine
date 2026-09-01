// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="SpriteAtlas"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    public sealed class SpriteAtlasProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Sprite Atlas";

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new SpriteAtlasWindow(editor, (AssetItem)item);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x5C7F69);

        /// <inheritdoc />
        public override Type AssetType => typeof(SpriteAtlas);

    }
}
