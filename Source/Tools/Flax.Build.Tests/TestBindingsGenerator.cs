// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using Flax.Build.Bindings;
using NUnit.Framework;

namespace Flax.Build.Tests
{
    [TestFixture]
    public class TestBindingsGenerator
    {
        [Test]
        public void CachedBindingsRequireTargetGeneratedOutputs()
        {
            var root = Path.Combine(Path.GetTempPath(), "FlaxBindingsTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                var bindings = new BindingsGenerator.BindingsResult
                {
                    GeneratedCppFilePath = Path.Combine(root, "Module.Bindings.Gen.cpp"),
                    GeneratedCSharpFilePath = Path.Combine(root, "Module.Bindings.Gen.cs"),
                };
                File.WriteAllText(bindings.GeneratedCppFilePath, "// generated");

                Assert.IsFalse(BindingsGenerator.HasGeneratedBindingsOutputs(bindings, true));
                File.WriteAllText(bindings.GeneratedCSharpFilePath, string.Empty);
                Assert.IsFalse(BindingsGenerator.HasGeneratedBindingsOutputs(bindings, true));
                Assert.IsTrue(BindingsGenerator.HasGeneratedBindingsOutputs(bindings, false));

                File.WriteAllText(bindings.GeneratedCSharpFilePath, "// generated");
                Assert.IsTrue(BindingsGenerator.HasGeneratedBindingsOutputs(bindings, true));
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }
    }
}
