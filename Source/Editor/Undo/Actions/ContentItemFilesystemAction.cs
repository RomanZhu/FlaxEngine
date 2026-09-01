// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
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
            public string[] SidecarOriginalPaths;
            public string[] SidecarTrashPaths;
            public bool IsFolder;
            public Guid AssetId;
            public Guid TrashAssetGuid;
            public long SizeInBytes;
            public bool IsStaged;
        }

        private Editor _editor;
        private Entry[] _entries;
        private readonly Operation _operation;
        private bool _isDeleted;
        private bool _requiresRecovery;
        private Guid _trashTransactionId;

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
                    UnloadStagedAssets(_entries[i].TrashPath, _entries[i].IsFolder);
                if (!TryCreateNativeTrashBatch(out var trash) || AssetOperationService.DiscardTrash(trash))
                {
                    _requiresRecovery = true;
                    Editor.LogError("Native Content trash cleanup failed; recovery data was preserved.");
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

        internal static string GetSceneFragmentsFolderPath(Guid sceneId)
        {
            if (sceneId == Guid.Empty)
                return null;
            return StringUtils.CombinePaths(Globals.ProjectFolder, "ExternalActors", sceneId.ToString("N").ToLowerInvariant());
        }

        private static Entry CreateEntry(ContentItem item)
        {
            var originalPath = StringUtils.NormalizePath(item.Path);
            var metadataOriginalPath = GetMetadataSidecarPath(originalPath, item.IsFolder);
            var sceneId = !item.IsFolder && IsSceneFilePath(originalPath) && item is AssetItem sceneItem ? sceneItem.ID : Guid.Empty;
            var sidecarOriginalPath = GetSceneFragmentsFolderPath(sceneId);
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
                MetadataOriginalPath = metadataOriginalPath,
                IsFolder = item.IsFolder,
                AssetId = item is AssetItem assetItem ? assetItem.ID : Guid.Empty,
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

            return ExecuteNativeStage(items);
        }

        private bool StageByPath()
        {
            if (_isDeleted)
                return true;

            return ExecuteNativeStage(null);
        }

        private bool Restore()
        {
            if (!AnyEntryStaged())
                return true;

            if (!TryCreateNativeTrashBatch(out var trash) || AssetOperationService.RestoreEntries(trash))
            {
                _requiresRecovery = true;
                return false;
            }
            for (int i = 0; i < _entries.Length; i++)
            {
                var entry = _entries[i];
                entry.IsStaged = false;
                _entries[i] = entry;
                RefreshParent(entry.OriginalPath, true);
            }
            _isDeleted = false;
            _requiresRecovery = false;
            return true;
        }

        private bool ExecuteNativeStage(IReadOnlyList<ContentItem> items)
        {
            var requests = new AssetTrashEntryRequest[_entries.Length];
            for (int i = 0; i < _entries.Length; i++)
            {
                requests[i] = new AssetTrashEntryRequest
                {
                    SourcePath = _entries[i].OriginalPath,
                    ExpectedAssetGuid = _entries[i].AssetId,
                    IsFolder = _entries[i].IsFolder,
                };
            }
            if (AssetOperationService.TrashEntries(requests, out var trash) ||
                trash.Entries == null || trash.Entries.Length != _entries.Length)
                return false;

            _trashTransactionId = trash.TransactionId;
            for (int i = 0; i < _entries.Length; i++)
            {
                var native = trash.Entries[i];
                var entry = _entries[i];
                entry.TrashPath = native.TrashPath;
                entry.TrashAssetGuid = native.AssetGuid;
                entry.MetadataOriginalPath = native.OriginalMetaPath;
                entry.MetadataTrashPath = native.TrashMetaPath;
                entry.SidecarOriginalPaths = native.Fragments?.Select(x => x.OriginalPath).ToArray();
                entry.SidecarTrashPaths = native.Fragments?.Select(x => x.TrashPath).ToArray();
                entry.IsStaged = true;
                _entries[i] = entry;
            }
            _isDeleted = true;

            try
            {
                for (int i = 0; i < _entries.Length; i++)
                {
                    var item = items != null ? items[i] : _editor.ContentDatabase.Find(_entries[i].OriginalPath);
                    if (item != null)
                        _editor.ContentDatabase.RemoveFromDatabasePreservingAssets(item);
                    RefreshParent(_entries[i].OriginalPath, true);
                }
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                if (!TryCreateNativeTrashBatch(out var rollbackTrash) || AssetOperationService.RestoreEntries(rollbackTrash))
                {
                    _requiresRecovery = true;
                }
                else
                {
                    for (int i = 0; i < _entries.Length; i++)
                    {
                        var entry = _entries[i];
                        entry.IsStaged = false;
                        _entries[i] = entry;
                        RefreshParent(entry.OriginalPath, true);
                    }
                    _isDeleted = false;
                    _requiresRecovery = false;
                }
                return false;
            }
            return true;
        }

        private bool TryCreateNativeTrashBatch(out AssetTrashBatch trash)
        {
            trash = default;
            if (_trashTransactionId == Guid.Empty || _entries == null || _entries.Length == 0)
                return false;
            var entries = new AssetTrashEntry[_entries.Length];
            for (int i = 0; i < _entries.Length; i++)
            {
                var entry = _entries[i];
                var fragmentCount = entry.SidecarOriginalPaths?.Length ?? 0;
                if ((entry.SidecarTrashPaths?.Length ?? 0) != fragmentCount)
                    return false;
                var fragments = new AssetTrashFragment[fragmentCount];
                for (int fragmentIndex = 0; fragmentIndex < fragments.Length; fragmentIndex++)
                {
                    fragments[fragmentIndex] = new AssetTrashFragment
                    {
                        OriginalPath = entry.SidecarOriginalPaths[fragmentIndex],
                        TrashPath = entry.SidecarTrashPaths[fragmentIndex],
                    };
                }
                entries[i] = new AssetTrashEntry
                {
                    AssetGuid = entry.TrashAssetGuid,
                    OriginalPath = entry.OriginalPath,
                    TrashPath = entry.TrashPath,
                    OriginalMetaPath = entry.MetadataOriginalPath,
                    TrashMetaPath = entry.MetadataTrashPath,
                    Fragments = fragments,
                    IsFolder = entry.IsFolder,
                };
            }
            trash = new AssetTrashBatch
            {
                TransactionId = _trashTransactionId,
                Entries = entries,
            };
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
