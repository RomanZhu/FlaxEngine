// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;
using Flax.CLI.Services;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class EngineResolutionTests
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
    public void ProjectNicknameAndLockResolveTheExactCustomEngine()
    {
        var paths = new AppPaths(Path.Combine(_temporaryDirectory, "state"));
        var registry = new EngineRegistry(paths);
        var firstRoot = CreateEngine("First", 1, 11, 0, 1);
        var secondRoot = CreateEngine("Second", 1, 12, 0, 2);
        registry.Add(firstRoot, "first", "local", "local");
        var second = registry.Add(secondRoot, "team-main", "local", "local");
        registry.SetDefault("first");

        var projectRoot = Path.Combine(_temporaryDirectory, "Game");
        Directory.CreateDirectory(projectRoot);
        File.WriteAllText(Path.Combine(projectRoot, "Game.flaxproj"), """
        {
          "Name": "Game",
          "MinEngineVersion": "1.12.0",
          "EngineNickname": "team-main"
        }
        """);
        var project = ProjectContext.Find(projectRoot);
        var resolver = new ContextResolver(registry, new ConfigStore(paths));

        Assert.That(resolver.Resolve(project, null).Path, Is.EqualTo(secondRoot));

        resolver.Pin(project, second);
        Assert.That(resolver.Resolve(project with { EngineNickname = null }, null).Fingerprint, Is.EqualTo(second.Fingerprint));
    }

    [Test]
    public void IncompatibleExplicitEngineIsRejected()
    {
        var paths = new AppPaths(Path.Combine(_temporaryDirectory, "state"));
        var registry = new EngineRegistry(paths);
        registry.Add(CreateEngine("Old", 1, 10, 0, 1), "old", "local", "local");
        var projectRoot = Path.Combine(_temporaryDirectory, "Game");
        Directory.CreateDirectory(projectRoot);
        File.WriteAllText(Path.Combine(projectRoot, "Game.flaxproj"), "{ \"Name\": \"Game\", \"MinEngineVersion\": \"1.12.0\" }");
        var resolver = new ContextResolver(registry, new ConfigStore(paths));

        var exception = Assert.Throws<CliException>(() => resolver.Resolve(ProjectContext.Find(projectRoot), "old"));

        Assert.That(exception!.ExitCode, Is.EqualTo(ExitCode.ContextRequired));
    }

    [Test]
    public void LauncherOnlyEngineIdDoesNotChangeWhenTheBinaryChanges()
    {
        var paths = new AppPaths(Path.Combine(_temporaryDirectory, "state"));
        var engineRoot = CreateEngine("Local", 1, 12, 0, 1);
        Directory.CreateDirectory(paths.LauncherDirectory);
        File.WriteAllText(paths.VersionsFile, $"0{Environment.NewLine}{engineRoot}{Environment.NewLine}");
        var registry = new EngineRegistry(paths);
        var initial = registry.List().Single();

        File.SetLastWriteTimeUtc(initial.EditorPath!, DateTime.UtcNow.AddMinutes(1));
        var changed = registry.List().Single();

        Assert.That(changed.Id, Is.EqualTo(initial.Id));
        Assert.That(changed.Fingerprint, Is.Not.EqualTo(initial.Fingerprint));
    }

    private string CreateEngine(string name, int major, int minor, int revision, int build)
    {
        var root = Path.Combine(_temporaryDirectory, name);
        Directory.CreateDirectory(root);
        File.WriteAllText(Path.Combine(root, "Flax.flaxproj"), $$"""
        {
          "Name": "Flax",
          "Version": { "Major": {{major}}, "Minor": {{minor}}, "Revision": {{revision}}, "Build": {{build}} }
        }
        """);
        var editor = OperatingSystem.IsWindows()
            ? Path.Combine(root, "Binaries", "Editor", "Win64", "Development", "FlaxEditor.exe")
            : OperatingSystem.IsMacOS()
                ? Path.Combine(root, "Binaries", "Editor", "Mac", "Development", "FlaxEditor")
                : Path.Combine(root, "Binaries", "Editor", "Linux", "Development", "FlaxEditor");
        Directory.CreateDirectory(Path.GetDirectoryName(editor)!);
        File.WriteAllBytes(editor, [1, 2, 3]);
        var tool = Path.Combine(root, "Binaries", "Tools", OperatingSystem.IsWindows() ? "Flax.Build.exe" : "Flax.Build");
        Directory.CreateDirectory(Path.GetDirectoryName(tool)!);
        File.WriteAllBytes(tool, [1, 2, 3]);
        return root;
    }
}
