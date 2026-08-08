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

    private async Task<ProcessResult> Run(EngineInfo engine, ProjectContext project, List<string> arguments, CommandContext context)
    {
        if (engine.BuildToolPath == null)
            throw new CliException(ExitCode.ContextRequired, "FLX-BUILD-0004", $"Flax.Build was not found in '{engine.Path}'.");
        ProcessResult result;
        if (engine.BuildToolPath.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
        {
            arguments.Insert(0, engine.BuildToolPath);
            result = await processes.RunAsync("dotnet", arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
        }
        else
        {
            result = await processes.RunAsync(engine.BuildToolPath, arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
        }
        return NormalizeResult(result);
    }

    internal static ProcessResult NormalizeResult(ProcessResult result)
    {
        if (result.ExitCode != 0)
            return result;

        foreach (var line in result.StandardError.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (line.StartsWith("Exception:", StringComparison.OrdinalIgnoreCase) ||
                line.StartsWith("Missing target", StringComparison.OrdinalIgnoreCase) ||
                line.StartsWith("Error:", StringComparison.OrdinalIgnoreCase))
                return result with { ExitCode = 1 };
        }

        if (result.StandardOutput.Contains("Build failed", StringComparison.OrdinalIgnoreCase) ||
            result.StandardOutput.Contains("Compilation failed", StringComparison.OrdinalIgnoreCase))
            return result with { ExitCode = 1 };

        return result;
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
