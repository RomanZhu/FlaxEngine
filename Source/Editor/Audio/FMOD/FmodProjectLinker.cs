// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using FlaxEngine;

namespace FlaxEditor.FMOD
{
    /// <summary>Links a local Studio project to tracked banks and generated metadata.</summary>
    internal static class FmodProjectLinker
    {
        public static string LinkProject(string project)
        {
            FmodEditorSettings.StudioProjectPath = project;
            var banks = DetectBankOutput(project);
            if (!string.IsNullOrEmpty(banks))
                FmodEditorSettings.BankOutputPath = banks;
            return banks;
        }

        public static string DetectBankOutput(string project = null)
        {
            project ??= FmodEditorSettings.StudioProjectPath;
            if (string.IsNullOrWhiteSpace(project) || !File.Exists(project))
                return string.Empty;
            var root = Path.GetDirectoryName(project);
            if (string.IsNullOrEmpty(root))
                return string.Empty;
            var candidates = new[]
            {
                Path.Combine(root, "Build", "Desktop"),
                Path.Combine(root, "Build"),
                Path.Combine(root, "Banks", "Desktop"),
                Path.Combine(root, "Banks"),
            };
            foreach (var candidate in candidates)
                if (Directory.Exists(candidate) && Directory.GetFiles(candidate, "Master.bank", SearchOption.AllDirectories).Length != 0)
                    return Path.GetDirectoryName(Directory.GetFiles(candidate, "Master.bank", SearchOption.AllDirectories)[0]);
            var master = Directory.GetFiles(root, "Master.bank", SearchOption.AllDirectories).FirstOrDefault();
            return master == null ? string.Empty : Path.GetDirectoryName(master);
        }

        public static bool ImportAndSynchronize(out string message)
        {
            var source = FmodEditorSettings.BankOutputPath;
            if (string.IsNullOrWhiteSpace(source) || !Directory.Exists(source))
            {
                message = "Select a built-bank folder or link an FMOD Studio project first.";
                return false;
            }
            var destination = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            Directory.CreateDirectory(destination);
            var copied = 0;
            var skipped = 0;
            var pending = Directory.GetFiles(source, "*", SearchOption.AllDirectories)
                .Where(x => x.EndsWith(".bank", StringComparison.OrdinalIgnoreCase) ||
                            x.EndsWith("fmod-metadata.json", StringComparison.OrdinalIgnoreCase) ||
                            x.EndsWith("metadata.json", StringComparison.OrdinalIgnoreCase))
                .Select(file => new
                {
                    Source = Path.GetFullPath(file),
                    Output = Path.GetFullPath(Path.Combine(destination, Path.GetRelativePath(source, file)))
                })
                .ToArray();
            var needsCopy = pending.Any(x => !string.Equals(x.Source, x.Output, StringComparison.OrdinalIgnoreCase));
            if (needsCopy)
            {
                AudioEventSystem.StopAll(AudioStopMode.Immediate);
                AudioEventSystem.UnloadAllBanks();
            }
            foreach (var item in pending)
            {
                if (string.Equals(item.Source, item.Output, StringComparison.OrdinalIgnoreCase))
                {
                    skipped++;
                    continue;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(item.Output) ?? destination);
                File.Copy(item.Source, item.Output, true);
                copied++;
            }
            if (copied == 0 && skipped == 0)
            {
                message = "The selected folder contains no FMOD banks.";
                return false;
            }
            if (!FmodCatalogBuilder.BuildCatalog(destination, destination))
            {
                message = "Banks were copied, but runtime metadata extraction failed. See the Editor log.";
                return false;
            }
            var report = FmodAssetSynchronizer.Synchronize(destination, Globals.ProjectContentFolder);
            if (!report.Succeeded)
            {
                message = string.Join(Environment.NewLine, report.Errors);
                return false;
            }
            var root = Editor.Instance.ContentDatabase.Find(Globals.ProjectContentFolder);
            if (root != null)
                Editor.Instance.ContentDatabase.RefreshFolder(root, true);
            if (needsCopy)
                Editor.Instance?.FMOD?.ReloadBanks();
            message = $"Imported {copied} bank/metadata files ({skipped} already in place); synchronized {report.EventsCreated + report.EventsUpdated} events, {report.BanksCreated + report.BanksUpdated} banks, {report.SnapshotsCreated + report.SnapshotsUpdated} snapshots, {report.BusesCreated + report.BusesUpdated} buses, and {report.VcasCreated + report.VcasUpdated} VCAs.";
            return true;
        }
    }
}
