// Copyright (c) Wojciech Figat. All rights reserved.

#if FLAX_TESTS && FLAX_EDITOR
using System;
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
        public void TestProjectOwnedCommandIsDeferredUntilScriptsLoad()
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
        public void TestProjectCommandStartupWaitIsBounded()
        {
            var deadline = DateTime.UtcNow.AddSeconds(-1);
            Assert.IsFalse(CliRequestService.EvaluateCommandStartup(false, DateTime.UtcNow, deadline, out var timedOut));
            Assert.IsTrue(timedOut);
        }
    }
}
#endif
