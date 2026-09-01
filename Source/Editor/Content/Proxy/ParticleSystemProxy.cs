// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Implementation of <see cref="BinaryAssetItem"/> for <see cref="ParticleSystem"/> assets.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetItem" />
    class ParticleSystemItem : BinaryAssetItem
    {
        /// <inheritdoc />
        public ParticleSystemItem(string path, ref Guid id, string typeName, Type type)
        : base(path, ref id, typeName, type, ContentItemSearchFilter.Particles)
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
            return new ParticleEffect { ParticleSystem = FlaxEngine.Content.LoadAssetAsync<ParticleSystem>(ObjectID) };
        }
    }

    /// <summary>
    /// A <see cref="ParticleSystem"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Particles/Particle System")]
    public class ParticleSystemProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Particle System";

        /// <inheritdoc />
        public override string FileExtension => "particlesystem";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".particlesystem", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new ParticleSystemWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override AssetItem ConstructItem(string path, string typeName, ref Guid id)
        {
            return new ParticleSystemItem(path, ref id, typeName, AssetType);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0xFF790200);

        /// <inheritdoc />
        public override Type AssetType => typeof(ParticleSystem);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            if (AuthoredAssetDocumentService.Create(outputPath, typeof(ParticleSystem).FullName) == Guid.Empty)
                throw new Exception("Failed to create new asset.");
        }

    }
}
