// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.Windows.Assets;
using FlaxEngine.Tools;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestTextureProductionValidation
    {
        private static readonly byte[] MutatedPng = Convert.FromBase64String(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Zp1sAAAAASUVORK5CYII=");

        [Test]
        public void TestRepresentativePngHumanLifecycle()
        {
            var timer = System.Diagnostics.Stopwatch.StartNew();
            var fixture = ProductionValidationFixture.Create();
            var diagnostics = new List<string>();
            var stage = "setup";
            LogMessageDelegate capture = (level, message, stackTrace, threadId) =>
            {
                if (level == LogType.Warning || level == LogType.Error || level == LogType.Fatal)
                {
                    lock (diagnostics)
                        diagnostics.Add("[" + stage + "] " + level + ": " + message);
                }
            };
            Debug.LogMessageReceived += capture;
            try
            {
                ValidateLifecycle(fixture, ref stage);
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout,
                    "Representative PNG lifecycle exceeded the five-minute abort ceiling.");
                lock (diagnostics)
                    Assert.IsEmpty(diagnostics, string.Join(Environment.NewLine, diagnostics));
            }
            finally
            {
                Debug.LogMessageReceived -= capture;
                fixture.Dispose();
                AssetPipelineService.RefreshSources(new[] { fixture.RootPath }, false);
            }
        }

        public static int RunRepresentativePngHumanLifecycle()
        {
            new TestTextureProductionValidation().TestRepresentativePngHumanLifecycle();
            return 0;
        }

        private static void ValidateLifecycle(ProductionValidationFixture fixture, ref string stage)
        {
            stage = "initial import";
            Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.TexturePath }, false));
            Assert.IsFalse(AssetPipelineService.BuildAsset(fixture.TextureId));
            ProductionValidationFixture.WaitForImports(new[] { fixture.TextureId });

            Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(fixture.TextureId, out var record));
            Assert.AreEqual(fixture.TextureId, record.ID);
            Assert.AreEqual(fixture.TextureId, record.SourceAssetID);
            Assert.AreEqual(typeof(Texture).FullName, record.TypeName);
            Assert.AreEqual("Flax.Texture", record.ProcessorID);
            Assert.AreEqual(AssetSourceKind.ImportedSource, record.SourceKind);
            Assert.AreEqual(AssetRecordStatus.Ready, record.Status);
            Assert.AreNotEqual(typeof(RawDataAsset).FullName, record.TypeName);
            Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(fixture.TextureId));

            var sourceDependency = AssetDatabaseQueryService.GetDependencies(fixture.TextureId)
                .Single(x => x.Kind == "SourceFile");
            StringAssert.AreEqualIgnoringCase(Path.GetFullPath(fixture.TexturePath), Path.GetFullPath(sourceDependency.SourcePath));
            Assert.IsNotEmpty(sourceDependency.ContentHash);
            var initialSourceHash = sourceDependency.ContentHash;
            var initialPublication = GetCurrentPublication(fixture.TextureId);

            stage = "Project selection and inspector";
            var workspace = FlaxEditor.Editor.Instance.ContentDatabase;
            var item = workspace.FindAsset(fixture.TextureId);
            Assert.NotNull(item);
            Assert.AreEqual(typeof(Texture).FullName, item.TypeName);
            Assert.IsTrue(item.IsOfType<Texture>());
            Assert.IsFalse(item.IsOfType<RawDataAsset>());
            Assert.IsTrue(item.IsCanonicalSource);
            Assert.IsInstanceOf<BinaryAssetItem>(item);
            Assert.IsInstanceOf<TextureProxy>(workspace.GetProxy(item));

            var contentWindow = FlaxEditor.Editor.Instance.Windows.ContentWin;
            contentWindow.Select(item, true);
            Assert.AreSame(item, contentWindow.Selection.Single());
            var inspected = FlaxEditor.Editor.Instance.Windows.PropertiesWin.Presenter.Selection;
            Assert.AreEqual(1, inspected.Count, "Inspector did not bind the selected texture source.");
            StringAssert.Contains("TextureImportAssetPropertiesProxy", inspected[0].GetType().Name);
            var editorWindow = FlaxEditor.Editor.Instance.ContentEditing.Open(item, true);
            try
            {
                Assert.IsInstanceOf<TextureWindow>(editorWindow);
            }
            finally
            {
                editorWindow?.Close();
                contentWindow.ClearSelection(false);
            }

            stage = "settings save and reload";
            Assert.IsFalse(TextureImporterService.LoadMetadata(fixture.TexturePath, out TextureTool.Options settings));
            settings.FlipX = !settings.FlipX;
            var expectedFlipX = settings.FlipX;
            Assert.IsFalse(TextureImporterService.ApplyMetadata(fixture.TexturePath, settings));
            ProductionValidationFixture.WaitForImports(new[] { fixture.TextureId });
            Assert.IsFalse(TextureImporterService.LoadMetadata(fixture.TexturePath, out var reloadedSettings));
            Assert.AreEqual(expectedFlipX, reloadedSettings.FlipX);
            Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(fixture.TextureId));
            var settingsPublication = GetCurrentPublication(fixture.TextureId);
            Assert.AreNotEqual(initialPublication.Artifact, settingsPublication.Artifact,
                "Tracked texture settings did not change the exact runtime artifact key.");

            stage = "source dependency invalidation";
            File.WriteAllBytes(fixture.TexturePath, MutatedPng);
            Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.TexturePath }, false));
            Assert.IsFalse(AssetPipelineService.IsArtifactCurrent(fixture.TextureId),
                "Mutated PNG bytes did not invalidate the exact texture artifact.");
            Assert.IsFalse(AssetPipelineService.BuildAsset(fixture.TextureId));
            ProductionValidationFixture.WaitForImports(new[] { fixture.TextureId });
            var mutatedDependency = AssetDatabaseQueryService.GetDependencies(fixture.TextureId)
                .Single(x => x.Kind == "SourceFile");
            Assert.AreNotEqual(initialSourceHash, mutatedDependency.ContentHash);
            var mutatedPublication = GetCurrentPublication(fixture.TextureId);
            Assert.AreNotEqual(settingsPublication.Artifact, mutatedPublication.Artifact);

            stage = "exact runtime and cook input";
            var texture = FlaxEngine.Content.LoadAssetAsync<Texture>(fixture.TextureId);
            Assert.NotNull(texture);
            texture.Reload();
            Assert.IsFalse(texture.WaitForLoaded());
            Assert.AreEqual(typeof(Texture), texture.GetType());
            Assert.AreEqual(1, texture.Width);
            Assert.AreEqual(1, texture.Height);
            Assert.IsTrue(texture.IsUsingExactArtifact);
            Assert.AreEqual(mutatedPublication.Artifact, texture.ArtifactKey);
            Assert.IsTrue(File.Exists(texture.StoragePath));
            StringAssert.Contains("/library/artifacts/", StringUtils.NormalizePath(texture.StoragePath).ToLowerInvariant());
            Assert.IsFalse(FlaxEditor.GameCooker.ValidateBinaryAssetCookForTesting(fixture.TextureId),
                "The exact texture artifact failed the cooker binary-asset path.");

            stage = "cached restart";
            AssetPipelineService.DrainArtifactPublications();
            Assert.IsFalse(AssetPipelineService.Shutdown());
            Assert.IsFalse(AssetPipelineService.Initialize());
            Assert.IsFalse(AssetPipelineService.LoadOrScan());
            Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(fixture.TextureId));
            Assert.IsFalse(TextureImporterService.LoadMetadata(fixture.TexturePath, out var restartedSettings));
            Assert.AreEqual(expectedFlipX, restartedSettings.FlipX);
            Assert.IsFalse(AssetPipelineService.BuildAsset(fixture.TextureId));
            ProductionValidationFixture.WaitForImports(new[] { fixture.TextureId });
            Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(),
                "Unchanged restart unexpectedly scheduled a texture import.");

            stage = "cached external remove and re-add";
            var retainedSource = File.ReadAllBytes(fixture.TexturePath);
            var retainedMetadata = File.ReadAllBytes(fixture.TexturePath + ".meta");
            File.Delete(fixture.TexturePath);
            File.Delete(fixture.TexturePath + ".meta");
            Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.TexturePath }, false));
            Assert.IsFalse(AssetDatabaseQueryService.TryGetMainRecordAtPath(fixture.TexturePath, out _));
            File.WriteAllBytes(fixture.TexturePath, retainedSource);
            File.WriteAllBytes(fixture.TexturePath + ".meta", retainedMetadata);
            Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { fixture.TexturePath }, false));
            Assert.AreEqual(fixture.TextureId, AssetDatabaseQueryService.AssetPathToGUID(fixture.TexturePath));
            AssetPipelineService.DrainArtifactPublications();
            Assert.IsFalse(AssetPipelineService.BuildAsset(fixture.TextureId));
            ProductionValidationFixture.WaitForImports(new[] { fixture.TextureId });
            Assert.IsTrue(AssetPipelineService.IsArtifactCurrent(fixture.TextureId));
            Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(),
                "Byte-identical PNG re-add unexpectedly scheduled a texture import.");

            Assert.IsTrue(AssetDatabaseQueryService.TryGetRecord(fixture.TextureId, out var restoredRecord));
            Assert.AreEqual(typeof(Texture).FullName, restoredRecord.TypeName);
            Assert.AreNotEqual(typeof(RawDataAsset).FullName, restoredRecord.TypeName);
            Assert.IsFalse(TextureImporterService.LoadMetadata(fixture.TexturePath, out var restoredSettings));
            Assert.AreEqual(expectedFlipX, restoredSettings.FlipX);
            var relevantDiagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                (x.AssetGuid == fixture.TextureId ||
                 string.Equals(x.SourcePath, fixture.TexturePath, StringComparison.OrdinalIgnoreCase))).ToArray();
            Assert.IsEmpty(relevantDiagnostics,
                string.Join(Environment.NewLine, relevantDiagnostics.Select(x => x.Code + ": " + x.Message)));
        }

        private static AssetDatabasePublicationInfo GetCurrentPublication(Guid id)
        {
            var publications = AssetDatabaseQueryService.GetPublications(id);
            Assert.IsNotEmpty(publications);
            Assert.AreEqual(1, publications.Length,
                "Representative PNG published an unexpected non-runtime output (thumbnails must stay disabled).");
            var current = publications.OrderBy(x => x.TargetID, StringComparer.Ordinal).First();
            Assert.IsNotEmpty(current.Artifact);
            Assert.IsNotEmpty(current.InputFingerprint);
            return current;
        }
    }
}
#endif
