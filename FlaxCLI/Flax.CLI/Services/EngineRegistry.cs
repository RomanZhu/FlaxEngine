// Copyright (c) Wojciech Figat. All rights reserved.

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed class EngineMetadataState
{
    public int SchemaVersion { get; set; } = 1;
    public List<EngineMetadata> Engines { get; set; } = [];
}

internal sealed class EngineMetadata
{
    public string Path { get; set; } = string.Empty;
    public string? Id { get; set; }
    public string? Nickname { get; set; }
    public string Source { get; set; } = "local";
    public string Channel { get; set; } = "local";
    public string? Fingerprint { get; set; }
}

internal sealed record EngineInfo(
    string Id,
    string Path,
    string Version,
    string? Nickname,
    string Source,
    string Channel,
    string Fingerprint,
    bool IsDefault,
    bool IsValid,
    string? EditorPath,
    string? BuildToolPath);

internal sealed class EngineRegistry(AppPaths paths)
{
    public IReadOnlyList<EngineInfo> List()
    {
        var (roots, defaultIndex) = ReadLauncher();
        var metadata = ReadMetadata();
        return roots.Select((root, index) => Inspect(root, metadata.Engines.FirstOrDefault(x => PathEquals(x.Path, root)), index == defaultIndex)).ToArray();
    }

    public EngineInfo Add(string root, string? nickname, string source, string channel)
    {
        using var transaction = AtomicFile.AcquireLock(paths.VersionsFile + ".transaction.lock", TimeSpan.FromSeconds(10));
        root = Normalize(root);
        if (!Directory.Exists(root) || !File.Exists(Path.Combine(root, "Flax.flaxproj")))
            throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"'{root}' is not a Flax engine root (Flax.flaxproj is missing).");
        var (roots, defaultIndex) = ReadLauncher();
        if (!roots.Any(x => PathEquals(x, root)))
            roots.Add(root);
        WriteLauncher(roots, defaultIndex < 0 ? 0 : defaultIndex);

