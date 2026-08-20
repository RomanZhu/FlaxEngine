// Copyright (c) Wojciech Figat. All rights reserved.

using System.Diagnostics;
using System.IO;
using FlaxEngine;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// Locates and launches the optional FMOD Studio authoring application.
    /// </summary>
    public static class FmodStudioLocator
    {
        /// <summary>
        /// Gets an FMOD Studio executable next to the configured project or from standard Windows locations.
        /// </summary>
        public static string FindExecutable()
        {
            var project = FmodEditorSettings.StudioProjectPath;
            if (!string.IsNullOrEmpty(project))
            {
                var sibling = Path.Combine(Path.GetDirectoryName(project) ?? string.Empty, "fmodstudio.exe");
                if (File.Exists(sibling))
                    return sibling;
            }

            var candidates = new[]
            {
                @"C:\Program Files\FMOD SoundSystem\FMOD Studio\bin\fmodstudio.exe",
                @"C:\Program Files (x86)\FMOD SoundSystem\FMOD Studio\bin\fmodstudio.exe",
            };
            foreach (var candidate in candidates)
            {
                if (File.Exists(candidate))
                    return candidate;
            }
            return string.Empty;
        }

        /// <summary>
        /// Opens the configured Studio project.
        /// </summary>
        public static bool OpenProject()
        {
            var project = FmodEditorSettings.StudioProjectPath;
            var executable = FindExecutable();
            if (string.IsNullOrEmpty(project) || string.IsNullOrEmpty(executable))
                return false;
            Process.Start(new ProcessStartInfo(executable, $"\"{project}\"") { UseShellExecute = true });
            return true;
        }

        /// <summary>
        /// Builds banks using the configured Studio project. FMOD remains an optional authoring dependency.
        /// </summary>
        public static bool BuildBanks()
        {
            var project = FmodEditorSettings.StudioProjectPath;
            var executable = FindExecutable();
            if (string.IsNullOrEmpty(project) || string.IsNullOrEmpty(executable))
                return false;
            using var process = Process.Start(new ProcessStartInfo(executable, $"-build \"{project}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            });
            process.WaitForExit();
            return process.ExitCode == 0;
        }
    }
}
