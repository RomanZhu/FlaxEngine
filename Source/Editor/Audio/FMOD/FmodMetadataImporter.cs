// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using FlaxEngine.Json;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// Metadata-only FMOD catalog reader. It works in editors and cookers without loading the FMOD SDK.
    /// </summary>
    public static class FmodMetadataImporter
    {
        public sealed class Bank
        {
            public string id;
            public string file;
            public string[] events;
            public string[] dependencies;
        }

        public sealed class Document
        {
            public int schema;
            public string revision;
            public Bank[] banks;
        }

        /// <summary>
        /// Reads and validates an optional fmod-metadata.json sidecar.
        /// </summary>
        public static bool TryRead(string banksDirectory, out Document document, out string error)
        {
            document = null;
            error = null;
            var path = Path.Combine(banksDirectory, "fmod-metadata.json");
            if (!File.Exists(path))
                path = Path.Combine(banksDirectory, "metadata.json");
            if (!File.Exists(path))
                return false;
            try
            {
                document = JsonSerializer.Deserialize<Document>(File.ReadAllText(path));
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
            if (document == null || document.schema < 1 || document.banks == null)
            {
                error = "Metadata schema or bank list is missing.";
                return false;
            }
            var available = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var file in Directory.GetFiles(banksDirectory, "*.bank", SearchOption.AllDirectories))
                available.Add(Path.GetRelativePath(banksDirectory, file).Replace('\\', '/'));
            foreach (var bank in document.banks)
            {
                if (bank == null || string.IsNullOrWhiteSpace(bank.file))
                    continue;
                if (!available.Contains(bank.file.Replace('\\', '/')))
                {
                    error = $"Metadata references missing bank '{bank.file}'.";
                    document = null;
                    return false;
                }
            }
            return true;
        }
    }
}
