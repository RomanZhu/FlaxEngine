// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Text.Json;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

/// <summary>Persistent records for commands launched with --detach.</summary>
internal sealed class JobStore(AppPaths paths)
{
    public string GetDirectory(ProjectContext? project)
    {
        var root = project == null ? Path.Combine(paths.StateDirectory, "jobs") : Path.Combine(project.Root, ".flax", "jobs");
        Directory.CreateDirectory(root);
        return root;
    }

    public JobRecord Start(ProjectContext? project, IReadOnlyList<string> originalArgs, string workingDirectory)
    {
        var id = $"job-{DateTime.UtcNow:yyyyMMddHHmmss}-{Guid.NewGuid():N}";
        var directory = GetDirectory(project);
        var recordPath = Path.Combine(directory, id + ".json");
        var stdoutPath = Path.Combine(directory, id + ".stdout.log");
        var stderrPath = Path.Combine(directory, id + ".stderr.log");
        var command = BuildChildCommand(originalArgs);
        var worker = BuildSelfCommand();
        worker.Arguments.Add("--format");
        worker.Arguments.Add("json");
        worker.Arguments.Add("jobs");
        worker.Arguments.Add("worker");
        worker.Arguments.Add("--record");
        worker.Arguments.Add(recordPath);
        worker.Arguments.Add("--");
        worker.Arguments.AddRange(command.Arguments);
        var start = new ProcessStartInfo
        {
            FileName = worker.FileName,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var arg in worker.Arguments)
            start.ArgumentList.Add(arg);
        var process = Process.Start(start) ?? throw new CliException(ExitCode.OperationFailed, "FLX-JOB-0006", "The detached job worker could not be started.");
        var record = new JobRecord
        {
            Id = id,
            State = "queued",
            Pid = process.Id,
            Project = project?.Root,
            Command = command.Arguments.ToArray(),
            FileName = command.FileName,
            CreatedUtc = DateTime.UtcNow,
            StartedUtc = DateTime.UtcNow,
            RecordPath = recordPath,
            StdoutPath = stdoutPath,
            StderrPath = stderrPath,
        };
        Write(record);
        return record;
    }

