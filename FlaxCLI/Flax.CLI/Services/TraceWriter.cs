// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal static class TraceWriter
{
    public static string Write(AppPaths paths, string requestId, string command, CliResult result, TimeSpan duration)
    {
        var directory = Path.Combine(paths.StateDirectory, "traces");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"{DateTime.UtcNow:yyyyMMdd-HHmmss}-{requestId}.json");
        var trace = new
        {
            schemaVersion = 1,
            requestId,
            command,
            exitCode = (int)result.ExitCode,
            durationMs = (long)duration.TotalMilliseconds,
            errors = result.Errors.Select(x => x.Code),
            warnings = result.Warnings.Select(x => x.Code),
            host = new
            {
                os = System.Runtime.InteropServices.RuntimeInformation.OSDescription,
                architecture = System.Runtime.InteropServices.RuntimeInformation.OSArchitecture.ToString(),
            },
        };
        AtomicFile.WriteText(path, JsonSerializer.Serialize(trace, JsonSupport.Options) + Environment.NewLine);
        return path;
    }
}
