// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using FlaxEditor.Content;
using FlaxEngine;
using FlaxEngine.Utilities;

namespace FlaxEditor.Actions
{
    /// <summary>
    /// Undo action for content item creation and deletion that stages deleted files in the project cache.
    /// </summary>
    [Serializable]
    internal sealed class ContentItemFilesystemAction : ITryUndoAction
    {
        private enum Operation
        {
            Delete,
            Create,
        }

        [Serializable]
        private struct Entry
        {
            public string OriginalPath;
            public string TrashPath;
            public string MetadataOriginalPath;
            public string MetadataTrashPath;
            public string SidecarOriginalPath;
            public string SidecarTrashPath;
            public bool IsFolder;
            public bool HasMetadataSidecar;
            public bool HasSidecarFolder;
            public Guid AssetId;
            public string TypeName;
            public long SizeInBytes;
            public bool IsStaged;
        }

        private Editor _editor;
        private Entry[] _entries;
        private readonly Operation _operation;
        private bool _isDeleted;
        private bool _requiresRecovery;
        private const int FilesystemRetryCount = 12;
        private const int FilesystemRetryDelayMs = 50;

        /// <inheritdoc />
        public string ActionString { get; }

        /// <inheritdoc />
        public UndoActionInfo ActionInfo
        {
            get
            {
                var entries = _entries;
                if (entries == null || entries.Length == 0)
                    return new UndoActionInfo { Operation = ActionString };

                var entry = entries[0];
                long sizeInBytes = 0;
                bool sizeKnown = true;
                for (int i = 0; i < entries.Length; i++)
                {
                    if (entries[i].SizeInBytes >= 0)
                        sizeInBytes += entries[i].SizeInBytes;
                    else
                        sizeKnown = false;
                }

                var flags = UndoActionFlags.RequiresReload | UndoActionFlags.AffectsContentDatabase | UndoActionFlags.DiskBacked;
                return new UndoActionInfo
                {
                    Operation = _operation == Operation.Delete ? "Delete" : "Create",
                    TargetType = entries.Length == 1 ? (entry.AssetId != Guid.Empty ? UndoActionTargetType.Asset : UndoActionTargetType.ContentItem) : UndoActionTargetType.Multiple,
                    TargetName = Path.GetFileNameWithoutExtension(entry.OriginalPath),
                    TargetPath = entry.OriginalPath,
                    TargetId = entry.AssetId,
                    DisplayEditorTypeName = typeof(Windows.ContentWindow).FullName,
                    Flags = flags,
                    SizeInBytes = sizeKnown ? sizeInBytes : -1,
                };
            }
        }

        private ContentItemFilesystemAction(Editor editor, Operation operation, Entry[] entries, bool isDeleted, string actionString)
        {
            _editor = editor ?? throw new ArgumentNullException(nameof(editor));
            _operation = operation;
            _entries = entries ?? throw new ArgumentNullException(nameof(entries));
            _isDeleted = isDeleted;
            ActionString = actionString;
        }

        /// <summary>
        /// Deletes the items by staging them in the project cache and removing them from the content database.
        /// </summary>
        /// <param name="editor">The editor.</param>
        /// <param name="items">The items to delete.</param>
        /// <returns>The undo action, or null if nothing was deleted.</returns>
        public static ContentItemFilesystemAction Delete(Editor editor, List<ContentItem> items)
        {
            if (editor == null)
                throw new ArgumentNullException(nameof(editor));
            if (items == null || items.Count == 0)
                return null;

            var filteredItems = FilterTopLevelItems(items);
            if (filteredItems.Count == 0)
                return null;

            var entries = new Entry[filteredItems.Count];
            for (int i = 0; i < filteredItems.Count; i++)
                entries[i] = CreateEntry(filteredItems[i]);

            var action = new ContentItemFilesystemAction(editor, Operation.Delete, entries, false, GetActionString("Delete", entries));
            if (!action.Stage(filteredItems))
            {
                action.Dispose();
                return null;
            }

            return action;
        }

        /// <summary>
        /// Creates an undo action for a newly-created content item.
        /// </summary>
        /// <param name="editor">The editor.</param>
        /// <param name="item">The created item.</param>
        /// <returns>The undo action.</returns>
        public static ContentItemFilesystemAction Create(Editor editor, ContentItem item)
        {
            if (editor == null)
                throw new ArgumentNullException(nameof(editor));
            if (item == null)
                throw new ArgumentNullException(nameof(item));

            var entries = new[] { CreateEntry(item) };
            return new ContentItemFilesystemAction(editor, Operation.Create, entries, false, GetActionString("Create", entries));
        }

        /// <summary>
        /// Creates an undo action for newly-created content items.
        /// </summary>
        /// <param name="editor">The editor.</param>
        /// <param name="items">The created items.</param>
        /// <returns>The undo action, or null if the list is empty.</returns>
        public static ContentItemFilesystemAction Create(Editor editor, List<ContentItem> items)
        {
            if (editor == null)
                throw new ArgumentNullException(nameof(editor));
            if (items == null || items.Count == 0)
                return null;

            var filteredItems = FilterTopLevelItems(items);
            if (filteredItems.Count == 0)
                return null;

            var entries = new Entry[filteredItems.Count];
            for (int i = 0; i < filteredItems.Count; i++)
                entries[i] = CreateEntry(filteredItems[i]);
            return new ContentItemFilesystemAction(editor, Operation.Create, entries, false, GetActionString("Create", entries));
        }

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryDo()
        {
            return _operation == Operation.Delete ? StageByPath() : Restore();
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            return _operation == Operation.Delete ? Restore() : StageByPath();
        }

