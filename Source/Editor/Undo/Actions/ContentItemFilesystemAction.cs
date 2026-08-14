// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
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
            public string SidecarOriginalPath;
            public string SidecarTrashPath;
            public bool IsFolder;
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
                for (int i = 0; i < _entries.Length; i++)
                {
                    DeletePath(_entries[i].TrashPath, _entries[i].IsFolder);
                    if (_entries[i].HasSidecarFolder)
                        DeletePath(_entries[i].SidecarTrashPath, true);
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
            var sidecarOriginalPath = GetSceneActorsFolderPath(originalPath, item.IsFolder);
            var hasSidecarFolder = sidecarOriginalPath != null && Directory.Exists(sidecarOriginalPath);
            var sizeInBytes = GetPathSize(originalPath, item.IsFolder);
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
                SidecarOriginalPath = sidecarOriginalPath,
                SidecarTrashPath = StringUtils.CombinePaths(trashRoot, "SceneActors"),
                IsFolder = item.IsFolder,
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

            var stagedEntries = new List<int>(_entries.Length);
            ContentMutationDiagnostics.Log("mutation.stage.begin", $"action='{ActionString}'; entries={_entries.Length}; source=live-items");
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i];
                if (!StageEntry(item, ref _entries[i]))
                {
                    RollbackStage(stagedEntries);
                    ContentMutationDiagnostics.Log("mutation.stage.failed", $"action='{ActionString}'; entry={i}; recovery={_requiresRecovery}");
                    return false;
                }
                stagedEntries.Add(i);
            }

            _isDeleted = true;
            ContentMutationDiagnostics.Log("mutation.stage.committed", $"action='{ActionString}'; entries={_entries.Length}");
            return true;
        }

        private bool StageByPath()
        {
            if (_isDeleted)
                return true;

            if (!PreflightStage())
                return false;

            var stagedEntries = new List<int>(_entries.Length);
            ContentMutationDiagnostics.Log("mutation.stage.begin", $"action='{ActionString}'; entries={_entries.Length}; source=paths");
            for (int i = 0; i < _entries.Length; i++)
            {
                if (_entries[i].IsStaged)
                    continue;
                var item = _editor.ContentDatabase.Find(_entries[i].OriginalPath);
                if (!StageEntry(item, ref _entries[i]))
                {
                    RollbackStage(stagedEntries);
                    ContentMutationDiagnostics.Log("mutation.stage.failed", $"action='{ActionString}'; entry={i}; recovery={_requiresRecovery}");
                    return false;
                }
                stagedEntries.Add(i);
            }

            _isDeleted = AreAllEntriesStaged();
            ContentMutationDiagnostics.Log("mutation.stage.committed", $"action='{ActionString}'; entries={_entries.Length}");
            return true;
        }

        private bool Restore()
        {
            if (!AnyEntryStaged())
                return true;

            if (!PreflightRestore())
                return false;

            var restoredEntries = new List<int>(_entries.Length);
            ContentMutationDiagnostics.Log("mutation.restore.begin", $"action='{ActionString}'; entries={_entries.Length}");
            for (int i = 0; i < _entries.Length; i++)
            {
                if (!_entries[i].IsStaged)
                    continue;
                if (!RestoreEntry(ref _entries[i]))
                {
                    RollbackRestore(restoredEntries);
                    ContentMutationDiagnostics.Log("mutation.restore.failed", $"action='{ActionString}'; entry={i}; recovery={_requiresRecovery}");
                    return false;
                }
                restoredEntries.Add(i);
            }

            _isDeleted = false;
            ContentMutationDiagnostics.Log("mutation.restore.committed", $"action='{ActionString}'; entries={_entries.Length}");
            return true;
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
            if (!MovePath(entry.TrashPath, entry.OriginalPath, entry.IsFolder))
                return false;
            if (entry.HasSidecarFolder && !MovePath(entry.SidecarTrashPath, entry.SidecarOriginalPath, true))
            {
                if (!MovePath(entry.OriginalPath, entry.TrashPath, entry.IsFolder))
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

        private void RollbackStage(List<int> stagedEntries)
        {
            for (int i = stagedEntries.Count - 1; i >= 0; i--)
            {
                var index = stagedEntries[i];
                if (!RestoreEntry(ref _entries[index]))
                    _requiresRecovery = true;
            }
            _isDeleted = AreAllEntriesStaged();
        }

        private void RollbackRestore(List<int> restoredEntries)
        {
            for (int i = restoredEntries.Count - 1; i >= 0; i--)
            {
                var index = restoredEntries[i];
                var item = _editor.ContentDatabase.Find(_entries[index].OriginalPath);
                if (!StageEntry(item, ref _entries[index]))
                    _requiresRecovery = true;
            }
            _isDeleted = AreAllEntriesStaged();
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
            if (!CopyPath(entry.OriginalPath, entry.TrashPath, entry.IsFolder))
                return false;
            entry.HasSidecarFolder = entry.SidecarOriginalPath != null && Directory.Exists(entry.SidecarOriginalPath);
            if (entry.HasSidecarFolder && !CopyPath(entry.SidecarOriginalPath, entry.SidecarTrashPath, true))
            {
                DeletePath(entry.TrashPath, entry.IsFolder);
                DeletePath(entry.SidecarTrashPath, true);
                return false;
            }

            if (item != null)
            {
                if (!DeleteContentItem(item, entry))
                {
                    RollbackFailedStage(ref entry);
                    return false;
                }
            }
            else if (!DeletePathWithRetries(entry.OriginalPath, entry.IsFolder, "original content item"))
            {
                RollbackFailedStage(ref entry);
                return false;
            }
            if (entry.HasSidecarFolder && Directory.Exists(entry.SidecarOriginalPath) && !DeletePathWithRetries(entry.SidecarOriginalPath, true, "original scene actors folder"))
            {
                Editor.LogWarning("Cannot remove original scene actors folder after staging: " + entry.SidecarOriginalPath);
                RollbackFailedStage(ref entry);
                return false;
            }

            entry.IsStaged = true;
            RefreshParent(entry.OriginalPath, true);
            return true;
        }

        private void RollbackFailedStage(ref Entry entry)
        {
            var rollbackSucceeded = true;
            if (!PathExists(entry.OriginalPath, entry.IsFolder))
                rollbackSucceeded &= MovePath(entry.TrashPath, entry.OriginalPath, entry.IsFolder);
            else
                DeletePath(entry.TrashPath, entry.IsFolder);

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

        private bool DeleteContentItem(ContentItem item, Entry entry)
        {
            try
            {
                _editor.Thumbnails.DeletePreview(item);
                _editor.ContentDatabase.Delete(item, true);
                FlaxEngine.Scripting.FlushRemovedObjects();
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                Editor.LogWarning("Cannot delete staged content item: " + entry.OriginalPath);
                return false;
            }

            if (PathExists(entry.OriginalPath, entry.IsFolder) && !DeletePathWithRetries(entry.OriginalPath, entry.IsFolder, "original content item"))
            {
                Editor.LogWarning("Cannot remove original content item after staging: " + entry.OriginalPath);
                return false;
            }

            return true;
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

        private static void DeletePath(string path, bool isFolder)
        {
            DeletePathWithRetries(path, isFolder, "staged content item");
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
            if (!sourcePath.Equals(targetPath, StringComparison.OrdinalIgnoreCase) && PathExists(targetPath))
            {
                Editor.LogWarning("Cannot move content item. Target already exists: " + targetPath);
                return false;
            }

            if (!_editor.ContentDatabase.Move(item, targetPath))
                return false;
            item = _editor.ContentDatabase.Find(targetPath);
            if (item == null)
            {
                Editor.LogWarning("Cannot move content item. Target was not found after move: " + targetPath);
                return false;
            }

            _editor.Windows.ContentWin.Select(item, true);
            return true;
        }

        private static bool PathExists(string path)
        {
            return File.Exists(path) || Directory.Exists(path);
        }
    }
}
