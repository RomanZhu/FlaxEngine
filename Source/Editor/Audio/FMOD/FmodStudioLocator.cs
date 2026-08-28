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
                var output = Output?.Trim();
                var error = Error?.Trim();
                string detail;
                if (string.IsNullOrWhiteSpace(output))
                    detail = error;
                else if (string.IsNullOrWhiteSpace(error) || string.Equals(output, error, StringComparison.Ordinal))
                    detail = output;
                else
                    detail = output + Environment.NewLine + error;
                detail ??= string.Empty;
                if (detail.Length > 1800)
                    detail = detail.Substring(detail.Length - 1800);
                return $"{status}\nBuilder: {ValueOrMissing(Executable)}\nProject: {ValueOrMissing(Project)}\nExit code: {exit}" +
                       (string.IsNullOrWhiteSpace(detail) ? string.Empty : $"\n\nStudio output:\n{detail.Trim()}");
            }

            private static string ValueOrMissing(string value) => string.IsNullOrWhiteSpace(value) ? "NOT FOUND" : value;
        }

        /// <summary>Detailed result of a general FMOD Studio command-line invocation.</summary>
        public sealed class ToolResult
        {
            /// <summary>True when FMOD Studio returned exit code zero.</summary>
            public bool Success;
            /// <summary>Human-readable operation name.</summary>
            public string Operation = string.Empty;
            /// <summary>Linked FMOD Studio project path.</summary>
            public string Project = string.Empty;
            /// <summary>Resolved command-line executable path.</summary>
            public string Executable = string.Empty;
            /// <summary>Resolved FMOD Studio installation version.</summary>
            public string ToolVersion = string.Empty;
            /// <summary>Resolved script path, when applicable.</summary>
            public string Script = string.Empty;
            /// <summary>Process exit code, or null when the process was not started.</summary>
            public int? ExitCode;
            /// <summary>Captured standard output.</summary>
            public string Output = string.Empty;
            /// <summary>Captured standard error or startup failure.</summary>
            public string Error = string.Empty;

            /// <summary>Formats the complete failure without discarding either output stream.</summary>
            public string ToDisplayString()
            {
                var status = Success ? "PASS" : "FAIL";
                var exit = ExitCode.HasValue ? ExitCode.Value.ToString() : "not started";
                var details = new[] { Output?.Trim(), Error?.Trim() }
                    .Where(x => !string.IsNullOrWhiteSpace(x))
                    .Distinct(StringComparer.Ordinal)
                    .ToArray();
                var detail = string.Join(Environment.NewLine, details);
                if (detail.Length > 4000)
                    detail = "... diagnostic output truncated to final 4000 characters ...\n" + detail.Substring(detail.Length - 4000);
                return $"{status} — FMOD Studio {Operation}.\nBuilder: {ValueOrMissing(Executable)}\nVersion: {ValueOrMissing(ToolVersion)}\nProject: {ValueOrMissing(Project)}\nExit code: {exit}" +
                       (string.IsNullOrWhiteSpace(Script) ? string.Empty : $"\nScript: {Script}") +
                       (string.IsNullOrWhiteSpace(detail) ? string.Empty : $"\n\nStudio output:\n{detail}");
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

        /// <summary>Gets the file version reported by the discovered command-line authoring tool.</summary>
        public static string GetCommandLineVersion()
        {
            var executable = FindCommandLineExecutable();
            if (string.IsNullOrEmpty(executable) || !File.Exists(executable))
                return string.Empty;
            var directoryName = Path.GetFileName(Path.GetDirectoryName(executable) ?? string.Empty) ?? string.Empty;
            const string versionPrefix = "FMOD Studio ";
            return directoryName.StartsWith(versionPrefix, StringComparison.OrdinalIgnoreCase)
                ? directoryName.Substring(versionPrefix.Length)
                : directoryName;
        }

        private static ToolResult RunTool(string operation, string script, params string[] arguments)
        {
            var result = new ToolResult
            {
                Operation = operation,
                Project = FmodEditorSettings.StudioProjectPath,
                Executable = FindCommandLineExecutable(),
                ToolVersion = GetCommandLineVersion(),
                Script = script ?? string.Empty,
            };
            if (!File.Exists(result.Project))
            {
                result.Error = "The linked .fspro does not exist. Link the FMOD Studio project before running authoring commands.";
                return result;
            }
            if (string.IsNullOrEmpty(result.Executable))
            {
                result.Error = "No FMOD Studio command-line tool was found. Expected fmodstudiocl.exe in a standard or versioned FMOD Studio installation.";
                return result;
            }

            try
            {
                var startInfo = new ProcessStartInfo(result.Executable)
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    WorkingDirectory = Path.GetDirectoryName(result.Project) ?? Environment.CurrentDirectory,
                };
                foreach (var argument in arguments)
                    startInfo.ArgumentList.Add(argument);
                startInfo.ArgumentList.Add(result.Project);
                using var process = Process.Start(startInfo);
                if (process == null)
                {
                    result.Error = "Windows did not start the FMOD Studio command-line tool.";
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
                if (!result.Success && string.IsNullOrWhiteSpace(result.Output) && string.IsNullOrWhiteSpace(result.Error))
                    result.Error = "FMOD Studio returned a failure exit code without diagnostics.";
            }
            catch (Exception ex)
            {
                result.Error = ex.Message;
            }
            return result;
        }

        /// <summary>Runs FMOD Studio's project corruption and validity diagnostic.</summary>
        public static ToolResult RunDiagnosticsDetailed()
        {
            return RunTool("project diagnostic", string.Empty, "-diagnostic");
        }

        /// <summary>Resolves and runs an explicit JavaScript file contained by the linked FMOD project.</summary>
        public static ToolResult RunProjectScriptDetailed(string script)
        {
            var project = FmodEditorSettings.StudioProjectPath;
            var projectDirectory = Path.GetDirectoryName(project) ?? string.Empty;
            string fullPath;
            try
            {
                fullPath = Path.GetFullPath(Path.IsPathRooted(script) ? script : Path.Combine(projectDirectory, script));
            }
            catch (Exception ex)
            {
                return new ToolResult { Operation = "authoring script", Project = project, Script = script ?? string.Empty, Error = "Invalid script path: " + ex.Message };
            }

            var root = Path.GetFullPath(projectDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (!fullPath.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                return new ToolResult { Operation = "authoring script", Project = project, Script = fullPath, Error = "Authoring scripts must be contained within the linked FMOD project directory." };
            if (!string.Equals(Path.GetExtension(fullPath), ".js", StringComparison.OrdinalIgnoreCase))
                return new ToolResult { Operation = "authoring script", Project = project, Script = fullPath, Error = "The authoring script must be a .js file." };
            if (!File.Exists(fullPath))
                return new ToolResult { Operation = "authoring script", Project = project, Script = fullPath, Error = "The authoring script does not exist." };
            return RunTool("authoring script", fullPath, "-script", fullPath);
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
                if (!result.Success && string.IsNullOrWhiteSpace(result.Output) && string.IsNullOrWhiteSpace(result.Error))
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
