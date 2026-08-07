// Copyright (c) Wojciech Figat. All rights reserved.

using System.Text.Json;
using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed class ContextResolver(EngineRegistry engines, ConfigStore config)
{
    public EngineInfo Resolve(ProjectContext? project, string? explicitSelector)
    {
        if (!string.IsNullOrWhiteSpace(explicitSelector))
            return RequireCompatible(engines.ResolveSelector(explicitSelector), project);

        if (project != null && File.Exists(project.LockFile))
            return ResolveLock(project);

        if (!string.IsNullOrWhiteSpace(project?.EngineNickname))
        {
            try
            {
                return RequireCompatible(engines.ResolveSelector(project.EngineNickname), project);
            }
            catch (CliException ex) when (ex.ExitCode == ExitCode.ContextRequired)
            {
                throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Project engine nickname '{project.EngineNickname}' has no exact registered match.", ex.Details);
            }
        }

        var compatible = engines.List().Where(x => x.IsValid && IsCompatible(x, project)).ToList();
        var stable = compatible.Where(x => x.Channel.Equals("stable", StringComparison.OrdinalIgnoreCase)).OrderByDescending(x => Parse(x.Version)).FirstOrDefault();
        if (stable != null)
            return stable;

        var configured = config.GetString("defaultEngine", project?.ProjectConfigFile) ?? engines.GetDefault()?.Id;
        if (!string.IsNullOrWhiteSpace(configured))
        {
            try
            {
                return RequireCompatible(engines.ResolveSelector(configured), project);
            }
            catch (CliException)
            {
                // Continue to the single-compatible-engine rule. Invalid defaults are reported as candidates below.
            }
        }

        if (compatible.Count == 1)
            return compatible[0];
        throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", "An engine could not be selected without ambiguity.", new
        {
            minimumVersion = project?.MinimumEngineVersion.ToString(),
            candidates = compatible.Select(x => new { x.Id, x.Version, x.Nickname, x.Path }),
        });
    }

    public void Pin(ProjectContext project, EngineInfo engine)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(project.LockFile)!);
        var value = new EngineLock
        {
            Engine = new EngineLockIdentity
            {
                Version = engine.Version,
                Channel = engine.Channel,
                Nickname = engine.Nickname,
                Fingerprint = engine.Fingerprint,
                Source = engine.Source,
            },
        };
        AtomicFile.WriteText(project.LockFile, JsonSerializer.Serialize(value, JsonSupport.Options) + Environment.NewLine);
    }

    private EngineInfo ResolveLock(ProjectContext project)
    {
        EngineLock value;
        try
        {
            value = JsonSerializer.Deserialize<EngineLock>(File.ReadAllText(project.LockFile), JsonSupport.Options)
                ?? throw new JsonException("The lock file is empty.");
        }
        catch (JsonException ex)
        {
            throw new CliException(ExitCode.ContextRequired, "FLX-LOCK-0004", $"Engine lock '{project.LockFile}' is invalid.", new { exception = ex.Message });
        }
        if (value.SchemaVersion != 1)
            throw new CliException(ExitCode.ContextRequired, "FLX-LOCK-0004", $"Unsupported engine lock schema {value.SchemaVersion}.");
        var matches = engines.List().Where(x =>
            (value.Engine.Fingerprint == null || x.Fingerprint.Equals(value.Engine.Fingerprint, StringComparison.OrdinalIgnoreCase)) &&
            (value.Engine.Nickname == null || string.Equals(x.Nickname, value.Engine.Nickname, StringComparison.OrdinalIgnoreCase)) &&
            (value.Engine.Version == null || x.Version.Equals(value.Engine.Version, StringComparison.OrdinalIgnoreCase)) &&
            (value.Engine.Channel == null || x.Channel.Equals(value.Engine.Channel, StringComparison.OrdinalIgnoreCase)) &&
            (value.Engine.Source == null || x.Source.Equals(value.Engine.Source, StringComparison.OrdinalIgnoreCase))).ToList();
        if (matches.Count != 1)
            throw new CliException(ExitCode.ContextRequired, "FLX-LOCK-0004", "The project engine lock has no unique installed match.", new { candidates = matches.Select(x => new { x.Id, x.Path }) });
        return RequireCompatible(matches[0], project);
    }

    private static EngineInfo RequireValid(EngineInfo engine)
    {
        if (!engine.IsValid)
            throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Engine '{engine.Path}' has no runnable Editor for this host.");
        return engine;
    }

    private static EngineInfo RequireCompatible(EngineInfo engine, ProjectContext? project)
    {
        RequireValid(engine);
        if (!IsCompatible(engine, project))
            throw new CliException(ExitCode.ContextRequired, "FLX-ENGINE-0004", $"Engine {engine.Version} is older than the project's minimum {project!.MinimumEngineVersion}.");
        return engine;
    }

    private static bool IsCompatible(EngineInfo engine, ProjectContext? project) =>
        project == null || Parse(engine.Version) >= project.MinimumEngineVersion;

    private static SemanticVersion Parse(string value) => SemanticVersion.TryParse(value, out var result) ? result : default;
}
