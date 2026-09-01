// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Create;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Timeline;
using FlaxEditor.GUI.Timeline.Tracks;
using FlaxEditor.Viewport.Previews;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// A <see cref="ParticleEmitter"/> asset proxy object.
    /// </summary>
    /// <seealso cref="FlaxEditor.Content.BinaryAssetProxy" />
    [ContentContextMenu("New/Particles/Particle Emitter")]
    public class ParticleEmitterProxy : BinaryAssetProxy
    {
        /// <inheritdoc />
        public override string Name => "Particle Emitter";

        /// <inheritdoc />
        public override string FileExtension => "particleemitter";

        /// <inheritdoc />
        public override bool AcceptsAsset(string typeName, string path)
        {
            if (typeName != TypeName)
                return false;
            var extension = System.IO.Path.GetExtension(path);
            return string.Equals(extension, ".particleemitter", StringComparison.OrdinalIgnoreCase);
        }

        /// <inheritdoc />
        public override EditorWindow Open(Editor editor, ContentItem item)
        {
            return new ParticleEmitterWindow(editor, item as AssetItem);
        }

        /// <inheritdoc />
        public override Color AccentColor => Color.FromRGB(0xFF79D2B0);

        /// <inheritdoc />
        public override Type AssetType => typeof(ParticleEmitter);

        /// <inheritdoc />
        public override bool CanCreate(ContentFolder targetLocation)
        {
            return targetLocation.CanHaveAssets;
        }

        /// <inheritdoc />
        public override void Create(string outputPath, object arg)
        {
            Editor.Instance.ContentImporting.Create(new ParticleEmitterCreateEntry(outputPath));
        }

        /// <inheritdoc />
        public override void OnContentWindowContextMenu(ContextMenu menu, ContentItem item)
        {
            base.OnContentWindowContextMenu(menu, item);

            if (item is BinaryAssetItem binaryAssetItem)
            {
                var button = menu.AddButton("Create Particle System", CreateParticleSystemClicked);
                button.Tag = binaryAssetItem;
            }
        }

        private void CreateParticleSystemClicked(ContextMenuButton obj)
        {
            var binaryAssetItem = (BinaryAssetItem)obj.Tag;
            CreateParticleSystem(binaryAssetItem);
        }

        /// <summary>
        /// Creates the particle system from the given particle emitter.
        /// </summary>
        /// <param name="emitterItem">The particle emitter item to use as a base for the particle system.</param>
        public static void CreateParticleSystem(BinaryAssetItem emitterItem)
        {
            var particleSystemName = emitterItem.ShortName + " Particle System";
            var particleSystemProxy = Editor.Instance.ContentDatabase.GetProxy<ParticleSystem>();
            Editor.Instance.Windows.ContentWin.NewItem(particleSystemProxy, null, item => OnParticleSystemCreated(item, emitterItem), particleSystemName);
        }

        private static void OnParticleSystemCreated(ContentItem item, BinaryAssetItem particleItem)
        {
            var assetItem = (AssetItem)item;
            var particleSystem = FlaxEngine.Content.LoadAssetAsync<ParticleSystem>(assetItem.ObjectID);
            if (particleSystem == null || particleSystem.WaitForLoaded())
            {
                Editor.LogError("Failed to load created particle system.");
                return;
            }

            ParticleEmitter emitter = FlaxEngine.Content.LoadAssetAsync<ParticleEmitter>(particleItem.ObjectID);
            if (emitter == null || emitter.WaitForLoaded())
            {
                Editor.LogError("Failed to load base particle emitter.");
            }

            ParticleSystemPreview tempPreview = new ParticleSystemPreview(false);
            ParticleSystemTimeline timeline = new ParticleSystemTimeline(tempPreview);
            timeline.Load(particleSystem);

            var track = (ParticleEmitterTrack)timeline.NewTrack(ParticleEmitterTrack.GetArchetype());
            track.Asset = emitter;
            track.TrackMedia.DurationFrames = timeline.DurationFrames;
            track.Rename(particleItem.ShortName);
            timeline.AddTrack(track);
            timeline.Save(particleSystem);
        }

    }
}
