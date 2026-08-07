// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;

namespace Flax.CLI.Core;

internal enum ExitCode
{
    Success = 0,
    InternalFailure = 1,
    Usage = 2,
    Authorization = 3,
    ContextRequired = 4,
    OperationFailed = 6,
    Interrupted = 130,
    Terminated = 143,
}

internal sealed record CliMessage(string Code, string Message, object? Details = null);

internal sealed class CliResult
{
    public ExitCode ExitCode { get; init; }
    public object? Data { get; init; }
    public List<CliMessage> Errors { get; } = [];
    public List<CliMessage> Warnings { get; } = [];
    public List<object> Events { get; } = [];

    public static CliResult Ok(object? data = null) => new() { Data = data };

    public static CliResult Fail(ExitCode exitCode, string code, string message, object? details = null)
    {
        var result = new CliResult { ExitCode = exitCode };
        result.Errors.Add(new CliMessage(code, message, details));
        return result;
    }
}

internal sealed class CliException(ExitCode exitCode, string code, string message, object? details = null) : Exception(message)
{
    public ExitCode ExitCode { get; } = exitCode;
    public string Code { get; } = code;
    public object? Details { get; } = details;
}

internal sealed class CommandContext
{
    public required GlobalOptions Options { get; init; }
    public required CancellationToken CancellationToken { get; init; }
    public required Stopwatch Stopwatch { get; init; }
}

internal sealed class GlobalOptions
{
    public string Format { get; set; } = "human";
    public bool FormatSpecified { get; set; }
    public bool Quiet { get; set; }
    public bool Verbose { get; set; }
    public bool NoColor { get; set; }
    public bool NonInteractive { get; set; }
    public bool Help { get; set; }
    public bool Version { get; set; }
    public bool Trace { get; set; }
    public string? Project { get; set; }
    public string? Engine { get; set; }
    public TimeSpan? Timeout { get; set; }
    public TimeSpan GracefulShutdownTimeout { get; set; } = TimeSpan.FromSeconds(10);
    public List<string> CommandTokens { get; } = [];
    public List<string> PassThrough { get; } = [];
}
