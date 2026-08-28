// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEngine.Json;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;

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
            public Event[] events;
            public string[] dependencies;
        }

        public sealed class Event
        {
            public string id;
            public string path;
            public bool is3D = true;
            public bool isOneShot;
            public float minDistance = 1.0f;
            public float maxDistance = 100.0f;
            public float length;
            public string[] bankDependencies;
            public Parameter[] parameters;
        }

        public sealed class Parameter
        {
            public string id;
            public string name;
            public uint data1;
            public uint data2;
            public float minimum;
            public float maximum = 1.0f;
            public float defaultValue;
            public int type;
            public uint flags;
            public string[] labels;
        }

        public sealed class Snapshot
        {
            public string id;
            public string path;
            public Parameter weightParameter;
        }

        public sealed class Bus
        {
            public string id;
            public string path;
        }

        public sealed class VCA
        {
            public string id;
            public string path;
        }

        public sealed class Document
        {
            public int schema;
            public string revision;
            public Bank[] banks;
            public Snapshot[] snapshots;
            public Bus[] buses;
            public VCA[] vcas;
        }

        /// <summary>
        /// Reads and validates an optional fmod-metadata.json sidecar.
        /// </summary>
        public static bool TryRead(string banksDirectory, out Document document, out string error)
        {
            return TryReadAt(banksDirectory, FindMetadataPath(banksDirectory), out document, out error);
        }

        /// <summary>Reads the metadata sidecar from the requested active platform/locale variant.</summary>
        public static bool TryRead(string banksDirectory, string platform, string locale, out Document document, out string error)
        {
            var platformDirectory = string.IsNullOrWhiteSpace(platform) ? banksDirectory : Path.Combine(banksDirectory, platform);
            if (!Directory.Exists(platformDirectory))
                platformDirectory = banksDirectory;
            var metadataDirectory = platformDirectory;
            if (!string.IsNullOrWhiteSpace(locale) && !string.Equals(locale, "default", StringComparison.OrdinalIgnoreCase))
            {
                var localeDirectory = Path.Combine(platformDirectory, locale);
                if (Directory.Exists(localeDirectory))
                    metadataDirectory = localeDirectory;
            }
            var path = FindDirectMetadataPath(metadataDirectory);
            if (!File.Exists(path) && !string.Equals(metadataDirectory, platformDirectory, StringComparison.OrdinalIgnoreCase))
                path = FindDirectMetadataPath(platformDirectory);
            if (!File.Exists(path))
                path = FindDirectMetadataPath(banksDirectory);
            return TryReadAt(banksDirectory, path, out document, out error);
        }

        private static bool TryReadAt(string banksDirectory, string path, out Document document, out string error)
        {
            document = null;
            error = null;
            if (!File.Exists(path))
                return false;
            try
            {
                var root = JObject.Parse(File.ReadAllText(path));
                // Older catalogs emitted event GUID strings. Normalize them to the
                // metadata event object shape while retaining backward compatibility.
                if (root["banks"] is JArray banks)
                {
                    foreach (var bank in banks.OfType<JObject>())
                    {
                        if (bank["events"] is not JArray events)
                            continue;
                        for (var index = 0; index < events.Count; index++)
                        {
                            if (events[index].Type == JTokenType.String)
                                events[index] = new JObject { ["id"] = events[index] };
                        }
                    }
                }
                document = FlaxJsonSerializer.Deserialize<Document>(root.ToString(Formatting.None));
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
                {
                    error = "FMOD metadata contains a bank without a file path.";
                    document = null;
                    return false;
                }
                bank.file = NormalizeBankPath(bank.file, path, banksDirectory, available);
                if (!available.Contains(bank.file))
                {
                    error = $"Metadata references missing bank '{bank.file}'.";
                    document = null;
                    return false;
                }
            }
            var bankIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var eventIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var bank in document.banks)
            {
                if (bank == null || !Guid.TryParse(bank.id, out var bankId) || !bankIds.Add(bankId.ToString("D")))
                {
                    error = "FMOD metadata contains a duplicate or invalid bank GUID.";
                    document = null;
                    return false;
                }
                foreach (var dependency in bank.dependencies ?? Array.Empty<string>())
                {
                    if (!Guid.TryParse(dependency, out var dependencyId) || !bankIds.Contains(dependencyId.ToString("D")))
                    {
                        // Dependencies may point forward; validate after collecting all bank IDs.
                        if (!Guid.TryParse(dependency, out _))
                        {
                            error = $"FMOD metadata bank '{bank.file}' has an invalid dependency GUID.";
                            document = null;
                            return false;
                        }
                    }
                }
                foreach (var eventData in bank.events ?? Array.Empty<Event>())
                {
                    if (eventData == null || !Guid.TryParse(eventData.id, out var eventId) || !eventIds.Add(eventId.ToString("D")))
                    {
                        error = $"FMOD metadata bank '{bank.file}' contains a duplicate or invalid event GUID.";
                        document = null;
                        return false;
                    }
                    foreach (var dependency in eventData.bankDependencies ?? Array.Empty<string>())
                    {
                        if (!Guid.TryParse(dependency, out _))
                        {
                            error = $"FMOD event '{eventData.path}' has an invalid bank dependency GUID.";
                            document = null;
                            return false;
                        }
                    }
                }
            }
            foreach (var bank in document.banks)
                foreach (var dependency in bank.dependencies ?? Array.Empty<string>())
                    if (!Guid.TryParse(dependency, out var dependencyId) || !bankIds.Contains(dependencyId.ToString("D")))
                    {
                        error = $"FMOD metadata bank '{bank.file}' references missing dependency '{dependency}'.";
                        document = null;
                        return false;
                    }
            foreach (var bank in document.banks)
                foreach (var eventData in bank.events ?? Array.Empty<Event>())
                    foreach (var dependency in eventData.bankDependencies ?? Array.Empty<string>())
                        if (!Guid.TryParse(dependency, out var dependencyId) || !bankIds.Contains(dependencyId.ToString("D")))
                        {
                            error = $"FMOD metadata event '{eventData.path}' references missing bank dependency '{dependency}'.";
                            document = null;
                            return false;
                        }
            var mixerIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var snapshot in document.snapshots ?? Array.Empty<Snapshot>())
                if (snapshot == null || !Guid.TryParse(snapshot.id, out var snapshotId) || !mixerIds.Add(snapshotId.ToString("D")))
                {
                    error = "FMOD metadata contains a duplicate or invalid snapshot GUID.";
                    document = null;
                    return false;
                }
            foreach (var bus in document.buses ?? Array.Empty<Bus>())
                if (bus == null || !Guid.TryParse(bus.id, out var busId) || !mixerIds.Add(busId.ToString("D")))
                {
                    error = "FMOD metadata contains a duplicate or invalid bus GUID.";
                    document = null;
                    return false;
                }
            foreach (var vca in document.vcas ?? Array.Empty<VCA>())
                if (vca == null || !Guid.TryParse(vca.id, out var vcaId) || !mixerIds.Add(vcaId.ToString("D")))
                {
                    error = "FMOD metadata contains a duplicate or invalid VCA GUID.";
                    document = null;
                    return false;
                }
            return true;
        }

        private static string FindMetadataPath(string banksDirectory)
        {
            var root = FindDirectMetadataPath(banksDirectory);
            if (File.Exists(root))
                return root;
            var candidates = Directory.Exists(banksDirectory)
                ? Directory.GetFiles(banksDirectory, "fmod-metadata.json", SearchOption.AllDirectories)
                    .Concat(Directory.GetFiles(banksDirectory, "metadata.json", SearchOption.AllDirectories))
                    .OrderBy(value => value, StringComparer.OrdinalIgnoreCase)
                : Enumerable.Empty<string>();
            return candidates.FirstOrDefault() ?? root;
        }

        private static string FindDirectMetadataPath(string directory)
        {
            var path = Path.Combine(directory, "fmod-metadata.json");
            if (File.Exists(path))
                return path;
            return Path.Combine(directory, "metadata.json");
        }

        private static string NormalizeBankPath(string value, string metadataPath, string banksDirectory, HashSet<string> available)
        {
            var normalized = value.Replace('\\', '/').TrimStart('/');
            if (available.Contains(normalized))
                return normalized;
            var metadataDirectory = Path.GetDirectoryName(metadataPath) ?? banksDirectory;
            var candidate = Path.GetFullPath(Path.Combine(metadataDirectory, normalized));
            if (File.Exists(candidate))
                return Path.GetRelativePath(banksDirectory, candidate).Replace('\\', '/');
            return normalized;
        }
    }
}