    public async Task<int> RunWorkerAsync(string recordPath, IReadOnlyList<string> targetArguments, CancellationToken cancellationToken)
    {
        JobRecord? record = null;
        try
        {
            record = JsonSerializer.Deserialize<JobRecord>(File.ReadAllText(recordPath), JsonSupport.Options) ?? throw new InvalidDataException("Detached job record is invalid.");
            record.State = "running";
            record.StartedUtc = DateTime.UtcNow;
            Write(record);
            var command = BuildChildCommand(targetArguments);
            var workingDirectory = !string.IsNullOrWhiteSpace(record.Project) && Directory.Exists(record.Project) ? record.Project : Environment.CurrentDirectory;
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = command.FileName,
                    WorkingDirectory = workingDirectory,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                },
            };
            foreach (var arg in command.Arguments) process.StartInfo.ArgumentList.Add(arg);
            if (!process.Start()) throw new InvalidOperationException("The detached target process could not be started.");
            record.TargetPid = process.Id;
            Write(record);
            var output = process.StandardOutput.ReadToEndAsync(cancellationToken);
            var error = process.StandardError.ReadToEndAsync(cancellationToken);
            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            await File.WriteAllTextAsync(record.StdoutPath, await output.ConfigureAwait(false), cancellationToken).ConfigureAwait(false);
            await File.WriteAllTextAsync(record.StderrPath, await error.ConfigureAwait(false), cancellationToken).ConfigureAwait(false);
            var current = JsonSerializer.Deserialize<JobRecord>(File.ReadAllText(recordPath), JsonSupport.Options) ?? record;
            if (!string.Equals(current.State, "cancelled", StringComparison.OrdinalIgnoreCase))
            {
                current.ExitCode = process.ExitCode;
                current.State = process.ExitCode == 0 ? "succeeded" : "failed";
                current.FinishedUtc = DateTime.UtcNow;
                Write(current);
            }
            return process.ExitCode;
        }
        catch (OperationCanceledException)
        {
            if (record != null)
            {
                record.State = "cancelled";
                record.ExitCode = 130;
                record.FinishedUtc = DateTime.UtcNow;
                Write(record);
            }
            return 130;
        }
        catch (Exception ex)
        {
            if (record != null)
            {
                record.State = "failed";
                record.Error = ex.Message;
                record.ExitCode = 1;
                record.FinishedUtc = DateTime.UtcNow;
                Write(record);
            }
            return 1;
        }
    }

    public IReadOnlyList<JobRecord> List(ProjectContext? project)
    {
        var directory = GetDirectory(project);
        var records = new List<JobRecord>();
        foreach (var path in Directory.EnumerateFiles(directory, "job-*.json"))
        {
            try
            {
                var record = JsonSerializer.Deserialize<JobRecord>(File.ReadAllText(path), JsonSupport.Options);
                if (record != null)
                    records.Add(Refresh(record));
            }
            catch (Exception ex) when (ex is IOException or JsonException)
            {
                // A partially written record is ignored; the atomic writer will repair it on the next update.
            }
        }
        return records.OrderByDescending(x => x.CreatedUtc).ToArray();
    }

    public JobRecord Require(string id, ProjectContext? project)
    {
        var match = List(project).FirstOrDefault(x => string.Equals(x.Id, id, StringComparison.OrdinalIgnoreCase) || x.Id.StartsWith(id, StringComparison.OrdinalIgnoreCase));
        if (match == null)
            throw new CliException(ExitCode.ContextRequired, "FLX-JOB-NOTFOUND-0004", $"Detached job '{id}' was not found.");
        return match;
    }

    public JobRecord Refresh(JobRecord record)
    {
        if (record.State is "succeeded" or "failed" or "cancelled")
            return record;
        try
        {
            using var process = Process.GetProcessById(record.Pid);
            if (!process.HasExited)
                return record;
        }
        catch (ArgumentException)
        {
            // The process has exited and may no longer have a process table entry.
        }
        var exitCode = record.ExitCode;
        if (record.ExitCode == null)
            exitCode = TryReadExitCode(record.StdoutPath);
        record.State = exitCode.GetValueOrDefault() == 0 ? "succeeded" : "failed";
        record.ExitCode = exitCode ?? 1;
        record.FinishedUtc ??= DateTime.UtcNow;
        Write(record);
        return record;
    }

    public JobRecord Cancel(JobRecord record)
    {
        if (record.State is "succeeded" or "failed" or "cancelled")
            return record;
        try
        {
            using var process = Process.GetProcessById(record.Pid);
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
        }
        catch (ArgumentException) { }
        record.State = "cancelled";
        record.FinishedUtc = DateTime.UtcNow;
        record.ExitCode = 130;
        Write(record);
        return record;
    }

    public void Prune(ProjectContext? project, TimeSpan age)
    {
        foreach (var record in List(project).Where(x => x.FinishedUtc.HasValue && DateTime.UtcNow - x.FinishedUtc.Value > age))
        {
            TryDelete(record.RecordPath);
            TryDelete(record.StdoutPath);
            TryDelete(record.StderrPath);
        }
    }

    private static (string FileName, List<string> Arguments) BuildChildCommand(IReadOnlyList<string> originalArgs)
    {
        var command = BuildSelfCommand();
        var processPath = command.FileName;
        var args = originalArgs.Where(x => !string.Equals(x, "--detach", StringComparison.OrdinalIgnoreCase)).ToList();
        if (!args.Any(x => string.Equals(x, "--format", StringComparison.OrdinalIgnoreCase) || x.StartsWith("--format=", StringComparison.OrdinalIgnoreCase)))
        {
            args.Insert(0, "--format");
            args.Insert(1, "json");
        }
        if (Path.GetFileNameWithoutExtension(processPath).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            var entry = command.Arguments.FirstOrDefault(x => x.EndsWith(".dll", StringComparison.OrdinalIgnoreCase));
            if (!string.IsNullOrWhiteSpace(entry))
            {
                args.Insert(0, entry);
                return (processPath, args);
            }
        }
        return (processPath, args);
    }

    private static (string FileName, List<string> Arguments) BuildSelfCommand()
    {
        var processPath = Environment.ProcessPath ?? throw new CliException(ExitCode.InternalFailure, "FLX-JOB-0006", "The current process path is unavailable.");
        var raw = Environment.GetCommandLineArgs();
        if (Path.GetFileNameWithoutExtension(processPath).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            var entry = raw.FirstOrDefault(x => x.EndsWith(".dll", StringComparison.OrdinalIgnoreCase));
            return (processPath, entry == null ? [] : [entry]);
        }
        // Apphost executables do not need the parent's arguments when launching a worker.
        return (processPath, []);
    }

    private void Write(JobRecord record)
    {
        AtomicFile.WriteText(record.RecordPath, JsonSerializer.Serialize(record, JsonSupport.Options) + Environment.NewLine);
    }

    private static int? TryReadExitCode(string path)
    {
        try
        {
            var text = File.ReadAllText(path);
            using var doc = JsonDocument.Parse(text);
            if (doc.RootElement.TryGetProperty("exitCode", out var value) && value.TryGetInt32(out var code))
                return code;
        }
        catch { }
        return null;
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }
}

internal sealed class JobRecord
{
    public string Id { get; set; } = string.Empty;
    public string State { get; set; } = "queued";
    public int Pid { get; set; }
    public string? Project { get; set; }
    public string FileName { get; set; } = string.Empty;
    public string[] Command { get; set; } = [];
    public DateTime CreatedUtc { get; set; }
    public DateTime? StartedUtc { get; set; }
    public DateTime? FinishedUtc { get; set; }
    public int? ExitCode { get; set; }
    public string RecordPath { get; set; } = string.Empty;
    public string StdoutPath { get; set; } = string.Empty;
    public string StderrPath { get; set; } = string.Empty;
    public int? TargetPid { get; set; }
    public string? Error { get; set; }
}
