// Copyright (c) Wojciech Figat. All rights reserved.

using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class PlayerBridgeGuardTests
{
    [Test]
    public void PlayerBridgeIsLimitedToDebugAndDevelopmentGames()
    {
        var root = FindRepositoryRoot();
        var bridge = File.ReadAllText(Path.Combine(root, "Source", "Engine", "Scripting", "CliPlayerBridge.cs"));
        var scripting = File.ReadAllText(Path.Combine(root, "Source", "Engine", "Scripting", "Scripting.cs"));

        StringAssert.Contains("#if FLAX_GAME && (BUILD_DEBUG || BUILD_DEVELOPMENT)", bridge);
        StringAssert.Contains("#elif FLAX_GAME && (BUILD_DEBUG || BUILD_DEVELOPMENT)", scripting);
    }

    [Test]
    public void PlayerManifestAdvertisesOnlyImplementedCapabilityGroups()
    {
        var root = FindRepositoryRoot();
        var bridge = File.ReadAllText(Path.Combine(root, "Source", "Engine", "Scripting", "CliPlayerBridge.cs"));
        var capabilityLines = bridge.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
            .Where(x => x.Contains("capabilities =", StringComparison.Ordinal))
            .ToArray();

        Assert.That(capabilityLines, Has.Length.EqualTo(2));
        foreach (var capabilityLine in capabilityLines)
        {
            StringAssert.Contains("player", capabilityLine);
            StringAssert.Contains("runtimeInput", capabilityLine);
            StringAssert.Contains("performance", capabilityLine);
            StringAssert.DoesNotContain("playtest", capabilityLine);
            StringAssert.DoesNotContain("commands", capabilityLine);
        }
    }

    private static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory != null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "Source", "Engine", "Scripting", "CliPlayerBridge.cs")))
                return directory.FullName;
            directory = directory.Parent;
        }
        throw new DirectoryNotFoundException("Could not locate the Flax Engine repository root.");
    }
}
