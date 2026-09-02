// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS && FLAX_EDITOR
using System;
using System.Linq;
using FlaxEditor;
using NUnit.Framework;

namespace FlaxEngine.Tests
{
    [TestFixture]
    public class TestCliCommandStartup
    {
        [CliCommand("tests.project-command-after-load")]
        public static object ProjectCommand() => new { ready = true };

        [Test]
        public void TestLoadedCommandDiscoveryAfterStartupGate()
        {
            var deadline = DateTime.UtcNow.AddSeconds(1);
            Assert.IsFalse(CliRequestService.EvaluateCommandStartup(false, DateTime.UtcNow, deadline, out var timedOut));
            Assert.IsFalse(timedOut);
            Assert.IsTrue(CliRequestService.EvaluateCommandStartup(true, DateTime.UtcNow, deadline, out timedOut));
            Assert.IsFalse(timedOut);

            var command = CliCommandRegistry.RequireCommand(CliCommandRegistry.Discover(), "tests.project-command-after-load");
            Assert.AreEqual(typeof(TestCliCommandStartup).Assembly, command.Method.DeclaringType.Assembly);
        }

        [Test]
        public void TestLoadedAssembliesAreAvailableWithoutEngineScriptingContext()
        {
            var assemblies = Utils.GetAssemblies();
            CollectionAssert.Contains(assemblies, typeof(Utils).Assembly);
            CollectionAssert.Contains(assemblies, typeof(TestCliCommandStartup).Assembly);
        }

        [Test]
        public void TestCommandDiscoveryCacheReleasesAssembliesForScriptsReload()
        {
            var beforeReload = CliCommandRegistry.Discover();
            CliCommandRegistry.ClearCache();
            var afterReload = CliCommandRegistry.Discover();

            Assert.AreNotSame(beforeReload, afterReload);
            Assert.AreEqual(beforeReload.Length, afterReload.Length);
            Assert.AreEqual(beforeReload.Select(x => x.Attribute.Name), afterReload.Select(x => x.Attribute.Name));
        }

        [Test]
        public void TestProjectCommandStartupWaitIsBounded()
        {
            Assert.IsFalse(CliRequestService.IsCommandStartupReady(true, true));
            Assert.IsFalse(CliRequestService.IsCommandStartupReady(false, false));
            Assert.IsTrue(CliRequestService.IsCommandStartupReady(true, false));

            var deadline = DateTime.UtcNow.AddSeconds(-1);
            Assert.IsFalse(CliRequestService.EvaluateCommandStartup(false, DateTime.UtcNow, deadline, out var timedOut));
            Assert.IsTrue(timedOut);
        }
    }
}
#endif
