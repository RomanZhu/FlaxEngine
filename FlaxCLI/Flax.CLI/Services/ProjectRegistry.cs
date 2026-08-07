// Copyright (c) Wojciech Figat. All rights reserved.

using Flax.CLI.Core;

namespace Flax.CLI.Services;

internal sealed class ProjectRegistry(AppPaths paths)
{
    public IReadOnlyList<string> List()
    {
        if (!File.Exists(paths.ProjectsFile))
            return [];
        return File.ReadAllLines(paths.ProjectsFile)
            .Select(x => x.Trim())
            .Where(x => x.Length != 0)
            .Select(Path.GetFullPath)
            .Distinct(PathComparer)
            .ToArray();
    }

    public void Add(string projectRoot)
    {
        using var transaction = AtomicFile.AcquireLock(paths.ProjectsFile + ".transaction.lock", TimeSpan.FromSeconds(10));
        var canonical = Path.GetFullPath(projectRoot);
        var projects = List().ToList();
        if (!projects.Contains(canonical, PathComparer))
            projects.Add(canonical);
        Save(projects);
    }

    public bool Remove(string projectRoot)
    {
        using var transaction = AtomicFile.AcquireLock(paths.ProjectsFile + ".transaction.lock", TimeSpan.FromSeconds(10));
        var canonical = Path.GetFullPath(projectRoot);
        var projects = List().ToList();
        var removed = projects.RemoveAll(x => PathComparer.Equals(x, canonical)) != 0;
        if (removed)
            Save(projects);
        return removed;
    }

    private void Save(IReadOnlyList<string> projects) =>
        AtomicFile.WriteText(paths.ProjectsFile, string.Join(Environment.NewLine, projects) + (projects.Count == 0 ? string.Empty : Environment.NewLine));

    internal static StringComparer PathComparer { get; } = OperatingSystem.IsWindows() || OperatingSystem.IsMacOS()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;
}
