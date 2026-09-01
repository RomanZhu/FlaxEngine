// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using System;
using System.IO;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestProductionValidationFixture
    {
        [Test]
        public void TestStagesReducedCanonicalSourceCohortWithoutImporting()
        {
            string root;
            string externalActors;
            using (var fixture = ProductionValidationFixture.Create())
            {
                root = fixture.RootPath;
                externalActors = fixture.ExternalActorsPath;
                Assert.AreEqual(TimeSpan.FromMinutes(5), ProductionValidationFixture.ImportTimeout);
                Assert.AreEqual(7, fixture.SourcePaths.Count);
                Assert.AreEqual(fixture.SourcePaths.Count, fixture.AssetIds.Count);
                foreach (var path in fixture.SourcePaths)
                {
                    Assert.IsTrue(File.Exists(path), path);
                    Assert.IsTrue(File.Exists(path + ".meta"), path + ".meta");
                }

                var glb = File.ReadAllBytes(fixture.ModelPath);
                Assert.Greater(glb.Length, 20);
                Assert.AreEqual(0x46546c67u, BitConverter.ToUInt32(glb, 0));
                Assert.AreEqual(2u, BitConverter.ToUInt32(glb, 4));
                Assert.AreEqual((uint)glb.Length, BitConverter.ToUInt32(glb, 8));

                var indexPath = Path.Combine(externalActors, "scene-fragments.index");
                var fragmentPath = Path.Combine(externalActors, "00", "2.sceneactor");
                Assert.IsTrue(File.Exists(indexPath));
                Assert.IsTrue(File.Exists(fragmentPath));
                StringAssert.Contains(fixture.ExternalSceneId.ToString("N"), File.ReadAllText(indexPath));
                StringAssert.Contains("\"rootActorLocalId\": 2", File.ReadAllText(fragmentPath));
            }

            Assert.IsFalse(Directory.Exists(root));
            Assert.IsFalse(Directory.Exists(externalActors));
        }

        public static int RunStagesReducedCanonicalSourceCohortWithoutImporting()
        {
            new TestProductionValidationFixture().TestStagesReducedCanonicalSourceCohortWithoutImporting();
            return 0;
        }
    }
}
#endif
