// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;
using Flax.CLI.Services;

namespace Flax.CLI.Adapters;

internal sealed class FlaxBuildAdapter(ProcessCoordinator processes)
{
    public Task<ProcessResult> GenerateAsync(EngineInfo engine, ProjectContext project, string? ide, IReadOnlyList<string> rawArguments, CommandContext context)
    {
        var arguments = new List<string> { "-genproject", "-log", $"-workspace={project.Root}" };
        if (!string.IsNullOrWhiteSpace(ide))
            arguments.Add("-" + NormalizeIde(ide));
        arguments.AddRange(rawArguments);
        return Run(engine, project, arguments, context);
    }

    public Task<ProcessResult> CompileAsync(EngineInfo engine, ProjectContext project, IReadOnlyList<string> targets, string? configuration, string? platform, string? architecture, IReadOnlyList<string> rawArguments, bool clean, CommandContext context)
    {
        var arguments = new List<string> { clean ? "-clean" : "-build", "-log", "-mutex", $"-workspace={project.Root}" };
        if (targets.Count != 0) arguments.Add($"-buildtargets={string.Join(',', targets)}");
        if (configuration != null) arguments.Add($"-configuration={configuration}");
        if (platform != null) arguments.Add($"-platform={platform}");
        if (architecture != null) arguments.Add($"-arch={architecture}");
        arguments.AddRange(rawArguments);
        return Run(engine, project, arguments, context);
    }

    private Task<ProcessResult> Run(EngineInfo engine, ProjectContext project, List<string> arguments, CommandContext context)
    {
        if (engine.BuildToolPath == null)
            throw new CliException(ExitCode.ContextRequired, "FLX-BUILD-0004", $"Flax.Build was not found in '{engine.Path}'.");
        if (engine.BuildToolPath.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
        {
            arguments.Insert(0, engine.BuildToolPath);
            return processes.RunAsync("dotnet", arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
        }
        return processes.RunAsync(engine.BuildToolPath, arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
    }

    private static string NormalizeIde(string value) => value.ToLowerInvariant() switch
    {
        "visualstudio" or "visual-studio" or "vs" or "vs2022" => "vs2022",
        "vs2026" => "vs2026",
        "visualstudiocode" or "visual-studio-code" or "code" or "vscode" => "vscode",
        "rider" => "rider",
        _ => throw CommandLine.Usage($"Unknown IDE '{value}'."),
    };
}