        var state = ReadMetadata();
        var existing = state.Engines.FirstOrDefault(x => PathEquals(x.Path, root));
        if (existing == null)
        {
            existing = new EngineMetadata { Path = root };
            state.Engines.Add(existing);
        }
        existing.Nickname = nickname ?? existing.Nickname;
        existing.Source = source;
        existing.Channel = channel;
        var inspected = Inspect(root, existing, roots.Count == 1);
        existing.Id = inspected.Id;
        existing.Fingerprint = inspected.Fingerprint;
        WriteMetadata(state);
        return inspected;
    }

    public bool Remove(string selector)
    {
        using var transaction = AtomicFile.AcquireLock(paths.VersionsFile + ".transaction.lock", TimeSpan.FromSeconds(10));
        var selected = ResolveSelector(selector);
        var (roots, defaultIndex) = ReadLauncher();
        var index = roots.FindIndex(x => PathEquals(x, selected.Path));
        if (index < 0)
            return false;
        roots.RemoveAt(index);
        if (roots.Count == 0)
            defaultIndex = -1;
        else if (defaultIndex == index)
            defaultIndex = Math.Min(index, roots.Count - 1);
        else if (defaultIndex > index)
            defaultIndex--;
        WriteLauncher(roots, defaultIndex);
        var state = ReadMetadata();
        state.Engines.RemoveAll(x => PathEquals(x.Path, selected.Path));
        WriteMetadata(state);
        return true;
    }

    public EngineInfo SetDefault(string selector)
    {
        using var transaction = AtomicFile.AcquireLock(paths.VersionsFile + ".transaction.lock", TimeSpan.FromSeconds(10));
        var selected = ResolveSelector(selector);
        var (roots, _) = ReadLauncher();
        var index = roots.FindIndex(x => PathEquals(x, selected.Path));
        WriteLauncher(roots, index);
        return selected with { IsDefault = true };
    }

    public EngineInfo? GetDefault() => List().FirstOrDefault(x => x.IsDefault);

    public EngineInfo ResolveSelector(string selector)
    {
        if (Path.IsPathFullyQualified(selector) || selector.StartsWith(".", StringComparison.Ordinal))
        {
            var path = Normalize(selector);
            return List().FirstOrDefault(x => PathEquals(x.Path, path))
                ?? throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Engine path '{path}' is not registered.");
        }
        var engines = List();
        if (selector.Equals("default", StringComparison.OrdinalIgnoreCase))
            return engines.FirstOrDefault(x => x.IsDefault) ?? throw Missing(selector, engines);
        var normalizedSelector = selector.ToLowerInvariant();
        IEnumerable<EngineInfo> candidates = normalizedSelector switch
        {
            "latest" => engines.Where(x => x.IsValid),
            "stable" => engines.Where(x => x.IsValid && x.Channel.Equals("stable", StringComparison.OrdinalIgnoreCase)),
            "daily" => engines.Where(x => x.IsValid && x.Channel.Equals("daily", StringComparison.OrdinalIgnoreCase)),
            _ => engines.Where(x =>
                x.Id.StartsWith(selector, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(x.Nickname, selector, StringComparison.OrdinalIgnoreCase) ||
                x.Version.Equals(selector, StringComparison.OrdinalIgnoreCase) ||
                x.Version.StartsWith(selector + ".", StringComparison.OrdinalIgnoreCase)),
        };
        var matches = candidates.ToList();
        if (matches.Count == 0)
            throw Missing(selector, engines);
        if (normalizedSelector is "latest" or "stable" or "daily")
            return matches.OrderByDescending(x => ParseVersion(x.Version)).First();
        if (matches.Count != 1)
            throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Engine selector '{selector}' is ambiguous.", new { candidates = matches.Select(Summary) });
        return matches[0];
    }

    private static CliException Missing(string selector, IEnumerable<EngineInfo> engines) =>
        new(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Engine selector '{selector}' did not match a registered engine.", new { candidates = engines.Select(Summary) });

    private static object Summary(EngineInfo x) => new { x.Id, x.Version, x.Nickname, x.Path };

    private EngineInfo Inspect(string root, EngineMetadata? metadata, bool isDefault)
    {
        var projectFile = Path.Combine(root, "Flax.flaxproj");
        var version = ReadEngineVersion(projectFile);
        var editor = FindEditor(root);
        var buildTool = FindBuildTool(root);
        var fingerprint = Fingerprint(projectFile, editor);
        var id = metadata?.Id ?? StableId(root);
        return new EngineInfo(id, root, version.ToString(), metadata?.Nickname, metadata?.Source ?? "local", metadata?.Channel ?? "local", fingerprint, isDefault, File.Exists(projectFile) && editor != null, editor, buildTool);
    }

    private static SemanticVersion ReadEngineVersion(string projectFile)
    {
        if (!File.Exists(projectFile))
            return default;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(projectFile), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            return ProjectContext.ReadVersion(document.RootElement, "Version");
        }
        catch (JsonException)
        {
            return default;
        }
    }

    internal static string? FindEditor(string root)
    {
        var platform = OperatingSystem.IsWindows() ? "Win64" : OperatingSystem.IsMacOS() ? "Mac" : "Linux";
        var name = OperatingSystem.IsWindows() ? "FlaxEditor.exe" : "FlaxEditor";
        foreach (var configuration in new[] { "Development", "Release", "Debug" })
        {
            var candidate = Path.Combine(root, "Binaries", "Editor", platform, configuration, name);
            if (File.Exists(candidate))
                return candidate;
        }
        return null;
    }

    internal static string? FindBuildTool(string root)
    {
        foreach (var name in OperatingSystem.IsWindows() ? new[] { "Flax.Build.exe", "Flax.Build.dll" } : new[] { "Flax.Build", "Flax.Build.dll" })
        {
            var candidate = Path.Combine(root, "Binaries", "Tools", name);
            if (File.Exists(candidate))
                return candidate;
        }
        return null;
    }

    private static string Fingerprint(string projectFile, string? editor)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        if (File.Exists(projectFile))
            hash.AppendData(File.ReadAllBytes(projectFile));
        if (editor != null)
        {
            var info = new FileInfo(editor);
            hash.AppendData(Encoding.UTF8.GetBytes($"{info.Name}|{info.Length}|{info.LastWriteTimeUtc.Ticks}"));
        }
        return "sha256:" + Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
    }

    private static string StableId(string root)
    {
        var normalized = Normalize(root);
        if (OperatingSystem.IsWindows() || OperatingSystem.IsMacOS())
            normalized = normalized.ToUpperInvariant();
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(normalized));
        return Convert.ToHexString(hash)[..12].ToLowerInvariant();
    }

    private (List<string> Roots, int DefaultIndex) ReadLauncher()
    {
        if (!File.Exists(paths.VersionsFile))
            return ([], -1);
        var lines = File.ReadAllLines(paths.VersionsFile).Select(x => x.Trim()).Where(x => x.Length != 0).ToList();
        var defaultIndex = lines.Count != 0 && int.TryParse(lines[0], out var parsed) ? parsed : -1;
        if (defaultIndex >= 0)
            lines.RemoveAt(0);
        return (lines.Select(Normalize).Distinct(ProjectRegistry.PathComparer).ToList(), defaultIndex);
    }

    private void WriteLauncher(IReadOnlyList<string> roots, int defaultIndex)
    {
        var lines = new List<string> { defaultIndex.ToString() };
        lines.AddRange(roots);
        AtomicFile.WriteText(paths.VersionsFile, string.Join(Environment.NewLine, lines) + Environment.NewLine);
    }

    private EngineMetadataState ReadMetadata()
    {
        if (!File.Exists(paths.EngineMetadataFile))
            return new EngineMetadataState();
        try
        {
            return JsonSerializer.Deserialize<EngineMetadataState>(File.ReadAllText(paths.EngineMetadataFile), JsonSupport.Options) ?? new EngineMetadataState();
        }
        catch (JsonException ex)
        {
            throw new CliException(ExitCode.ContextRequired, "FLX-STATE-0004", $"Engine metadata '{paths.EngineMetadataFile}' is invalid.", new { exception = ex.Message });
        }
    }

    private void WriteMetadata(EngineMetadataState state) =>
        AtomicFile.WriteText(paths.EngineMetadataFile, JsonSerializer.Serialize(state, JsonSupport.Options) + Environment.NewLine);

    private static string Normalize(string value) => Path.TrimEndingDirectorySeparator(Path.GetFullPath(value));
    private static bool PathEquals(string left, string right) => ProjectRegistry.PathComparer.Equals(Normalize(left), Normalize(right));
    private static SemanticVersion ParseVersion(string value) => SemanticVersion.TryParse(value, out var result) ? result : default;
}
