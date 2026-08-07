// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;

namespace Flax.CLI.Core;

internal sealed record ProjectContext(
    string Root,
    string ProjectFile,
    string Name,
    SemanticVersion MinimumEngineVersion,
    string? EngineNickname)
{
    public string LockFile => Path.Combine(Root, ".flax", "engine.lock");
    public string ProjectConfigFile => Path.Combine(Root, ".flax", "cli.json");

    public static ProjectContext Find(string? input)
    {
        var searchParents = string.IsNullOrWhiteSpace(input);
        var candidate = Path.GetFullPath(searchParents ? Environment.CurrentDirectory : input!);
        string projectFile;
        if (File.Exists(candidate))
        {
            if (!candidate.EndsWith(".flaxproj", StringComparison.OrdinalIgnoreCase))
                throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"'{candidate}' is not a .flaxproj file.");
            projectFile = candidate;
        }
        else if (Directory.Exists(candidate))
        {
            var directory = new DirectoryInfo(candidate);
            while (true)
            {
                var files = Directory.GetFiles(directory.FullName, "*.flaxproj", SearchOption.TopDirectoryOnly);
                if (files.Length > 1)
                    throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"More than one .flaxproj file exists in '{directory.FullName}'.", new { candidates = files });
                if (files.Length == 1)
                {
                    projectFile = files[0];
                    break;
                }
                if (!searchParents || directory.Parent == null)
                    throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"No .flaxproj file exists in '{candidate}' or its parents.");
                directory = directory.Parent;
            }
        }
        else
        {
            throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"Project path '{candidate}' does not exist.");
        }

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(projectFile), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            var root = document.RootElement;
            var name = ReadString(root, "Name") ?? Path.GetFileNameWithoutExtension(projectFile);
            var nickname = ReadString(root, "EngineNickname");
            var version = ReadVersion(root, "MinEngineVersion");
            return new ProjectContext(Path.GetDirectoryName(projectFile)!, projectFile, name, version, nickname);
        }
        catch (JsonException ex)
        {
            throw new CliException(ExitCode.ContextRequired, "FLX-PROJECT-0004", $"Project file '{projectFile}' is invalid JSON.", new { exception = ex.Message });
        }
    }

    internal static SemanticVersion ReadVersion(JsonElement root, string propertyName)
    {
        if (!root.TryGetProperty(propertyName, out var element))
            return default;
        if (element.ValueKind == JsonValueKind.String && SemanticVersion.TryParse(element.GetString(), out var stringVersion))
            return stringVersion;
        if (element.ValueKind != JsonValueKind.Object)
            return default;
        var major = ReadInt(element, "Major");
        var minor = ReadInt(element, "Minor");
        var revision = ReadInt(element, "Revision");
        var build = ReadInt(element, "Build");
        return new SemanticVersion(major, minor, revision, build);
    }

    private static int ReadInt(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var value) && value.TryGetInt32(out var result) ? result : 0;

    private static string? ReadString(JsonElement root, string propertyName) =>
        root.TryGetProperty(propertyName, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
}

internal sealed class EngineLock
{
    public int SchemaVersion { get; set; } = 1;
    public EngineLockIdentity Engine { get; set; } = new();
}

internal sealed class EngineLockIdentity
{
    public string? Version { get; set; }
    public string? Channel { get; set; }
    public string? Nickname { get; set; }
    public string? Fingerprint { get; set; }
    public string? Source { get; set; }
}
