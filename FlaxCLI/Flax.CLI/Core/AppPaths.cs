// Copyright (c) Wojciech Figat. All rights reserved.

namespace Flax.CLI.Core;

internal sealed class AppPaths
{
    public string LauncherDirectory { get; }
    public string ConfigDirectory { get; }
    public string StateDirectory { get; }
    public string CacheDirectory { get; }
    public string RuntimeDirectory { get; }
    public string ConfigFile => Path.Combine(ConfigDirectory, "config.json");
    public string EngineMetadataFile => Path.Combine(StateDirectory, "engines.json");
    public string VersionsFile => Path.Combine(LauncherDirectory, "Versions.txt");
    public string ProjectsFile => Path.Combine(LauncherDirectory, "Projects.txt");

    public AppPaths(string? isolatedRoot = null)
    {
        if (isolatedRoot != null)
        {
            isolatedRoot = Path.GetFullPath(isolatedRoot);
            LauncherDirectory = Path.Combine(isolatedRoot, "launcher");
            ConfigDirectory = Path.Combine(isolatedRoot, "config");
            StateDirectory = Path.Combine(isolatedRoot, "state");
            CacheDirectory = Path.Combine(isolatedRoot, "cache");
            RuntimeDirectory = Path.Combine(isolatedRoot, "runtime");
            return;
        }
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (OperatingSystem.IsWindows())
        {
            LauncherDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Flax");
            var local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Flax", "CLI");
            ConfigDirectory = Path.Combine(LauncherDirectory, "CLI");
            StateDirectory = Path.Combine(local, "state");
            CacheDirectory = Environment.GetEnvironmentVariable("FLAX_CACHE_PATH") ?? Path.Combine(local, "cache");
            RuntimeDirectory = Path.Combine(local, "runtime");
        }
        else if (OperatingSystem.IsMacOS())
        {
            LauncherDirectory = Path.Combine(home, "Library", "Application Support", "Flax");
            ConfigDirectory = Path.Combine(LauncherDirectory, "CLI");
            StateDirectory = Path.Combine(LauncherDirectory, "CLI", "state");
            CacheDirectory = Environment.GetEnvironmentVariable("FLAX_CACHE_PATH") ?? Path.Combine(home, "Library", "Caches", "Flax", "CLI");
            RuntimeDirectory = Path.Combine(CacheDirectory, "runtime");
        }
        else
        {
            LauncherDirectory = Path.Combine(Environment.GetEnvironmentVariable("XDG_CONFIG_HOME") ?? Path.Combine(home, ".config"), "flax");
            ConfigDirectory = LauncherDirectory;
            StateDirectory = Path.Combine(Environment.GetEnvironmentVariable("XDG_STATE_HOME") ?? Path.Combine(home, ".local", "state"), "flax");
            CacheDirectory = Environment.GetEnvironmentVariable("FLAX_CACHE_PATH") ?? Path.Combine(Environment.GetEnvironmentVariable("XDG_CACHE_HOME") ?? Path.Combine(home, ".cache"), "flax");
            RuntimeDirectory = Path.Combine(Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR") ?? Path.GetTempPath(), $"flax-{Environment.UserName}");
        }
    }
}
