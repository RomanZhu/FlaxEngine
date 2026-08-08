// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Commands;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class InputDiagnosticsTests
{
    [Test]
    public void RelativePointerArgumentsPreserveSignedDeltas()
    {
        var arguments = CommandDispatcher.ParseCommandArguments(
            null,
            ["--state", "relative", "--dx", "120", "--dy", "-40"]);

        Assert.That(arguments["state"]!.GetValue<string>(), Is.EqualTo("relative"));
        Assert.That(arguments["dx"]!.GetValue<float>(), Is.EqualTo(120.0f));
        Assert.That(arguments["dy"]!.GetValue<float>(), Is.EqualTo(-40.0f));
    }

    [Test]
    public void InputProbeQueriesSupportRepeatedKeysAxesAndActions()
    {
        var arguments = CommandDispatcher.ParseCommandArguments(
            null,
            ["--key", "W", "--key=A", "--axis", "Mouse X", "--axis=Mouse Y", "--action", "Jump"]);

        Assert.That(arguments["key"]!.AsArray().Select(x => x!.GetValue<string>()), Is.EqualTo(new[] { "W", "A" }));
        Assert.That(arguments["axis"]!.AsArray().Select(x => x!.GetValue<string>()), Is.EqualTo(new[] { "Mouse X", "Mouse Y" }));
        Assert.That(arguments["action"]!.GetValue<string>(), Is.EqualTo("Jump"));
    }
}
