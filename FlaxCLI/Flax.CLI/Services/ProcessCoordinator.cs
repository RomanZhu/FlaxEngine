// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.Runtime.InteropServices;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed record ProcessResult(int ProcessId, int ExitCode, string StandardOutput, string StandardError);

internal sealed class ProcessCoordinator
{
    public ProcessResult StartDetached(string executable, IEnumerable<string> arguments, string workingDirectory)
    {
        var startInfo = Create(executable, arguments, workingDirectory, redirect: false);
        var process = Process.Start(startInfo) ?? throw new CliException(ExitCode.OperationFailed, "FLX-PROCESS-0006", $"Failed to start '{executable}'.");
        return new ProcessResult(process.Id, 0, string.Empty, string.Empty);
    }

    public async Task<ProcessResult> RunAsync(string executable, IEnumerable<string> arguments, string workingDirectory, CancellationToken cancellationToken, TimeSpan gracefulTimeout, TimeSpan? operationTimeout)
    {
        using var process = new Process { StartInfo = Create(executable, arguments, workingDirectory, redirect: true), EnableRaisingEvents = true };
        if (!process.Start())
            throw new CliException(ExitCode.OperationFailed, "FLX-PROCESS-0006", $"Failed to start '{executable}'.");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        using var timeoutCancellation = operationTimeout.HasValue ? CancellationTokenSource.CreateLinkedTokenSource(cancellationToken) : null;
        timeoutCancellation?.CancelAfter(operationTimeout!.Value);
        var effectiveCancellation = timeoutCancellation?.Token ?? cancellationToken;
        try
        {
            await process.WaitForExitAsync(effectiveCancellation);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited)
            {
                if (OperatingSystem.IsWindows())
                    process.CloseMainWindow();
                else
                    NativeMethods.Kill(process.Id, 15);
                using var timeout = new CancellationTokenSource(gracefulTimeout);
                try { await process.WaitForExitAsync(timeout.Token); } catch (OperationCanceledException) { }
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            }
            if (!cancellationToken.IsCancellationRequested)
                throw new CliException(ExitCode.OperationFailed, "FLX-PROCESS-0006", $"The child process exceeded the {operationTimeout!.Value.TotalSeconds:g} second timeout.");
            throw;
        }
        return new ProcessResult(process.Id, process.ExitCode, await stdout, await stderr);
    }

    private static ProcessStartInfo Create(string executable, IEnumerable<string> arguments, string workingDirectory, bool redirect)
    {
        var result = new ProcessStartInfo
        {
            FileName = executable,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = redirect,
            RedirectStandardOutput = redirect,
            RedirectStandardError = redirect,
        };
        foreach (var argument in arguments)
            result.ArgumentList.Add(argument);
        return result;
    }

    private static class NativeMethods
    {
        [DllImport("libc", EntryPoint = "kill", SetLastError = true)]
        private static extern int KillNative(int processId, int signal);

        public static void Kill(int processId, int signal)
        {
            _ = KillNative(processId, signal);
        }
    }
}