        /// <inheritdoc />
        public void Dispose()
        {
            if (_entries != null && _isDeleted && !_requiresRecovery)
            {
                var cleanupFailures = new List<Entry>();
                for (int i = 0; i < _entries.Length; i++)
                {
                    UnloadStagedAssets(_entries[i].TrashPath, _entries[i].IsFolder);
                    var cleanupSucceeded = DeletePath(_entries[i].TrashPath, _entries[i].IsFolder);
                    if (_entries[i].HasMetadataSidecar)
                        cleanupSucceeded &= DeletePath(_entries[i].MetadataTrashPath, false);
                    if (_entries[i].HasSidecarFolder)
                        cleanupSucceeded &= DeletePath(_entries[i].SidecarTrashPath, true);
                    if (!cleanupSucceeded)
                        cleanupFailures.Add(_entries[i]);
                }
                if (cleanupFailures.Count != 0)
                {
                    var plan = new ContentMutationPlan(ContentMutationOperationKind.Cleanup);
                    for (int i = 0; i < cleanupFailures.Count; i++)
                    {
                        var entry = cleanupFailures[i];
                        if (ContentMutationPathUtils.Exists(entry.TrashPath))
                            plan.Entries.Add(new ContentMutationEntry(entry.TrashPath, entry.OriginalPath, ContentMutationPathRole.UndoTrash, entry.IsFolder));
                        if (entry.HasMetadataSidecar && File.Exists(entry.MetadataTrashPath))
                            plan.Entries.Add(new ContentMutationEntry(entry.MetadataTrashPath, entry.MetadataOriginalPath, ContentMutationPathRole.MetadataSidecar, false));
                        if (entry.HasSidecarFolder && ContentMutationPathUtils.Exists(entry.SidecarTrashPath))
                            plan.Entries.Add(new ContentMutationEntry(entry.SidecarTrashPath, entry.SidecarOriginalPath, ContentMutationPathRole.ExternalActorSidecar, true));
                    }
                    if (plan.Entries.Count != 0)
                        ContentMutationTransaction.PreserveRecoveryRecord(plan, "Undo-history cleanup could not remove staged Content data.");
                }
            }
            else if (_entries != null && _requiresRecovery)
            {
                for (int i = 0; i < _entries.Length; i++)
                {
                    if (_entries[i].IsStaged)
                        Editor.LogError("Content mutation recovery data was preserved at: " + _entries[i].TrashPath);
                }
            }
            _editor = null;
            _entries = null;
        }

        private static List<ContentItem> FilterTopLevelItems(List<ContentItem> items)
        {
            var result = new List<ContentItem>(items.Count);
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i];
                if (item == null)
                    continue;

                var isNestedInSelection = false;
                for (int j = 0; j < items.Count; j++)
                {
                    var other = items[j];
                    if (other == null || ReferenceEquals(item, other) || !other.IsFolder)
                        continue;
                    if (IsPathInside(item.Path, other.Path))
                    {
                        isNestedInSelection = true;
                        break;
                    }
                }

