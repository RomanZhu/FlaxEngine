// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Flax.CLI.Adapters;
using Flax.CLI.Commands;
using Flax.CLI.Core;
using Flax.CLI.Services;

var stopwatch = Stopwatch.StartNew();
var requestId = Guid.NewGuid().ToString("N");
var commandName = "help";
GlobalOptions options = new();
CliResult result;
AppPaths? appPaths = null;
var cancellation = new CancellationTokenSource();
var cancellationExitCode = ExitCode.Interrupted;
Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cancellationExitCode = ExitCode.Interrupted;
    cancellation.Cancel();
};
using var sigterm = !OperatingSystem.IsWindows()
    ? PosixSignalRegistration.Create(PosixSignal.SIGTERM, signal => { signal.Cancel = true; cancellationExitCode = ExitCode.Terminated; cancellation.Cancel(); })
    : null;

try
{
    options = CommandLine.Parse(args);
    var paths = appPaths = new AppPaths();
    var config = new ConfigStore(paths);
    ApplyDefaults(options, config);
    commandName = options.Version ? "version" : options.CommandTokens.FirstOrDefault()?.ToLowerInvariant() ?? "help";

    var processes = new ProcessCoordinator();
    var engines = new EngineRegistry(paths);
    var projects = new ProjectRegistry(paths);
    var resolver = new ContextResolver(engines, config);
    var dispatcher = new CommandDispatcher(paths, engines, projects, config, resolver, new FlaxBuildAdapter(processes), new TestAdapter(processes), new EditorAdapter(processes), new EditorBridgeClient(paths));
    result = await dispatcher.ExecuteAsync(new CommandContext { Options = options, CancellationToken = cancellation.Token, Stopwatch = stopwatch });
}
catch (OperationCanceledException)
{
    var code = cancellationExitCode == ExitCode.Terminated ? "FLX-CLI-0143" : "FLX-CLI-0130";
    var message = cancellationExitCode == ExitCode.Terminated ? "The operation was terminated." : "The operation was interrupted.";
    result = CliResult.Fail(cancellationExitCode, code, message);
}
catch (CliException ex)
{
    result = CliResult.Fail(ex.ExitCode, ex.Code, ex.Message, ex.Details);
}
catch (Exception ex)
{
    result = CliResult.Fail(ExitCode.InternalFailure, "FLX-CLI-0001", "An unexpected CLI failure occurred.", new { incidentId = requestId, exception = options.Verbose ? ex.ToString() : ex.Message });
}

stopwatch.Stop();
if (options.Trace && appPaths != null)
{
    try
    {
        var tracePath = TraceWriter.Write(appPaths, requestId, commandName, result, stopwatch.Elapsed);
        result.Warnings.Add(new CliMessage("FLX-TRACE-0000", "A redacted diagnostic trace was written.", new { path = tracePath }));
    }
    catch (Exception ex)
    {
        result.Warnings.Add(new CliMessage("FLX-TRACE-W001", "The diagnostic trace could not be written.", new { exception = ex.Message }));
    }
}
// MCP owns stdout for the lifetime of its JSON-RPC session. Do not append the
// normal CLI envelope after stdin closes, or MCP clients would see a spurious
// non-JSON-RPC message.
if (!string.Equals(commandName, "mcp", StringComparison.OrdinalIgnoreCase))
    new OutputWriter(Console.Out, Console.Error).Write(commandName, result, options, stopwatch.Elapsed, requestId);
else if (result.ExitCode != ExitCode.Success)
    Console.Error.WriteLine(JsonSerializer.Serialize(result, JsonSupport.Options));
return (int)result.ExitCode;

static void ApplyDefaults(GlobalOptions options, ConfigStore config)
{
    options.Project ??= Environment.GetEnvironmentVariable("FLAX_PROJECT");
    string? projectConfig = null;
    try
    {
        projectConfig = ProjectContext.Find(options.Project).ProjectConfigFile;
    }
    catch (CliException)
    {
        // Commands that do not need a project must remain usable outside a project directory.
    }
    if (!options.FormatSpecified)
    {
        options.Format = Environment.GetEnvironmentVariable("FLAX_FORMAT")
            ?? config.GetString("format", projectConfig)
            ?? (Console.IsOutputRedirected ? "tsv" : "human");
        if (!new[] { "human", "tsv", "json", "ndjson" }.Contains(options.Format, StringComparer.OrdinalIgnoreCase))
            throw CommandLine.Usage($"Unknown output format '{options.Format}'.");
        options.Format = options.Format.ToLowerInvariant();
    }
    options.Engine ??= Environment.GetEnvironmentVariable("FLAX_ENGINE") ?? config.GetString("engine", projectConfig);
    options.NonInteractive |= IsTrue(Environment.GetEnvironmentVariable("FLAX_NON_INTERACTIVE")) || config.GetBoolean("nonInteractive", projectFile: projectConfig);
}

static bool IsTrue(string? value) => value is "1" || string.Equals(value, "true", StringComparison.OrdinalIgnoreCase);
