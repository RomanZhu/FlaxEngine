// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Shared helpers for canonical graph documents (material/anim/visual script/behavior/particle functions).
    /// </summary>
    internal static class CanonicalGraphDocuments
    {
        public static bool UseTextGraphAssets => true;

        public static bool UseNewAssetDatabase => true;

        public static void EnsureCanAuthor(string typeName, string outputPath)
        {
            if (!ConvertedTypePolicy.AllowsLegacyBinaryAuthoring(typeName, outputPath))
                throw new InvalidOperationException("Converted asset types cannot write authoritative .flax files.");
        }

        public static bool IsGraphDocumentPath(string path)
        {
            var extension = Path.GetExtension(path);
            return extension.Equals(".materialfunction", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".animgraphfunction", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".animgraph", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".visualscript", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".behaviortree", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".particlefunction", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".particleemitter", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".particlesystem", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".collisiondata", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".material", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".materialinstance", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".sceneanimation", StringComparison.OrdinalIgnoreCase) ||
                   extension.Equals(".skeletonmask", StringComparison.OrdinalIgnoreCase);
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
            if (extension.Equals(".visualscript", StringComparison.OrdinalIgnoreCase))
                return typeof(VisualScript).FullName;
            if (extension.Equals(".behaviortree", StringComparison.OrdinalIgnoreCase))
                return typeof(BehaviorTree).FullName;
            if (extension.Equals(".particlefunction", StringComparison.OrdinalIgnoreCase))
                return typeof(ParticleEmitterFunction).FullName;
            if (extension.Equals(".particleemitter", StringComparison.OrdinalIgnoreCase))
                return typeof(ParticleEmitter).FullName;
            if (extension.Equals(".particlesystem", StringComparison.OrdinalIgnoreCase))
                return typeof(ParticleSystem).FullName;
            if (extension.Equals(".collisiondata", StringComparison.OrdinalIgnoreCase))
                return typeof(CollisionData).FullName;
            if (extension.Equals(".material", StringComparison.OrdinalIgnoreCase))
                return typeof(Material).FullName;
            if (extension.Equals(".materialinstance", StringComparison.OrdinalIgnoreCase))
                return typeof(MaterialInstance).FullName;
            if (extension.Equals(".sceneanimation", StringComparison.OrdinalIgnoreCase))
                return typeof(SceneAnimation).FullName;
            if (extension.Equals(".skeletonmask", StringComparison.OrdinalIgnoreCase))
                return typeof(SkeletonMask).FullName;
            return null;
        }

        public static string VisualScriptProperties(string baseType, int flags)
        {
            var type = string.IsNullOrEmpty(baseType) ? "FlaxEngine.Script" : baseType;
            return "{\n  \"baseType\": \"" + type + "\",\n  \"flags\": " + flags + "\n}\n";
        }

        public static string MaterialProperties(MaterialInfo info)
        {
            return "{\n  \"blendMode\": " + (int)info.BlendMode +
                   ",\n  \"domain\": " + (int)info.Domain +
                   ",\n  \"maskThreshold\": " + info.MaskThreshold.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                   ",\n  \"opacityThreshold\": " + info.OpacityThreshold.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                   ",\n  \"shadingModel\": " + (int)info.ShadingModel + "\n}\n";
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

        public static bool SaveCloneSurface(AssetItem item, byte[] surface, string propertiesJson = null)
        {
            using var save = Editor.Instance.ContentDatabase.TrackAssetSave(item.Path);
            var failed = string.IsNullOrEmpty(propertiesJson)
                ? AssetDatabaseFacade.SaveGraphSurface(item.Path, surface)
                : AssetDatabaseFacade.SaveGraphSurface(item.Path, surface, false, propertiesJson);
            save.Complete(!failed);
            if (failed)
                Editor.LogError("Cannot save canonical graph document " + item.Path);
            else
                item.RefreshThumbnail();
            return failed;
        }
    }
}
