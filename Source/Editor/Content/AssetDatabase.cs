// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Generic editor asset operations backed by the canonical source database.
    /// </summary>
    public static class AssetDatabase
    {
        private static readonly HashSet<string> DeferredImports = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private static int _assetEditingDepth;
        private static int _autoRefreshSuppressionDepth;
        private static bool _deferredRefresh;
        private static ImportAssetOptions _deferredRefreshOptions;

        /// <summary>Returns the source GUID at a canonical logical or absolute path.</summary>
        public static string AssetPathToGUID(string path)
        {
            var id = AssetDatabaseQueryService.AssetPathToGUID(path);
            return id == Guid.Empty ? string.Empty : id.ToString("N");
        }

        /// <summary>Returns the canonical logical path for a live source GUID.</summary>
        public static string GUIDToAssetPath(string guid)
        {
            return Guid.TryParse(guid, out var id) ? AssetDatabaseQueryService.GUIDToAssetPath(id) : string.Empty;
        }

        /// <summary>Resolves a loaded asset to its persistent GUID.</summary>
        public static bool TryGetGUID(FlaxEngine.Object obj, out string guid)
        {
            if (obj is Asset asset && AssetDatabaseQueryService.TryGetAssetGuid(asset, out var id))
            {
                guid = id.ToString();
                return true;
            }
            guid = string.Empty;
            return false;
        }

        /// <summary>Gets all live main source paths.</summary>
        public static string[] GetAllAssetPaths()
        {
            return AssetDatabaseQueryService.GetAllAssetPaths();
        }

        /// <summary>Loads the main asset at a canonical source path.</summary>
        public static T LoadAssetAtPath<T>(string path) where T : Asset
        {
            if (!AssetDatabaseQueryService.TryGetMainRecordAtPath(path, out var record) || record.SourceKind == AssetSourceKind.Folder)
                return null;
            return FlaxEngine.Content.LoadAssetAsync<T>(record.ID);
        }

        /// <summary>Loads the main asset at a canonical source path.</summary>
        public static Asset LoadMainAssetAtPath(string path)
        {
            return LoadAssetAtPath<Asset>(path);
        }

        /// <summary>Loads the object currently mapped to a persistent file GUID and local file ID.</summary>
        public static Asset LoadAssetObject(Guid objectID)
        {
            return FlaxEngine.Content.LoadAssetAsync(objectID);
        }

        /// <summary>Loads the main object and all live subassets at a canonical source path.</summary>
        public static Asset[] LoadAllAssetsAtPath(string path)
        {
            var physicalPath = ResolvePhysicalPath(path);
            var records = AssetDatabaseQueryService.QueryRecords(new AssetDatabaseQuery { PathPrefix = physicalPath });
            var result = new List<Asset>();
            for (var i = 0; i < records.Length; i++)
            {
                var record = records[i];
                if (record.Status == AssetRecordStatus.MissingSource || record.SourceKind == AssetSourceKind.Folder ||
                    !string.Equals(Path.GetFullPath(record.SourcePath), physicalPath, StringComparison.OrdinalIgnoreCase))
                    continue;
                var asset = FlaxEngine.Content.LoadAssetAsync<Asset>(record.ID);
                if (asset != null)
                    result.Add(asset);
            }
            return result.ToArray();
        }

        /// <summary>Reconciles and imports one source path.</summary>
        public static void ImportAsset(string path, ImportAssetOptions options = ImportAssetOptions.Default)
        {
            if (_assetEditingDepth != 0)
            {
                DeferredImports.Add(path);
                return;
            }
            if (AssetPipelineService.ImportAsset(path, options))
                throw new InvalidOperationException(GetLastDiagnostic("Asset import failed."));
        }

        /// <summary>Reconciles all canonical sources.</summary>
        public static void Refresh(ImportAssetOptions options = ImportAssetOptions.Default)
        {
            if (_assetEditingDepth != 0 || _autoRefreshSuppressionDepth != 0)
            {
                _deferredRefresh = true;
                _deferredRefreshOptions |= options;
                return;
            }
            if (AssetPipelineService.Refresh(options))
                throw new InvalidOperationException(GetLastDiagnostic("Asset refresh failed."));
        }

        /// <summary>Moves a source and its metadata while preserving identity.</summary>
        public static string MoveAsset(string oldPath, string newPath)
        {
            var source = ResolvePhysicalPath(oldPath);
            var destination = ResolvePhysicalPath(newPath);
            if (!IsProjectContentPath(source) || !IsProjectContentPath(destination))
                return "Asset moves must remain inside the project Content folder.";
            if (!AssetDatabaseQueryService.TryGetMainRecordAtPath(source, out var record) || !record.IsMain)
                return "Source asset is not registered in the canonical Asset Database.";
            return AssetOperationService.MoveAsset(source, destination)
                ? GetLastDiagnostic("Asset move transaction failed.")
                : string.Empty;
        }

        /// <summary>Renames a source while preserving identity.</summary>
        public static string RenameAsset(string path, string newName)
        {
            var source = ResolvePhysicalPath(path);
            if (string.IsNullOrWhiteSpace(newName) || Path.GetFileName(newName) != newName)
                return "The new asset name is invalid.";
            var extension = File.Exists(source) ? Path.GetExtension(source) : string.Empty;
            if (extension.Length != 0 && string.IsNullOrEmpty(Path.GetExtension(newName)))
                newName += extension;
            return MoveAsset(source, Path.Combine(Path.GetDirectoryName(source) ?? string.Empty, newName));
        }

        /// <summary>Copies a source and its importer settings with a new file GUID.</summary>
        public static bool CopyAsset(string sourcePath, string destinationPath)
        {
            var source = ResolvePhysicalPath(sourcePath);
            var destination = ResolvePhysicalPath(destinationPath);
            if (!IsProjectContentPath(source) || !IsProjectContentPath(destination))
                return false;
            if (!AssetDatabaseQueryService.TryGetMainRecordAtPath(source, out var record) || !record.IsMain)
                return false;
            return !AssetOperationService.CopyAsset(source, destination, out _);
        }

        /// <summary>Deletes a source and its adjacent metadata.</summary>
        public static bool DeleteAsset(string path)
        {
            var physicalPath = ResolvePhysicalPath(path);
            if (!IsProjectContentPath(physicalPath))
                return false;
            if (!AssetDatabaseQueryService.TryGetMainRecordAtPath(physicalPath, out var record) || !record.IsMain)
                return false;
            return !AssetOperationService.DeleteAsset(physicalPath);
        }

        /// <summary>Creates a folder and schedules metadata reconciliation.</summary>
        public static string CreateFolder(string parentFolder, string newFolderName)
        {
            var parent = ResolvePhysicalPath(parentFolder);
            if (!IsProjectContentPath(parent) || string.IsNullOrWhiteSpace(newFolderName) || Path.GetFileName(newFolderName) != newFolderName)
                return string.Empty;
            var path = Path.Combine(parent, newFolderName);
            var result = Editor.Instance.ContentDatabase.CreatePath(path, true, () => Directory.CreateDirectory(path));
            if (!result.Succeeded)
                return string.Empty;
            QueueImport(path);
            return AssetPathToGUID(path);
        }

        /// <summary>Generates a non-colliding source path.</summary>
        public static string GenerateUniqueAssetPath(string path)
        {
            var physicalPath = ResolvePhysicalPath(path);
            if (!File.Exists(physicalPath) && !Directory.Exists(physicalPath))
                return path;
            var directory = Path.GetDirectoryName(physicalPath) ?? string.Empty;
            var extension = Path.GetExtension(physicalPath);
            var name = Path.GetFileNameWithoutExtension(physicalPath);
            for (var index = 1; ; index++)
            {
                var candidate = Path.Combine(directory, $"{name} {index}{extension}");
                if (!File.Exists(candidate) && !Directory.Exists(candidate))
                    return ToLogicalPath(candidate);
            }
        }

        /// <summary>Suspends automatic imports until the balanced stop call.</summary>
        public static void StartAssetEditing()
        {
            if (_assetEditingDepth == 0)
            {
                Editor.Instance.ContentDatabase.SuspendAssetDatabaseAutoRefresh();
                AssetOperationService.StartEditing();
            }
            _assetEditingDepth++;
        }

        /// <summary>Ends one asset-editing scope and imports accumulated paths when balanced.</summary>
        public static void StopAssetEditing()
        {
            if (_assetEditingDepth == 0)
                throw new InvalidOperationException("StopAssetEditing called without a matching StartAssetEditing.");
            if (--_assetEditingDepth != 0)
                return;
            var failed = AssetOperationService.StopEditing();
            Editor.Instance.ContentDatabase.ResumeAssetDatabaseAutoRefresh();
            if (failed)
                throw new InvalidOperationException(GetLastDiagnostic("Asset editing batch refresh failed."));
            FlushDeferredImports();
        }

        /// <summary>Suppresses focus/watcher initiated refresh requests.</summary>
        public static void DisallowAutoRefresh()
        {
            if (_autoRefreshSuppressionDepth == 0)
                Editor.Instance.ContentDatabase.SuspendAssetDatabaseAutoRefresh();
            _autoRefreshSuppressionDepth++;
        }

        /// <summary>Balances one auto-refresh suppression request.</summary>
        public static void AllowAutoRefresh()
        {
            if (_autoRefreshSuppressionDepth == 0)
                throw new InvalidOperationException("AllowAutoRefresh called without a matching DisallowAutoRefresh.");
            if (--_autoRefreshSuppressionDepth != 0)
                return;
            Editor.Instance.ContentDatabase.ResumeAssetDatabaseAutoRefresh();
            FlushDeferredImports();
        }

        private static void QueueImport(string path)
        {
            if (_assetEditingDepth != 0 || _autoRefreshSuppressionDepth != 0)
                DeferredImports.Add(path);
            else
                ImportAsset(path, Directory.Exists(path) ? ImportAssetOptions.ImportRecursive : ImportAssetOptions.Default);
        }

        private static void FlushDeferredImports()
        {
            if (_assetEditingDepth != 0 || _autoRefreshSuppressionDepth != 0)
                return;
            if (_deferredRefresh)
            {
                var options = _deferredRefreshOptions;
                _deferredRefresh = false;
                _deferredRefreshOptions = ImportAssetOptions.Default;
                Refresh(options);
            }
            var paths = new List<string>(DeferredImports);
            DeferredImports.Clear();
            for (var i = 0; i < paths.Count; i++)
                QueueImport(paths[i]);
        }

        private static string ResolvePhysicalPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return string.Empty;
            path = path.Replace('/', Path.DirectorySeparatorChar);
            return Path.GetFullPath(Path.IsPathRooted(path) ? path : Path.Combine(Globals.ProjectFolder, path));
        }

        private static string ToLogicalPath(string path)
        {
            var root = Path.GetFullPath(Globals.ProjectFolder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var fullPath = Path.GetFullPath(path);
            return fullPath.StartsWith(root, StringComparison.OrdinalIgnoreCase)
                ? fullPath.Substring(root.Length).Replace(Path.DirectorySeparatorChar, '/')
                : fullPath.Replace(Path.DirectorySeparatorChar, '/');
        }

        private static bool IsProjectContentPath(string path)
        {
            if (string.IsNullOrEmpty(path))
                return false;
            var root = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar);
            var fullPath = Path.GetFullPath(path);
            return string.Equals(fullPath, root, StringComparison.OrdinalIgnoreCase) ||
                   fullPath.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        }

        private static string GetLastDiagnostic(string fallback)
        {
            var diagnostics = AssetDatabaseQueryService.GetDiagnostics();
            return diagnostics.Length == 0 ? fallback : diagnostics[0].Message;
        }
    }
}
