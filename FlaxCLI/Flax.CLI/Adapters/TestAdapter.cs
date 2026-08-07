// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;
using Flax.CLI.Services;

namespace Flax.CLI.Adapters;

internal sealed class TestAdapter(ProcessCoordinator processes)
{
    public IReadOnlyList<object> List(EngineInfo engine, ProjectContext? project)
    {
        var result = new List<object>();
        Add(result, "native", Path.Combine(engine.Path, "Binaries", "Editor", PlatformFolder(), "Development", NativeTestName()), engine.Path);
        Add(result, "build", Path.Combine(engine.Path, "Binaries", "Tests", "Flax.Build.Tests.dll"), engine.Path);
        Add(result, "managed", Path.Combine(engine.Path, "Binaries", "Tests", "FlaxEngine.CSharp.dll"), engine.Path);
        if (project != null)
        {
            Add(result, "project-managed", Path.Combine(project.Root, "Binaries", "Tests", project.Name + ".dll"), project.Root);
            Add(result, "project-native", Path.Combine(project.Root, "Binaries", "Tests", NativeTestName()), project.Root);
        }
        return result;
    }

    public Task<ProcessResult> RunAsync(EngineInfo engine, ProjectContext? project, string kind, string? path, string? filter, IReadOnlyList<string> passThrough, CommandContext context)
    {
        kind = kind.ToLowerInvariant();
        var resolved = ResolvePath(engine, project, kind, path);
        var arguments = new List<string>();
        string executable;
        string workingDirectory;
        if (kind is "native" or "project-native")
        {
            executable = resolved;
            arguments.Add("-headless");
            if (!string.IsNullOrWhiteSpace(filter))
                arguments.Add("-test=" + filter);
            workingDirectory = Path.GetDirectoryName(resolved)!;
        }
        else
        {
            executable = "dotnet";
            arguments.Add("test");
            arguments.Add("-f");
            arguments.Add("net8.0");
            arguments.Add(resolved);
            if (!string.IsNullOrWhiteSpace(filter))
            {
                arguments.Add("--filter");
                arguments.Add(filter);
            }
            workingDirectory = project?.Root ?? engine.Path;
        }
        arguments.AddRange(passThrough);
        return processes.RunAsync(executable, arguments, workingDirectory, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
    }

    private static string ResolvePath(EngineInfo engine, ProjectContext? project, string kind, string? requested)
    {
        var defaultPath = kind switch
        {
            "native" => Path.Combine(engine.Path, "Binaries", "Editor", PlatformFolder(), "Development", NativeTestName()),
            "build" => Path.Combine(engine.Path, "Binaries", "Tests", "Flax.Build.Tests.dll"),
            "managed" => Path.Combine(engine.Path, "Binaries", "Tests", "FlaxEngine.CSharp.dll"),
            "project-managed" => project == null ? throw new CliException(ExitCode.ContextRequired, "FLX-TEST-0004", "project-managed tests require --project.") : Path.Combine(project.Root, "Binaries", "Tests", project.Name + ".dll"),
            "project-native" => project == null ? throw new CliException(ExitCode.ContextRequired, "FLX-TEST-0004", "project-native tests require --project.") : Path.Combine(project.Root, "Binaries", "Tests", NativeTestName()),
            _ => throw new CliException(ExitCode.Usage, "FLX-CLI-0002", $"Unknown test kind '{kind}'."),
        };
        var result = Path.GetFullPath(requested ?? defaultPath);
        var roots = new[] { engine.Path, project?.Root }.Where(x => !string.IsNullOrWhiteSpace(x)).Select(x => Path.GetFullPath(x!).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (!roots.Any(root => result.Equals(root, StringComparison.OrdinalIgnoreCase) || result.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)))
            throw new CliException(ExitCode.Authorization, "FLX-TEST-AUTH-0003", "Test executable or assembly must remain under the selected engine or project root.");
        if (!File.Exists(result))
            throw new CliException(ExitCode.ContextRequired, "FLX-TEST-0004", $"Test target '{result}' does not exist.");
        return result;
    }

    private static void Add(List<object> result, string kind, string path, string root)
    {
        result.Add(new { kind, path, available = File.Exists(path), root });
    }

    private static string PlatformFolder() => OperatingSystem.IsWindows() ? "Win64" : OperatingSystem.IsMacOS() ? "Mac" : "Linux";

    private static string NativeTestName() => OperatingSystem.IsWindows() ? "FlaxTests.exe" : "FlaxTests";
}
