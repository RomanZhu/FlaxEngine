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
        private static string SettingsPath
        {
            get
            {
                var project = Globals.ProjectFolder ?? string.Empty;
                var hash = SHA256.HashData(Encoding.UTF8.GetBytes(project));
                var key = Convert.ToHexString(hash).ToLowerInvariant();
                var directory = Path.Combine(Editor.LocalCachePath, "FMOD");
                Directory.CreateDirectory(directory);
                return Path.Combine(directory, key + ".fspro");
            }
        }

        /// <summary>
        /// Gets or sets the optional local FMOD Studio project path.
        /// </summary>
        public static string StudioProjectPath
        {
            get => File.Exists(SettingsPath) ? File.ReadAllText(SettingsPath).Trim() : string.Empty;
            set
            {
                var path = value ?? string.Empty;
                if (!string.IsNullOrWhiteSpace(path) && !File.Exists(path))
                    throw new FileNotFoundException("FMOD Studio project was not found.", path);
                File.WriteAllText(SettingsPath, path.Trim());
            }
        }
    }
}
