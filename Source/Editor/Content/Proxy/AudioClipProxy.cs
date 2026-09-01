// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Text;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Implementation of <see cref="BinaryAssetItem"/> for <see cref="AudioClip"/> assets.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetItem" />
    class AudioClipItem : BinaryAssetItem
    {
        /// <inheritdoc />
        public AudioClipItem(string path, ref Guid id, string typeName, Type type)
        : base(path, ref id, typeName, type, ContentItemSearchFilter.Audio)
        {
        }

        /// <inheritdoc />
        public override bool OnEditorDrag(object context)
        {
            return true;
        }

        /// <inheritdoc />
        public override Actor OnEditorDrop(object context)
        {
            return new AudioSource { Clip = FlaxEngine.Content.LoadAssetAsync<AudioClip>(ObjectID) };
        }

        /// <inheritdoc />
        protected override void OnBuildTooltipText(StringBuilder sb)
        {
            base.OnBuildTooltipText(sb);

            var asset = FlaxEngine.Content.LoadAsset<AudioClip>(ObjectID, 100);
            if (asset)
            {
                var info = asset.Info;
                sb.Append("Duration: ").Append(asset.Length).AppendLine();
                sb.Append("Channels: ").Append(info.NumChannels).AppendLine();
                sb.Append("Bit Depth: ").Append(info.BitDepth).AppendLine();
            }
        }
    }

    /// <summary>
    /// A <see cref="AudioClip"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    class AudioClipProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Audio Clip";

        /// <inheritdoc />
        public override bool CanReimport(ContentItem item)
        {
            return false;
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new AudioClipWindow(editor, (AssetItem)item);
        }

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            return new AudioClipItem(path, ref id, typeName, AssetType);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0xB3452B);

        /// <inheritdoc />
        public override Type AssetType => typeof(AudioClip);

    }
}
