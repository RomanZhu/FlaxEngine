// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class CommandLineTests
{
    [Test]
    public void GlobalOptionsAreAcceptedAfterTheCommand()
    {
        var result = CommandLine.Parse(["compile", ".", "--target", "GameEditor", "--json", "--project", "D:/Game"]);

        Assert.That(result.Format, Is.EqualTo("json"));
        Assert.That(result.Project, Is.EqualTo("D:/Game"));
        Assert.That(result.CommandTokens, Is.EqualTo(new[] { "compile", ".", "--target", "GameEditor" }));
    }

    [Test]
    public void PassThroughArgumentsAreKeptVerbatim()
    {
        var result = CommandLine.Parse(["play", ".", "--", "--connect", "127.0.0.1:7777"]);

        Assert.That(result.CommandTokens, Is.EqualTo(new[] { "play", "." }));
        Assert.That(result.PassThrough, Is.EqualTo(new[] { "--connect", "127.0.0.1:7777" }));
    }

    [Test]
    public void InvalidTimeoutIsAUsageError()
    {
        var exception = Assert.Throws<CliException>(() => CommandLine.Parse(["compile", "--timeout", "0"]));

        Assert.That(exception!.ExitCode, Is.EqualTo(ExitCode.Usage));
    }
}
