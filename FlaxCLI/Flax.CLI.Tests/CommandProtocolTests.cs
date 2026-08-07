// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using System.Text.Json.Nodes;
using Flax.CLI.Adapters;
using Flax.CLI.Commands;
using Flax.CLI.Core;
using Flax.CLI.Protocol;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class CommandProtocolTests
{
    [Test]
    public void RequestUsesTheVersionedCommandContract()
    {
        var request = new EditorCommandRequest
        {
            RequestId = "request",
            ProjectPath = "D:/Game",
            Command = new EditorCommandRequestOptions
            {
                Action = "invoke",
                Name = "example.port",
                Arguments = new JsonObject
                {
                    ["manifest"] = "Conversion/unity-scene.json",
                    ["dry-run"] = true,
                },
                Confirm = true,
            },
            EventPath = "D:/Temp/events.ndjson",
            ResultPath = "D:/Temp/result.json",
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(request, JsonSupport.Options));

        Assert.That(document.RootElement.GetProperty("operation").GetString(), Is.EqualTo("command"));
        Assert.That(document.RootElement.GetProperty("command").GetProperty("action").GetString(), Is.EqualTo("invoke"));
        Assert.That(document.RootElement.GetProperty("command").GetProperty("name").GetString(), Is.EqualTo("example.port"));
        Assert.That(document.RootElement.GetProperty("command").GetProperty("arguments").GetProperty("dry-run").GetBoolean(), Is.True);
        Assert.That(document.RootElement.GetProperty("command").GetProperty("confirm").GetBoolean(), Is.True);
    }

    [Test]
    public void CommandArgumentsRunAOneShotEditorRequest()
    {
        var project = new ProjectContext("D:/Game", "D:/Game/Game.flaxproj", "Game", default, null);

        var arguments = EditorAdapter.CreateCommandArguments(project, "D:/Temp/request.json");

        Assert.That(arguments, Does.Contain("-cliRequest"));
        Assert.That(arguments, Does.Contain("-exit"));
        Assert.That(arguments, Does.Not.Contain("-build"));
        Assert.That(arguments[^1], Is.EqualTo("D:/Temp/request.json"));
    }

    [Test]
    public void TypedOptionsSupportJsonFlagsValuesAndRepeatedArrays()
    {
        var arguments = CommandDispatcher.ParseCommandArguments(
            "{\"manifest\":\"Conversion/unity-scene.json\"}",
            ["--dry-run", "--layer", "Ground", "--layer=Gameplay", "--count", "21405"]);

        Assert.That(arguments["manifest"]!.GetValue<string>(), Is.EqualTo("Conversion/unity-scene.json"));
        Assert.That(arguments["dry-run"]!.GetValue<bool>(), Is.True);
        Assert.That(arguments["count"]!.GetValue<int>(), Is.EqualTo(21405));
        Assert.That(arguments["layer"]!.AsArray().Select(x => x!.GetValue<string>()), Is.EqualTo(new[] { "Ground", "Gameplay" }));
    }

    [Test]
    public void TypedOptionsRejectPositionals()
    {
        var exception = Assert.Throws<CliException>(() => CommandDispatcher.ParseCommandArguments(null, ["manifest.json"]));

        Assert.That(exception!.ExitCode, Is.EqualTo(ExitCode.Usage));
    }

    [TestCase("scenes", "hierarchy", "scenes.hierarchy")]
    [TestCase("actors", "component.set", "actors.component.set")]
    [TestCase("prefabs", "instantiate", "prefabs.instantiate")]
    public void AuthoringGroupsRouteToTypedCommandNames(string group, string action, string expected)
    {
        Assert.That(CommandDispatcher.AuthoringCommandName(group, action), Is.EqualTo(expected));
    }
}
