// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Adapters;
using Flax.CLI.Services;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class FlaxBuildAdapterTests
{
    [TestCase("Exception: No host compiler tools found")]
    [TestCase("Missing target SvalkerFlaxEditor")]
    [TestCase("Error: project generation failed")]
    public void BuildToolFailureMarkersOverrideAZeroProcessExitCode(string standardError)
    {
        var result = new ProcessResult(10, 0, "No targets to build", standardError);

        Assert.That(FlaxBuildAdapter.NormalizeResult(result).ExitCode, Is.EqualTo(1));
    }

    [Test]
    public void InformationalSdkMessagesDoNotOverrideAZeroExitCode()
    {
        var result = new ProcessResult(10, 0, "Missing Android SDK. Cannot build for Android platform.\nDone!", string.Empty);

        Assert.That(FlaxBuildAdapter.NormalizeResult(result).ExitCode, Is.Zero);
    }
}
