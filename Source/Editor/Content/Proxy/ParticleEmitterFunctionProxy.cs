// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="ParticleEmitterFunction"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Particles/Particle Emitter Function")]
    public class ParticleEmitterFunctionProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Particle Emitter Function";

        /// <inheritdoc />
        public override string FileExtension => CanonicalGraphDocuments.UseTextGraphAssets ? "particlefunction" : Extension;

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".flax", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(extension, ".particlefunction", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new ParticleEmitterFunctionWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0x1795a3);

        /// <inheritdoc />
        public override Type AssetType => typeof(ParticleEmitterFunction);

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
                if (AssetDatabaseFacade.CreateGraphDocument(outputPath, typeof(ParticleEmitterFunction).FullName) == Guid.Empty)
                    throw new Exception("Failed to create new asset.");
                return;
            }
            if (Editor.CreateAsset("ParticleEmitterFunction", outputPath))
                throw new Exception("Failed to create new asset.");
        }
    }
}
