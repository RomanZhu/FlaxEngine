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
public sealed class AssetProtocolTests
{
    [Test]
    public void RequestUsesTheVersionedAssetContract()
    {
        var request = new AssetRequest
        {
            RequestId = "request",
            ProjectPath = "D:/Game",
            Asset = new AssetRequestOptions
            {
                Action = "set",
                Path = "D:/Game/Content/Settings.json",
                PropertyPath = "Instance.Speed",
                Value = JsonValue.Create(42),
            },
            EventPath = "D:/Temp/events.ndjson",
            ResultPath = "D:/Temp/result.json",
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(request, JsonSupport.Options));

        Assert.That(document.RootElement.GetProperty("operation").GetString(), Is.EqualTo("asset"));
        Assert.That(document.RootElement.GetProperty("asset").GetProperty("action").GetString(), Is.EqualTo("set"));
        Assert.That(document.RootElement.GetProperty("asset").GetProperty("value").GetInt32(), Is.EqualTo(42));
    }

    [Test]
    public void AssetArgumentsExitOldEditorsButCarryTypedRequest()
    {
        var project = new ProjectContext("D:/Game", "D:/Game/Game.flaxproj", "Game", default, null);

        var arguments = EditorAdapter.CreateAssetArguments(project, "D:/Temp/request.json", ["--custom"]);

        Assert.That(arguments, Does.Contain("-cliRequest"));
        Assert.That(arguments, Does.Contain("-exit"));
        Assert.That(arguments, Does.Not.Contain("-build"));
        Assert.That(arguments[^1], Is.EqualTo("--custom"));
    }

    [TestCase(".", "Content")]
    [TestCase("Materials/Test.flax", "Content/Materials/Test.flax")]
    [TestCase("Content/Scenes/Main.scene", "Content/Scenes/Main.scene")]
    public void RelativeAssetPathsResolveUnderContent(string input, string expectedSuffix)
    {
        var root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "FlaxCliAssetPathTest"));
        var project = new ProjectContext(root, Path.Combine(root, "Game.flaxproj"), "Game", default, null);

        var result = CommandDispatcher.AssetPath(project, input);

        Assert.That(result, Is.EqualTo(Path.GetFullPath(expectedSuffix, root)));
    }
}
