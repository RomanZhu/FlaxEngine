// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS
using FlaxEditor;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestCliAssetVerification
    {
        [TestCase(false, false, "NotBuilt", "Load")]
        [TestCase(true, false, "NotBuilt", "RequestBuild")]
        [TestCase(true, true, "NotBuilt", "WaitForBuild")]
        [TestCase(true, false, "Queued", "WaitForBuild")]
        [TestCase(true, false, "Building", "WaitForBuild")]
        [TestCase(true, false, "Publishing", "WaitForBuild")]
        [TestCase(true, true, "ReadyExact", "Load")]
        [TestCase(true, true, "Failed", "Fail")]
        [TestCase(true, true, "Cancelled", "Fail")]
        public void TestReadinessGate(bool requiresBuild, bool buildRequested, string status, string expected)
        {
            Assert.AreEqual(expected, CliAssetCommands.GetVerificationStep(requiresBuild, buildRequested, status).ToString());
        }
    }
}
#endif
