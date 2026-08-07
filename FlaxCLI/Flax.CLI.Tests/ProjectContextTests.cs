// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class ProjectContextTests
{
    private string _temporaryDirectory = null!;

    [SetUp]
    public void SetUp()
    {
        _temporaryDirectory = Path.Combine(Path.GetTempPath(), "flax-cli-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_temporaryDirectory);
    }

    [TearDown]
    public void TearDown()
    {
        if (Directory.Exists(_temporaryDirectory))
            Directory.Delete(_temporaryDirectory, recursive: true);
    }

    [Test]
    public void ParsesTrailingCommasAndEngineConstraints()
    {
        var projectFile = Path.Combine(_temporaryDirectory, "Game.flaxproj");
        File.WriteAllText(projectFile, """
        {
          "Name": "Game",
          "MinEngineVersion": { "Major": 1, "Minor": 12, "Revision": 3, "Build": 7, },
          "EngineNickname": "team-main",
        }
        """);

        var project = ProjectContext.Find(_temporaryDirectory);

        Assert.That(project.Name, Is.EqualTo("Game"));
        Assert.That(project.MinimumEngineVersion, Is.EqualTo(new SemanticVersion(1, 12, 3, 7)));
        Assert.That(project.EngineNickname, Is.EqualTo("team-main"));
    }

    [TestCase("1.12", 1, 12, 0, 0)]
    [TestCase("1.12.3", 1, 12, 3, 0)]
    [TestCase("1.12.3.4", 1, 12, 3, 4)]
    public void ParsesSemanticVersions(string text, int major, int minor, int patch, int build)
    {
        Assert.That(SemanticVersion.TryParse(text, out var version), Is.True);
        Assert.That(version, Is.EqualTo(new SemanticVersion(major, minor, patch, build)));
    }
}
