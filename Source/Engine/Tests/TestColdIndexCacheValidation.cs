// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
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
                var rootIds = new[] { fixture.TextureId, fixture.ModelId };
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                Assert.IsFalse(ModelImporterService.ReconcileSubAssets(fixture.ModelId));
                AssetPipelineService.DrainArtifactPublications();
                foreach (var id in rootIds)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                WaitForImports(rootIds, timer);
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);
                Assert.IsNotEmpty(AssetPipelineService.DrainArtifactPublications(),
                    "The real PNG/GLB cohort produced no artifact publication.");
                Assert.IsTrue(rootIds.All(id => AssetPipelineService.IsArtifactCurrent(id)));

                var initialModelIds = GetModelIds(fixture);
                Assert.Greater(initialModelIds.Length, 1, "The real GLB importer exposed no subassets.");

                AssetPipelineService.DrainArtifactPublications();
                Assert.IsFalse(AssetPipelineService.Shutdown());
                Assert.IsFalse(AssetPipelineService.Initialize());
                Assert.IsFalse(AssetPipelineService.LoadOrScan());
                CollectionAssert.AreEquivalent(initialModelIds, GetModelIds(fixture));
                foreach (var id in rootIds)
                    Assert.IsFalse(AssetPipelineService.BuildAsset(id));
                WaitForImports(rootIds, timer);
                Assert.IsEmpty(AssetPipelineService.DrainArtifactPublications(),
                    "Unchanged restart scheduled a cached PNG/GLB import.");

                var retained = roots.ToDictionary(path => path, path => new[]
                {
                    File.ReadAllBytes(path),
                    File.ReadAllBytes(path + ".meta"),
                }, StringComparer.OrdinalIgnoreCase);
                foreach (var path in roots)
                {
                    File.Delete(path);
                    File.Delete(path + ".meta");
                }
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                foreach (var path in roots)
                    Assert.IsFalse(AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out _));

                foreach (var path in roots)
                {
                    File.WriteAllBytes(path, retained[path][0]);
                    File.WriteAllBytes(path + ".meta", retained[path][1]);
                }
                Assert.IsFalse(AssetPipelineService.RefreshSources(roots, false));
                Assert.IsFalse(ModelImporterService.ReconcileSubAssets(fixture.ModelId));
                Assert.AreEqual(fixture.TextureId, AssetDatabaseQueryService.AssetPathToGUID(fixture.TexturePath));
                Assert.AreEqual(fixture.ModelId, AssetDatabaseQueryService.AssetPathToGUID(fixture.ModelPath));
                CollectionAssert.AreEquivalent(initialModelIds, GetModelIds(fixture));

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
                lock (relevantConsole)
                    Assert.IsEmpty(relevantConsole, string.Join(Environment.NewLine, relevantConsole));
                Assert.Less(timer.Elapsed, ProductionValidationFixture.ImportTimeout);
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