                if (!isNestedInSelection && !result.Contains(item))
                    result.Add(item);
            }
            return result;
        }

        private static bool IsPathInside(string path, string folderPath)
        {
            path = StringUtils.NormalizePath(path);
            folderPath = StringUtils.NormalizePath(folderPath).TrimEnd('/');
            var comparison = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            return path.Length > folderPath.Length &&
                   path.StartsWith(folderPath, comparison) &&
                   path[folderPath.Length] == '/';
        }

        private static bool IsSceneFilePath(string path)
        {
            return string.Equals(Path.GetExtension(path), ".scene", StringComparison.OrdinalIgnoreCase);
        }

        internal static string GetMetadataSidecarPath(string path, bool isFolder)
        {
            return StringUtils.NormalizePath(path) + ".meta";
        }

        internal static string GetSceneActorsFolderPath(string path, bool isFolder)
        {
            if (!isFolder && !IsSceneFilePath(path))
                return null;

            path = StringUtils.NormalizePath(Path.GetFullPath(path));
            var contentFolder = StringUtils.NormalizePath(Path.GetFullPath(Globals.ProjectContentFolder)).TrimEnd('/', '\\');
            if (path.Length <= contentFolder.Length ||
                !path.StartsWith(contentFolder, StringComparison.OrdinalIgnoreCase) ||
                (path[contentFolder.Length] != '/' && path[contentFolder.Length] != '\\'))
            {
                return null;
            }

            var relativePath = StringUtils.NormalizePath(path.Substring(contentFolder.Length + 1));
            if (!isFolder)
                relativePath = StringUtils.GetPathWithoutExtension(relativePath);
            if (string.IsNullOrEmpty(relativePath))
                return null;
            return StringUtils.CombinePaths(Globals.ProjectFolder, "SceneActors", relativePath);
        }

        private static Entry CreateEntry(ContentItem item)
        {
            var trashRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "EditorTrash", Guid.NewGuid().ToString("N"));
            var originalPath = StringUtils.NormalizePath(item.Path);
            var metadataOriginalPath = GetMetadataSidecarPath(originalPath, item.IsFolder);
            var sidecarOriginalPath = GetSceneActorsFolderPath(originalPath, item.IsFolder);
            var hasMetadataSidecar = metadataOriginalPath != null && File.Exists(metadataOriginalPath);
            var hasSidecarFolder = sidecarOriginalPath != null && Directory.Exists(sidecarOriginalPath);
            var sizeInBytes = GetPathSize(originalPath, item.IsFolder);
            if (hasMetadataSidecar)
            {
                var metadataSizeInBytes = GetPathSize(metadataOriginalPath, false);
                if (metadataSizeInBytes >= 0)
                    sizeInBytes = sizeInBytes >= 0 ? sizeInBytes + metadataSizeInBytes : metadataSizeInBytes;
            }
            if (hasSidecarFolder)
            {
                var sidecarSizeInBytes = GetPathSize(sidecarOriginalPath, true);
                if (sidecarSizeInBytes >= 0)
                    sizeInBytes = sizeInBytes >= 0 ? sizeInBytes + sidecarSizeInBytes : sidecarSizeInBytes;
            }
            return new Entry
            {
                OriginalPath = originalPath,
                TrashPath = StringUtils.CombinePaths(trashRoot, item.FileName),
                MetadataOriginalPath = metadataOriginalPath,
                MetadataTrashPath = metadataOriginalPath == null ? null : StringUtils.CombinePaths(trashRoot, item.FileName + ".meta"),
                SidecarOriginalPath = sidecarOriginalPath,
                SidecarTrashPath = StringUtils.CombinePaths(trashRoot, "SceneActors"),
                IsFolder = item.IsFolder,
                HasMetadataSidecar = hasMetadataSidecar,
                HasSidecarFolder = hasSidecarFolder,
                AssetId = item is AssetItem assetItem ? assetItem.ID : Guid.Empty,
                TypeName = item is AssetItem typedAssetItem ? typedAssetItem.TypeName : null,
                SizeInBytes = sizeInBytes,
            };
        }

        private static string GetActionString(string verb, Entry[] entries)
        {
            if (entries.Length == 1)
                return verb + " " + Path.GetFileName(entries[0].OriginalPath);
            return verb + " Content Items";
        }

        private bool Stage(List<ContentItem> items)
        {
            if (_isDeleted)
                return true;

            if (!PreflightStage())
                return false;
            return ExecuteStage(items, "live-items");
        }

        private bool StageByPath()
        {
            if (_isDeleted)
                return true;

            if (!PreflightStage())
                return false;
            return ExecuteStage(null, "paths");
        }

        private bool Restore()
        {
            if (!AnyEntryStaged())
                return true;

            if (!PreflightRestore())
                return false;
            return ExecuteRestore();
        }

        private bool ExecuteStage(IReadOnlyList<ContentItem> items, string source)
        {
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Delete);
            var steps = new List<ContentMutationStep>(_entries.Length);
            for (int i = 0; i < _entries.Length; i++)
            {
                if (_entries[i].IsStaged)
                    continue;
                var entryIndex = i;
                var firstPlanEntry = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(_entries[i].OriginalPath, _entries[i].TrashPath, ContentMutationPathRole.UndoTrash, _entries[i].IsFolder)
                {
                    DestinationParentProducedByTransaction = true,
                });
                if (_entries[i].HasMetadataSidecar)
                {
                    plan.Entries.Add(new ContentMutationEntry(_entries[i].MetadataOriginalPath, _entries[i].MetadataTrashPath, ContentMutationPathRole.MetadataSidecar, false)
                    {
                        DestinationParentProducedByTransaction = true,
                    });
                }
                if (_entries[i].HasSidecarFolder)
                {
                    plan.Entries.Add(new ContentMutationEntry(_entries[i].SidecarOriginalPath, _entries[i].SidecarTrashPath, ContentMutationPathRole.ExternalActorSidecar, true)
                    {
                        DestinationParentProducedByTransaction = true,
                    });
                }
                var planIndices = Enumerable.Range(firstPlanEntry, plan.Entries.Count - firstPlanEntry).ToArray();
                steps.Add(new ContentMutationStep(
                    "delete-stage-" + i,
                    planIndices,
                    () => CommitStageEntry(entryIndex, items != null ? items[entryIndex] : _editor.ContentDatabase.Find(_entries[entryIndex].OriginalPath)),
                    () => RollbackStageEntry(entryIndex),
                    () => _entries[entryIndex].IsStaged &&
                          PathExists(_entries[entryIndex].TrashPath, _entries[entryIndex].IsFolder) &&
                          !PathExists(_entries[entryIndex].OriginalPath, _entries[entryIndex].IsFolder) &&
                          (!_entries[entryIndex].HasMetadataSidecar || (File.Exists(_entries[entryIndex].MetadataTrashPath) && !File.Exists(_entries[entryIndex].MetadataOriginalPath)))));
            }

            ContentMutationDiagnostics.Log("mutation.stage.begin", $"transaction={plan.Id:N}; action='{ActionString}'; entries={_entries.Length}; source={source}");
            var result = new ContentMutationTransaction(plan).Execute(steps);
            _requiresRecovery |= result.RequiresRecovery;
            _isDeleted = AreAllEntriesStaged();
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.stage.committed" : "mutation.stage.failed", $"transaction={plan.Id:N}; action='{ActionString}'; entries={_entries.Length}; recovery={_requiresRecovery}; failure={result.Failure}");
            return result.Succeeded && _isDeleted;
        }

        private bool ExecuteRestore()
        {
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Restore);
            var steps = new List<ContentMutationStep>(_entries.Length);
            for (int i = 0; i < _entries.Length; i++)
            {
                if (!_entries[i].IsStaged)
                    continue;
                var entryIndex = i;
                var firstPlanEntry = plan.Entries.Count;
                plan.Entries.Add(new ContentMutationEntry(_entries[i].TrashPath, _entries[i].OriginalPath, ContentMutationPathRole.Main, _entries[i].IsFolder));
                if (_entries[i].HasMetadataSidecar)
                    plan.Entries.Add(new ContentMutationEntry(_entries[i].MetadataTrashPath, _entries[i].MetadataOriginalPath, ContentMutationPathRole.MetadataSidecar, false));
                if (_entries[i].HasSidecarFolder)
                    plan.Entries.Add(new ContentMutationEntry(_entries[i].SidecarTrashPath, _entries[i].SidecarOriginalPath, ContentMutationPathRole.ExternalActorSidecar, true)
                    {
                        DestinationParentProducedByTransaction = true,
                    });
                var planIndices = Enumerable.Range(firstPlanEntry, plan.Entries.Count - firstPlanEntry).ToArray();
                steps.Add(new ContentMutationStep(
                    "restore-" + i,
                    planIndices,
                    () => CommitRestoreEntry(entryIndex),
                    () => RollbackRestoreEntry(entryIndex),
                    () => !_entries[entryIndex].IsStaged &&
                          PathExists(_entries[entryIndex].OriginalPath, _entries[entryIndex].IsFolder) &&
                          !PathExists(_entries[entryIndex].TrashPath, _entries[entryIndex].IsFolder) &&
                          (!_entries[entryIndex].HasMetadataSidecar || (File.Exists(_entries[entryIndex].MetadataOriginalPath) && !File.Exists(_entries[entryIndex].MetadataTrashPath)))));
            }

            ContentMutationDiagnostics.Log("mutation.restore.begin", $"transaction={plan.Id:N}; action='{ActionString}'; entries={_entries.Length}");
            var result = new ContentMutationTransaction(plan).Execute(steps);
            _requiresRecovery |= result.RequiresRecovery;
            _isDeleted = AnyEntryStaged();
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.restore.committed" : "mutation.restore.failed", $"transaction={plan.Id:N}; action='{ActionString}'; entries={_entries.Length}; recovery={_requiresRecovery}; failure={result.Failure}");
            return result.Succeeded && !_isDeleted;
        }

        private ContentMutationResult CommitStageEntry(int index, ContentItem item)
        {
            return StageEntry(item, ref _entries[index])
                ? ContentMutationResult.Success(_entries[index].OriginalPath, _entries[index].TrashPath)
                : ContentMutationResult.Fail(ContentMutationFailure.DeleteFailed, _entries[index].OriginalPath, _entries[index].TrashPath, "Failed to stage the Content item for deletion.", _requiresRecovery);
        }

        private bool RollbackStageEntry(int index)
        {
            if (!_entries[index].IsStaged)
                return PathExists(_entries[index].OriginalPath, _entries[index].IsFolder) && !PathExists(_entries[index].TrashPath, _entries[index].IsFolder);
            return RestoreEntry(ref _entries[index]);
        }

        private ContentMutationResult CommitRestoreEntry(int index)
        {
            return RestoreEntry(ref _entries[index])
                ? ContentMutationResult.Success(_entries[index].TrashPath, _entries[index].OriginalPath)
                : ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, _entries[index].TrashPath, _entries[index].OriginalPath, "Failed to restore the staged Content item.", _requiresRecovery);
        }

        private bool RollbackRestoreEntry(int index)
        {
            if (_entries[index].IsStaged)
                return PathExists(_entries[index].TrashPath, _entries[index].IsFolder) && !PathExists(_entries[index].OriginalPath, _entries[index].IsFolder);
            var item = _editor.ContentDatabase.Find(_entries[index].OriginalPath);
            return StageEntry(item, ref _entries[index]);
        }

        private bool PreflightStage()
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                var entry = _entries[i];
                if (entry.IsStaged)
                    continue;
                if (!PathExists(entry.OriginalPath, entry.IsFolder))
                {
                    Editor.LogWarning("Cannot stage content item because the source is missing: " + entry.OriginalPath);
                    return false;
                }
                if (ContainsReparsePoint(entry.OriginalPath, entry.IsFolder))
                {
                    Editor.LogWarning("Cannot stage content item containing a filesystem link or reparse point: " + entry.OriginalPath);
                    return false;
                }
                if (File.Exists(entry.TrashPath) || Directory.Exists(entry.TrashPath))
                {
                    Editor.LogWarning("Cannot stage content item because the recovery path already exists: " + entry.TrashPath);
                    return false;
                }

                entry.HasMetadataSidecar = entry.MetadataOriginalPath != null && File.Exists(entry.MetadataOriginalPath);
                if (entry.HasMetadataSidecar && (ContainsReparsePoint(entry.MetadataOriginalPath, false) || File.Exists(entry.MetadataTrashPath) || Directory.Exists(entry.MetadataTrashPath)))
                {
                    Editor.LogWarning("Cannot stage asset metadata because its recovery path is unsafe or already exists: " + entry.MetadataOriginalPath);
                    return false;
                }
                entry.HasSidecarFolder = entry.SidecarOriginalPath != null && Directory.Exists(entry.SidecarOriginalPath);
                if (entry.HasSidecarFolder && (ContainsReparsePoint(entry.SidecarOriginalPath, true) || File.Exists(entry.SidecarTrashPath) || Directory.Exists(entry.SidecarTrashPath)))
                {
                    Editor.LogWarning("Cannot stage scene actors folder because its recovery path is unsafe or already exists: " + entry.SidecarOriginalPath);
                    return false;
                }
                _entries[i] = entry;
            }
            return true;
        }

        private bool PreflightRestore()
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                var entry = _entries[i];
                if (!entry.IsStaged)
                    continue;
                if (!PathExists(entry.TrashPath, entry.IsFolder))
                {
                    Editor.LogWarning("Cannot restore staged content item because recovery data is missing: " + entry.TrashPath);
                    _requiresRecovery = true;
                    return false;
                }
                if (PathExists(entry.OriginalPath, entry.IsFolder) || File.Exists(entry.OriginalPath) || Directory.Exists(entry.OriginalPath))
                {
                    Editor.LogWarning("Cannot restore staged content item because the original path already exists: " + entry.OriginalPath);
                    return false;
                }
                if (entry.HasMetadataSidecar && (!File.Exists(entry.MetadataTrashPath) || File.Exists(entry.MetadataOriginalPath) || Directory.Exists(entry.MetadataOriginalPath)))
                {
                    Editor.LogWarning("Cannot restore staged asset metadata because recovery data is missing or the original path exists: " + entry.MetadataOriginalPath);
                    return false;
                }
                if (entry.HasSidecarFolder && (!Directory.Exists(entry.SidecarTrashPath) || File.Exists(entry.SidecarOriginalPath) || Directory.Exists(entry.SidecarOriginalPath)))
                {
                    Editor.LogWarning("Cannot restore staged scene actors folder because recovery data is missing or the original path exists: " + entry.SidecarOriginalPath);
                    return false;
                }
            }
            return true;
        }

        private bool RestoreEntry(ref Entry entry)
        {
            if (!MoveContentPath(ref entry, entry.TrashPath, entry.OriginalPath))
                return false;
            if (entry.HasMetadataSidecar && !MovePath(entry.MetadataTrashPath, entry.MetadataOriginalPath, false))
            {
                if (!MoveContentPath(ref entry, entry.OriginalPath, entry.TrashPath))
                {
                    _requiresRecovery = true;
                    Editor.LogError("Failed to roll back a partial content restore. Recovery data: " + entry.TrashPath);
                }
                return false;
            }
            if (entry.HasSidecarFolder && !MovePath(entry.SidecarTrashPath, entry.SidecarOriginalPath, true))
            {
                if (entry.HasMetadataSidecar && !MovePath(entry.MetadataOriginalPath, entry.MetadataTrashPath, false))
                    _requiresRecovery = true;
                if (!MoveContentPath(ref entry, entry.OriginalPath, entry.TrashPath))
                {
                    _requiresRecovery = true;
                    Editor.LogError("Failed to roll back a partial content restore. Recovery data: " + entry.TrashPath);
                }
                return false;
            }

            entry.IsStaged = false;
            RefreshParent(entry.OriginalPath, true);
            return true;
        }

        private bool AnyEntryStaged()
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                if (_entries[i].IsStaged)
                    return true;
            }
            return false;
        }

        private bool AreAllEntriesStaged()
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                if (!_entries[i].IsStaged)
                    return false;
            }
            return true;
        }

        private void RefreshParent(string path, bool checkSubDirs = false)
        {
            var parentPath = Path.GetDirectoryName(path);
            if (string.IsNullOrEmpty(parentPath))
            {
                _editor.ContentDatabase.Rebuild(true);
                return;
            }

            if (_editor.ContentDatabase.Find(parentPath) is ContentFolder parent)
                _editor.ContentDatabase.RefreshFolder(parent, checkSubDirs);
            else
                _editor.ContentDatabase.Rebuild(true);
        }

        private bool StageEntry(ContentItem item, ref Entry entry)
        {
            entry.HasMetadataSidecar = entry.MetadataOriginalPath != null && File.Exists(entry.MetadataOriginalPath);
            entry.HasSidecarFolder = entry.SidecarOriginalPath != null && Directory.Exists(entry.SidecarOriginalPath);
            if (!MoveContentPath(ref entry, entry.OriginalPath, entry.TrashPath))
                return false;
            if (entry.HasMetadataSidecar && !MovePath(entry.MetadataOriginalPath, entry.MetadataTrashPath, false))
            {
                RollbackFailedStage(ref entry);
                return false;
            }
            if (entry.HasSidecarFolder && !MovePath(entry.SidecarOriginalPath, entry.SidecarTrashPath, true))
            {
                RollbackFailedStage(ref entry);
                return false;
            }

            if (item != null)
            {
                try
                {
                    _editor.ContentDatabase.RemoveFromDatabasePreservingAssets(item);
                }
                catch (Exception ex)
                {
                    Editor.LogWarning(ex);
                    Editor.LogWarning("Cannot remove staged Content item from the database: " + entry.OriginalPath);
                    RollbackFailedStage(ref entry);
                    return false;
                }
            }

            entry.IsStaged = true;
            RefreshParent(entry.OriginalPath, true);
            return true;
        }

        private void RollbackFailedStage(ref Entry entry)
        {
            var rollbackSucceeded = true;
            if (!PathExists(entry.OriginalPath, entry.IsFolder))
                rollbackSucceeded &= MoveContentPath(ref entry, entry.TrashPath, entry.OriginalPath);
            else
                DeletePath(entry.TrashPath, entry.IsFolder);

            if (entry.HasMetadataSidecar)
            {
                if (!File.Exists(entry.MetadataOriginalPath))
                    rollbackSucceeded &= MovePath(entry.MetadataTrashPath, entry.MetadataOriginalPath, false);
                else
                    DeletePath(entry.MetadataTrashPath, false);
            }
            if (entry.HasSidecarFolder)
            {
                if (!Directory.Exists(entry.SidecarOriginalPath))
                    rollbackSucceeded &= MovePath(entry.SidecarTrashPath, entry.SidecarOriginalPath, true);
                else
                    DeletePath(entry.SidecarTrashPath, true);
            }

            if (!rollbackSucceeded)
            {
                _requiresRecovery = true;
                Editor.LogError("Failed to roll back content staging. Recovery data was preserved at: " + entry.TrashPath);
            }
            entry.IsStaged = !PathExists(entry.OriginalPath, entry.IsFolder) && PathExists(entry.TrashPath, entry.IsFolder);
            RefreshParent(entry.OriginalPath, true);
        }

        private static bool CopyPath(string sourcePath, string targetPath, bool isFolder)
        {
            for (int retry = 0; retry < FilesystemRetryCount; retry++)
            {
                try
                {
                    var targetDir = Path.GetDirectoryName(targetPath);
                    if (!string.IsNullOrEmpty(targetDir))
                        Directory.CreateDirectory(targetDir);

                    if (isFolder)
                    {
                        if (!Directory.Exists(sourcePath))
                        {
                            Editor.LogWarning("Cannot stage folder. Source is missing: " + sourcePath);
                            return false;
                        }
                        if (Directory.Exists(targetPath) || File.Exists(targetPath))
                        {
                            Editor.LogWarning("Cannot stage folder. Target already exists: " + targetPath);
                            return false;
                        }
                        CopyDirectory(sourcePath, targetPath);
                    }
                    else
                    {
                        if (!File.Exists(sourcePath))
                        {
                            Editor.LogWarning("Cannot stage file. Source is missing: " + sourcePath);
                            return false;
                        }
                        if (File.Exists(targetPath) || Directory.Exists(targetPath))
                        {
                            Editor.LogWarning("Cannot stage file. Target already exists: " + targetPath);
                            return false;
                        }
                        File.Copy(sourcePath, targetPath);
                    }
                    return true;
                }
                catch (IOException ex)
                {
                    DeletePath(targetPath, isFolder);
                    if (retry + 1 == FilesystemRetryCount)
                    {
                        Editor.LogWarning(ex);
                        Editor.LogWarning(string.Format("Cannot stage content item from '{0}' to '{1}'", sourcePath, targetPath));
                        return false;
                    }
                }
                catch (UnauthorizedAccessException ex)
                {
                    DeletePath(targetPath, isFolder);
                    if (retry + 1 == FilesystemRetryCount)
                    {
                        Editor.LogWarning(ex);
                        Editor.LogWarning(string.Format("Cannot stage content item from '{0}' to '{1}'", sourcePath, targetPath));
                        return false;
                    }
                }
                catch (Exception ex)
                {
                    DeletePath(targetPath, isFolder);
                    Editor.LogWarning(ex);
                    Editor.LogWarning(string.Format("Cannot stage content item from '{0}' to '{1}'", sourcePath, targetPath));
                    return false;
                }

                FlaxEngine.Scripting.FlushRemovedObjects();
                Thread.Sleep(FilesystemRetryDelayMs);
            }

            return false;
        }

        private static void CopyDirectory(string sourcePath, string targetPath)
        {
            if ((File.GetAttributes(sourcePath) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Cannot stage a folder through a filesystem link or reparse point: " + sourcePath);
            Directory.CreateDirectory(targetPath);

            var files = Directory.GetFiles(sourcePath);
            for (int i = 0; i < files.Length; i++)
            {
                if ((File.GetAttributes(files[i]) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Cannot stage a file through a filesystem link or reparse point: " + files[i]);
                var targetFile = Path.Combine(targetPath, Path.GetFileName(files[i]));
                File.Copy(files[i], targetFile);
            }

            var folders = Directory.GetDirectories(sourcePath);
            for (int i = 0; i < folders.Length; i++)
            {
                if ((File.GetAttributes(folders[i]) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Cannot stage a folder through a filesystem link or reparse point: " + folders[i]);
                var targetFolder = Path.Combine(targetPath, Path.GetFileName(folders[i]));
                CopyDirectory(folders[i], targetFolder);
            }
        }

        private static bool MoveContentPath(ref Entry entry, string sourcePath, string targetPath)
        {
            try
            {
                var targetDirectory = Path.GetDirectoryName(targetPath);
                if (!string.IsNullOrEmpty(targetDirectory))
                    Directory.CreateDirectory(targetDirectory);

                if (entry.IsFolder)
                    return !FlaxEngine.Content.RenameAssetFolder(sourcePath, targetPath);
                if (entry.AssetId != Guid.Empty)
                    return !FlaxEngine.Content.RenameAsset(sourcePath, targetPath);
                return MovePath(sourcePath, targetPath, false);
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                Editor.LogWarning(string.Format("Cannot move staged Content data from '{0}' to '{1}'", sourcePath, targetPath));
                return false;
            }
        }

        private static void UnloadStagedAssets(string path, bool isFolder)
        {
            path = StringUtils.NormalizePath(path);
            var prefix = path.TrimEnd('/') + "/";
            var comparison = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            var assets = FlaxEngine.Content.Assets;
            bool unloaded = false;
            for (int i = 0; i < assets.Length; i++)
            {
                var assetPath = StringUtils.NormalizePath(assets[i].Path);
                if (string.Equals(assetPath, path, comparison) || isFolder && assetPath.StartsWith(prefix, comparison))
                {
                    FlaxEngine.Content.UnloadAsset(assets[i]);
                    unloaded = true;
                }
            }
            if (unloaded)
                FlaxEngine.Scripting.FlushRemovedObjects();
        }

        private static bool MovePath(string sourcePath, string targetPath, bool isFolder)
        {
            for (int retry = 0; retry < FilesystemRetryCount; retry++)
            {
                try
                {
                    var targetDir = Path.GetDirectoryName(targetPath);
                    if (!string.IsNullOrEmpty(targetDir))
                        Directory.CreateDirectory(targetDir);

                    if (isFolder)
                    {
                        if (!Directory.Exists(sourcePath))
                        {
                            Editor.LogWarning("Cannot move folder. Source is missing: " + sourcePath);
                            return false;
                        }
                        if (Directory.Exists(targetPath) || File.Exists(targetPath))
                        {
                            Editor.LogWarning("Cannot move folder. Target already exists: " + targetPath);
                            return false;
                        }
                        Directory.Move(sourcePath, targetPath);
                    }
                    else
                    {
                        if (!File.Exists(sourcePath))
                        {
                            Editor.LogWarning("Cannot move file. Source is missing: " + sourcePath);
                            return false;
                        }
                        if (File.Exists(targetPath) || Directory.Exists(targetPath))
                        {
                            Editor.LogWarning("Cannot move file. Target already exists: " + targetPath);
                            return false;
                        }
                        File.Move(sourcePath, targetPath);
                    }
                    return true;
                }
                catch (IOException ex)
                {
                    if (retry + 1 == FilesystemRetryCount)
                    {
                        Editor.LogWarning(ex);
                        Editor.LogWarning(string.Format("Cannot move content item from '{0}' to '{1}'", sourcePath, targetPath));
                        return false;
                    }
                }
                catch (UnauthorizedAccessException ex)
                {
                    if (retry + 1 == FilesystemRetryCount)
                    {
                        Editor.LogWarning(ex);
                        Editor.LogWarning(string.Format("Cannot move content item from '{0}' to '{1}'", sourcePath, targetPath));
                        return false;
                    }
                }
                catch (Exception ex)
                {
                    Editor.LogWarning(ex);
                    Editor.LogWarning(string.Format("Cannot move content item from '{0}' to '{1}'", sourcePath, targetPath));
                    return false;
                }

                FlaxEngine.Scripting.FlushRemovedObjects();
                Thread.Sleep(FilesystemRetryDelayMs);
            }

            return false;
        }

        private static bool DeletePath(string path, bool isFolder)
        {
            return DeletePathWithRetries(path, isFolder, "staged content item");
        }

        private static bool DeletePathWithRetries(string path, bool isFolder, string description)
        {
            try
            {
                for (int retry = 0; retry < FilesystemRetryCount; retry++)
                {
                    try
                    {
                        if (isFolder)
                        {
                            if (!Directory.Exists(path))
                                return true;
                            Directory.Delete(path, true);
                        }
                        else
                        {
                            if (!File.Exists(path))
                                return true;
                            File.Delete(path);
                        }
                        return true;
                    }
                    catch (IOException ex)
                    {
                        if (retry + 1 == FilesystemRetryCount)
                        {
                            Editor.LogWarning(ex);
                            Editor.LogWarning("Cannot remove " + description + ": " + path);
                            return false;
                        }
                    }
                    catch (UnauthorizedAccessException ex)
                    {
                        if (retry + 1 == FilesystemRetryCount)
                        {
                            Editor.LogWarning(ex);
                            Editor.LogWarning("Cannot remove " + description + ": " + path);
                            return false;
                        }
                    }

                    FlaxEngine.Scripting.FlushRemovedObjects();
                    Thread.Sleep(FilesystemRetryDelayMs);
                }
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                Editor.LogWarning("Cannot remove " + description + ": " + path);
                return false;
            }

            return false;
        }

        private static bool PathExists(string path, bool isFolder)
        {
            return isFolder ? Directory.Exists(path) : File.Exists(path);
        }

        private static bool ContainsReparsePoint(string path, bool isFolder)
        {
            try
            {
                if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                    return true;
                if (!isFolder)
                    return false;

                var pending = new Stack<string>();
                pending.Push(path);
                while (pending.Count != 0)
                {
                    var folder = pending.Pop();
                    foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
                    {
                        var attributes = File.GetAttributes(entry);
                        if ((attributes & FileAttributes.ReparsePoint) != 0)
                            return true;
                        if ((attributes & FileAttributes.Directory) != 0)
                            pending.Push(entry);
                    }
                }
                return false;
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                return true;
            }
        }

        private static long GetPathSize(string path, bool isFolder)
        {
            try
            {
                if (isFolder)
                {
                    if (!Directory.Exists(path))
                        return -1;

                    long size = 0;
                    var pending = new Stack<string>();
                    pending.Push(path);
                    while (pending.Count != 0)
                    {
                        var folder = pending.Pop();
                        var folderAttributes = File.GetAttributes(folder);
                        if ((folderAttributes & FileAttributes.ReparsePoint) != 0)
                            return -1;
                        foreach (var entry in Directory.EnumerateFileSystemEntries(folder))
                        {
                            var attributes = File.GetAttributes(entry);
                            if ((attributes & FileAttributes.ReparsePoint) != 0)
                                return -1;
                            if ((attributes & FileAttributes.Directory) != 0)
                                pending.Push(entry);
                            else
                                size += new FileInfo(entry).Length;
                        }
                    }
                    return size;
                }

                return File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0 ? new FileInfo(path).Length : -1;
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                Editor.LogWarning("Cannot calculate staged content item size: " + path);
                return -1;
            }
        }
    }

    /// <summary>
    /// Undo action for moving or renaming content items.
    /// </summary>
    [Serializable]
    internal sealed class MoveContentItemAction : ITryUndoAction, IUndoActionMetadata
    {
        private Editor _editor;
        private readonly string _oldPath;
        private readonly string _newPath;
        private readonly Guid _assetId;
        private readonly bool _isAsset;
        private bool _isMoved = true;

        /// <inheritdoc />
        public string ActionString { get; }

        /// <inheritdoc />
        public UndoActionInfo ActionInfo => new UndoActionInfo
        {
            Operation = ActionString,
            TargetType = _isAsset ? UndoActionTargetType.Asset : UndoActionTargetType.ContentItem,
            TargetName = Path.GetFileNameWithoutExtension(_oldPath),
            TargetPath = _oldPath,
            SecondaryTargetPath = _newPath,
            TargetId = _assetId,
            DisplayEditorTypeName = typeof(Windows.ContentWindow).FullName,
            Flags = UndoActionFlags.RequiresReload | UndoActionFlags.AffectsContentDatabase,
        };

        /// <summary>
        /// Initializes a new instance of the <see cref="MoveContentItemAction"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        /// <param name="oldPath">The old path.</param>
        /// <param name="newPath">The new path.</param>
        /// <param name="actionString">The action name.</param>
        public MoveContentItemAction(Editor editor, string oldPath, string newPath, string actionString)
        {
            _editor = editor ?? throw new ArgumentNullException(nameof(editor));
            _oldPath = StringUtils.NormalizePath(oldPath);
            _newPath = StringUtils.NormalizePath(newPath);
            var item = editor.ContentDatabase.Find(_isMoved ? _newPath : _oldPath) ?? editor.ContentDatabase.Find(_oldPath);
            if (item is AssetItem assetItem)
            {
                _assetId = assetItem.ID;
                _isAsset = true;
            }
            ActionString = actionString;
        }

        /// <inheritdoc />
        public void Do()
        {
            TryDo();
        }

        /// <inheritdoc />
        public bool TryDo()
        {
            if (_isMoved)
                return true;
            if (Move(_oldPath, _newPath))
            {
                _isMoved = true;
                return true;
            }
            return false;
        }

        /// <inheritdoc />
        public void Undo()
        {
            TryUndo();
        }

        /// <inheritdoc />
        public bool TryUndo()
        {
            if (!_isMoved)
                return true;
            if (Move(_newPath, _oldPath))
            {
                _isMoved = false;
                return true;
            }
            return false;
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _editor = null;
        }

        private bool Move(string sourcePath, string targetPath)
        {
            var item = _editor.ContentDatabase.Find(sourcePath);
            if (item == null)
            {
                Editor.LogWarning("Cannot move content item. Source is missing: " + sourcePath);
                return false;
            }
            var result = _editor.ContentDatabase.TryMove(new[] { (item, targetPath) });
            if (!result.Succeeded)
            {
                Editor.LogWarning($"Cannot replay Content move '{sourcePath}' to '{targetPath}': {result.Failure}. {result.Message}");
                return false;
            }
            item = _editor.ContentDatabase.Find(targetPath);
            if (item == null)
            {
                Editor.LogWarning("Cannot move content item. Target was not found after move: " + targetPath);
                return false;
            }

            _editor.Windows.ContentWin.Select(item, true);
            return true;
        }
    }

    /// <summary>
    /// Undo action for a transactionally committed batch of Content moves.
    /// </summary>
    [Serializable]
    internal sealed class MoveContentItemsAction : ITryUndoAction, IUndoActionMetadata
    {
        private Editor _editor;
        private readonly string[] _oldPaths;
        private readonly string[] _newPaths;
        private bool _isMoved = true;

        public string ActionString { get; }

        public UndoActionInfo ActionInfo => new UndoActionInfo
        {
            Operation = ActionString,
            TargetType = UndoActionTargetType.ContentItem,
            TargetName = _oldPaths.Length != 0 ? Path.GetFileNameWithoutExtension(_oldPaths[0]) : null,
            TargetPath = _oldPaths.Length != 0 ? _oldPaths[0] : null,
            SecondaryTargetPath = _newPaths.Length != 0 ? _newPaths[0] : null,
            DisplayEditorTypeName = typeof(Windows.ContentWindow).FullName,
            Flags = UndoActionFlags.RequiresReload | UndoActionFlags.AffectsContentDatabase,
        };

        public MoveContentItemsAction(Editor editor, IReadOnlyList<string> oldPaths, IReadOnlyList<string> newPaths, string actionString)
        {
            _editor = editor ?? throw new ArgumentNullException(nameof(editor));
            if (oldPaths == null || newPaths == null || oldPaths.Count == 0 || oldPaths.Count != newPaths.Count)
                throw new ArgumentException("Content batch move paths must be non-empty and have matching counts.");
            _oldPaths = oldPaths.Select(StringUtils.NormalizePath).ToArray();
            _newPaths = newPaths.Select(StringUtils.NormalizePath).ToArray();
            ActionString = actionString;
        }

        public void Do()
        {
            TryDo();
        }

        public bool TryDo()
        {
            if (_isMoved)
                return true;
            if (!Move(_oldPaths, _newPaths))
                return false;
            _isMoved = true;
            return true;
        }

        public void Undo()
        {
            TryUndo();
        }

        public bool TryUndo()
        {
            if (!_isMoved)
                return true;
            if (!Move(_newPaths, _oldPaths))
                return false;
            _isMoved = false;
            return true;
        }

        public void Dispose()
        {
            _editor = null;
        }

        private bool Move(IReadOnlyList<string> sourcePaths, IReadOnlyList<string> destinationPaths)
        {
            var moves = new List<(ContentItem Item, string Destination)>(sourcePaths.Count);
            for (int i = 0; i < sourcePaths.Count; i++)
            {
                var item = _editor.ContentDatabase.Find(sourcePaths[i]);
                if (item == null)
                {
                    Editor.LogWarning("Cannot replay Content batch move. Source is missing: " + sourcePaths[i]);
                    return false;
                }
                moves.Add((item, destinationPaths[i]));
            }

            var result = _editor.ContentDatabase.TryMove(moves);
            if (!result.Succeeded)
            {
                Editor.LogWarning($"Cannot replay Content batch move: {result.Failure}. {result.Message}");
                return false;
            }

            for (int i = 0; i < destinationPaths.Count; i++)
            {
                var item = _editor.ContentDatabase.Find(destinationPaths[i]);
                if (item != null)
                    _editor.Windows.ContentWin.Select(item, true, i != 0);
            }
            return true;
        }
    }
}
