// Copyright (c) Wojciech Figat. All rights reserved.

using NUnit.Framework;

namespace Flax.Build.Tests
{
    [TestFixture]
    public class TestLog
    {
        [TestCase(@"C:\Project\Script.cs(20,10): error CS1001: Identifier expected", 2)]
        [TestCase(@"shader.cpp:12:4: fatal error: missing header", 2)]
        [TestCase(@"C:\Project\Script.cs(20,10): warning CS0219: Unused variable", 1)]
        [TestCase("1 Error(s)", 0)]
        [TestCase("Build failed with exit code 1", 0)]
        public void TestToolOutputClassification(string message, int expected)
        {
            Assert.AreEqual((Log.ToolOutputType)expected, Log.ClassifyToolOutput(message));
        }
    }
}
