// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace FlaxEngine.Tests
{
    /// <summary>
    /// Small canonical-source cohort shared by production asset pipeline validation tests.
    /// Creation only stages source files; call <see cref="ImportAndWait"/> when artifacts are required.
    /// </summary>
    public sealed class ProductionValidationFixture : IDisposable
    {
        /// <summary>Maximum duration of an import-backed fixture operation.</summary>
        public static readonly TimeSpan ImportTimeout = TimeSpan.FromMinutes(5);

        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
        private bool _disposed;

        /// <summary>Fixture root under the current project's Content directory.</summary>
        public string RootPath { get; }

        /// <summary>Representative one-pixel PNG source.</summary>
        public string TexturePath { get; }

        /// <summary>Representative GLB source containing a named triangle mesh.</summary>
        public string ModelPath { get; }

        /// <summary>Canonical particle-system authored text source.</summary>
        public string ParticleSystemPath { get; }

        /// <summary>Canonical collision-data authored text source.</summary>
        public string CollisionDataPath { get; }

        /// <summary>Canonical prefab source.</summary>
        public string PrefabPath { get; }

        /// <summary>Canonical inline scene source.</summary>
        public string ScenePath { get; }

        /// <summary>Canonical scene source backed by the project-root ExternalActors store.</summary>
        public string ExternalScenePath { get; }

        /// <summary>Project-root directory containing the external scene fragment index and payload.</summary>
        public string ExternalActorsPath { get; }

        public Guid TextureId { get; } = Guid.NewGuid();
        public Guid ModelId { get; } = Guid.NewGuid();
        public Guid ParticleSystemId { get; } = Guid.NewGuid();
        public Guid CollisionDataId { get; } = Guid.NewGuid();
        public Guid PrefabId { get; } = Guid.NewGuid();
        public Guid SceneId { get; } = Guid.NewGuid();
        public Guid ExternalSceneId { get; } = Guid.NewGuid();

        /// <summary>All canonical source paths in deterministic dependency order.</summary>
        public IReadOnlyList<string> SourcePaths { get; }

        /// <summary>All root asset IDs corresponding to <see cref="SourcePaths"/>.</summary>
        public IReadOnlyList<Guid> AssetIds { get; }

        private ProductionValidationFixture(string rootPath)
        {
            RootPath = rootPath;
            TexturePath = Path.Combine(rootPath, "Fixture.png");
            ModelPath = Path.Combine(rootPath, "Fixture.glb");
            ParticleSystemPath = Path.Combine(rootPath, "Fixture.particlesystem");
            CollisionDataPath = Path.Combine(rootPath, "Fixture.collisiondata");
            PrefabPath = Path.Combine(rootPath, "Fixture.prefab");
            ScenePath = Path.Combine(rootPath, "Fixture.scene");
            ExternalScenePath = Path.Combine(rootPath, "ExternalFixture.scene");
            ExternalActorsPath = Path.Combine(Globals.ProjectFolder, "ExternalActors", ExternalSceneId.ToString("N"));
            SourcePaths = new[]
            {
                TexturePath,
                ModelPath,
                ParticleSystemPath,
                CollisionDataPath,
                PrefabPath,
                ScenePath,
                ExternalScenePath,
            };
            AssetIds = new[]
            {
                TextureId,
                ModelId,
                ParticleSystemId,
                CollisionDataId,
                PrefabId,
                SceneId,
                ExternalSceneId,
            };
        }

        /// <summary>Stages a fresh source-only cohort without scanning or importing it.</summary>
        public static ProductionValidationFixture Create(string rootPath = null)
        {
            rootPath = rootPath ?? Path.Combine(Globals.ProjectContentFolder,
                "__ProductionValidation_" + Guid.NewGuid().ToString("N"));
            if (Directory.Exists(rootPath) || File.Exists(rootPath))
                throw new IOException("Production validation fixture path already exists: " + rootPath);

            var fixture = new ProductionValidationFixture(rootPath);
            fixture.WriteSources();
            return fixture;
        }

        /// <summary>
        /// Registers and asynchronously builds the complete cohort, cancelling unfinished builds after five minutes.
        /// </summary>
        public void ImportAndWait(Action pump = null)
        {
            ThrowIfDisposed();
            if (AssetPipelineService.RefreshSources(ToArray(SourcePaths), false))
                throw new InvalidOperationException("Failed to register the production validation fixture sources.");
            if (ModelImporterService.ReconcileSubAssets(ModelId))
                throw new InvalidOperationException("Failed to reconcile the production validation GLB subassets.");

            var pending = new List<Guid>(AssetIds);
            foreach (var id in pending)
            {
                if (AssetPipelineService.BuildAsset(id))
                    throw BuildFailure(id, "request");
            }
            WaitForImports(pending, pump);
        }

        /// <summary>Waits for externally requested fixture imports and cancels them at the five-minute deadline.</summary>
        public static void WaitForImports(IEnumerable<Guid> assetIds, Action pump = null)
        {
            if (assetIds == null)
                throw new ArgumentNullException(nameof(assetIds));
            var pending = new List<Guid>(assetIds);
            var timer = Stopwatch.StartNew();
            while (pending.Count != 0)
            {
                for (var i = pending.Count - 1; i >= 0; i--)
                {
                    var id = pending[i];
                    var status = AssetPipelineService.GetBuildStatus(id);
                    if (status == "ReadyExact" && AssetPipelineService.IsArtifactCurrent(id))
                    {
                        pending.RemoveAt(i);
                    }
                    else if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                    {
                        throw BuildFailure(id, status);
                    }
                }

                if (pending.Count == 0)
                    return;
                if (timer.Elapsed >= ImportTimeout)
                {
                    foreach (var id in pending)
                        AssetPipelineService.CancelBuild(id);
                    throw new TimeoutException("Production validation imports exceeded the five-minute deadline. Pending: " +
                                               string.Join(", ", pending));
                }
                pump?.Invoke();
                Thread.Sleep(25);
            }
        }

        /// <summary>Deletes staged source and ExternalActors files. Database reconciliation remains test-controlled.</summary>
        public void Dispose()
        {
            if (_disposed)
                return;
            _disposed = true;
            if (Directory.Exists(RootPath))
                Directory.Delete(RootPath, true);
            if (Directory.Exists(ExternalActorsPath))
                Directory.Delete(ExternalActorsPath, true);
        }

        private void WriteSources()
        {
            Directory.CreateDirectory(RootPath);
            File.WriteAllBytes(TexturePath, Convert.FromBase64String(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="));
            WriteGlb(ModelPath);
            WriteText(ParticleSystemPath,
                "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.ParticleSystem\",\n  \"framesPerSecond\": 60.0,\n  \"durationFrames\": 0,\n  \"tracks\": [],\n  \"parameterOverrides\": []\n}\n");
            WriteText(CollisionDataPath,
                "{\n  \"documentVersion\": 1,\n  \"type\": \"FlaxEngine.CollisionData\",\n  \"collisionType\": \"None\",\n  \"sourceModel\": null,\n  \"modelLodIndex\": 0,\n  \"materialSlotsMask\": 4294967295,\n  \"convexFlags\": [],\n  \"convexVertexLimit\": 255\n}\n");
            WriteText(PrefabPath,
                "{\n  \"prefabVersion\": 4,\n  \"objects\": [\n    { \"fileId\": 1, \"type\": \"FlaxEngine.EmptyActor\", \"name\": \"Fixture Prefab\" }\n  ]\n}\n");
            WriteText(ScenePath,
                "{\n  \"sceneVersion\": 4,\n  \"objects\": [\n    { \"fileId\": 1, \"type\": \"FlaxEngine.Scene\" },\n    { \"fileId\": 2, \"type\": \"FlaxEngine.EmptyActor\", \"parentFileId\": 1, \"name\": \"Inline Fixture Actor\" }\n  ]\n}\n");
            WriteText(ExternalScenePath,
                "{\n  \"sceneVersion\": 4,\n  \"externalActors\": true,\n  \"objects\": [\n    { \"fileId\": 1, \"type\": \"FlaxEngine.Scene\", \"useExternalActors\": true }\n  ]\n}\n");

            WriteMetadata(TexturePath, TextureId, "Flax.Texture", "FlaxEngine.Texture");
            WriteMetadata(ModelPath, ModelId, "Flax.Model", "FlaxEngine.Model");
            WriteMetadata(ParticleSystemPath, ParticleSystemId, "Flax.ParticleSystem", "FlaxEngine.ParticleSystem");
            WriteMetadata(CollisionDataPath, CollisionDataId, "Flax.CollisionData", "FlaxEngine.CollisionData");
            WriteMetadata(PrefabPath, PrefabId, "Flax.JsonDocument", "FlaxEngine.Prefab");
            WriteMetadata(ScenePath, SceneId, "Flax.JsonDocument", "FlaxEngine.SceneAsset");
            WriteMetadata(ExternalScenePath, ExternalSceneId, "Flax.JsonDocument", "FlaxEngine.SceneAsset");
            WriteExternalActors();
        }

        private static void WriteMetadata(string sourcePath, Guid id, string processor, string typeName)
        {
            WriteText(sourcePath + ".meta",
                "{\n" +
                "  \"fileFormatVersion\": 2,\n" +
                "  \"guid\": \"" + id.ToString("N") + "\",\n" +
                "  \"folderAsset\": false,\n" +
                "  \"importer\": { \"id\": \"" + processor + "\", \"version\": 1, \"settings\": {} },\n" +
                "  \"objectIds\": { \"main\": { \"fileId\": 1, \"type\": \"" + typeName + "\" } },\n" +
                "  \"labels\": [],\n" +
                "  \"userData\": {}\n" +
                "}\n");
        }

        private static void WriteText(string path, string value)
        {
            File.WriteAllText(path, value, Utf8);
        }

        private void WriteExternalActors()
        {
            const string relativePath = "00/2.sceneactor";
            var fragment =
                "{\n" +
                "  \"formatVersion\": 1,\n" +
                "  \"ownerSceneGuid\": \"" + ExternalSceneId.ToString("N") + "\",\n" +
                "  \"rootActorLocalId\": 2,\n" +
                "  \"containedLocalIds\": [2],\n" +
                "  \"serializerVersion\": 1,\n" +
                "  \"payload\": [\n" +
                "    { \"fileId\": 2, \"type\": \"FlaxEngine.EmptyActor\", \"parentFileId\": 1, \"name\": \"External Fixture Actor\" }\n" +
                "  ]\n" +
                "}\n";
            var fragmentBytes = Utf8.GetBytes(fragment);
            string contentHash;
            using (var sha = SHA256.Create())
                contentHash = ToLowerHex(sha.ComputeHash(fragmentBytes));

            var fragmentPath = Path.Combine(ExternalActorsPath, "00", "2.sceneactor");
            Directory.CreateDirectory(Path.GetDirectoryName(fragmentPath));
            File.WriteAllBytes(fragmentPath, fragmentBytes);
            WriteText(Path.Combine(ExternalActorsPath, "scene-fragments.index"),
                "{\n" +
                "  \"formatVersion\": 1,\n" +
                "  \"ownerSceneGuid\": \"" + ExternalSceneId.ToString("N") + "\",\n" +
                "  \"indexRevision\": 1,\n" +
                "  \"fragments\": [\n" +
                "    { \"rootActorLocalId\": 2, \"relativePhysicalPath\": \"" + relativePath +
                "\", \"contentHash\": \"" + contentHash + "\", \"size\": " + fragmentBytes.Length +
                ", \"serializerVersion\": 1 }\n" +
                "  ]\n" +
                "}\n");
        }

        private static string ToLowerHex(byte[] bytes)
        {
            var result = new StringBuilder(bytes.Length * 2);
            foreach (var value in bytes)
                result.Append(value.ToString("x2"));
            return result.ToString();
        }

        private static InvalidOperationException BuildFailure(Guid id, string stage)
        {
            var diagnostic = AssetPipelineService.GetBuildDiagnostic(id);
            return new InvalidOperationException("Production validation import " + stage + " failed for " +
                                                 id.ToString("N") + ": " + diagnostic.Message);
        }

        private static void WriteGlb(string path)
        {
            const string json =
                "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Flax ProductionValidationFixture\"}," +
                "\"buffers\":[{\"byteLength\":44}]," +
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962}," +
                "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}]," +
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"," +
                "\"max\":[1,1,0],\"min\":[0,0,0]}," +
                "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]," +
                "\"meshes\":[{\"name\":\"FixtureTriangle\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]," +
                "\"nodes\":[{\"name\":\"FixtureTriangle\",\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
            var jsonBytes = Utf8.GetBytes(json);
            var paddedJsonLength = (jsonBytes.Length + 3) & ~3;
            const int binaryLength = 44;
            const int headerLength = 12;
            const int chunkHeaderLength = 8;
            var totalLength = headerLength + chunkHeaderLength + paddedJsonLength + chunkHeaderLength + binaryLength;

            using (var stream = File.Create(path))
            using (var writer = new BinaryWriter(stream, Utf8))
            {
                writer.Write(0x46546c67u);
                writer.Write(2u);
                writer.Write((uint)totalLength);
                writer.Write((uint)paddedJsonLength);
                writer.Write(0x4e4f534au);
                writer.Write(jsonBytes);
                for (var i = jsonBytes.Length; i < paddedJsonLength; i++)
                    writer.Write((byte)' ');

                writer.Write((uint)binaryLength);
                writer.Write(0x004e4942u);
                writer.Write(0.0f);
                writer.Write(0.0f);
                writer.Write(0.0f);
                writer.Write(1.0f);
                writer.Write(0.0f);
                writer.Write(0.0f);
                writer.Write(0.0f);
                writer.Write(1.0f);
                writer.Write(0.0f);
                writer.Write((ushort)0);
                writer.Write((ushort)1);
                writer.Write((ushort)2);
                writer.Write((ushort)0);
            }
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
                throw new ObjectDisposedException(nameof(ProductionValidationFixture));
        }

        private static string[] ToArray(IReadOnlyList<string> values)
        {
            var result = new string[values.Count];
            for (var i = 0; i < values.Count; i++)
                result[i] = values[i];
            return result;
        }
    }
}
#endif
