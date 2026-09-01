// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using FlaxEditor;
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
    }
}
#endif
