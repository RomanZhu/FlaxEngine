// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>Recovery and verification for interrupted project source migrations.</summary>
    public static class CliAssetMigrationRecoveryCommands
    {
        /// <summary>Builds and verifies staged canonical replacements before deleting migration backups.</summary>
        [CliCommand("assets.migration.resume-project", Description = "Resume and verify an interrupted project asset migration.", Access = CliCommandAccess.MutatesProject)]
        public static CliCommandOperation ResumeProject()
        {
            return new RecoveryOperation();
        }

        private sealed class RecoveryOperation : CliCommandOperation
        {
            private enum Phase
            {
                Discover,
                Imported,
                Documents,
                Finalize,
                Completed,
            }

            private sealed class Entry
            {
                public Guid ID;
                public string TypeName;
                public string SourcePath;
                public string BackupPath;
                public bool IsModel;
            }

            private sealed class DocumentEntry
            {
                public Guid ID;
                public string TypeName;
                public string SourcePath;
            }

            private readonly List<Entry> _imported = new();
            private readonly List<DocumentEntry> _documents = new();
            private Phase _phase;
            private int _index;
            private bool _requested;
            private bool _reconciled;
            private bool _reloaded;
            private DateTime _readySinceUtc;
            private DateTime _reloadSinceUtc;
            private CliCommandResult _result;

            public override bool IsCompleted => _phase == Phase.Completed;
            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                try
                {
                    switch (_phase)
                    {
                    case Phase.Discover:
                        Discover();
                        _phase = Phase.Imported;
                        break;
                    case Phase.Imported:
                        ProcessImported();
                        break;
                    case Phase.Documents:
                        ProcessDocument();
                        break;
                    case Phase.Finalize:
                        FinalizeOne();
                        break;
                    }
                }
                catch (Exception ex)
                {
                    _result = CliCommandResult.Failure("FLX-ASSET-PROJECT-MIGRATION-RESUME-0006", ex.Message, AssetDatabaseFacade.GetDiagnostics());
                    _phase = Phase.Completed;
                }
            }

            public override void Cancel()
            {
                _result = CliCommandResult.Failure("FLX-ASSET-PROJECT-MIGRATION-RESUME-CANCELLED", "Project migration verification was cancelled; staged backups were retained.");
                _phase = Phase.Completed;
            }

            private void Discover()
            {
                if (AssetDatabaseFacade.Scan(true))
                    throw new InvalidOperationException("Canonical asset database validation failed before recovery.");
                var records = AssetDatabaseFacade.GetRecords().Where(x => x.IsMain).ToArray();
                var recordsById = records.ToDictionary(x => x.ID);
                var backupFolder = Path.Combine(Globals.ProjectLibraryFolder, "MigrationBackups");
                if (!Directory.Exists(backupFolder))
                    throw new DirectoryNotFoundException($"Migration backup folder was not found: '{backupFolder}'.");

                foreach (var backupPath in Directory.EnumerateFiles(backupFolder, "*.flax", SearchOption.TopDirectoryOnly).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                {
                    if (!Guid.TryParseExact(Path.GetFileNameWithoutExtension(backupPath), "N", out var id) || !recordsById.TryGetValue(id, out var record))
                        throw new InvalidDataException($"Migration backup '{backupPath}' has no matching canonical root record.");
                    if (!FlaxEngine.Content.GetAssetInfo(backupPath, out var legacyInfo) || legacyInfo.ID != id || legacyInfo.TypeName != record.TypeName)
                        throw new InvalidDataException($"Migration backup identity or type does not match '{record.SourcePath}'.");
                    var isModel = string.Equals(record.ProcessorID, "Flax.Model", StringComparison.Ordinal);
                    var isTexture = string.Equals(record.ProcessorID, "Flax.Texture", StringComparison.Ordinal);
                    if (!isModel && !isTexture)
                        throw new InvalidDataException($"Migration backup '{backupPath}' maps to unsupported processor '{record.ProcessorID}'.");
                    _imported.Add(new Entry
                    {
                        ID = id,
                        TypeName = record.TypeName,
                        SourcePath = record.SourcePath,
                        BackupPath = backupPath,
                        IsModel = isModel,
                    });
                }

                foreach (var record in records.Where(IsMigratedDocument).OrderBy(x => x.SourcePath, StringComparer.OrdinalIgnoreCase))
                    _documents.Add(new DocumentEntry { ID = record.ID, TypeName = record.TypeName, SourcePath = record.SourcePath });
                if (_imported.Count == 0)
                    throw new InvalidOperationException("No staged imported-asset backups are available to verify.");
            }

            private void ProcessImported()
            {
                if (_index >= _imported.Count)
                {
                    _index = 0;
                    _requested = false;
                    _reconciled = false;
                    _phase = Phase.Documents;
                    return;
                }

                var entry = _imported[_index];
                if (entry.IsModel && !_reconciled)
                {
                    if (AssetDatabaseFacade.ReconcileModel(entry.ID))
                        throw new InvalidOperationException($"Model subasset reconciliation failed for '{entry.SourcePath}'.");
                    _reconciled = true;
                }
                if (!_requested)
                {
                    var failed = entry.IsModel ? AssetDatabaseFacade.BuildModel(entry.ID) : AssetDatabaseFacade.BuildTexture(entry.ID);
                    if (failed)
                        throw new InvalidOperationException($"Canonical build request failed for '{entry.SourcePath}'.");
                    _requested = true;
                    return;
                }

                var status = entry.IsModel ? AssetDatabaseFacade.GetModelBuildStatus(entry.ID) : AssetDatabaseFacade.GetTextureBuildStatus(entry.ID);
                if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                    throw new InvalidOperationException($"Canonical build {status} for '{entry.SourcePath}'.");
                if (status != "ReadyExact")
                    return;
                if (_readySinceUtc == default)
                {
                    _readySinceUtc = DateTime.UtcNow;
                    return;
                }
                var settleTime = entry.IsModel ? TimeSpan.FromMilliseconds(500) : TimeSpan.FromMilliseconds(100);
                if (DateTime.UtcNow - _readySinceUtc < settleTime)
                    return;
                if (!_reloaded)
                {
                    ReloadAsset(entry.ID, entry.SourcePath);
                    _reloaded = true;
                    _reloadSinceUtc = DateTime.UtcNow;
                    return;
                }
                if (!ValidateLoadedAsset(entry.ID, entry.TypeName, entry.SourcePath, entry.IsModel))
                {
                    if (DateTime.UtcNow - _reloadSinceUtc > TimeSpan.FromSeconds(10))
                        throw new InvalidDataException($"Canonical asset did not finish initializing: '{entry.SourcePath}'.");
                    return;
                }
                _index++;
                _requested = false;
                _reconciled = false;
                _reloaded = false;
                _readySinceUtc = default;
                _reloadSinceUtc = default;
            }

            private void ProcessDocument()
            {
                if (_index >= _documents.Count)
                {
                    _index = 0;
                    _requested = false;
                    _phase = Phase.Finalize;
                    return;
                }

                var entry = _documents[_index];
                if (!_requested)
                {
                    if (AssetDatabaseFacade.BuildGraph(entry.ID))
                        throw new InvalidOperationException($"Canonical document build request failed for '{entry.SourcePath}'.");
                    _requested = true;
                    return;
                }
                var status = AssetDatabaseFacade.GetGraphBuildStatus(entry.ID);
                if (status == "Failed" || status == "Cancelled" || status == "NotBuilt")
                    throw new InvalidOperationException($"Canonical document build {status} for '{entry.SourcePath}'.");
                if (status != "ReadyExact")
                    return;
                if (_readySinceUtc == default)
                {
                    _readySinceUtc = DateTime.UtcNow;
                    return;
                }
                if (DateTime.UtcNow - _readySinceUtc < TimeSpan.FromMilliseconds(100))
                    return;
                if (!_reloaded)
                {
                    ReloadAsset(entry.ID, entry.SourcePath);
                    _reloaded = true;
                    _reloadSinceUtc = DateTime.UtcNow;
                    return;
                }
                if (!ValidateLoadedAsset(entry.ID, entry.TypeName, entry.SourcePath, false))
                {
                    if (DateTime.UtcNow - _reloadSinceUtc > TimeSpan.FromSeconds(10))
                        throw new InvalidDataException($"Canonical document did not finish initializing: '{entry.SourcePath}'.");
                    return;
                }
                _index++;
                _requested = false;
                _reloaded = false;
                _readySinceUtc = default;
                _reloadSinceUtc = default;
            }

            private void FinalizeOne()
            {
                if (_index < _imported.Count)
                {
                    var entry = _imported[_index++];
                    if (AssetDatabaseFacade.FinalizeLegacyImportedMigration(entry.BackupPath))
                        throw new IOException($"Could not remove verified migration backup '{entry.BackupPath}'.");
                    return;
                }
                if (AssetDatabaseFacade.Scan(true))
                    throw new InvalidOperationException("Canonical asset database validation failed after migration finalization.");
                _result = CliCommandResult.Success(new
                {
                    importedSources = _imported.Count,
                    textures = _imported.Count(x => !x.IsModel),
                    models = _imported.Count(x => x.IsModel),
                    textDocuments = _documents.Count,
                    retainedBinaryAssets = Directory.EnumerateFiles(Globals.ProjectContentFolder, "*.flax", SearchOption.AllDirectories).Count(),
                });
                _phase = Phase.Completed;
            }

            private static void ReloadAsset(Guid id, string sourcePath)
            {
                var asset = FlaxEngine.Content.LoadAsync<Asset>(id);
                if (asset == null)
                    throw new InvalidDataException($"Canonical asset could not be resolved by preserved GUID: '{sourcePath}'.");
                asset.Reload();
            }

            private static bool ValidateLoadedAsset(Guid id, string expectedType, string sourcePath, bool requireModelGeometry)
            {
                if (!FlaxEngine.Content.GetAssetInfo(id, out var info) || info.ID != id || info.TypeName != expectedType)
                    throw new InvalidDataException($"Canonical identity or type validation failed for '{sourcePath}'.");
                var asset = FlaxEngine.Content.LoadAsync<Asset>(id);
                if (asset == null)
                    throw new InvalidDataException($"Canonical asset could not be resolved by preserved GUID: '{sourcePath}'.");
                if (asset.WaitForLoaded())
                    throw new InvalidDataException($"Canonical asset failed to load by preserved GUID: '{sourcePath}'.");
                if (asset.GetType().FullName != expectedType)
                    throw new InvalidDataException($"Loaded canonical asset type changed for '{sourcePath}'.");
                if (asset is Texture texture && (texture.Width <= 0 || texture.Height <= 0))
                    return false;
                if (requireModelGeometry && asset is Model model &&
                    (model.LODs.Length == 0 || model.LODs[0].Meshes.Length == 0 || model.LODs[0].Meshes.Sum(x => x.TriangleCount) == 0))
                    return false;
                return true;
            }

            private static bool IsMigratedDocument(AssetDatabaseRecordInfo record)
            {
                if (!IsUnderProjectContent(record.SourcePath))
                    return false;
                switch (Path.GetExtension(record.SourcePath).ToLowerInvariant())
                {
                case ".material":
                case ".materialfunction":
                case ".materialinstance":
                case ".skeletonmask":
                case ".sceneanimation":
                case ".particlesystem":
                case ".particleemitter":
                case ".particlefunction":
                case ".visualscript":
                case ".behaviortree":
                case ".collisiondata":
                    return true;
                default:
                    return false;
                }
            }

            private static bool IsUnderProjectContent(string path)
            {
                if (string.IsNullOrEmpty(path))
                    return false;
                var root = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar;
                return Path.GetFullPath(path).StartsWith(root, StringComparison.OrdinalIgnoreCase);
            }
        }
    }
}
