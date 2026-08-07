// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using Flax.CLI.Core;
using Flax.CLI.Protocol;
using Flax.CLI.Services;

namespace Flax.CLI.Adapters;

internal sealed class EditorAdapter(ProcessCoordinator processes)
{
    internal sealed record BuildInvocationResult(ProcessResult Process, bool Structured, JsonElement? Result, IReadOnlyList<JsonElement> Events);
    internal sealed record AssetInvocationResult(ProcessResult Process, bool Structured, JsonElement? Result, IReadOnlyList<JsonElement> Events);
    internal sealed record CommandInvocationResult(ProcessResult Process, bool Structured, JsonElement? Result, IReadOnlyList<JsonElement> Events);

    public ProcessResult Open(EngineInfo engine, ProjectContext project, bool play, IReadOnlyList<string> passThrough)
    {
        var arguments = new List<string> { "-project", project.ProjectFile };
        if (play)
            arguments.Add("-play");
        arguments.AddRange(passThrough);
        return processes.StartDetached(RequireEditor(engine), arguments, project.Root);
    }

    public async Task<BuildInvocationResult> BuildAsync(EngineInfo engine, ProjectContext project, string preset, string target, string? outputPath, IReadOnlyList<string> customDefines, bool clean, bool runAfterBuild, IReadOnlyList<string> passThrough, CommandContext context)
    {
        var requestId = Guid.NewGuid().ToString("N");
        var requestDirectory = Path.Combine(Path.GetTempPath(), "Flax", "CLI", "requests", requestId);
        var requestPath = Path.Combine(requestDirectory, "request.json");
        var eventPath = Path.Combine(requestDirectory, "events.ndjson");
        var resultPath = Path.Combine(requestDirectory, "result.json");
        Directory.CreateDirectory(requestDirectory);
        var request = new GameBuildRequest
        {
            RequestId = requestId,
            ProjectPath = project.Root,
            Preset = preset,
            Target = target,
            OutputPath = outputPath == null ? null : Path.GetFullPath(outputPath, project.Root),
            CustomDefines = customDefines.Count == 0 ? null : customDefines.ToArray(),
            Options = new GameBuildRequestOptions { Clean = clean, RunAfterBuild = runAfterBuild },
            EventPath = eventPath,
            ResultPath = resultPath,
        };
        File.WriteAllText(requestPath, JsonSerializer.Serialize(request, JsonSupport.Options));
        try
        {
            var arguments = CreateBuildArguments(project, preset, target, requestPath, clean, passThrough);
            var process = await processes.RunAsync(RequireEditor(engine), arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
            var structured = File.Exists(resultPath);
            JsonElement? result = structured ? ParseJson(File.ReadAllText(resultPath), resultPath) : null;
            var events = new List<JsonElement>();
            if (File.Exists(eventPath))
            {
                foreach (var line in File.ReadLines(eventPath).Where(x => !string.IsNullOrWhiteSpace(x)))
                    events.Add(ParseJson(line, eventPath));
            }
            return new BuildInvocationResult(process, structured, result, events);
        }
        finally
        {
            try
            {
                if (Directory.Exists(requestDirectory))
                    Directory.Delete(requestDirectory, recursive: true);
            }
            catch (IOException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
            catch (UnauthorizedAccessException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
        }
    }

    public async Task<AssetInvocationResult> AssetAsync(EngineInfo engine, ProjectContext project, AssetRequestOptions options, IReadOnlyList<string> passThrough, CommandContext context)
    {
        var requestId = Guid.NewGuid().ToString("N");
        var requestDirectory = Path.Combine(Path.GetTempPath(), "Flax", "CLI", "requests", requestId);
        var requestPath = Path.Combine(requestDirectory, "request.json");
        var eventPath = Path.Combine(requestDirectory, "events.ndjson");
        var resultPath = Path.Combine(requestDirectory, "result.json");
        Directory.CreateDirectory(requestDirectory);
        var request = new AssetRequest
        {
            RequestId = requestId,
            ProjectPath = project.Root,
            Asset = options,
            EventPath = eventPath,
            ResultPath = resultPath,
        };
        File.WriteAllText(requestPath, JsonSerializer.Serialize(request, JsonSupport.Options));
        try
        {
            var arguments = CreateAssetArguments(project, requestPath, passThrough);
            var process = await processes.RunAsync(RequireEditor(engine), arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
            var structured = File.Exists(resultPath);
            JsonElement? result = structured ? ParseJson(File.ReadAllText(resultPath), resultPath) : null;
            var events = new List<JsonElement>();
            if (File.Exists(eventPath))
            {
                foreach (var line in File.ReadLines(eventPath).Where(x => !string.IsNullOrWhiteSpace(x)))
                    events.Add(ParseJson(line, eventPath));
            }
            return new AssetInvocationResult(process, structured, result, events);
        }
        finally
        {
            try
            {
                if (Directory.Exists(requestDirectory))
                    Directory.Delete(requestDirectory, recursive: true);
            }
            catch (IOException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
            catch (UnauthorizedAccessException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
        }
    }

    public async Task<CommandInvocationResult> CommandAsync(EngineInfo engine, ProjectContext project, EditorCommandRequestOptions options, CommandContext context)
    {
        var requestId = Guid.NewGuid().ToString("N");
        var requestDirectory = Path.Combine(Path.GetTempPath(), "Flax", "CLI", "requests", requestId);
        var requestPath = Path.Combine(requestDirectory, "request.json");
        var eventPath = Path.Combine(requestDirectory, "events.ndjson");
        var resultPath = Path.Combine(requestDirectory, "result.json");
        Directory.CreateDirectory(requestDirectory);
        var request = new EditorCommandRequest
        {
            RequestId = requestId,
            ProjectPath = project.Root,
            Command = options,
            EventPath = eventPath,
            ResultPath = resultPath,
        };
        File.WriteAllText(requestPath, JsonSerializer.Serialize(request, JsonSupport.Options));
        try
        {
            var arguments = CreateCommandArguments(project, requestPath);
            var process = await processes.RunAsync(RequireEditor(engine), arguments, project.Root, context.CancellationToken, context.Options.GracefulShutdownTimeout, context.Options.Timeout);
            var structured = File.Exists(resultPath);
            JsonElement? result = structured ? ParseJson(File.ReadAllText(resultPath), resultPath) : null;
            var events = new List<JsonElement>();
            if (File.Exists(eventPath))
            {
                foreach (var line in File.ReadLines(eventPath).Where(x => !string.IsNullOrWhiteSpace(x)))
                    events.Add(ParseJson(line, eventPath));
            }
            return new CommandInvocationResult(process, structured, result, events);
        }
        finally
        {
            try
            {
                if (Directory.Exists(requestDirectory))
                    Directory.Delete(requestDirectory, recursive: true);
            }
            catch (IOException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
            catch (UnauthorizedAccessException)
            {
                // A failed cleanup must not replace the Editor operation result.
            }
        }
    }

    internal static List<string> CreateBuildArguments(ProjectContext project, string preset, string target, string requestPath, bool clean, IReadOnlyList<string> passThrough)
    {
        var arguments = new List<string>
        {
            "-project", project.ProjectFile,
            "-headless", "-mute", "-null", "-std",
            "-cliRequest", requestPath,
            "-build", $"{preset}.{target}",
        };
        if (clean)
            arguments.Add("-clearcooker");
        arguments.AddRange(passThrough);
        return arguments;
    }

    internal static List<string> CreateAssetArguments(ProjectContext project, string requestPath, IReadOnlyList<string> passThrough)
    {
        var arguments = new List<string>
        {
            "-project", project.ProjectFile,
            "-headless", "-mute", "-null", "-std", "-exit",
            "-cliRequest", requestPath,
        };
        arguments.AddRange(passThrough);
        return arguments;
    }

    internal static List<string> CreateCommandArguments(ProjectContext project, string requestPath)
    {
        return
        [
            "-project", project.ProjectFile,
            "-headless", "-mute", "-null", "-std", "-exit",
            "-cliRequest", requestPath,
        ];
    }

    private static JsonElement ParseJson(string value, string path)
    {
        try
        {
            using var document = JsonDocument.Parse(value);
            return document.RootElement.Clone();
        }
        catch (JsonException ex)
        {
            throw new CliException(ExitCode.OperationFailed, "FLX-PROTOCOL-0006", $"Editor protocol output '{path}' is invalid JSON.", new { exception = ex.Message });
        }
    }

    private static string RequireEditor(EngineInfo engine) => engine.EditorPath
        ?? throw new CliException(ExitCode.ContextRequired, "FLX-EDITOR-0004", $"A FlaxEditor executable was not found in '{engine.Path}'.");
}
