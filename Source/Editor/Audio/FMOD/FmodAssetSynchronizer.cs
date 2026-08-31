// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
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

            // Banks are regenerated before events. If an event is selected, its Properties layout can
            // contain editors for those bank references, so release the whole selected audio asset before
            // any dependency is reloaded. The selection is rebuilt after its own asset finishes loading.
            PrepareSelectedAudioAssetForReload(contentRoot);

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
            if (!EnsureGeneratedDirectories(contentRoot, report, "Audio", "Events", "Banks", "Snapshots", "Buses", "VCAs"))
                return report;

            var knownBankIds = new HashSet<Guid>();
            foreach (var bankData in metadata.banks)
                if (Guid.TryParse(bankData?.id, out var knownBankId))
                    knownBankIds.Add(knownBankId);
            var eventRecords = new Dictionary<Guid, FmodMetadataImporter.Event>();
            var eventBanks = new Dictionary<Guid, List<Guid>>();
            var bankAssetIds = new Dictionary<Guid, Guid>();
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
                if (SaveGeneratedAsset(bankPath, bank))
                    report.Errors.Add($"Failed to save FMOD bank asset '{bankPath}'.");
                if (TryReadAssetId(bankPath, out var bankAssetId))
                    bankAssetIds[bankId] = bankAssetId;
                else
                    report.Errors.Add($"Failed to read the generated FMOD bank asset ID from '{bankPath}'.");
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
                var parameterDescriptions = ParseParameterDescriptions(eventData.parameters, report, $"event '{eventData.path}' parameters");
                var bankAssets = new JArray();
                foreach (var dependency in dependencies)
                {
                    if (!bankAssetIds.TryGetValue(dependency, out var bankAssetId))
                        continue;
                    if (!FlaxEngine.Content.GetAssetObjectId(bankAssetId, out var objectId) || !objectId.IsValid)
                    {
                        report.Errors.Add($"FMOD bank asset '{bankAssetId}' has no canonical file GUID/local-ID identity.");
                        continue;
                    }
                    bankAssets.Add(new JObject
                    {
                        ["guid"] = objectId.Guid.ToString("N"),
                        ["localId"] = objectId.LocalId,
                    });
                }
                var eventAssetData = new JObject
                {
                    [nameof(AudioEvent.BackendId)] = eventId.ToString("N"),
                    [nameof(AudioEvent.Path)] = eventData.path ?? string.Empty,
                    [nameof(AudioEvent.Is3D)] = eventData.is3D,
                    [nameof(AudioEvent.IsOneShot)] = eventData.isOneShot,
                    [nameof(AudioEvent.MinDistance)] = eventData.minDistance,
                    [nameof(AudioEvent.MaxDistance)] = eventData.maxDistance,
                    [nameof(AudioEvent.Length)] = eventData.length,
                    [nameof(AudioEvent.Parameters)] = CreateParameterIdsJson(parameterDescriptions),
                    [nameof(AudioEvent.BankDependencies)] = new JArray(dependencies.Select(x => x.ToString("N"))),
                    [nameof(AudioEvent.BankAssets)] = bankAssets,
                    // Editor-only authored metadata. Keeping this out of the native AudioEvent
                    // layout avoids exposing nested string-containing arrays through bindings.
                    ["ParameterDescriptions"] = CreateParameterDescriptionsJson(parameterDescriptions),
                };
                if (SaveGeneratedAsset(eventPath, eventAssetData, typeof(AudioEvent).FullName))
                    report.Errors.Add($"Failed to save FMOD event asset '{eventPath}'.");
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
                if (SaveGeneratedAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD snapshot asset '{path}'.");
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
                if (SaveGeneratedAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD bus asset '{path}'.");
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
                if (SaveGeneratedAsset(path, asset))
                    report.Errors.Add($"Failed to save FMOD VCA asset '{path}'.");
            }
        }

        private static bool EnsureGeneratedDirectories(string contentRoot, Report report, params string[] names)
        {
            var useCanonicalAssetSystem = Editor.Instance?.GameProject?.AssetSystemVersion == ProjectInfo.CurrentAssetSystemVersion;
            var parent = contentRoot;
            for (var i = 0; i < names.Length; i++)
            {
                if (i == 1)
                    parent = Path.Combine(contentRoot, names[0]);
                var path = Path.Combine(parent, names[i]);
                if (Directory.Exists(path))
                    continue;
                if (!useCanonicalAssetSystem)
                {
                    Directory.CreateDirectory(path);
                    continue;
                }
                if (string.IsNullOrEmpty(AssetDatabase.CreateFolder(parent, names[i])))
                {
                    report.Errors.Add($"Failed to create FMOD asset folder '{path}'.");
                    return false;
                }
            }
            return true;
        }

        private static bool SaveGeneratedAsset(string path, object asset)
        {
            var data = JToken.Parse(FlaxEngine.Json.JsonSerializer.Serialize(asset));
            return SaveGeneratedAsset(path, data, asset.GetType().FullName);
        }

        private static bool SaveGeneratedAsset(string path, JToken data, string typeName)
        {
            NormalizeNativeGuids(data);
            if (IsGeneratedAssetCurrent(path, data))
                return false;

            if (Editor.Instance?.GameProject?.AssetSystemVersion == ProjectInfo.CurrentAssetSystemVersion)
            {
                var logicalPath = AssetDatabase.ToLogicalPathInternal(path);
                if (File.Exists(path))
                {
                    if (!AssetPipelineCallbacks.WillSave(new[] { logicalPath }).Contains(logicalPath, StringComparer.OrdinalIgnoreCase))
                        return false;
                }
                else
                {
                    AssetPipelineCallbacks.WillCreate(logicalPath);
                }
            }

            var contentDatabase = Editor.Instance?.ContentDatabase;
            using var saveScope = contentDatabase?.TrackAssetSave(path);
            var succeeded = false;
            AssetItem loadedItem = null;
            try
            {
                // CreateJson reloads an existing loaded JsonAsset immediately after writing it.
                // Let open asset editors discard their native instance before that reload occurs.
                if (contentDatabase?.Find(path) is AssetItem item && item.IsLoaded)
                {
                    loadedItem = item;
                    var loadedAsset = FlaxEngine.Content.GetAsset(item.ID);
                    Editor.Instance.Windows.PropertiesWin?.PrepareForAssetReload(loadedAsset);
                }

                // Authored event metadata contains nested structures with managed strings. Keep it
                // out of generated C# setters and use the native JSON creation path with serialized data.
                bool failed;
                using (AssetPipelineCallbacks.BypassNativeDecision())
                    failed = Editor.Internal_SaveJsonAsset(path, data.ToString(Formatting.None), typeName);
                if (failed)
                    return true;

                // Match the normal disk-change path: notify item owners only after Reload has been requested.
                loadedItem?.NotifyReloaded();

                succeeded = true;
                return false;
            }
            catch (Exception ex)
            {
                Editor.LogError($"Failed to save generated FMOD asset '{path}'. Exception: {ex}");
                return true;
            }
            finally
            {
                saveScope?.Complete(succeeded);
            }
        }

        private static bool IsGeneratedAssetCurrent(string path, JToken generatedData)
        {
            if (!File.Exists(path))
                return false;
            try
            {
                var currentRoot = JToken.Parse(File.ReadAllText(path));
                var currentData = currentRoot["Data"];
                NormalizeNativeGuids(currentData);
                NormalizeNativeGuids(generatedData);
                return JToken.DeepEquals(currentData, generatedData);
            }
            catch
            {
                // Invalid or obsolete assets should flow through the normal replacement path,
                // which reports any failure with the generated asset path.
                return false;
            }
        }

        private static bool TryReadAssetId(string path, out Guid id)
        {
            id = Guid.Empty;
            try
            {
                return Guid.TryParse((string)JObject.Parse(File.ReadAllText(path))["ID"], out id);
            }
            catch
            {
                return false;
            }
        }

        private static JArray CreateParameterIdsJson(IEnumerable<AudioParameterDescription> descriptions)
        {
            return new JArray(descriptions.Select(x => CreateParameterIdJson(x.Id)));
        }

        private static JArray CreateParameterDescriptionsJson(IEnumerable<AudioParameterDescription> descriptions)
        {
            return new JArray(descriptions.Select(x => new JObject
            {
                [nameof(AudioParameterDescription.Id)] = CreateParameterIdJson(x.Id),
                [nameof(AudioParameterDescription.Minimum)] = x.Minimum,
                [nameof(AudioParameterDescription.Maximum)] = x.Maximum,
                [nameof(AudioParameterDescription.DefaultValue)] = x.DefaultValue,
                [nameof(AudioParameterDescription.Type)] = x.Type,
                [nameof(AudioParameterDescription.Flags)] = x.Flags,
                [nameof(AudioParameterDescription.Labels)] = x.Labels ?? string.Empty,
            }));
        }

        private static JObject CreateParameterIdJson(AudioParameterId id)
        {
            return new JObject
            {
                [nameof(AudioParameterId.ID)] = id.ID.ToString("N"),
                [nameof(AudioParameterId.Data1)] = id.Data1,
                [nameof(AudioParameterId.Data2)] = id.Data2,
                [nameof(AudioParameterId.Name)] = id.Name ?? string.Empty,
            };
        }

        private static void PrepareSelectedAudioAssetForReload(string contentRoot)
        {
            var editor = Editor.Instance;
            var selection = editor?.Windows?.ContentWin?.Selection;
            if (selection == null)
                return;

            var audioRoot = Path.GetFullPath(Path.Combine(contentRoot, "Audio"))
                                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
            for (var i = 0; i < selection.Count; i++)
            {
                if (selection[i] is not AssetItem item)
                    continue;
                var itemPath = Path.GetFullPath(item.Path);
                if (!itemPath.StartsWith(audioRoot, StringComparison.OrdinalIgnoreCase))
                    continue;

                var asset = FlaxEngine.Content.GetAsset(item.ID);
                if (asset != null)
                    editor.Windows.PropertiesWin?.PrepareForAssetReload(asset);
                break;
            }
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

        private static AudioParameterDescription[] ParseParameterDescriptions(FmodMetadataImporter.Parameter[] values, Report report, string context)
        {
            if (values == null || values.Length == 0)
                return Array.Empty<AudioParameterDescription>();
            var result = new List<AudioParameterDescription>(values.Length);
            foreach (var value in values)
            {
                if (value == null)
                {
                    report.Errors.Add($"FMOD metadata contains an invalid parameter in {context}.");
                    continue;
                }
                Guid parameterGuid = Guid.Empty;
                if (!string.IsNullOrWhiteSpace(value.id) && !Guid.TryParse(value.id, out parameterGuid))
                {
                    report.Errors.Add($"FMOD metadata contains an invalid parameter GUID in {context}.");
                    continue;
                }
                var parameterId = new AudioParameterId
                {
                    ID = parameterGuid,
                    Data1 = value.data1,
                    Data2 = value.data2,
                    Name = value.name ?? string.Empty,
                };
                if (parameterId.ID == Guid.Empty && parameterId.Data1 == 0 && parameterId.Data2 == 0 && string.IsNullOrWhiteSpace(parameterId.Name))
                    report.Errors.Add($"FMOD metadata contains an empty parameter in {context}.");
                else if (!result.Any(x => x.Id.Equals(parameterId)))
                    result.Add(new AudioParameterDescription
                    {
                        Id = parameterId,
                        Minimum = value.minimum,
                        Maximum = value.maximum,
                        DefaultValue = value.defaultValue,
                        Type = value.type,
                        Flags = value.flags,
                        Labels = string.Join("\n", (value.labels ?? Array.Empty<string>()).Where(x => x != null)),
                    });
            }
            return result.ToArray();
        }

        private static AudioParameterId ParseParameter(FmodMetadataImporter.Parameter value, Report report, string context)
        {
            return ParseParameterDescriptions(value == null ? null : new[] { value }, report, context).Select(x => x.Id).FirstOrDefault();
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
