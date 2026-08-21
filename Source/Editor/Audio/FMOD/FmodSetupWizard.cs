// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using FlaxEditor.Content.Settings;
using FlaxEngine;
using FlaxEngine.GUI;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.FMOD
{
    /// <summary>State-derived FMOD setup workflow. It intentionally stores no completed flag.</summary>
    internal static class FmodSetupWizard
    {
        private static FmodSetupWizardWindow _window;
        private static bool _runtimeValidationPassed;
        private static string _runtimeValidationReport = "Not run in this editor session.";
        private static DateTime _runtimeValidationTime;
        private sealed class BankAsset
        {
            public string File;
            public string Path;
            public JsonAsset Asset;
        }

        private static BankAsset[] FindBanks()
        {
            var result = new List<BankAsset>();
            foreach (var file in Directory.GetFiles(Globals.ProjectContentFolder, "*.json", SearchOption.AllDirectories))
            {
                try
                {
                    var json = JObject.Parse(File.ReadAllText(file));
                    if ((string)json["TypeName"] != "FlaxEngine.AudioBank")
                        continue;
                    var relative = "Content/" + System.IO.Path.GetRelativePath(Globals.ProjectContentFolder, file).Replace('\\', '/');
                    var asset = FlaxEngine.Content.Load<JsonAsset>(relative);
                    if (asset)
                        result.Add(new BankAsset { File = relative, Path = (string)json["Data"]?["Path"] ?? string.Empty, Asset = asset });
                }
                catch
                {
                }
            }
            return result.ToArray();
        }

        private static BankAsset Find(BankAsset[] banks, string name)
        {
            return banks.FirstOrDefault(x => string.Equals(System.IO.Path.GetFileNameWithoutExtension(x.Path.Replace("bank:/", string.Empty)), name, StringComparison.OrdinalIgnoreCase) || string.Equals(System.IO.Path.GetFileNameWithoutExtension(x.File), name, StringComparison.OrdinalIgnoreCase));
        }

        public static string Validate(bool log)
        {
            var settings = GameSettings.Load<AudioSettings>();
            var banks = FindBanks();
            var compiled = Directory.Exists(System.IO.Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks")) && Directory.GetFiles(System.IO.Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks"), "*.bank", SearchOption.AllDirectories).Length > 0;
            var listenerCount = Level.GetActors<AudioListener>().Length;
            var emitters = Level.GetActors<AudioEmitter>();
            var emitterCount = emitters.Length;
            AudioEventSystem.CaptureDiagnostics(out var diagnostics);
            var unresolvedEmitters = diagnostics.Initialized
                ? emitters.Where(emitter =>
                {
                    var audioEvent = emitter.Event.Instance;
                    var id = audioEvent?.BackendId ?? Guid.Empty;
                    var path = !string.IsNullOrWhiteSpace(audioEvent?.Path) ? audioEvent.Path : emitter.EventPath;
                    return !AudioEventSystem.GetEventParameters(id, path, out _);
                }).Select(emitter => emitter.Name).ToArray()
                : Array.Empty<string>();
            var emittersConfigured = emitterCount == 0 || emitters.All(x => x.Event || !string.IsNullOrWhiteSpace(x.EventPath));
            var emittersResolved = !diagnostics.Initialized || unresolvedEmitters.Length == 0;
            var pages = new[]
            {
                ("1. Version and libraries", (diagnostics.Initialized || settings.EventBackend == AudioEventBackendType.FMODStudio) && !string.IsNullOrEmpty(FmodStudioLocator.FindCommandLineExecutable()),
                    (diagnostics.RuntimeVersion ?? "backend starts with Play mode") + "; " + FmodStudioLocator.GetInstallationSummary().Replace('\n', ' ')),
                ("2. Linking", !string.IsNullOrWhiteSpace(FmodEditorSettings.StudioProjectPath) || compiled, string.IsNullOrWhiteSpace(FmodEditorSettings.StudioProjectPath) ? "compiled bank folder" : FmodEditorSettings.StudioProjectPath),
                ("3. Banks and metadata", settings.MasterStringsBank && settings.MasterBank, $"{banks.Length} typed banks"),
                ("4. Output ownership", settings.OutputOwner == AudioOutputOwner.EventBackend, settings.OutputOwner.ToString()),
                ("5. Listeners", listenerCount > 0, $"{listenerCount} listener(s)"),
                ("6. Sources and emitters", emittersConfigured && emittersResolved,
                    unresolvedEmitters.Length == 0 ? $"{emitterCount} emitter(s); runtime lookup resolved" : $"unresolved runtime event: {string.Join(", ", unresolvedEmitters)}"),
                ("7. Runtime configuration", settings.FmodMaxChannels >= 32 && settings.FmodRealChannels >= 1,
                    $"{settings.FmodRealChannels} real / {settings.FmodMaxChannels} virtual channels; Live Update {settings.EnableLiveUpdate}:{settings.LiveUpdatePort}"),
                ("8. Source control", true, "shared settings contain no per-user Studio path"),
                ("9. Validation test", _runtimeValidationPassed || diagnostics.CombinedOutputRms > 0.00001f,
                    _runtimeValidationPassed
                        ? $"{_runtimeValidationReport} ({_runtimeValidationTime:HH:mm:ss})"
                        : diagnostics.CombinedOutputRms > 0.00001f
                            ? $"live scene output RMS {diagnostics.CombinedOutputRms:0.000000}"
                            : "click Run validation; Flax will start Play and execute measured 2D and 3D probes"),
                ("10. Summary", compiled, compiled ? "compiled banks present" : "compiled banks missing"),
            };
            var builder = new StringBuilder();
            builder.AppendLine("FMOD setup is derived from current project state:");
            foreach (var page in pages)
                builder.Append(page.Item2 ? "PASS  " : "TODO  ").Append(page.Item1).Append(": ").AppendLine(page.Item3);
            var text = builder.ToString();
            if (log)
            {
                foreach (var line in text.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    if (line.StartsWith("TODO", StringComparison.Ordinal)) Editor.LogWarning(line);
                    else Editor.Log(line);
                }
            }
            return text;
        }

        public static string GetIncompleteSummary()
        {
            var lines = Validate(false).Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .Where(x => x.StartsWith("TODO", StringComparison.Ordinal))
                .ToArray();
            if (lines.Length == 0)
                return "PASS — every setup check is complete.";
            return "ACTION REQUIRED\n" + string.Join("\n", lines) +
                   "\n\nUse the instruction after each check, then Refresh status.";
        }

        public static void RecordRuntimeValidation(bool passed, string report)
        {
            _runtimeValidationPassed = passed;
            _runtimeValidationReport = string.IsNullOrWhiteSpace(report) ? (passed ? "Runtime probes passed" : "Runtime probes failed") : report;
            _runtimeValidationTime = DateTime.Now;
        }

        public static string RuntimeValidationReport => _runtimeValidationReport;
        public static bool RuntimeValidationPassed => _runtimeValidationPassed;
        public static DateTime RuntimeValidationTime => _runtimeValidationTime;

        public static void StartRuntimeValidation(Editor editor)
        {
            Show(editor);
            _window.StartRuntimeValidation();
        }

        public static void Show(Editor editor)
        {
            _window ??= new FmodSetupWizardWindow(editor);
            _window.Show();
            _window.Focus();
            _window.RefreshState();
        }

        public static string ApplyDiscoveredSettings()
        {
            var banks = FindBanks();
            var masterStrings = Find(banks, "Master.strings");
            var master = Find(banks, "Master");
            if (masterStrings == null || master == null)
                return "Synchronize typed Master and Master Strings bank assets first.";
            var settings = GameSettings.Load<AudioSettings>();
            settings.EventBackend = AudioEventBackendType.FMODStudio;
            settings.OutputOwner = AudioOutputOwner.EventBackend;
            settings.MasterStringsBank = masterStrings.Asset;
            settings.MasterBank = master.Asset;
            settings.StartupBanks = banks.Where(x => x != master && x != masterStrings && x.Path.IndexOf("dialogue_", StringComparison.OrdinalIgnoreCase) < 0).Select(x => new JsonAssetReference<AudioBank>(x.Asset)).ToArray();
            if (GameSettings.Save(settings))
                return "Failed to save Audio Settings.";
            return Validate(false);
        }

        public static string ToggleLiveUpdate()
        {
            var settings = GameSettings.Load<AudioSettings>();
            settings.EnableLiveUpdate = !settings.EnableLiveUpdate;
            if (GameSettings.Save(settings))
                return "FAIL - Audio Settings could not be saved.";
            return settings.EnableLiveUpdate
                ? $"PASS - FMOD Live Update is enabled on port {settings.LiveUpdatePort}. Restart Play mode, then connect FMOD Studio to localhost:{settings.LiveUpdatePort}."
                : "FMOD Live Update is disabled. Restart Play mode to apply the change.";
        }
    }
}
