// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Content.Settings;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Shared helpers for canonical Material Function and Animation Graph documents.
    /// </summary>
    internal static class CanonicalGraphDocuments
    {
        public static bool UseTextGraphAssets
        {
            get
            {
                var settings = GameSettings.Load<AssetPipelineSettings>();
                return settings != null && settings.UseNewAssetDatabase && settings.UseLibraryArtifacts && settings.UseTextGraphAssets;
            }
        }

        public static bool IsGraphDocumentPath(string path)
        {
            var extension = Path.GetExtension(path);
            return extension.Equals(".materialfunction", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".animgraphfunction", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".animgraph", StringComparison.OrdinalIgnoreCase);
        }

        public static string TypeNameFromPath(string path)
        {
            var extension = Path.GetExtension(path);
            if (extension.Equals(".materialfunction", StringComparison.OrdinalIgnoreCase))
                return typeof(MaterialFunction).FullName;
            if (extension.Equals(".animgraphfunction", StringComparison.OrdinalIgnoreCase))
                return typeof(AnimationGraphFunction).FullName;
            if (extension.Equals(".animgraph", StringComparison.OrdinalIgnoreCase))
                return typeof(AnimationGraph).FullName;
            return null;
        }

        public static T LoadClone<T>(AssetItem item) where T : Asset
        {
            var original = FlaxEngine.Content.LoadAsync<T>(item.ID);
            if (original == null || original.WaitForLoaded())
                return null;
            var storagePath = (original as BinaryAsset)?.StoragePath;
            if (string.IsNullOrEmpty(storagePath) || Editor.Instance.ContentEditing.FastTempAssetClone(storagePath, out var clonePath))
                return null;
            var clone = FlaxEngine.Content.LoadAsync<T>(clonePath);
            if (clone == null)
                return null;
            if (clone.ID == item.ID)
                throw new InvalidOperationException("Cloned asset has the same IDs.");
            return clone;
        }

        public static bool SaveCloneSurface(AssetItem item, byte[] surface)
        {
            using var save = Editor.Instance.ContentDatabase.TrackAssetSave(item.Path);
            var failed = AssetDatabaseFacade.SaveGraphSurface(item.Path, surface);
            save.Complete(!failed);
            if (failed)
                Editor.LogError("Cannot save canonical graph document " + item.Path);
            else
                item.RefreshThumbnail();
            return failed;
        }
    }
}
