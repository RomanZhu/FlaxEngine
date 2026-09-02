// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.IO;
using System.Linq;
using FlaxEditor;
using FlaxEditor.Content;
using Newtonsoft.Json.Linq;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestCliAssetVerification
    {
        [TestCase(false, false, false, "NotBuilt", "Load")]
        [TestCase(true, false, false, "NotBuilt", "RequestBuild")]
        [TestCase(true, false, false, "Queued", "RequestBuild")]
        [TestCase(true, false, false, "Building", "RequestBuild")]
        [TestCase(true, false, false, "Publishing", "RequestBuild")]
        [TestCase(true, false, false, "ReadyExact", "RequestBuild")]
        [TestCase(true, true, false, "NotBuilt", "WaitForBuild")]
        [TestCase(true, true, false, "Queued", "WaitForBuild")]
        [TestCase(true, true, false, "ReadyExact", "WaitForBuild")]
        [TestCase(true, true, true, "Building", "Load")]
        [TestCase(true, true, false, "Failed", "Fail")]
        [TestCase(true, true, false, "Cancelled", "Fail")]
        public void TestReadinessGate(bool requiresBuild, bool buildRequested, bool artifactCurrent, string status, string expected)
        {
            Assert.AreEqual(expected, CliAssetCommands.GetVerificationStep(requiresBuild, buildRequested, artifactCurrent, status).ToString());
        }

        [TestCase(false, true)]
        [TestCase(true, false)]
        public void TestReloadPolicy(bool requiresBuild, bool expected)
        {
            Assert.AreEqual(expected, CliAssetCommands.ShouldForceVerificationReload(requiresBuild));
        }

        [Test]
        public void TestHeadlessCreateRouting()
        {
            Assert.IsTrue(CliAssetCommands.UsesDirectHeadlessCreate(new PrefabProxy()));
            Assert.IsTrue(CliAssetCommands.UsesDirectHeadlessCreate(new ParticleEmitterProxy()));
        }

        [Test]
        public void TestHeadlessSceneCreatePublishesMetadata()
        {
            var path = Path.Combine(Globals.ProjectContentFolder, $"CliScene-{Guid.NewGuid():N}.scene");
            try
            {
                CliAuthoringCommands.CreateScene(path);
                Assert.IsTrue(File.Exists(path));
                Assert.IsTrue(File.Exists(path + ".meta"));

                var source = JObject.Parse(File.ReadAllText(path));
                var metadata = JObject.Parse(File.ReadAllText(path + ".meta"));
                Assert.IsTrue(AssetGuid.TryParse(metadata.Value<string>("guid"), out var metadataGuid));
                var metadataId = metadataGuid.Value;
                var referencedIds = source.SelectTokens("$..guid").Select(x =>
                    AssetGuid.TryParse(x.Value<string>(), out var referenceGuid) ? referenceGuid.Value : Guid.Empty);
                Assert.Contains(metadataId, referencedIds.ToArray(), "Scene-local references must use the canonical metadata GUID.");

                Assert.IsFalse(AssetPipelineService.RefreshSources(new[] { path }, false));
                Assert.IsTrue(AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out var record));
                Assert.AreEqual(metadataId, record.ID);
                Assert.IsFalse(AssetPipelineService.BuildAssetForeground(record.ID));
                Scripting.FlushRemovedObjects();
                Assert.IsFalse(Level.LoadScene(record.ID), "Canonical scene failed to load by its persistent GUID.");
                var loadedScene = Level.FindScene(record.ID);
                Assert.NotNull(loadedScene);
                Assert.IsFalse(Level.UnloadScene(loadedScene));
                Scripting.FlushRemovedObjects();
            }
            finally
            {
                File.Delete(path);
                File.Delete(path + ".meta");
                AssetPipelineService.RefreshSources(new[] { path }, false);
            }
        }

        public static int RunHeadlessSceneCreatePublishesMetadata()
        {
            new TestCliAssetVerification().TestHeadlessSceneCreatePublishesMetadata();
            return 0;
        }
    }
}
#endif
