// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Content.Documents;
using FlaxEngine;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Shared helpers for canonical graph documents (material/anim/visual script/behavior/particle functions).
    /// </summary>
    internal static class CanonicalGraphDocuments
    {
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

        public static T Open<T>(AssetItem item, out AssetDocumentSession session) where T : Asset
        {
            if (item == null)
                throw new ArgumentNullException(nameof(item));
            session = AssetDocumentRegistry.Open(item.ObjectID, AssetDatabaseFacade.LoadGraphSurface);
            return FlaxEngine.Content.LoadAssetAsync<T>(item.ObjectID);
        }

        public static T LoadWorkingArtifact<T>(AssetItem item) where T : Asset
        {
            var original = FlaxEngine.Content.LoadAssetAsync<T>(item.ObjectID);
            if (original == null || original.WaitForLoaded())
                return null;
            var storagePath = (original as BinaryAsset)?.StoragePath;
            if (string.IsNullOrEmpty(storagePath) || Editor.Instance.ContentEditing.FastTempAssetClone(storagePath, out var copyPath))
                return null;
            return FlaxEngine.Content.LoadAsync<T>(copyPath);
        }

        public static byte[] GetSurface(AssetDocumentSession session)
        {
            if (session == null)
                throw new ArgumentNullException(nameof(session));
            return session.GetDocument<byte[]>();
        }

        public static void SetSurface(AssetDocumentSession session, byte[] surface)
        {
            if (session == null)
                throw new ArgumentNullException(nameof(session));
            session.SetDocument(surface);
        }

        public static bool SaveSurface(AssetItem item, AssetDocumentSession session, string propertiesJson = null, bool allowOverwriteConflict = false)
        {
            if (item == null)
                throw new ArgumentNullException(nameof(item));
            if (session == null || session.ObjectID != item.ObjectID)
                throw new ArgumentException("The document session does not own this asset item.", nameof(session));

            using var save = Editor.Instance.ContentDatabase.TrackAssetSave(session.SourcePath);
            var saved = session.Save(value => !AssetDatabaseFacade.SaveGraphSurface(
                value.SourcePath,
                value.GetDocument<byte[]>(),
                allowOverwriteConflict,
                propertiesJson), allowOverwriteConflict, false);
            save.Complete(saved);
            if (!saved)
            {
                if (session.HasExternalConflict)
                    Editor.LogError("Cannot save canonical graph document because the source changed externally: " + session.SourcePath);
                else
                    Editor.LogError("Cannot save canonical graph document " + session.SourcePath);
            }
            else
                item.RefreshThumbnail();
            return !saved;
        }

        public static void Close(AssetItem item, ref AssetDocumentSession session)
        {
            if (session == null)
                return;
            AssetDocumentRegistry.Close(session.ObjectID);
            session = null;
        }
    }
}
