// Copyright (c) Wojciech Figat. All rights reserved.

using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class InputBridgeGuardTests
{
    [Test]
    public void BridgesExposeRelativePointerInjectionAndRuntimeProbe()
    {
        var root = FindRepositoryRoot();
        var playerBridge = File.ReadAllText(Path.Combine(root, "Source", "Engine", "Scripting", "CliPlayerBridge.cs"));
        var editorBridge = File.ReadAllText(Path.Combine(root, "Source", "Editor", "CLI", "CliBridgeModule.cs"));
        var mouseHeader = File.ReadAllText(Path.Combine(root, "Source", "Engine", "Input", "Mouse.h"));

        StringAssert.Contains("runtime.input.inspect", playerBridge);
        StringAssert.Contains("OnMouseMoveRelative", playerBridge);
        StringAssert.Contains("runtime.input.inspect", editorBridge);
        StringAssert.Contains("OnMouseMoveRelative", editorBridge);
        StringAssert.Contains("API_FUNCTION() void OnMouseMoveRelative", mouseHeader);
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
