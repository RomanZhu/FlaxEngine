// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text;

namespace Flax.CLI.Core;

internal static class AtomicFile
{
    public static void WriteText(string path, string contents)
    {
        var directory = Path.GetDirectoryName(path) ?? throw new ArgumentException("A file path is required.", nameof(path));
        Directory.CreateDirectory(directory);
        using var fileLock = AcquireLock(path + ".lock", TimeSpan.FromSeconds(10));
        var temporaryPath = Path.Combine(directory, $".{Path.GetFileName(path)}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp");
        try
        {
            using (var stream = new FileStream(temporaryPath, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(contents);
                writer.Flush();
                stream.Flush(true);
            }
            File.Move(temporaryPath, path, true);
        }
        finally
        {
            if (File.Exists(temporaryPath))
                File.Delete(temporaryPath);
        }
    }

    public static FileStream AcquireLock(string lockPath, TimeSpan timeout)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(lockPath)!);
        var started = DateTime.UtcNow;
        while (true)
        {
            try
            {
                var stream = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                stream.SetLength(0);
                using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true);
                writer.Write($"{Environment.ProcessId}|{DateTimeOffset.UtcNow:O}");
                writer.Flush();
                stream.Position = 0;
                return stream;
            }
            catch (IOException) when (DateTime.UtcNow - started < timeout)
            {
                Thread.Sleep(50);
            }
            catch (IOException ex)
            {
                throw new CliException(ExitCode.ContextRequired, "FLX-STATE-0004", $"Timed out waiting for the registry lock '{lockPath}'.", new { exception = ex.Message });
            }
        }
    }
}
