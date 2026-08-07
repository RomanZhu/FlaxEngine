// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Text.Json;
using Flax.CLI.Core;
using Flax.CLI.Protocol;
using Flax.CLI.Services;
using NUnit.Framework;

namespace Flax.CLI.Tests;

[TestFixture]
public sealed class EditorBridgeClientTests
{
    private string _temporaryDirectory = null!;
    private AppPaths _paths = null!;

    [SetUp]
    public void SetUp()
    {
        _temporaryDirectory = Path.Combine(Path.GetTempPath(), "flax-cli-bridge-tests", Guid.NewGuid().ToString("N"));
        _paths = new AppPaths(_temporaryDirectory);
        Directory.CreateDirectory(_paths.RuntimeDirectory);
    }

    [TearDown]
    public void TearDown()
    {
        if (Directory.Exists(_temporaryDirectory))
            Directory.Delete(_temporaryDirectory, recursive: true);
    }

    [Test]
    public void DiscoverySelectsALiveInstanceByProjectAndIdPrefix()
    {
        var process = Process.GetCurrentProcess();
        var project = Path.Combine(_temporaryDirectory, "Game");
        Directory.CreateDirectory(project);
        var tokenPath = Path.Combine(_paths.RuntimeDirectory, "editor.token");
        File.WriteAllText(tokenPath, "token");
        var manifest = new EditorInstanceManifest
        {
            SchemaVersion = 1,
            InstanceId = "abcdef0123456789",
            Pid = process.Id,
            ProcessStartTimeUtc = process.StartTime.ToUniversalTime(),
            Kind = "editor",
            ProjectPath = project,
            EngineVersion = "1.12",
            ProtocolVersion = 1,
            Transport = "namedPipe",
            Endpoint = "unused-test-pipe",
            TokenPath = tokenPath,
            State = "ready",
            Capabilities = ["commands"],
        };
        File.WriteAllText(Path.Combine(_paths.RuntimeDirectory, "editor.instance.json"), JsonSerializer.Serialize(manifest, JsonSupport.Options));

        var selected = new EditorBridgeClient(_paths).Select(project, "abcdef", required: true);

        Assert.That(selected, Is.Not.Null);
        Assert.That(selected!.Pid, Is.EqualTo(process.Id));
        Assert.That(selected.Capabilities, Does.Contain("commands"));
    }

    [Test]
    public void DiscoveryPrunesAStaleDescriptorAndToken()
    {
        var tokenPath = Path.Combine(_paths.RuntimeDirectory, "stale.token");
        var manifestPath = Path.Combine(_paths.RuntimeDirectory, "stale.instance.json");
        File.WriteAllText(tokenPath, "token");
        var manifest = new EditorInstanceManifest
        {
            SchemaVersion = 1,
            InstanceId = "stale",
            Pid = int.MaxValue,
            Kind = "editor",
            ProjectPath = _temporaryDirectory,
            ProtocolVersion = 1,
            Transport = "namedPipe",
            Endpoint = "unused-test-pipe",
            TokenPath = tokenPath,
        };
        File.WriteAllText(manifestPath, JsonSerializer.Serialize(manifest, JsonSupport.Options));

        var instances = new EditorBridgeClient(_paths).Discover();

        Assert.That(instances, Is.Empty);
        Assert.That(File.Exists(manifestPath), Is.False);
        Assert.That(File.Exists(tokenPath), Is.False);
    }

    [Test]
    public void DiscoveryIgnoresDescriptorsWhoseTokenIsOutsideTheRuntimeDirectory()
    {
        var process = Process.GetCurrentProcess();
        var externalToken = Path.Combine(_temporaryDirectory, "external.token");
        File.WriteAllText(externalToken, "do-not-touch");
        var manifest = new EditorInstanceManifest
        {
            SchemaVersion = 1,
            InstanceId = "invalid-token-path",
            Pid = process.Id,
            ProcessStartTimeUtc = process.StartTime.ToUniversalTime(),
            Kind = "editor",
            ProjectPath = _temporaryDirectory,
            ProtocolVersion = 1,
            Transport = "namedPipe",
            Endpoint = "unused-test-pipe",
            TokenPath = externalToken,
        };
        File.WriteAllText(Path.Combine(_paths.RuntimeDirectory, "invalid.instance.json"), JsonSerializer.Serialize(manifest, JsonSupport.Options));

        var instances = new EditorBridgeClient(_paths).Discover();

        Assert.That(instances, Is.Empty);
        Assert.That(File.ReadAllText(externalToken), Is.EqualTo("do-not-touch"));
    }

    [Test]
    public void InvocationRejectsAnUnadvertisedCapabilityBeforeConnecting()
    {
        var manifest = new EditorInstanceManifest
        {
            SchemaVersion = 1,
            InstanceId = "limited",
            Pid = Process.GetCurrentProcess().Id,
            Kind = "editor",
            ProjectPath = _temporaryDirectory,
            ProtocolVersion = 1,
            Transport = "namedPipe",
            Endpoint = "unused-test-pipe",
            TokenPath = Path.Combine(_paths.RuntimeDirectory, "missing.token"),
            Capabilities = ["commands"],
        };
        var context = new CommandContext
        {
            Options = new GlobalOptions(),
            CancellationToken = CancellationToken.None,
            Stopwatch = Stopwatch.StartNew(),
        };

        var error = Assert.ThrowsAsync<CliException>(() => new EditorBridgeClient(_paths).InvokeAsync(manifest, "console", null, null, false, context));

        Assert.That(error!.Code, Is.EqualTo("FLX-BRIDGE-CAPABILITY-0004"));
    }
}
