// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using FlaxEditor.Content.Settings;
using FlaxEditor.Modules;
using FlaxEngine;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// FMOD authoring integration. Runtime initialization remains owned by the engine audio backend.
    /// </summary>
    public sealed class FmodEditorModule : EditorModule
    {
        private FmodBankWatcher _watcher;

        public event Action BanksChanged;

        /// <summary>Gets the current bank watcher.</summary>
        public FmodBankWatcher BankWatcher => _watcher;

        public FmodEditorModule(Editor editor)
        : base(editor)
        {
            InitOrder = 100;
        }

        public override void OnInit()
        {
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            _watcher = new FmodBankWatcher(banks);
            if (_watcher != null)
                _watcher.Changed += OnBanksChanged;
        }

        public override void OnEndInit()
        {
            if (Editor.IsHeadlessMode)
                return;
            // The scene viewport is available only after the editor's end-init phase.
            FmodDebugOverlay.Attach(Editor);
            Editor.UI.AddMenuButton("Audio", "Open FMOD Studio Project", OpenProject);
            Editor.UI.AddMenuButton("Audio", "Build FMOD Banks", BuildBanks);
            Editor.UI.AddMenuButton("Audio", "Synchronize FMOD Metadata", () => SynchronizeMetadata());
            Editor.UI.AddMenuButton("Audio", "Build + Synchronize", () => BuildBanksAndSynchronize());
            Editor.UI.AddMenuButton("Audio", "Reload Banks", ReloadBanks);
            Editor.UI.AddMenuButton("Audio", "Open Audio Diagnostics", OpenDiagnostics);
        }

        public override void OnUpdate()
        {
            _watcher?.Update();
            if (!Editor.IsHeadlessMode)
                FmodDebugOverlay.Refresh();
        }

        private void OnBanksChanged()
        {
            var settings = GameSettings.Load<AudioSettings>();
            if (!settings.AutoReloadBanksOnBuild)
            {
                Editor.Log("FMOD banks changed; automatic reload is disabled in Audio Settings.");
                BanksChanged?.Invoke();
                return;
            }

            // A malformed or half-written sidecar must not be paired with newly written banks.
            // The watcher is debounced, but Studio can still replace the files in multiple writes.
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            var platform = GetActivePlatformName();
            if (FmodMetadataImporter.TryRead(banks, platform, settings.AudioLocale, out _, out var metadataError) == false && !string.IsNullOrEmpty(metadataError))
            {
                Editor.LogWarning("FMOD bank reload deferred because metadata is invalid: " + metadataError);
                BanksChanged?.Invoke();
                return;
            }

            Editor.Log("FMOD banks changed; metadata and preview instances can be reloaded.");
            ReloadBanks();
            BanksChanged?.Invoke();
        }

        public override void OnExit()
        {
            if (!Editor.IsHeadlessMode)
                FmodDebugOverlay.Detach();
            if (_watcher != null)
            {
                _watcher.Changed -= OnBanksChanged;
                _watcher.Dispose();
                _watcher = null;
            }
        }

        public bool SynchronizeMetadata()
        {
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            if (FmodMetadataImporter.TryRead(banks, out _, out var error))
            {
                var report = FmodAssetSynchronizer.Synchronize(banks, Globals.ProjectContentFolder);
                if (!report.Succeeded)
                {
                    foreach (var item in report.Errors)
                        Editor.LogError(item);
                    return false;
                }
                Editor.Log($"FMOD metadata synchronized: {report.EventsCreated} events created, {report.EventsUpdated} updated, {report.BanksCreated} banks created, {report.BanksUpdated} updated, {report.SnapshotsCreated + report.SnapshotsUpdated} snapshots, {report.BusesCreated + report.BusesUpdated} buses, {report.VcasCreated + report.VcasUpdated} VCAs.");
                return report.Succeeded;
            }
            if (!string.IsNullOrEmpty(error))
                Editor.LogError("FMOD metadata validation failed: " + error);
            else
                Editor.LogWarning("FMOD metadata sidecar is missing; using metadata-only fallback.");
            return string.IsNullOrEmpty(error);
        }

        private void OpenProject()
        {
            if (!FmodStudioLocator.OpenProject())
                Editor.LogError("FMOD Studio or its per-user project path is not configured.");
        }

        private void BuildBanks()
        {
            BuildBanksAndSynchronize();
        }

        private void OpenDiagnostics()
        {
            var manager = Level.FindActor<FMODAudioManager>();
            if (manager == null)
            {
                Editor.LogWarning("Add an FMOD Audio Manager actor to the scene to inspect live diagnostics.");
                return;
            }
            Editor.SceneEditing.Select(manager);
            Editor.Windows.PropertiesWin.FocusOrShow();
        }

        /// <summary>
        /// Reloads all banks from the current editor bank directory through the generic audio API.
        /// </summary>
        public void ReloadBanks()
        {
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            var settings = GameSettings.Load<AudioSettings>();
            var platform = GetActivePlatformName();
            var selectedBanks = SelectActiveBankDirectory(banks, platform, settings.AudioLocale);
            if (!Directory.Exists(selectedBanks))
            {
                Editor.LogError($"FMOD bank reload aborted because the active bank directory is missing: '{selectedBanks}'.");
                return;
            }

            var ids = new Dictionary<string, Guid>(StringComparer.OrdinalIgnoreCase);
            if (!FmodMetadataImporter.TryRead(banks, platform, settings.AudioLocale, out var metadata, out var metadataError) && !string.IsNullOrEmpty(metadataError))
            {
                Editor.LogError("FMOD bank reload aborted because active-variant metadata is invalid: " + metadataError);
                return;
            }
            if (metadata != null)
            {
                foreach (var bank in metadata.banks ?? Array.Empty<FmodMetadataImporter.Bank>())
                {
                    if (bank == null || !Guid.TryParse(bank.id, out var id) || string.IsNullOrWhiteSpace(bank.file))
                        continue;
                    ids[bank.file.Replace('\\', '/')] = id;
                }
            }

            var candidates = new List<BankReloadCandidate>();
            var hasExplicitLocale = !string.IsNullOrWhiteSpace(settings.AudioLocale) && !string.Equals(settings.AudioLocale, "default", StringComparison.OrdinalIgnoreCase) &&
                                    string.Equals(Path.GetFileName(selectedBanks), settings.AudioLocale, StringComparison.OrdinalIgnoreCase);
            var searchOption = hasExplicitLocale ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
            foreach (var file in Directory.GetFiles(selectedBanks, "*.bank", searchOption).OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
            {
                var relative = Path.GetRelativePath(banks, file).Replace('\\', '/');
                ids.TryGetValue(relative, out var id);
                candidates.Add(new BankReloadCandidate(file, relative, id));
            }
            if (metadata != null)
            {
                var untracked = candidates.Where(candidate => candidate.ID == Guid.Empty).Select(candidate => candidate.RelativePath).ToArray();
                if (untracked.Length != 0)
                {
                    Editor.LogError("FMOD bank reload aborted because active banks are absent from metadata: " + string.Join(", ", untracked));
                    return;
                }
            }

            var ordered = new List<BankReloadCandidate>();
            var loaded = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            AddConfiguredBank(settings.MasterStringsBank, candidates, ordered, loaded, "master strings");
            AddConfiguredBank(settings.MasterBank, candidates, ordered, loaded, "master");
            foreach (var startup in settings.StartupBanks ?? Array.Empty<JsonAssetReference<AudioBank>>())
                AddConfiguredBank(startup, candidates, ordered, loaded, "startup");
            foreach (var candidate in candidates.OrderBy(GetImplicitLoadPriority).ThenBy(item => item.RelativePath, StringComparer.OrdinalIgnoreCase))
                if (loaded.Add(candidate.RelativePath))
                    ordered.Add(candidate);

            if (ordered.Count == 0)
            {
                Editor.LogError("FMOD bank reload aborted because the active platform/locale contains no banks.");
                return;
            }

            // Only tear down the healthy generation after the replacement set and
            // its metadata have been fully validated.
            if (!AudioEventSystem.StopAll(AudioStopMode.Immediate))
                Editor.LogWarning("FMOD bank reload could not stop all live event instances.");
            if (!AudioEventSystem.UnloadAllBanks())
                Editor.LogWarning("FMOD bank reload reported an unload failure.");

            foreach (var candidate in ordered)
            {
                if (AudioEventSystem.LoadBank(candidate.ID, candidate.File, false))
                    Editor.Log($"Reloaded FMOD bank '{candidate.RelativePath}'.");
                else
                    Editor.LogError($"Failed to reload FMOD bank '{candidate.RelativePath}'.");
            }
        }

        private sealed class BankReloadCandidate
        {
            public readonly string File;
            public readonly string RelativePath;
            public readonly Guid ID;

            public BankReloadCandidate(string file, string relativePath, Guid id)
            {
                File = file;
                RelativePath = relativePath;
                ID = id;
            }
        }

        private static void AddConfiguredBank(JsonAssetReference<AudioBank> reference,
                                              List<BankReloadCandidate> candidates,
                                              List<BankReloadCandidate> ordered,
                                              HashSet<string> loaded,
                                              string label)
        {
            var asset = reference.Instance;
            if (asset == null || asset.BackendId == Guid.Empty)
                return;
            var candidate = candidates.FirstOrDefault(item => item.ID == asset.BackendId);
            if (candidate == null)
                return;
            if (loaded.Add(candidate.RelativePath))
                ordered.Add(candidate);
        }

        private static int GetImplicitLoadPriority(BankReloadCandidate candidate)
        {
            var name = Path.GetFileName(candidate.RelativePath);
            if (name.IndexOf("strings", StringComparison.OrdinalIgnoreCase) >= 0)
                return 0;
            if (name.IndexOf("master", StringComparison.OrdinalIgnoreCase) >= 0)
                return 1;
            return 2;
        }

        private static string SelectActiveBankDirectory(string root, string platform, string locale)
        {
            if (!Directory.Exists(root))
                return root;
            var selected = Path.Combine(root, platform);
            if (!Directory.Exists(selected))
                selected = root;
            if (!string.IsNullOrWhiteSpace(locale) && !string.Equals(locale, "default", StringComparison.OrdinalIgnoreCase))
            {
                var localized = Path.Combine(selected, locale);
                if (Directory.Exists(localized))
                    selected = localized;
            }
            return selected;
        }

        private static string GetActivePlatformName()
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return "Windows";
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
                return "Mac";
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                return "Linux";
            return "Desktop";
        }

        /// <summary>
        /// Performs the editor-safe equivalent of restarting the event backend: stop instances,
        /// release banks, then load the current deterministic bank set again.
        /// </summary>
        public void RestartEventBackend()
        {
            ReloadBanks();
            Editor.Log("FMOD event backend restart requested (stop, unload, and reload completed).");
        }

        /// <summary>
        /// Builds the configured Studio project and synchronizes metadata after a successful build.
        /// </summary>
        public bool BuildBanksAndSynchronize()
        {
            if (!FmodStudioLocator.BuildBanks())
            {
                Editor.LogError("FMOD Studio bank build failed or Studio is not configured.");
                return false;
            }
            var settings = GameSettings.Load<AudioSettings>();
            var synchronized = !settings.AutoSyncMetadataOnBankBuild || SynchronizeMetadata();
            if (synchronized)
                Editor.Log("FMOD build complete.");
            if (settings.AutoReloadBanksOnBuild && synchronized)
                ReloadBanks();
            return synchronized;
        }
    }
}
