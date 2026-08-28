// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using FlaxEditor.Content.Settings;
using FlaxEditor.FMOD;
using FlaxEngine;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Typed FMOD setup, inspection, validation, metering, and sound-physics commands.</summary>
    internal static class CliAudioCommands
    {
        private static readonly List<AudioEventHandle> PreviewHandles = new List<AudioEventHandle>();
        private static Script GymFocusedStation;

        private sealed class AudioAssetRecord
        {
            public string File;
            public Guid AssetId;
            public Guid BackendId;
            public string Type;
            public string Path;
        }

        private static AudioAssetRecord[] ScanAssets(string type = null)
        {
            var result = new List<AudioAssetRecord>();
            var root = Globals.ProjectContentFolder;
            foreach (var file in Directory.GetFiles(root, "*.json", SearchOption.AllDirectories))
            {
                try
                {
                    var json = JObject.Parse(File.ReadAllText(file));
                    var typeName = (string)json["TypeName"];
                    if (string.IsNullOrWhiteSpace(typeName) || !typeName.StartsWith("FlaxEngine.Audio", StringComparison.Ordinal))
                        continue;
                    if (!string.IsNullOrWhiteSpace(type) && !string.Equals(typeName, type, StringComparison.Ordinal))
                        continue;
                    var data = json["Data"] as JObject;
                    result.Add(new AudioAssetRecord
                    {
                        File = Path.GetRelativePath(root, file).Replace('\\', '/'),
                        // Flax JSON stores GUIDs as four contiguous uint32 words,
                        // which is intentionally different from System.Guid's
                        // dashed/text byte ordering. Use the engine parser so IDs
                        // emitted by CLI commands can be passed back to Content.
                        AssetId = ParseSerializedGuid((string)json["ID"]),
                        BackendId = Guid.TryParse((string)data?["BackendId"], out var backendId) ? backendId : Guid.Empty,
                        Type = typeName,
                        Path = (string)data?["Path"] ?? string.Empty,
                    });
                }
                catch
                {
                    // Non-asset JSON is valid Content and is intentionally ignored.
                }
            }
            return result.OrderBy(x => x.Type, StringComparer.Ordinal).ThenBy(x => x.Path, StringComparer.OrdinalIgnoreCase).ToArray();
        }

        private static Guid ParseSerializedGuid(string value)
        {
            if (string.IsNullOrWhiteSpace(value) || value.Length != 32)
                return Guid.Empty;
            try
            {
                return FlaxEngine.Json.JsonSerializer.ParseID(value);
            }
            catch
            {
                return Guid.Empty;
            }
        }

        private static JsonAssetReference<AudioBank> BankReference(AudioAssetRecord record)
        {
            var asset = record == null ? null : FlaxEngine.Content.Load<JsonAsset>("Content/" + record.File);
            return asset ? new JsonAssetReference<AudioBank>(asset) : default;
        }

        private static object InspectSetup()
        {
            var settings = GameSettings.Load<AudioSettings>();
            var assets = ScanAssets();
            var banks = assets.Where(x => x.Type == "FlaxEngine.AudioBank").ToArray();
            var bankFiles = Directory.Exists(Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks"))
                ? Directory.GetFiles(Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks"), "*.bank", SearchOption.AllDirectories).Select(Path.GetFileName).OrderBy(x => x).ToArray()
                : Array.Empty<string>();
            var errors = new List<string>();
            if (settings.EventBackend != AudioEventBackendType.FMODStudio) errors.Add("FMOD Studio is not the selected event backend.");
            if (!settings.MasterStringsBank) errors.Add("Master Strings bank is not assigned.");
            if (!settings.MasterBank) errors.Add("Master bank is not assigned.");
            if (bankFiles.Length == 0) errors.Add("No compiled .bank files were found under Content/Audio/Banks.");
            return new
            {
                valid = errors.Count == 0,
                errors = errors.ToArray(),
                settings = new
                {
                    backend = settings.EventBackend,
                    outputOwner = settings.OutputOwner,
                    masterStrings = settings.MasterStringsBank.Asset?.ID ?? Guid.Empty,
                    master = settings.MasterBank.Asset?.ID ?? Guid.Empty,
                    startup = (settings.StartupBanks ?? Array.Empty<JsonAssetReference<AudioBank>>()).Select(x => x.Asset?.ID ?? Guid.Empty).ToArray(),
                    settings.AudioLocale,
                    settings.EnableLiveUpdate,
                    settings.LiveUpdatePort,
                },
                assets = assets.Select(x => new { x.AssetId, x.BackendId, x.Type, x.Path, asset = x.File }).ToArray(),
                bankFiles,
            };
        }

        [CliCommand("audio.setup.inspect", Description = "Inspect derived FMOD setup state without relying on a completed flag.", Access = CliCommandAccess.ReadOnly)]
        public static object SetupInspect() => InspectSetup();

        [CliCommand("audio.setup.validate", Description = "Validate FMOD backend, master banks, startup banks, and compiled files.", Access = CliCommandAccess.ReadOnly)]
        public static object SetupValidate()
        {
            var setup = InspectSetup();
            return setup;
        }

        [CliCommand("audio.setup.build-banks", Description = "Build banks from the linked FMOD Studio project, synchronize successful output, and return exact diagnostics.", Access = CliCommandAccess.MutatesProject)]
        public static object SetupBuildBanks()
        {
            var result = FmodStudioLocator.BuildBanksDetailed();
            var synchronized = false;
            var synchronization = string.Empty;
            if (result.Success)
                synchronized = FmodProjectLinker.ImportAndSynchronize(out synchronization);
            return new
            {
                Success = result.Success && synchronized,
                BuildSucceeded = result.Success,
                Synchronized = synchronized,
                Synchronization = synchronization,
                result.Project,
                result.Executable,
                result.ExitCode,
                result.Output,
                result.Error,
                summary = result.ToDisplayString(),
            };
        }

        [CliCommand("audio.authoring.inspect", Description = "Inspect the linked FMOD Studio authoring project, command-line tool, version, and allowed script root.", Access = CliCommandAccess.ReadOnly)]
        public static object AuthoringInspect()
        {
            var project = FmodEditorSettings.StudioProjectPath;
            return new
            {
                linked = File.Exists(project),
                project,
                scriptRoot = string.IsNullOrWhiteSpace(project) ? string.Empty : Path.GetDirectoryName(project),
                executable = FmodStudioLocator.FindCommandLineExecutable(),
                version = FmodStudioLocator.GetCommandLineVersion(),
            };
        }

        [CliCommand("audio.authoring.diagnose", Description = "Run FMOD Studio project diagnostics and fail the command on invalid or corrupt authoring data.", Access = CliCommandAccess.ReadOnly)]
        public static object AuthoringDiagnose()
        {
            var result = FmodStudioLocator.RunDiagnosticsDetailed();
            if (!result.Success)
                throw new InvalidOperationException(result.ToDisplayString());
            return ToolReport(result);
        }

        private static object ToolReport(FmodStudioLocator.ToolResult result)
        {
            return new
            {
                success = result.Success,
                operation = result.Operation,
                project = result.Project,
                executable = result.Executable,
                version = result.ToolVersion,
                script = result.Script,
                exitCode = result.ExitCode,
                summary = result.Success ? "FMOD Studio " + result.Operation + " completed successfully." : result.ToDisplayString(),
            };
        }

        [CliCommand("audio.authoring.run", Description = "Run a linked-project FMOD JavaScript migration, require clean diagnostics, build banks, and synchronize typed Flax assets.", Access = CliCommandAccess.MutatesProject)]
        public static object AuthoringRun([CliOption("script", Description = "JavaScript path relative to, and contained by, the linked FMOD project.", Required = true)] string script)
        {
            // A dirty preflight is reported but does not block a migration whose
            // purpose may be to repair invalid authoring data. Every later stage
            // is fail-fast and clean post-migration diagnostics are mandatory.
            var before = FmodStudioLocator.RunDiagnosticsDetailed();
            var authoring = FmodStudioLocator.RunProjectScriptDetailed(script);
            if (!authoring.Success)
                throw new InvalidOperationException("FMOD authoring stopped before build/synchronization.\n" + authoring.ToDisplayString() +
                                                    "\n\nPreflight diagnostic:\n" + before.ToDisplayString());

            var after = FmodStudioLocator.RunDiagnosticsDetailed();
            if (!after.Success)
                throw new InvalidOperationException("FMOD authoring script completed, but post-migration diagnostics failed. Banks were not built or synchronized.\n" + after.ToDisplayString());

            var build = FmodStudioLocator.BuildBanksDetailed();
            if (!build.Success)
                throw new InvalidOperationException("FMOD authoring and diagnostics completed, but the bank build failed. Synchronization was not attempted.\n" + build.ToDisplayString());

            if (!FmodProjectLinker.ImportAndSynchronize(out var synchronization))
                throw new InvalidOperationException("FMOD banks built successfully, but typed Flax asset synchronization failed.\n" + synchronization);

            return new
            {
                success = true,
                preflightWasClean = before.Success,
                preflight = ToolReport(before),
                authoring = ToolReport(authoring),
                diagnostic = ToolReport(after),
                build = new
                {
                    build.Success,
                    build.Project,
                    build.Executable,
                    build.ExitCode,
                    summary = build.ToDisplayString(),
                },
                synchronized = true,
                synchronization,
                setup = InspectSetup(),
            };
        }

        [CliCommand("audio.browser.open", Description = "Open and focus the typed FMOD Event Browser.", Access = CliCommandAccess.ReadOnly)]
        public static object BrowserOpen()
        {
            Editor.Instance.FMOD.OpenEventBrowser();
            return new { opened = true, title = "FMOD Event Browser" };
        }

        [CliCommand("audio.setup.wizard.open", Description = "Open and focus the state-derived FMOD Setup Wizard.", Access = CliCommandAccess.ReadOnly)]
        public static object SetupWizardOpen()
        {
            FmodSetupWizard.Show(Editor.Instance);
            return new { opened = true, title = "FMOD Setup Wizard" };
        }

        [CliCommand("audio.setup.runtime-validation.start", Description = "Start the Setup Wizard measured 2D/3D runtime validation.", Access = CliCommandAccess.ReadOnly)]
        public static object SetupRuntimeValidationStart()
        {
            FmodSetupWizard.StartRuntimeValidation(Editor.Instance);
            return new { started = true, playMode = Editor.IsPlayMode };
        }

        [CliCommand("audio.setup.runtime-validation.status", Description = "Read the retained Setup Wizard runtime validation result.", Access = CliCommandAccess.ReadOnly)]
        public static object SetupRuntimeValidationStatus()
        {
            return new { passed = FmodSetupWizard.RuntimeValidationPassed, report = FmodSetupWizard.RuntimeValidationReport, timestamp = FmodSetupWizard.RuntimeValidationTime };
        }

        [CliCommand("audio.setup.apply", Description = "Discover typed bank assets and centrally assign Master, Master Strings, and startup banks.", Access = CliCommandAccess.MutatesProject)]
        public static object SetupApply([CliOption("dry-run")] bool dryRun = false)
        {
            var banks = ScanAssets("FlaxEngine.AudioBank");
            AudioAssetRecord Find(string value) => banks.FirstOrDefault(x => string.Equals(Path.GetFileNameWithoutExtension(x.Path.Replace("bank:/", string.Empty)), value, StringComparison.OrdinalIgnoreCase) || string.Equals(Path.GetFileNameWithoutExtension(x.File), value, StringComparison.OrdinalIgnoreCase));
            var masterStrings = Find("Master.strings");
            var master = Find("Master");
            if (masterStrings == null || master == null)
                throw new InvalidOperationException("Typed Master and Master Strings AudioBank assets are required before setup can be applied.");
            var startup = banks.Where(x => x != master && x != masterStrings && x.Path.IndexOf("dialogue_", StringComparison.OrdinalIgnoreCase) < 0).ToArray();
            if (!dryRun)
            {
                var masterStringsReference = BankReference(masterStrings);
                var masterReference = BankReference(master);
                var startupReferences = startup.Select(BankReference).ToArray();
                if (!masterStringsReference || !masterReference || startupReferences.Any(reference => !reference))
                    throw new InvalidOperationException("One or more discovered bank assets are not registered in the live Content database. Refresh assets or restart the Editor before applying setup; existing settings were not changed.");
                var settings = GameSettings.Load<AudioSettings>();
                settings.EventBackend = AudioEventBackendType.FMODStudio;
                settings.OutputOwner = AudioOutputOwner.EventBackend;
                settings.MasterStringsBank = masterStringsReference;
                settings.MasterBank = masterReference;
                settings.StartupBanks = startupReferences;
                if (GameSettings.Save(settings))
                    throw new InvalidOperationException("Failed to save Audio Settings.");
            }
            return new { dryRun, masterStrings = masterStrings.AssetId, master = master.AssetId, startup = startup.Select(x => x.AssetId).ToArray(), persisted = !dryRun, state = InspectSetup() };
        }

        [CliCommand("audio.banks.list", Description = "List typed bank assets and live runtime bank states.", Access = CliCommandAccess.ReadOnly)]
        public static object BanksList()
        {
            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            return new { assets = ScanAssets("FlaxEngine.AudioBank").Select(x => new { x.AssetId, x.BackendId, x.Path, asset = x.File }).ToArray(), runtime = snapshot.Banks };
        }

        [CliCommand("audio.banks.discover", Description = "Discover compiled bank files and typed bank assets without mutating content.", Access = CliCommandAccess.ReadOnly)]
        public static object BanksDiscover() => new
        {
            files = Directory.Exists(Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks"))
                ? Directory.GetFiles(Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks"), "*.bank", SearchOption.AllDirectories).Select(x => Path.GetRelativePath(Globals.ProjectContentFolder, x).Replace('\\', '/')).OrderBy(x => x).ToArray()
                : Array.Empty<string>(),
            assets = ScanAssets("FlaxEngine.AudioBank").Select(x => new { x.AssetId, x.BackendId, x.Path, asset = x.File }).ToArray(),
        };

        [CliCommand("audio.banks.validate", Description = "Validate typed bank paths against compiled files and detect duplicate IDs.", Access = CliCommandAccess.ReadOnly)]
        public static object BanksValidate()
        {
            var banks = ScanAssets("FlaxEngine.AudioBank");
            var issues = new List<object>();
            foreach (var bank in banks)
            {
                var path = bank.Path.Replace("bank:/", string.Empty).Replace('\\', '/');
                if (!path.EndsWith(".bank", StringComparison.OrdinalIgnoreCase)) path += ".bank";
                var fileName = Path.GetFileName(path);
                var found = Directory.GetFiles(Globals.ProjectContentFolder, fileName, SearchOption.AllDirectories).FirstOrDefault();
                if (found == null)
                {
                    var baseName = Path.GetFileNameWithoutExtension(fileName);
                    found = Directory.GetFiles(Globals.ProjectContentFolder, baseName + "_*.bank", SearchOption.AllDirectories).FirstOrDefault();
                }
                if (found == null) issues.Add(new { bank.AssetId, issue = "compiled bank missing", bank.Path });
                if (bank.BackendId == Guid.Empty) issues.Add(new { bank.AssetId, issue = "empty backend ID", bank.Path });
            }
            foreach (var duplicate in banks.Where(x => x.BackendId != Guid.Empty).GroupBy(x => x.BackendId).Where(x => x.Count() > 1))
                issues.Add(new { backendId = duplicate.Key, issue = "duplicate backend ID", assets = duplicate.Select(x => x.AssetId).ToArray() });
            return new { valid = issues.Count == 0, issues = issues.ToArray(), count = banks.Length };
        }

        [CliCommand("audio.banks.sync", Description = "Normalize typed bank assets to compiled filenames while preserving stable asset IDs.", Access = CliCommandAccess.MutatesProject)]
        public static object BanksSync([CliOption("dry-run")] bool dryRun = false)
        {
            var changed = new List<object>();
            foreach (var record in ScanAssets("FlaxEngine.AudioBank"))
            {
                var normalized = record.Path.Replace("bank:/", string.Empty).Replace('\\', '/');
                if (!normalized.EndsWith(".bank", StringComparison.OrdinalIgnoreCase))
                    normalized += ".bank";
                normalized = Path.GetFileName(normalized);
                if (string.Equals(record.Path, normalized, StringComparison.Ordinal))
                    continue;
                changed.Add(new { record.AssetId, before = record.Path, after = normalized });
                if (dryRun)
                    continue;
                var jsonAsset = FlaxEngine.Content.LoadAsync<JsonAsset>(record.AssetId);
                if (jsonAsset && jsonAsset.WaitForLoaded())
                    jsonAsset = null;
                var bank = jsonAsset ? new JsonAssetReference<AudioBank>(jsonAsset).Instance : null;
                if (bank == null)
                    throw new InvalidOperationException($"Failed to load typed AudioBank asset '{record.File}'.");
                bank.Path = normalized;
                if (Editor.SaveJsonAsset(Path.Combine(Globals.ProjectContentFolder, record.File.Replace('/', Path.DirectorySeparatorChar)), bank))
                    throw new InvalidOperationException($"Failed to save typed AudioBank asset '{record.File}'.");
            }
            return new { dryRun, synchronized = !dryRun, changed = changed.ToArray(), validation = BanksValidate() };
        }

        [CliCommand("audio.banks.load", Description = "Load one bank by stable backend ID and file path.", Access = CliCommandAccess.ReadOnly)]
        public static object BankLoad([CliOption("id", Required = true)] Guid id, [CliOption("path", Required = true)] string path, [CliOption("preload")] bool preload = false)
        {
            var loaded = AudioEventSystem.LoadBank(id, path, false);
            var samples = loaded && (!preload || AudioEventSystem.LoadBankSampleData(id));
            return new { id, path, loaded, samples };
        }

        [CliCommand("audio.banks.unload", Description = "Unload one bank by stable backend ID and file path.", Access = CliCommandAccess.ReadOnly)]
        public static object BankUnload([CliOption("id", Required = true)] Guid id, [CliOption("path")] string path = null) => new { id, path, unloaded = AudioEventSystem.UnloadBank(id, path ?? string.Empty) };

        [CliCommand("audio.events.list", Description = "List typed event assets with stable IDs and metadata paths.", Access = CliCommandAccess.ReadOnly)]
        public static object EventsList([CliOption("search")] string search = null)
        {
            var events = ScanAssets("FlaxEngine.AudioEvent");
            if (!string.IsNullOrWhiteSpace(search))
                events = events.Where(x => x.Path.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0 || x.File.IndexOf(search, StringComparison.OrdinalIgnoreCase) >= 0).ToArray();
            return events.Select(x => new { x.AssetId, x.BackendId, x.Path, asset = x.File }).ToArray();
        }

        [CliCommand("audio.events.find", Description = "Find typed events by path or asset name.", Access = CliCommandAccess.ReadOnly)]
        public static object EventsFind([CliOption("search", Required = true)] string search) => EventsList(search);

        [CliCommand("audio.events.inspect", Description = "Inspect one event asset by stable asset or backend ID.", Access = CliCommandAccess.ReadOnly)]
        public static object EventInspect([CliOption("id", Required = true)] Guid id)
        {
            var value = ScanAssets("FlaxEngine.AudioEvent").FirstOrDefault(x => x.AssetId == id || x.BackendId == id);
            if (value == null) throw new InvalidOperationException($"AudioEvent '{id}' was not found.");
            return new { value.AssetId, value.BackendId, value.Path, asset = value.File };
        }

        [CliCommand("audio.events.parameters", Description = "List authored middleware parameters and numeric ranges for one typed event.", Access = CliCommandAccess.ReadOnly)]
        public static object EventParameters([CliOption("id", Required = true)] Guid id)
        {
            var value = ScanAssets("FlaxEngine.AudioEvent").FirstOrDefault(x => x.AssetId == id || x.BackendId == id);
            if (value == null) throw new InvalidOperationException($"AudioEvent '{id}' was not found.");
            if (!AudioEventSystem.GetEventParameters(value.BackendId, value.Path, out var parameters))
                throw new InvalidOperationException($"Parameters for '{value.Path}' could not be read from the active backend.");
            return new
            {
                value.AssetId,
                value.BackendId,
                value.Path,
                parameters = parameters.Select(x => new { x.Id.Name, x.Id.Data1, x.Id.Data2, x.Minimum, x.Maximum, x.DefaultValue, x.Type, x.Flags }).ToArray(),
            };
        }

        [CliCommand("audio.events.preview", Description = "Create and play a typed event for runtime preview.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static object EventPreview([CliOption("id", Required = true)] Guid id, [CliOption("listener-mask")] uint listenerMask = 1)
        {
            var value = ScanAssets("FlaxEngine.AudioEvent").FirstOrDefault(x => x.AssetId == id || x.BackendId == id);
            if (value == null) throw new InvalidOperationException($"AudioEvent '{id}' was not found.");
            var options = new AudioEventCreateOptions { AutoPlay = true, ListenerMask = listenerMask };
            var handle = AudioEventSystem.CreateInstance(value.BackendId, value.Path, options);
            if (handle.Generation == 0) throw new InvalidOperationException($"Preview instance for '{value.Path}' could not be created.");
            PreviewHandles.RemoveAll(x => !AudioEventSystem.QueryInstance(x, out _));
            PreviewHandles.Add(handle);
            return new { value.AssetId, value.BackendId, value.Path, handle = new { handle.Index, handle.Generation }, started = true };
        }

        [CliCommand("audio.events.preview.stop", Description = "Stop and release one CLI-owned preview instance.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static object EventPreviewStop([CliOption("index", Required = true)] uint index, [CliOption("generation", Required = true)] uint generation)
        {
            var handle = new AudioEventHandle { Index = index, Generation = generation };
            var owned = PreviewHandles.Remove(handle);
            var stopped = owned && AudioEventSystem.StopAndRelease(handle, AudioStopMode.Immediate);
            return new { handle = new { index, generation }, owned, stopped };
        }

        [CliCommand("audio.events.preview.stop-all", Description = "Stop and release every CLI-owned preview instance.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static object EventPreviewStopAll()
        {
            var stopped = 0;
            foreach (var handle in PreviewHandles)
                if (AudioEventSystem.StopAndRelease(handle, AudioStopMode.Immediate))
                    stopped++;
            var requested = PreviewHandles.Count;
            PreviewHandles.Clear();
            return new { requested, stopped };
        }

        [CliCommand("audio.emitters.inspect", Description = "Inspect all live AudioEmitter Actors and backend signal-path state.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object EmittersInspect()
        {
            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            return Level.GetActors<AudioEmitter>().Select(x =>
            {
                var runtime = snapshot.Events?.FirstOrDefault(e => e.OwnerId == x.ID);
                return new
                {
                    actorId = x.ID,
                    x.Name,
                    eventId = x.Event.Asset?.ID ?? Guid.Empty,
                    x.EventPath,
                    x.ListenerMask,
                    x.PlaybackState,
                    x.LastPlayError,
                    runtime = ToSerializableEvent(runtime),
                };
            }).ToArray();
        }

        [CliCommand("audio.emitters.validate", Description = "Validate event references, masks, and current runtime silence causes.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object EmittersValidate()
        {
            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            var issues = new List<object>();
            foreach (var emitter in Level.GetActors<AudioEmitter>())
            {
                if (!emitter.Event) issues.Add(new { emitter = emitter.ID, issue = "missing typed event" });
                if (emitter.ListenerMask == 0) issues.Add(new { emitter = emitter.ID, issue = "zero listener mask" });
                var runtime = snapshot.Events?.FirstOrDefault(x => x.OwnerId == emitter.ID);
                if (runtime.HasValue && runtime.Value.Playing && !runtime.Value.Audible)
                    issues.Add(new { emitter = emitter.ID, issue = string.IsNullOrWhiteSpace(runtime.Value.SilenceCause) ? "playing instance is not audible" : runtime.Value.SilenceCause });
            }
            return new { valid = issues.Count == 0, issues = issues.ToArray() };
        }

        [CliCommand("audio.emitters.trace", Description = "Return the complete diagnostic record for one emitter.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object EmittersTrace([CliOption("actor", Required = true)] Guid actor)
        {
            var emitter = Level.GetActors<AudioEmitter>().FirstOrDefault(x => x.ID == actor);
            if (emitter == null) throw new InvalidOperationException($"AudioEmitter '{actor}' was not found.");
            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            return new { emitter = new { emitter.ID, emitter.Name, eventId = emitter.Event.Asset?.ID ?? Guid.Empty, emitter.ListenerMask, emitter.LastPlayError }, runtime = ToSerializableEvent(snapshot.Events?.FirstOrDefault(x => x.OwnerId == actor)), output = OutputMeter() };
        }

        private static object ToSerializableEvent(AudioEventRuntimeInfo? value)
        {
            if (!value.HasValue || value.Value.Handle.Generation == 0)
                return null;
            var x = value.Value;
            var source = x.SourcePositionCentimeters;
            var listener = x.ListenerPositionMeters;
            return new
            {
                x.OwnerId, x.Handle, x.EventId, x.Path, x.PlaybackState,
                x.Started, x.Playing, x.Audible, x.IsVirtual, x.ListenerMask,
                x.Volume, x.FinalVolume, x.Audibility, x.TimelinePosition,
                sourcePositionCentimeters = new { source.X, source.Y, source.Z },
                listenerPositionMeters = new { listener.X, listener.Y, listener.Z },
                x.DistanceMeters, x.MinimumDistanceMeters, x.MaximumDistanceMeters,
                x.SilenceCause,
            };
        }

        [CliCommand("audio.listeners.inspect", Description = "Inspect explicit listener indices, weights, and transforms.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object ListenersInspect() => Level.GetActors<AudioListener>().Select(x =>
        {
            var position = x.AttenuationActor?.Position ?? x.Position;
            var meters = position * 0.01f;
            var velocity = x.Velocity;
            return new
            {
                actorId = x.ID,
                x.Name,
                x.ListenerIndex,
                x.ListenerWeight,
                attenuationActor = x.AttenuationActor?.ID,
                positionCentimeters = new { position.X, position.Y, position.Z },
                positionMeters = new { meters.X, meters.Y, meters.Z },
                velocityCentimetersPerSecond = new { velocity.X, velocity.Y, velocity.Z },
            };
        }).ToArray();

        [CliCommand("audio.listeners.validate", Description = "Validate explicit listener indices, masks, and active listener availability.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object ListenersValidate()
        {
            var listeners = Level.GetActors<AudioListener>();
            var issues = new List<object>();
            if (listeners.Length == 0) issues.Add(new { issue = "no AudioListener in loaded scenes" });
            foreach (var duplicate in listeners.Where(x => x.IsActiveInHierarchy).GroupBy(x => x.ListenerIndex).Where(x => x.Count() > 1))
                issues.Add(new { listenerIndex = duplicate.Key, issue = "duplicate active listener index", actors = duplicate.Select(x => x.ID).ToArray() });
            return new { valid = issues.Count == 0, issues = issues.ToArray(), listeners = ListenersInspect() };
        }

        [CliCommand("audio.output.meter", Description = "Read the measured FMOD master output meter.", Access = CliCommandAccess.ReadOnly)]
        public static object OutputMeter()
        {
            AudioEventSystem.CaptureDiagnostics(out var value);
            return new { rms = value.OutputRms, peak = value.OutputPeak, combinedRms = value.CombinedOutputRms, dbfs = value.CombinedOutputDbfs, clipping = value.OutputClipping, value.SecondsSinceNonSilentOutput, reachingOutput = value.CombinedOutputRms > 0.00001f };
        }

        [CliCommand("audio.output.assert-audible", Description = "Fail unless measured master RMS exceeds a bounded threshold.", Access = CliCommandAccess.ReadOnly, RequiresPlayMode = true)]
        public static object AssertAudible([CliOption("minimum-rms")] float minimumRms = 0.00001f)
        {
            AudioEventSystem.CaptureDiagnostics(out var value);
            if (value.CombinedOutputRms < minimumRms)
                throw new InvalidOperationException($"Master output is silent: RMS {value.CombinedOutputRms:0.000000} is below {minimumRms:0.000000}. Active events: {value.ActiveInstances}; listeners: {value.ListenerCount}; last FMOD error: {value.LastError}.");
            return new { audible = true, rms = value.CombinedOutputRms, value.CombinedOutputDbfs };
        }

        [CliCommand("audio.physics.inspect", Description = "Inspect authorable AudioPhysics components and runtime contact explanations.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object PhysicsInspect() => Level.GetScripts<AudioPhysics>().Select(x => new { componentId = x.ID, actorId = x.Actor?.ID, x.EnableImpact, impactRules = x.ImpactRules?.Length ?? 0, x.EnableFriction, frictionRules = x.FrictionRules?.Length ?? 0, x.EnableExit, exitRules = x.ExitRules?.Length ?? 0, x.ActiveFrictionVoices, x.LastSelectedEvent, x.LastIntensity, x.LastExplanation }).ToArray();

        [CliCommand("audio.physics.validate", Description = "Validate all AudioPhysics components for colliders and usable rules.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object PhysicsValidate()
        {
            return Level.GetScripts<AudioPhysics>().Select(x => { var valid = x.Validate(out var message); return new { componentId = x.ID, actorId = x.Actor?.ID, valid, message }; }).ToArray();
        }

        [CliCommand("audio.physics.simulate-impact", Description = "Deterministically simulate one contact against an AudioPhysics component.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object PhysicsSimulate([CliOption("component", Required = true)] Guid component, [CliOption("speed")] float speed = 100.0f, [CliOption("normal-speed")] float normalSpeed = 100.0f, [CliOption("impulse")] float impulse = 1.0f, [CliOption("trigger")] bool trigger = false)
        {
            var value = Level.GetScripts<AudioPhysics>().FirstOrDefault(x => x.ID == component);
            if (value == null) throw new InvalidOperationException($"AudioPhysics component '{component}' was not found.");
            var played = value.SimulateImpact(speed, normalSpeed, impulse, trigger);
            return new { component, played, value.LastSelectedEvent, value.LastIntensity, value.LastExplanation };
        }

        [CliCommand("audio.physics.assert-contact-audible", Description = "Simulate a deterministic impact and require measured output.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object PhysicsAssertContactAudible([CliOption("component", Required = true)] Guid component, [CliOption("minimum-rms")] float minimumRms = 0.00001f)
        {
            var simulation = PhysicsSimulate(component, 300.0f, 300.0f, 10.0f, false);
            var audible = AssertAudible(minimumRms);
            return new { simulation, audible };
        }

        [CliCommand("audio.references.validate", Description = "Find obsolete string fallbacks and missing typed event/bank references.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object ReferencesValidate()
        {
            var issues = new List<object>();
            foreach (var emitter in Level.GetActors<AudioEmitter>())
            {
                if (!emitter.Event && !string.IsNullOrWhiteSpace(emitter.EventPath)) issues.Add(new { owner = emitter.ID, property = "Event", issue = "obsolete string fallback", emitter.EventPath });
                if (!emitter.Event && string.IsNullOrWhiteSpace(emitter.EventPath)) issues.Add(new { owner = emitter.ID, property = "Event", issue = "missing event reference", emitter.EventPath });
            }
            foreach (var loader in Level.GetActors<AudioBankLoader>())
                if ((loader.Banks == null || loader.Banks.Length == 0) && (loader.BankPaths == null || loader.BankPaths.Length == 0)) issues.Add(new { owner = loader.ID, property = "Banks", issue = "missing bank reference" });
            return new { valid = issues.Count == 0, issues = issues.ToArray() };
        }

        private static bool IsMissingReferenceValue(object value, Type type)
        {
            if (typeof(FlaxEngine.Object).IsAssignableFrom(type))
                return value is not FlaxEngine.Object obj || !obj;
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(JsonAssetReference<>))
            {
                if (value == null)
                    return true;
                var asset = type.GetField("Asset")?.GetValue(value) as JsonAsset;
                return !asset;
            }
            return false;
        }

        private static List<object> ValidateAudioGymReferences(out int stationCount)
        {
            var issues = new List<object>();
            stationCount = 0;
            foreach (var script in Level.GetScripts<Script>())
            {
                var type = script.GetType();
                var isStation = script.TypeName.IndexOf("Station", StringComparison.OrdinalIgnoreCase) >= 0;
                var isAudioGym = script.TypeName.IndexOf("AudioGym", StringComparison.OrdinalIgnoreCase) >= 0 ||
                                 string.Equals(type.Assembly.GetName().Name, "AudioGym", StringComparison.OrdinalIgnoreCase);
                if (!isAudioGym && !isStation)
                    continue;
                if (isStation)
                {
                    stationCount++;
                    if (script.Actor is not Collider collider)
                        issues.Add(new { owner = script.ID, property = "Actor", issue = "station must be attached to a Collider" });
                    else
                    {
                        if (!collider.IsTrigger)
                            issues.Add(new { owner = script.ID, property = "IsTrigger", issue = "station collider is not a trigger" });
                        if (collider.Center.LengthSquared > 0.0001f)
                            issues.Add(new { owner = script.ID, property = "Center", issue = "station collider uses a nonzero local center", value = collider.Center });
                    }
                }

                foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.DeclaredOnly))
                {
                    var fieldType = field.FieldType;
                    var value = field.GetValue(script);
                    if (fieldType.IsArray)
                    {
                        var elementType = fieldType.GetElementType();
                        var referenceElements = typeof(FlaxEngine.Object).IsAssignableFrom(elementType) ||
                                                (elementType.IsGenericType && elementType.GetGenericTypeDefinition() == typeof(JsonAssetReference<>));
                        if (!referenceElements)
                            continue;
                        if (value is not Array array || array.Length == 0)
                        {
                            issues.Add(new { owner = script.ID, property = field.Name, issue = "missing required reference array" });
                            continue;
                        }
                        for (var i = 0; i < array.Length; i++)
                            if (IsMissingReferenceValue(array.GetValue(i), elementType))
                                issues.Add(new { owner = script.ID, property = $"{field.Name}[{i}]", issue = "missing or unresolved reference" });
                    }
                    else if ((typeof(FlaxEngine.Object).IsAssignableFrom(fieldType) ||
                              (fieldType.IsGenericType && fieldType.GetGenericTypeDefinition() == typeof(JsonAssetReference<>))) &&
                             IsMissingReferenceValue(value, fieldType))
                    {
                        issues.Add(new { owner = script.ID, property = field.Name, issue = "missing or unresolved reference" });
                    }
                }
            }
            return issues;
        }

        [CliCommand("audio.references.repair", Description = "Repair unambiguous legacy event paths to stable typed asset references.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ReferencesRepair([CliOption("dry-run")] bool dryRun = true)
        {
            var repaired = new List<object>();
            var events = ScanAssets("FlaxEngine.AudioEvent").GroupBy(x => x.Path, StringComparer.OrdinalIgnoreCase).ToDictionary(x => x.Key, x => x.ToArray(), StringComparer.OrdinalIgnoreCase);
            var editedScenes = new HashSet<Scene>();
            foreach (var emitter in Level.GetActors<AudioEmitter>())
            {
                if (emitter.Event || string.IsNullOrWhiteSpace(emitter.EventPath) || !events.TryGetValue(emitter.EventPath, out var matches) || matches.Length != 1)
                    continue;
                var match = matches[0];
                repaired.Add(new { owner = emitter.ID, property = "Event", path = emitter.EventPath, assetId = match.AssetId });
                if (dryRun)
                    continue;
                var asset = FlaxEngine.Content.LoadAsync<JsonAsset>(match.AssetId);
                if (!asset || asset.WaitForLoaded())
                    throw new InvalidOperationException($"Failed to load AudioEvent asset '{match.File}'.");
                emitter.Event = new JsonAssetReference<AudioEvent>(asset);
                if (emitter.Scene)
                    editedScenes.Add(emitter.Scene);
            }
            if (!dryRun)
            {
                foreach (var scene in editedScenes)
                {
                    Editor.Instance.Scene.MarkSceneEdited(scene);
                    if (!Editor.Instance.Scene.SaveSceneSynchronously(scene))
                        throw new InvalidOperationException($"Failed to save repaired scene '{scene.Name}'.");
                }
            }
            return new { dryRun, repaired = repaired.ToArray(), persisted = !dryRun, validation = ReferencesValidate() };
        }

        [CliCommand("audio.gym.validate", Description = "Validate AudioGym station discovery, controls, references, and output diagnostics.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GymValidate()
        {
            var scripts = Level.GetScripts<Script>().Where(x => x.TypeName.IndexOf("AudioGym", StringComparison.OrdinalIgnoreCase) >= 0 || x.TypeName.IndexOf("Station", StringComparison.OrdinalIgnoreCase) >= 0).ToArray();
            var issues = ValidateAudioGymReferences(out var stationCount);
            AudioEventSystem.CaptureDiagnostics(out var snapshot);
            var valid = stationCount >= 12 && issues.Count == 0 && snapshot.LastErrorCode == 0;
            return new { valid, scripts = scripts.Select(x => new { x.ID, actorId = x.Actor?.ID, x.TypeName }).ToArray(), stationCount, listeners = Level.GetActors<AudioListener>().Length, emitters = Level.GetActors<AudioEmitter>().Length, outputRms = snapshot.CombinedOutputRms, lastErrorCode = snapshot.LastErrorCode, errors = snapshot.LastError, issues = issues.ToArray() };
        }

        [CliCommand("audio.gym.playtest", Description = "Validate the twelve-station contract and measured output during deterministic play.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object GymPlaytest([CliOption("minimum-rms")] float minimumRms = 0.00001f)
        {
            var validation = GymValidate();
            var issues = ValidateAudioGymReferences(out var stations);
            if (stations < 12) throw new InvalidOperationException($"AudioGym has {stations} stations; 12 are required.");
            if (issues.Count != 0) throw new InvalidOperationException($"AudioGym has {issues.Count} missing, unresolved, or invalid station reference(s).");
            return new { validation, audible = AssertAudible(minimumRms), stations };
        }

        private static Script FindGymStation(int station)
        {
            foreach (var script in Level.GetScripts<Script>())
            {
                var field = script.GetType().GetField("StationId", BindingFlags.Instance | BindingFlags.Public);
                if (field?.FieldType == typeof(int) && (int)field.GetValue(script) == station)
                    return script;
            }
            throw new InvalidOperationException($"AudioGym station {station} was not found.");
        }

        private static void InvokeStationLifecycle(Script script, string method)
        {
            script?.GetType().GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic)?.Invoke(script, null);
        }

        [CliCommand("audio.gym.focus", Description = "Focus one AudioGym station deterministically and move the runtime player into its trigger.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object GymFocus([CliOption("station", Required = true)] int station)
        {
            if (station < 1 || station > 12)
                throw new ArgumentOutOfRangeException(nameof(station), "Station must be in the range 1-12.");
            var script = FindGymStation(station);
            if (GymFocusedStation != null && GymFocusedStation != script)
                InvokeStationLifecycle(GymFocusedStation, "OnFocusExited");
            var focus = script.Actor is Collider collider ? collider.Transform.LocalToWorld(collider.Center) : script.Actor.Position;
            var player = Level.GetActors<CharacterController>().FirstOrDefault(x => string.Equals(x.Name, "Player", StringComparison.OrdinalIgnoreCase)) ?? Level.GetActors<CharacterController>().FirstOrDefault();
            if (player == null)
                throw new InvalidOperationException("Runtime CharacterController player was not found.");
            player.Position = focus - Vector3.Up * 60.0f;
            InvokeStationLifecycle(script, "OnFocusEntered");
            GymFocusedStation = script;
            return new
            {
                station,
                title = script.GetType().GetField("Title", BindingFlags.Instance | BindingFlags.Public)?.GetValue(script),
                scriptId = script.ID,
                actorId = script.Actor.ID,
                focus = new { x = focus.X, y = focus.Y, z = focus.Z },
                player = new { x = player.Position.X, y = player.Position.Y, z = player.Position.Z }
            };
        }

        [CliCommand("audio.gym.control", Description = "Invoke a numbered control on an AudioGym station and return output diagnostics.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object GymControl([CliOption("station", Required = true)] int station, [CliOption("slot", Required = true)] int slot, [CliOption("reverse")] bool reverse = false)
        {
            if (slot < 1 || slot > 6)
                throw new ArgumentOutOfRangeException(nameof(slot), "Slot must be in the range 1-6.");
            var script = FindGymStation(station);
            var method = script.GetType().GetMethod("ApplyControl", BindingFlags.Instance | BindingFlags.Public);
            if (method == null)
                throw new InvalidOperationException($"Station {station} does not expose ApplyControl.");
            method.Invoke(script, new object[] { slot, reverse });
            AudioEventSystem.CaptureDiagnostics(out var diagnostics);
            return new { station, slot, reverse, outputRms = diagnostics.CombinedOutputRms, outputDbfs = diagnostics.CombinedOutputDbfs, activeInstances = diagnostics.ActiveInstances, lastErrorCode = diagnostics.LastErrorCode, lastError = diagnostics.LastError };
        }

        [CliCommand("audio.gym.self-test", Description = "Run the station-authored backend readback self-test.", Access = CliCommandAccess.ReadOnly, RequiresScene = true, RequiresPlayMode = true)]
        public static object GymSelfTest([CliOption("station", Required = true)] int station)
        {
            var script = FindGymStation(station);
            var method = script.GetType().GetMethod("RunSelfTest", BindingFlags.Instance | BindingFlags.Public);
            if (method == null)
                throw new InvalidOperationException($"Station {station} does not expose RunSelfTest.");
            return new { station, result = method.Invoke(script, null) };
        }
    }
}
