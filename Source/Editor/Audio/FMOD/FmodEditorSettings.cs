// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// Per-user FMOD authoring settings. The Studio project path is intentionally kept outside the project files.
    /// </summary>
    public static class FmodEditorSettings
    {
        private static string SettingsPath(string extension)
        {
            var project = Globals.ProjectFolder ?? string.Empty;
            var hash = SHA256.HashData(Encoding.UTF8.GetBytes(project));
            var key = Convert.ToHexString(hash).ToLowerInvariant();
            var directory = Path.Combine(Editor.LocalCachePath, "FMOD");
            Directory.CreateDirectory(directory);
            return Path.Combine(directory, key + extension);
        }

        /// <summary>
        /// Gets or sets the optional local FMOD Studio project path.
        /// </summary>
        public static string StudioProjectPath
        {
            get => File.Exists(SettingsPath(".fspro")) ? File.ReadAllText(SettingsPath(".fspro")).Trim() : string.Empty;
            set
            {
                var path = value ?? string.Empty;
                if (!string.IsNullOrWhiteSpace(path) && !File.Exists(path))
                    throw new FileNotFoundException("FMOD Studio project was not found.", path);
                File.WriteAllText(SettingsPath(".fspro"), path.Trim());
            }
        }

        /// <summary>Gets or sets the per-user built-bank source directory.</summary>
        public static string BankOutputPath
        {
            get => File.Exists(SettingsPath(".banks")) ? File.ReadAllText(SettingsPath(".banks")).Trim() : string.Empty;
            set
            {
                var path = value ?? string.Empty;
                if (!string.IsNullOrWhiteSpace(path) && !Directory.Exists(path))
                    throw new DirectoryNotFoundException("FMOD built-bank directory was not found: " + path);
                File.WriteAllText(SettingsPath(".banks"), path.Trim());
            }
        }
    }
}
