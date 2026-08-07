// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using Flax.CLI.Adapters;
using Flax.CLI.Core;
using Flax.CLI.Protocol;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class GameBuildProtocolTests
{
    [Test]
    public void RequestUsesTheVersionedCamelCaseContract()
    {
        var request = new GameBuildRequest
        {
            RequestId = "request",
            ProjectPath = "D:/Game",
            Preset = "Development",
            Target = "Windows",
            OutputPath = "D:/Game/Artifacts",
            CustomDefines = ["CLIENT"],
            EventPath = "D:/Temp/events.ndjson",
            ResultPath = "D:/Temp/result.json",
            Options = new GameBuildRequestOptions { Clean = true },
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(request, JsonSupport.Options));

        Assert.That(document.RootElement.GetProperty("schemaVersion").GetInt32(), Is.EqualTo(1));
        Assert.That(document.RootElement.GetProperty("operation").GetString(), Is.EqualTo("build"));
        Assert.That(document.RootElement.GetProperty("options").GetProperty("clean").GetBoolean(), Is.True);
    }

    [Test]
    public void BuildArgumentsCarryTypedAndLegacyRequests()
    {
        var project = new ProjectContext("D:/Game", "D:/Game/Game.flaxproj", "Game", default, null);

        var arguments = EditorAdapter.CreateBuildArguments(project, "Development", "Windows", "D:/Temp/request.json", true, ["--custom"]);

        Assert.That(arguments, Does.Contain("-cliRequest"));
        Assert.That(arguments, Does.Contain("D:/Temp/request.json"));
        Assert.That(arguments, Does.Contain("-build"));
        Assert.That(arguments, Does.Contain("Development.Windows"));
        Assert.That(arguments, Does.Contain("-clearcooker"));
        Assert.That(arguments[^1], Is.EqualTo("--custom"));
    }
}
