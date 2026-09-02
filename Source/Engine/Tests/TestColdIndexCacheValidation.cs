// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using FlaxEngine.Tools;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestColdIndexCacheValidation
    {
        [Test]
        public void TestRealImportsSurviveRestartAndUnchangedReAdd()
        {
            var timer = System.Diagnostics.Stopwatch.StartNew();
            var fixture = ProductionValidationFixture.Create();
            var relevantConsole = new List<string>();
            var rootToken = Path.GetFileName(fixture.RootPath);
            var stage = "register valid PNG/GLB sources";
            LogMessageDelegate capture = (level, message, stackTrace, threadId) =>
            {
                if ((level == LogType.Warning || level == LogType.Error || level == LogType.Fatal) &&
                    message.IndexOf(rootToken, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    lock (relevantConsole)
                        relevantConsole.Add(level + ": " + message);
                }
            };
            Debug.LogMessageReceived += capture;
            try
            {
                var roots = new[] { fixture.TexturePath, fixture.ModelPath };
                AssetPipelineService.DrainArtifactPublications();
                File.Delete(fixture.TexturePath + ".meta");
                File.Delete(fixture.ModelPath + ".meta");
                var textureId = TextureImporterService.CreateMetadata(fixture.TexturePath, TextureTool.Options.Default);
                var modelId = ModelImporterService.CreateDefaultMetadata(fixture.ModelPath);
                Assert.AreNotEqual(Guid.Empty, textureId, "Texture metadata creation failed.");
                Assert.AreNotEqual(Guid.Empty, modelId, "Model metadata creation failed.");
                var rootIds = new[] { textureId, modelId };
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                stage = "reconcile GLB subassets";
                Assert.IsFalse(ModelImporterService.ReconcileSubAssets(modelId));
                stage = "request initial real imports";
                foreach (var id in rootIds)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                WaitForImports(rootIds, timer);
                stage = "validate initial publications";
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);
                Assert.IsTrue(rootIds.All(id => AssetPipelineService.IsArtifactCurrent(id)));
                var initialArtifacts = GetPublicationArtifacts(rootIds);

                var initialModelIds = GetModelIds(fixture);
                Assert.Greater(initialModelIds.Length, 1, "The real GLB importer exposed no subassets.");

                stage = "restart pipeline and reload durable index";
                AssetPipelineService.DrainArtifactPublications();
                Assert.IsFalse(AssetPipelineService.Shutdown());
                Assert.IsFalse(AssetPipelineService.Initialize());
                Assert.IsFalse(AssetPipelineService.LoadOrScan());
                CollectionAssert.AreEquivalent(initialModelIds, GetModelIds(fixture));
                stage = "prove restart cache hit";
                foreach (var id in rootIds)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                WaitForImports(rootIds, timer);
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(),
                    "Unchanged restart scheduled a cached PNG/GLB import.");
                AssertPublicationArtifacts(initialArtifacts, rootIds);

                var retained = roots.ToDictionary(path => path, path => new[]
                {
                    File.ReadAllBytes(path),
                    File.ReadAllBytes(path + ".meta"),
                }, StringComparer.OrdinalIgnoreCase);
                stage = "remove exact source and metadata bytes";
                foreach (var path in roots)
                {
                    File.Delete(path);
                    File.Delete(path + ".meta");
                }
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                foreach (var path in roots)
                    Assert.IsFalse(AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out _));

                stage = "restore exact source and metadata bytes";
                foreach (var path in roots)
                {
                    File.WriteAllBytes(path, retained[path][0]);
                    File.WriteAllBytes(path + ".meta", retained[path][1]);
                }
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                Assert.IsFalse(ModelImporterService.ReconcileSubAssets(modelId));
                Assert.AreEqual(textureId, AssetDatabaseQueryService.AssetPathToGUID(fixture.TexturePath));
                Assert.AreEqual(modelId, AssetDatabaseQueryService.AssetPathToGUID(fixture.ModelPath));
                CollectionAssert.AreEquivalent(initialModelIds, GetModelIds(fixture));

                stage = "prove re-add cache hit";
                AssetPipelineService.DrainArtifactPublications();
                foreach (var id in rootIds)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                WaitForImports(rootIds, timer);
                Assert.IsTrue(rootIds.All(id => AssetPipelineService.IsArtifactCurrent(id)));
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(),
                    "Byte-identical PNG/GLB re-add scheduled an import instead of selecting cache.");

                var diagnostics = AssetDatabaseQueryService.GetDiagnostics().Where(x =>
                    x.Severity != AssetPipelineDiagnosticSeverity.Info &&
                    (rootIds.Contains(x.AssetGuid) ||
                     roots.Contains(x.SourcePath, StringComparer.OrdinalIgnoreCase))).ToArray();
                Assert.IsEmpty(diagnostics,
                    string.Join(Environment.NewLine, diagnostics.Select(x => x.Code + ": " + x.Message)));
                stage = "validate scoped console";
                lock (relevantConsole)
                    Assert.IsEmpty(relevantConsole, string.Join(Environment.NewLine, relevantConsole));
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("ASSET-81 real import lifecycle failed during " + stage + ".", ex);
            }
            finally
            {
                Debug.LogMessageReceived -= capture;
                fixture.Dispose();
                AssetPipelineService.RefreshSources(new[] { fixture.RootPath }, false);
            }
        }

        private static Guid[] GetModelIds(ProductionValidationFixture fixture)
        {
            return AssetDatabaseQueryService.QueryRecords(new AssetDatabaseQuery
            {
                PathPrefix = fixture.RootPath,
                Limit = 64,
            }).Where(x => x.ProcessorID == "Flax.Model")
                .Select(x => x.ID).OrderBy(x => x).ToArray();
        }

        private static Dictionary<Guid, string[]> GetPublicationArtifacts(IEnumerable<Guid> assetIds)
        {
            var result = new Dictionary<Guid, string[]>();
            foreach (var id in assetIds)
            {
                var artifacts = AssetDatabaseQueryService.GetPublications(id)
                    .Select(x => x.Artifact).Where(x => !string.IsNullOrEmpty(x))
                    .OrderBy(x => x, StringComparer.Ordinal).ToArray();
                Assert.IsNotEmpty(artifacts, "Real import produced no durable artifact publication for " + id + ".");
                result.Add(id, artifacts);
            }
            return result;
        }

        private static void AssertPublicationArtifacts(Dictionary<Guid, string[]> expected, IEnumerable<Guid> assetIds)
        {
            foreach (var id in assetIds)
            {
                var actual = AssetDatabaseQueryService.GetPublications(id)
                    .Select(x => x.Artifact).Where(x => !string.IsNullOrEmpty(x))
                    .OrderBy(x => x, StringComparer.Ordinal).ToArray();
                CollectionAssert.AreEqual(expected[id], actual, "Cached artifact selection changed for " + id + ".");
            }
        }

        private static void WaitForImports(IEnumerable<Guid> assetIds, System.Diagnostics.Stopwatch total)
        {
            var pending = new List<Guid>(assetIds);
            while (pending.Count != 0)
            {
                for (var i = pending.Count - 1; i >= 0; i--)
                {
                    var id = pending[i];
                    var status = AssetPipelineService.GetBuildStatus(id);
                    if (status == "ReadyExact" && AssetPipelineService.IsArtifactCurrent(id))
                        pending.RemoveAt(i);
                    else if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                        Assert.Fail("Import ended as " + status + " for " + id + ": " +
                                    AssetPipelineService.GetBuildDiagnostic(id).Message);
                }
                if (pending.Count == 0)
                    return;
                if (total.Elapsed >= ProductionValidationFixture.ImportTimeout)
                {
                    foreach (var id in pending)
                        AssetPipelineService.CancelBuild(id);
                    Assert.Fail("Cold index/cache validation exceeded the five-minute hard deadline. Pending: " +
                                string.Join(", ", pending));
                }
                Thread.Sleep(25);
            }
        }

        public static int RunRealImportsSurviveRestartAndUnchangedReAdd()
        {
            new TestColdIndexCacheValidation().TestRealImportsSurviveRestartAndUnchangedReAdd();
            return 0;
        }
    }
}
#endif
