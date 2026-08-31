// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
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
            var useCanonicalAssetSystem = Editor.Instance?.GameProject?.AssetSystemVersion == ProjectInfo.CurrentAssetSystemVersion;
            if (useCanonicalAssetSystem)
            {
                if (!EnsureCanonicalDirectory(destination, out message))
                    return false;
            }
            else
            {
                Directory.CreateDirectory(destination);
            }
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
                var sourceIsDestination = string.Equals(item.Source, item.Output, StringComparison.OrdinalIgnoreCase);
                if (!useCanonicalAssetSystem && sourceIsDestination)
                {
                    skipped++;
                    continue;
                }
                var outputDirectory = Path.GetDirectoryName(item.Output) ?? destination;
                if (useCanonicalAssetSystem)
                {
                    if (!EnsureCanonicalDirectory(outputDirectory, out message))
                        return false;
                    AssetMutationResultInfo result;
                    var logicalPath = AssetDatabase.ToLogicalPathInternal(item.Output);
                    if (sourceIsDestination)
                    {
                        if (File.Exists(item.Output + ".meta"))
                        {
                            skipped++;
                            continue;
                        }
                        AssetPipelineCallbacks.WillCreate(logicalPath);
                        using (AssetPipelineCallbacks.BypassNativeDecision())
                            result = AssetDatabaseFacade.RegisterCanonicalSource(item.Output, false);
                    }
                    else
                    {
                        var replaceExisting = File.Exists(item.Output) || File.Exists(item.Output + ".meta");
                        if (replaceExisting)
                        {
                            if (!AssetPipelineCallbacks.WillSave(new[] { logicalPath }).Contains(logicalPath, StringComparer.OrdinalIgnoreCase))
                            {
                                message = $"FMOD import was rejected for '{item.Output}'.";
                                return false;
                            }
                        }
                        else
                        {
                            AssetPipelineCallbacks.WillCreate(logicalPath);
                        }
                        using (AssetPipelineCallbacks.BypassNativeDecision())
                            result = AssetDatabaseFacade.PublishExternalSource(item.Source, item.Output,
                                typeof(RawDataAsset).FullName, "Flax.Unsupported", replaceExisting);
                    }
                    if (!result.Succeeded)
                    {
                        message = string.IsNullOrEmpty(result.Message)
                            ? $"Failed to import FMOD source '{item.Source}'."
                            : result.Message;
                        return false;
                    }
                    AssetPipelineCallbacks.PostprocessAll(new[] { logicalPath },
                        Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
                }
                else
                {
                    Directory.CreateDirectory(outputDirectory);
                    File.Copy(item.Source, item.Output, true);
                }
                if (sourceIsDestination)
                    skipped++;
                else
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
            if (useCanonicalAssetSystem)
                AssetPipelineCallbacks.PostprocessAll(new[] { AssetDatabase.ToLogicalPathInternal(Path.Combine(destination, "fmod-metadata.json")) },
                    Array.Empty<string>(), Array.Empty<string>(), Array.Empty<string>(), false);
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

        private static bool EnsureCanonicalDirectory(string path, out string message)
        {
            message = string.Empty;
            path = Path.GetFullPath(path);
            var contentRoot = Path.GetFullPath(Globals.ProjectContentFolder);
            if (string.Equals(path, contentRoot, StringComparison.OrdinalIgnoreCase))
            {
                message = string.Empty;
                return true;
            }
            var parent = Path.GetDirectoryName(path);
            if (string.IsNullOrEmpty(parent) || !EnsureCanonicalDirectory(parent, out message))
                return false;
            if (Directory.Exists(path))
            {
                if (File.Exists(path + ".meta"))
                    return true;
                var result = AssetDatabaseFacade.RegisterCanonicalSource(path, false);
                if (result.Succeeded)
                    return true;
                message = string.IsNullOrEmpty(result.Message)
                    ? $"Failed to register FMOD content folder '{path}'."
                    : result.Message;
                return false;
            }
            if (!string.IsNullOrEmpty(AssetDatabase.CreateFolder(parent, Path.GetFileName(path))))
                return true;
            message = $"Failed to create FMOD content folder '{path}'.";
            return false;
        }
    }
}
