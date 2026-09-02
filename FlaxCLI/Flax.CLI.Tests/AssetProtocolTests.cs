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

    [Test]
    public void BatchContractCarriesMaterialInstanceConfiguration()
    {
        var batch = new AssetBatchInput
        {
            ContinueOnError = true,
            VerifyReload = true,
            Operations =
            [
                new AssetRequestOptions
                {
                    Action = "material-instance",
                    Path = "Materials/MI_Test.flax",
                    BaseMaterial = "Materials/M_Master.flax",
                    Parameters = new JsonObject { ["Texture"] = "Textures/T_Test.flax" },
                    IfExists = "update",
                },
            ],
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(batch, JsonSupport.Options));
        var root = document.RootElement;
        var operation = root.GetProperty("operations")[0];

        Assert.That(root.GetProperty("schemaVersion").GetInt32(), Is.EqualTo(1));
        Assert.That(root.GetProperty("continueOnError").GetBoolean(), Is.True);
        Assert.That(root.GetProperty("verifyReload").GetBoolean(), Is.True);
        Assert.That(operation.GetProperty("action").GetString(), Is.EqualTo("material-instance"));
        Assert.That(operation.GetProperty("parameters").GetProperty("Texture").GetString(), Is.EqualTo("Textures/T_Test.flax"));
        Assert.That(operation.GetProperty("ifExists").GetString(), Is.EqualTo("update"));
    }

    [Test]
    public void BatchContractCarriesTextureReimportSettings()
    {
        var batch = new AssetBatchInput
        {
            VerifyReload = true,
            Operations =
            [
                new AssetRequestOptions
                {
                    Action = "reimport",
                    Path = "Textures/UI/Journal/Paper.png",
                    Importer = "texture",
                    ImportOptions = new JsonObject
                    {
                        ["sRGB"] = true,
                        ["AlphaIsTransparency"] = true,
                        ["GenerateMipMaps"] = true,
                    },
                },
            ],
        };

        using var document = JsonDocument.Parse(JsonSerializer.Serialize(batch, JsonSupport.Options));
        var operation = document.RootElement.GetProperty("operations")[0];

        Assert.That(operation.GetProperty("importer").GetString(), Is.EqualTo("texture"));
        Assert.That(operation.GetProperty("importOptions").GetProperty("sRGB").GetBoolean(), Is.True);
        Assert.That(operation.GetProperty("importOptions").GetProperty("AlphaIsTransparency").GetBoolean(), Is.True);
        Assert.That(operation.GetProperty("importOptions").GetProperty("GenerateMipMaps").GetBoolean(), Is.True);
    }

    [Test]
    public void BatchOperationsResolveContentAndSourcePaths()
    {
        var root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "FlaxCliAssetBatchPathTest"));
        var project = new ProjectContext(root, Path.Combine(root, "Game.flaxproj"), "Game", default, null);
        var operation = new AssetRequestOptions
        {
            Action = "import",
            Destination = "Textures/Imported",
            Sources = ["Source/Texture.png"],
            IfExists = "skip",
        };

        CommandDispatcher.NormalizeAssetOperation(project, operation, false);

        Assert.That(operation.Destination, Is.EqualTo(Path.GetFullPath("Content/Textures/Imported", root)));
        Assert.That(operation.Sources![0], Is.EqualTo(Path.GetFullPath("Source/Texture.png")));
        Assert.That(operation.IfExists, Is.EqualTo("skip"));
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
