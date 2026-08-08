// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json.Nodes;
using Flax.CLI.Commands;
using Flax.CLI.Core;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
internal sealed class ProjectScaffoldingTests
{
    private string _temporaryDirectory = null!;

    [SetUp]
    public void SetUp()
    {
        _temporaryDirectory = Path.Combine(Path.GetTempPath(), "FlaxCliProjectScaffoldingTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_temporaryDirectory);
    }

    [TearDown]
    public void TearDown()
    {
        if (Directory.Exists(_temporaryDirectory))
            Directory.Delete(_temporaryDirectory, recursive: true);
    }

    [Test]
    public void EmptyProjectIncludesLoadableGameAndEditorTargets()
    {
        var projectFile = CommandDispatcher.CreateEmptyProjectScaffold(
            _temporaryDirectory,
            "My Project",
            new SemanticVersion(1, 12, 3));

        var project = JsonNode.Parse(File.ReadAllText(projectFile))!.AsObject();
        Assert.That(project["GameTarget"]!.GetValue<string>(), Is.EqualTo("My_ProjectTarget"));
        Assert.That(project["EditorTarget"]!.GetValue<string>(), Is.EqualTo("My_ProjectEditorTarget"));
        Assert.That(project["MinEngineVersion"]!.GetValue<string>(), Is.EqualTo("1.12.3"));
        Assert.That(File.Exists(Path.Combine(_temporaryDirectory, "Source", "My_ProjectTarget.Build.cs")), Is.True);
        var editorTargetPath = Path.Combine(_temporaryDirectory, "Source", "My_ProjectEditorTarget.Build.cs");
        Assert.That(File.Exists(editorTargetPath), Is.True);
        Assert.That(File.ReadAllText(editorTargetPath), Does.Contain("Modules.Add(\"My_ProjectEditor\")"));
        var modulePath = Path.Combine(_temporaryDirectory, "Source", "My_Project", "My_Project.Build.cs");
        Assert.That(File.ReadAllText(modulePath), Does.Contain("public class My_Project : GameModule"));
        var editorModulePath = Path.Combine(_temporaryDirectory, "Source", "My_ProjectEditor", "My_ProjectEditor.Build.cs");
        Assert.That(File.ReadAllText(editorModulePath), Does.Contain("public class My_ProjectEditor : GameEditorModule"));
    }

    [TestCase("9Lives", "_9Lives")]
    [TestCase("Test-World", "Test_World")]
    [TestCase("Game", "Game")]
    public void ProjectCodeNameIsAValidIdentifier(string name, string expected)
    {
        Assert.That(CommandDispatcher.GetProjectCodeName(name), Is.EqualTo(expected));
    }
}
