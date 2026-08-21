// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEngine;
using FlaxEngine.Json;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// Synchronizes metadata-only FMOD event and bank assets while preserving existing Flax asset IDs.
    /// </summary>
    public static class FmodAssetSynchronizer
    {
        public sealed class Report
        {
            public int EventsCreated;
            public int EventsUpdated;
            public int BanksCreated;
            public int BanksUpdated;
            public int SnapshotsCreated;
            public int SnapshotsUpdated;
            public int BusesCreated;
            public int BusesUpdated;
            public int VcasCreated;
            public int VcasUpdated;
            public readonly List<string> Errors = new List<string>();
            public bool Succeeded => Errors.Count == 0;
        }

        private static Dictionary<string, string> FindExistingAssets(string contentRoot, string typeName, string backendField)
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var file in Directory.GetFiles(contentRoot, "*.json", SearchOption.AllDirectories))
            {
                try
                {
                    var root = JObject.Parse(File.ReadAllText(file));
                    if (!string.Equals((string)root["TypeName"], typeName, StringComparison.Ordinal))
                        continue;
                    var id = (string)root["Data"]?[backendField];
                    if (Guid.TryParse(id, out var guid))
                        result[guid.ToString("D")] = file;
                }
                catch
                {
                    // Non-asset JSON files are expected in Content and are ignored.
                }
            }
            return result;
        }

        /// <summary>
        /// Imports the optional sidecar and writes stable Flax JSON assets under Content/Audio.
        /// </summary>
        public static Report Synchronize(string banksDirectory, string contentRoot)
        {
            var report = new Report();
            if (!FmodMetadataImporter.TryRead(banksDirectory, out var metadata, out var error))
            {
                if (!string.IsNullOrEmpty(error))
                    report.Errors.Add(error);
                return report;
            }

            var events = FindExistingAssets(contentRoot, "FlaxEngine.AudioEvent", "BackendId");
            var banks = FindExistingAssets(contentRoot, "FlaxEngine.AudioBank", "BackendId");
            var snapshots = FindExistingAssets(contentRoot, "FlaxEngine.AudioSnapshot", "BackendId");
            var buses = FindExistingAssets(contentRoot, "FlaxEngine.AudioBus", "BackendId");
            var vcas = FindExistingAssets(contentRoot, "FlaxEngine.AudioVCA", "BackendId");
            var eventDirectory = Path.Combine(contentRoot, "Audio", "Events");
            var bankDirectory = Path.Combine(contentRoot, "Audio", "Banks");
            var snapshotDirectory = Path.Combine(contentRoot, "Audio", "Snapshots");
            var busDirectory = Path.Combine(contentRoot, "Audio", "Buses");
            var vcaDirectory = Path.Combine(contentRoot, "Audio", "VCAs");
            Directory.CreateDirectory(eventDirectory);
            Directory.CreateDirectory(bankDirectory);
            Directory.CreateDirectory(snapshotDirectory);
            Directory.CreateDirectory(busDirectory);
            Directory.CreateDirectory(vcaDirectory);

            var knownBankIds = new HashSet<Guid>();
            foreach (var bankData in metadata.banks)
                if (Guid.TryParse(bankData?.id, out var knownBankId))
                    knownBankIds.Add(knownBankId);
            var eventRecords = new Dictionary<Guid, FmodMetadataImporter.Event>();
            var eventBanks = new Dictionary<Guid, List<Guid>>();
            var bankAssetPaths = new Dictionary<Guid, string>();
            foreach (var bankData in metadata.banks)
            {
                if (bankData == null || !Guid.TryParse(bankData.id, out var bankId))
                {
                    report.Errors.Add("FMOD metadata contains a bank with an invalid GUID.");
                    continue;
                }
                var bankPath = Path.Combine(bankDirectory, MakeSafeName(bankData.file, bankData.id) + ".json");
                var bankKey = bankId.ToString("D");
                if (banks.TryGetValue(bankKey, out var existingBankPath))
                {
                    bankPath = MoveToCanonicalPath(existingBankPath, bankPath, contentRoot);
                    report.BanksUpdated++;
                }
                else
                {
                    report.BanksCreated++;
                }
                var bank = new AudioBank
                {
                    BackendId = bankId,
                    Path = bankData.file ?? string.Empty,
                    NonBlocking = true,
                    IncludedEvents = ParseEventIds(bankData.events, report, $"bank '{bankData.file}' events"),
                    ReferencedBanks = ParseGuids(bankData.dependencies, report, $"bank '{bankData.file}' dependencies"),
                };
                ValidateBankReferences(bank.ReferencedBanks, knownBankIds, report, $"bank '{bankData.file}' dependencies");
                if (Editor.SaveJsonAsset(bankPath, bank))
                    report.Errors.Add($"Failed to save FMOD bank asset '{bankPath}'.");
                else
                    NormalizeNativeGuids(bankPath);
                bankAssetPaths[bankId] = bankPath;
                if (bankData.events == null)
                    continue;
                foreach (var eventData in bankData.events)
                {
                    if (eventData == null || !Guid.TryParse(eventData.id, out var eventId))
                    {
                        report.Errors.Add($"FMOD bank '{bankData.file}' contains an event with an invalid GUID.");
                        continue;
                    }
                    if (!eventRecords.ContainsKey(eventId))
                        eventRecords[eventId] = eventData;
                    if (!eventBanks.TryGetValue(eventId, out var owners))
                        eventBanks[eventId] = owners = new List<Guid>();
                    if (!owners.Contains(bankId))
                        owners.Add(bankId);
                }
            }

            foreach (var pair in eventRecords)
            {
                var eventId = pair.Key;
                var eventData = pair.Value;
                var name = MakeSafeName(eventData.path, eventData.id);
                var eventPath = Path.Combine(eventDirectory, name + ".json");
                if (events.TryGetValue(eventId.ToString("D"), out var existingEventPath))
                {
                    eventPath = MoveToCanonicalPath(existingEventPath, eventPath, contentRoot);
                    report.EventsUpdated++;
                }
                else
                    report.EventsCreated++;
                var dependencies = new List<Guid>(eventBanks[eventId]);
                foreach (var dependency in ParseGuids(eventData.bankDependencies, report, $"event '{eventData.path}' dependencies"))
                    if (!dependencies.Contains(dependency))
                        dependencies.Add(dependency);
                ValidateBankReferences(dependencies, knownBankIds, report, $"event '{eventData.path}' dependencies");
                var audioEvent = new AudioEvent
                {
                    BackendId = eventId,
                    Path = eventData.path ?? string.Empty,
                    Is3D = eventData.is3D,
                    IsOneShot = eventData.isOneShot,
                    MinDistance = eventData.minDistance,
                    MaxDistance = eventData.maxDistance,
                    Length = eventData.length,
                    BankDependencies = dependencies.ToArray(),
                    Parameters = ParseParameters(eventData.parameters, report, $"event '{eventData.path}' parameters"),
                };
                var bankAssets = new List<JsonAssetReference<AudioBank>>();
                foreach (var dependency in dependencies)
                {
                    if (!bankAssetPaths.TryGetValue(dependency, out var bankAssetPath))
                        continue;
                    // Content.Load resolves relative paths from the project root,
                    // not from Globals.ProjectContentFolder. Preserve the
                    // explicit Content prefix so generated bank references load
                    // from the same canonical location they were saved to.
                    var contentPath = "Content/" + Path.GetRelativePath(contentRoot, bankAssetPath).Replace('\\', '/');
                    var asset = FlaxEngine.Content.Load<JsonAsset>(contentPath);
                    if (asset)
                        bankAssets.Add(new JsonAssetReference<AudioBank>(asset));
                }
                audioEvent.BankAssets = bankAssets.ToArray();
                if (Editor.SaveJsonAsset(eventPath, audioEvent))
                    report.Errors.Add($"Failed to save FMOD event asset '{eventPath}'.");
                else
                    NormalizeNativeGuids(eventPath);
            }

            SynchronizeMixerAssets(metadata, snapshots, buses, vcas, snapshotDirectory, busDirectory, vcaDirectory, report, contentRoot);
            return report;
        }

        private static void SynchronizeMixerAssets(FmodMetadataImporter.Document metadata,
                                                    Dictionary<string, string> snapshots,
                                                    Dictionary<string, string> buses,
                                                    Dictionary<string, string> vcas,
                                                    string snapshotDirectory,
                                                    string busDirectory,
                                                    string vcaDirectory,
                                                    Report report,
                                                    string contentRoot)
        {
            foreach (var snapshotData in metadata.snapshots ?? Array.Empty<FmodMetadataImporter.Snapshot>())
            {
                if (snapshotData == null || !Guid.TryParse(snapshotData.id, out var id))
                    continue;
                var path = Path.Combine(snapshotDirectory, MakeSafeName(snapshotData.path, snapshotData.id) + ".json");
                if (snapshots.TryGetValue(id.ToString("D"), out var existing))
                {
                    path = MoveToCanonicalPath(existing, path, contentRoot);
                    report.SnapshotsUpdated++;
                }
                else
                    report.SnapshotsCreated++;
                var asset = new AudioSnapshot
                {
                    BackendId = id,
                    Path = snapshotData.path ?? string.Empty,
                    WeightParameter = ParseParameter(snapshotData.weightParameter, report, $"snapshot '{snapshotData.path}' weight parameter"),
                };
                if (Editor.SaveJsonAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD snapshot asset '{path}'.");
                else
                    NormalizeNativeGuids(path);
            }
            foreach (var busData in metadata.buses ?? Array.Empty<FmodMetadataImporter.Bus>())
            {
                if (busData == null || !Guid.TryParse(busData.id, out var id))
                    continue;
                var path = Path.Combine(busDirectory, MakeSafeName(busData.path, busData.id) + ".json");
                if (buses.TryGetValue(id.ToString("D"), out var existing))
                {
                    path = MoveToCanonicalPath(existing, path, contentRoot);
                    report.BusesUpdated++;
                }
                else
                    report.BusesCreated++;
                var asset = new AudioBus { BackendId = id, Path = busData.path ?? string.Empty };
                if (Editor.SaveJsonAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD bus asset '{path}'.");
                else
                    NormalizeNativeGuids(path);
            }
            foreach (var vcaData in metadata.vcas ?? Array.Empty<FmodMetadataImporter.VCA>())
            {
                if (vcaData == null || !Guid.TryParse(vcaData.id, out var id))
                    continue;
                var path = Path.Combine(vcaDirectory, MakeSafeName(vcaData.path, vcaData.id) + ".json");
                if (vcas.TryGetValue(id.ToString("D"), out var existing))
                {
                    path = MoveToCanonicalPath(existing, path, contentRoot);
                    report.VcasUpdated++;
                }
                else
                    report.VcasCreated++;
                var asset = new AudioVCA { BackendId = id, Path = vcaData.path ?? string.Empty };
                if (Editor.SaveJsonAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD VCA asset '{path}'.");
                else
                    NormalizeNativeGuids(path);
            }
        }

        private static void NormalizeNativeGuids(string path)
        {
            // Native Flax JSON Guid deserialization uses the engine's compact N
            // representation. Newtonsoft's default Guid converter writes D;
            // normalize every generated Guid string while preserving paths and
            // other authored strings.
            var root = JToken.Parse(File.ReadAllText(path));
            NormalizeNativeGuids(root);
            File.WriteAllText(path, root.ToString(Formatting.Indented));
        }

        private static void NormalizeNativeGuids(JToken token)
        {
            if (token is JValue value && value.Type == JTokenType.String &&
                Guid.TryParse((string)value, out var guid))
            {
                value.Value = guid.ToString("N");
                return;
            }
            foreach (var child in token.Children().ToArray())
                NormalizeNativeGuids(child);
        }

        private static string MakeSafeName(string path, string fallback)
        {
            var name = path;
            if (string.IsNullOrWhiteSpace(name))
                name = fallback;
            foreach (var invalid in Path.GetInvalidFileNameChars())
                name = name.Replace(invalid, '_');
            name = name.Replace('/', '_').Replace('\\', '_').Replace(':', '_');
            return string.IsNullOrWhiteSpace(name) ? fallback : name.Trim('_');
        }

        private static Guid[] ParseGuids(string[] values, Report report, string context)
        {
            if (values == null || values.Length == 0)
                return Array.Empty<Guid>();
            var result = new List<Guid>(values.Length);
            foreach (var value in values)
            {
                if (!Guid.TryParse(value, out var guid))
                    report.Errors.Add($"FMOD metadata has an invalid GUID in {context}.");
                else if (!result.Contains(guid))
                    result.Add(guid);
            }
            return result.ToArray();
        }

        private static Guid[] ParseEventIds(FmodMetadataImporter.Event[] values, Report report, string context)
        {
            if (values == null || values.Length == 0)
                return Array.Empty<Guid>();
            var result = new List<Guid>(values.Length);
            foreach (var value in values)
            {
                if (value == null || !Guid.TryParse(value.id, out var guid))
                    report.Errors.Add($"FMOD metadata has an invalid event GUID in {context}.");
                else if (!result.Contains(guid))
                    result.Add(guid);
            }
            return result.ToArray();
        }

        private static AudioParameterId[] ParseParameters(FmodMetadataImporter.Parameter[] values, Report report, string context)
        {
            if (values == null || values.Length == 0)
                return Array.Empty<AudioParameterId>();
            var result = new List<AudioParameterId>(values.Length);
            foreach (var value in values)
            {
                if (value == null)
                {
                    report.Errors.Add($"FMOD metadata contains an invalid parameter in {context}.");
                    continue;
                }
                if (!string.IsNullOrWhiteSpace(value.id) && !Guid.TryParse(value.id, out var id))
                {
                    report.Errors.Add($"FMOD metadata contains an invalid parameter GUID in {context}.");
                    continue;
                }
                var parameter = new AudioParameterId
                {
                    ID = Guid.TryParse(value.id, out var parsedId) ? parsedId : Guid.Empty,
                    Data1 = value.data1,
                    Data2 = value.data2,
                    Name = value.name ?? string.Empty,
                };
                if (parameter.ID == Guid.Empty && parameter.Data1 == 0 && parameter.Data2 == 0 && string.IsNullOrWhiteSpace(parameter.Name))
                    report.Errors.Add($"FMOD metadata contains an empty parameter in {context}.");
                else if (!result.Contains(parameter))
                    result.Add(parameter);
            }
            return result.ToArray();
        }

        private static AudioParameterId ParseParameter(FmodMetadataImporter.Parameter value, Report report, string context)
        {
            return ParseParameters(value == null ? null : new[] { value }, report, context).FirstOrDefault();
        }

        private static void ValidateBankReferences(IEnumerable<Guid> references, HashSet<Guid> knownBankIds, Report report, string context)
        {
            foreach (var reference in references)
                if (!knownBankIds.Contains(reference))
                    report.Errors.Add($"FMOD metadata references missing bank '{reference}' in {context}.");
        }

        private static string MoveToCanonicalPath(string existingPath, string canonicalPath, string root)
        {
            // Existing assets may be loaded and referenced by open scenes or
            // settings. Updating them in place preserves both their stable ID
            // and the Content database's live path mapping. A raw File.Move
            // invalidates those references until an Editor restart.
            return existingPath;
        }
    }
}
