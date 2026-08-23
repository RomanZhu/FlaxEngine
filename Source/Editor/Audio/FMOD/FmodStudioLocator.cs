// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;

namespace FlaxEditor.FMOD
{
    /// <summary>Locates and launches the optional FMOD Studio authoring application.</summary>
    public static class FmodStudioLocator
    {
        /// <summary>Detailed result of a bank build invocation.</summary>
        public sealed class BuildResult
        {
            public bool Success;
            public string Project = string.Empty;
            public string Executable = string.Empty;
            public int? ExitCode;
            public string Output = string.Empty;
            public string Error = string.Empty;

            public string ToDisplayString()
            {
                var status = Success ? "PASS — FMOD Studio bank build completed." : "FAIL — FMOD Studio bank build did not complete.";
                var exit = ExitCode.HasValue ? ExitCode.Value.ToString() : "not started";
                var detail = !string.IsNullOrWhiteSpace(Error) ? Error : Output;
                if (detail.Length > 1800)
                    detail = detail.Substring(detail.Length - 1800);
                return $"{status}\nBuilder: {ValueOrMissing(Executable)}\nProject: {ValueOrMissing(Project)}\nExit code: {exit}" +
                       (string.IsNullOrWhiteSpace(detail) ? string.Empty : $"\n\nStudio output:\n{detail.Trim()}");
            }

            private static string ValueOrMissing(string value) => string.IsNullOrWhiteSpace(value) ? "NOT FOUND" : value;
        }

        private static IEnumerable<string> EnumerateInstallDirectories()
        {
            var roots = new[]
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "FMOD SoundSystem"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "FMOD SoundSystem"),
            };
            foreach (var root in roots.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (!Directory.Exists(root))
                    continue;
                yield return Path.Combine(root, "FMOD Studio", "bin");
                foreach (var directory in Directory.GetDirectories(root, "FMOD Studio*").OrderByDescending(x => x, StringComparer.OrdinalIgnoreCase))
                    yield return directory;
            }
        }

        private static string FindInstalledExecutable(params string[] names)
        {
            foreach (var directory in EnumerateInstallDirectories())
            {
                foreach (var name in names)
                {
                    var candidate = Path.Combine(directory, name);
                    if (File.Exists(candidate))
                        return candidate;
                }
            }
            return string.Empty;
        }

        /// <summary>Gets the graphical FMOD Studio executable from the linked project folder or a standard/versioned installation.</summary>
        public static string FindExecutable()
        {
            var project = FmodEditorSettings.StudioProjectPath;
            if (!string.IsNullOrEmpty(project))
            {
                var directory = Path.GetDirectoryName(project) ?? string.Empty;
                foreach (var name in new[] { "FMOD Studio.exe", "fmodstudio.exe" })
                {
                    var sibling = Path.Combine(directory, name);
                    if (File.Exists(sibling))
                        return sibling;
                }
            }
            return FindInstalledExecutable("FMOD Studio.exe", "fmodstudio.exe");
        }

        /// <summary>Gets the headless FMOD Studio bank builder.</summary>
        public static string FindCommandLineExecutable()
        {
            var gui = FindExecutable();
            if (!string.IsNullOrEmpty(gui))
            {
                var sibling = Path.Combine(Path.GetDirectoryName(gui) ?? string.Empty, "fmodstudiocl.exe");
                if (File.Exists(sibling))
                    return sibling;
            }
            return FindInstalledExecutable("fmodstudiocl.exe", "fmodstudio.exe");
        }

        /// <summary>Returns the discovered authoring-tool path and installation version.</summary>
        public static string GetInstallationSummary()
        {
            var executable = FindCommandLineExecutable();
            if (string.IsNullOrEmpty(executable))
                return "FMOD Studio executable: NOT FOUND. Install FMOD Studio or select its executable.";
            var installDirectory = Path.GetDirectoryName(executable) ?? string.Empty;
            var directoryName = Path.GetFileName(installDirectory) ?? string.Empty;
            const string versionPrefix = "FMOD Studio ";
            var version = directoryName.StartsWith(versionPrefix, StringComparison.OrdinalIgnoreCase)
                ? directoryName.Substring(versionPrefix.Length)
                : "unknown";
            return $"FMOD Studio executable: {executable}\nAuthoring version: {version}";
        }

        /// <summary>Opens the configured Studio project.</summary>
        public static bool OpenProject() => OpenProject(out _);

        /// <summary>Opens the configured Studio project and returns an actionable failure reason.</summary>
        public static bool OpenProject(out string error)
        {
            var project = FmodEditorSettings.StudioProjectPath;
            if (!File.Exists(project))
            {
                error = string.IsNullOrWhiteSpace(project)
                    ? "No FMOD Studio project is linked."
                    : $"The linked FMOD Studio project no longer exists: '{project}'.";
                return false;
            }
            var executable = FindExecutable();
            if (string.IsNullOrEmpty(executable))
            {
                error = "No FMOD Studio executable was found. Install FMOD Studio and try again.";
                return false;
            }
            try
            {
                Process.Start(new ProcessStartInfo(executable, $"\"{project}\"") { UseShellExecute = true });
                error = string.Empty;
                return true;
            }
            catch (Exception ex)
            {
                error = "FMOD Studio could not be started: " + ex.Message;
                return false;
            }
        }

        /// <summary>Builds banks and returns actionable process diagnostics.</summary>
        public static BuildResult BuildBanksDetailed()
        {
            var result = new BuildResult
            {
                Project = FmodEditorSettings.StudioProjectPath,
                Executable = FindCommandLineExecutable(),
            };
            if (!File.Exists(result.Project))
            {
                result.Error = "The linked .fspro does not exist. Return to Studio Project and select it again.";
                return result;
            }
            if (string.IsNullOrEmpty(result.Executable))
            {
                result.Error = "No FMOD Studio command-line builder was found. Expected fmodstudiocl.exe in a standard or versioned FMOD Studio installation.";
                return result;
            }

            try
            {
                using var process = Process.Start(new ProcessStartInfo(result.Executable, $"-build \"{result.Project}\"")
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                });
                if (process == null)
                {
                    result.Error = "Windows did not start the FMOD Studio command-line builder.";
                    return result;
                }

                Task<string> outputTask = process.StandardOutput.ReadToEndAsync();
                Task<string> errorTask = process.StandardError.ReadToEndAsync();
                process.WaitForExit();
                Task.WaitAll(outputTask, errorTask);
                result.ExitCode = process.ExitCode;
                result.Output = outputTask.Result;
                result.Error = errorTask.Result;
                result.Success = process.ExitCode == 0;
                if (!result.Success && string.IsNullOrWhiteSpace(result.Error))
                    result.Error = "FMOD Studio returned a failure exit code. Review Studio output below.";
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
            }
            return result;
        }

        /// <summary>Builds banks using the configured Studio project.</summary>
        public static bool BuildBanks() => BuildBanksDetailed().Success;
    }
}
