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
    [TestCase(false)]
    [TestCase(true)]
    public void OpenedEditorsRunInCliMode(bool play)
    {
        var project = new ProjectContext("D:/Game", "D:/Game/Game.flaxproj", "Game", default, null);

        var arguments = EditorAdapter.CreateOpenArguments(project, play, ["--custom"]);

        Assert.That(arguments, Does.Contain("-climode"));
        Assert.That(arguments.Contains("-play"), Is.EqualTo(play));
        Assert.That(arguments[^1], Is.EqualTo("--custom"));
    }

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
                    ["manifest"] = "Conversion/legacy-scene.json",
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
    public void GeneratorRequestUsesTheSameTypedProtocol()
    {
        var request = new EditorCommandRequest
        {
            RequestId = "generator-request",
            ProjectPath = "D:/Game",
            Command = new EditorCommandRequestOptions
            {
                Action = "generator-invoke",
                Name = "city.generate",
                Arguments = new JsonObject
                {
                    ["seed"] = 42,
                    ["dry-run"] = true,
                },
            },
            EventPath = "D:/Temp/events.ndjson",
            ResultPath = "D:/Temp/result.json",
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(request, JsonSupport.Options));
        var command = document.RootElement.GetProperty("command");

        Assert.That(command.GetProperty("action").GetString(), Is.EqualTo("generator-invoke"));
        Assert.That(command.GetProperty("name").GetString(), Is.EqualTo("city.generate"));
        Assert.That(command.GetProperty("arguments").GetProperty("seed").GetInt32(), Is.EqualTo(42));
        Assert.That(command.GetProperty("arguments").GetProperty("dry-run").GetBoolean(), Is.True);
    }

    [Test]
    public void CommandArgumentsRunAOneShotEditorRequest()
    {
        var project = new ProjectContext("D:/Game", "D:/Game/Game.flaxproj", "Game", default, null);

        var arguments = EditorAdapter.CreateCommandArguments(project, "D:/Temp/request.json");

        Assert.That(arguments, Does.Contain("-cliRequest"));
        Assert.That(arguments, Does.Contain("-exit"));
        Assert.That(arguments, Does.Not.Contain("-build"));
        Assert.That(arguments, Does.Not.Contain("-std"));
        Assert.That(arguments[^1], Is.EqualTo("D:/Temp/request.json"));
    }

    [Test]
    public void TypedOptionsSupportJsonFlagsValuesAndRepeatedArrays()
    {
        var arguments = CommandDispatcher.ParseCommandArguments(
            "{\"manifest\":\"Conversion/legacy-scene.json\"}",
            ["--dry-run", "--layer", "Ground", "--layer=Gameplay", "--count", "21405"]);

        Assert.That(arguments["manifest"]!.GetValue<string>(), Is.EqualTo("Conversion/legacy-scene.json"));
        Assert.That(arguments["dry-run"]!.GetValue<bool>(), Is.True);
        Assert.That(arguments["count"]!.GetValue<int>(), Is.EqualTo(21405));
        Assert.That(arguments["layer"]!.AsArray().Select(x => x!.GetValue<string>()), Is.EqualTo(new[] { "Ground", "Gameplay" }));
    }

    [Test]
    public void TypedOptionsAcceptPowerShellRelaxedObjectLiterals()
    {
        var arguments = CommandDispatcher.ParseCommandArguments(
            null,
            ["--position", "{X:0,Y:20,Z:0}"]);

        var position = arguments["position"]!.AsObject();
        Assert.That(position["X"]!.GetValue<int>(), Is.Zero);
        Assert.That(position["Y"]!.GetValue<int>(), Is.EqualTo(20));
        Assert.That(position["Z"]!.GetValue<int>(), Is.Zero);
    }

    [TestCase("e1349060-c672-4d6d-a5af-e69b5529d59d")]
    [TestCase("01234567-89ab-cdef-0123-456789abcdef")]
    public void TypedOptionsPreserveBareGuidsAsStrings(string guid)
    {
        var arguments = CommandDispatcher.ParseCommandArguments(null, ["--parent", guid]);

        Assert.That(arguments["parent"]!.GetValue<string>(), Is.EqualTo(guid));
    }

    [Test]
    public void TypedOptionsRejectPositionals()
    {
        var exception = Assert.Throws<CliException>(() => CommandDispatcher.ParseCommandArguments(null, ["manifest.json"]));

        Assert.That(exception!.ExitCode, Is.EqualTo(ExitCode.Usage));
    }

    [TestCase("scenes", "hierarchy", "scenes.hierarchy")]
    [TestCase("actors", "component.set", "actors.component.set")]
    [TestCase("actors", "primitive.create", "actors.primitive.create")]
    [TestCase("actors", "property.set", "actors.property.set")]
    [TestCase("prefabs", "instantiate", "prefabs.instantiate")]
    [TestCase("history", "undo", "history.undo")]
    public void AuthoringGroupsRouteToTypedCommandNames(string group, string action, string expected)
    {
        Assert.That(CommandDispatcher.AuthoringCommandName(group, action), Is.EqualTo(expected));
    }
}
