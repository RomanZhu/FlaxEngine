// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using FlaxEditor.Content;
using FlaxEditor.Content.Settings;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Manages assets database and searches for workspace directory changes.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class ContentDatabaseModule : EditorModule
    {
        private bool _enableEvents;
        private bool _isDuringFastSetup;
        private bool _rebuildFlag;
        private bool _rebuildInitFlag;
        private int _itemsCreated;
        private int _itemsDeleted;
        private bool _useNewAssetDatabase;
        private ulong _assetDatabaseRevision;
        private const double AssetDiskChangeQuietPeriodSeconds = 0.5;
        private const double SelfAuthoredAssetDiskChangeLifetimeSeconds = 5.0;
        private readonly HashSet<ContentFolderTreeNode> _dirtyNodes = new HashSet<ContentFolderTreeNode>();
        private readonly object _assetDiskChangesLock = new object();
        private readonly HashSet<string> _pendingAssetDiskChanges = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> _pendingSourceRefresh = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> _pendingMissingMetadataRegistrations = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> _pendingTextureBuildSources = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<Guid> _pendingTextureBuildIds = new HashSet<Guid>();
        private readonly HashSet<Guid> _renameOnlyTextureIds = new HashSet<Guid>();
        private readonly Dictionary<Guid, AssetDatabaseRecordInfo> _textureRecordsBeforeWatcherScan = new Dictionary<Guid, AssetDatabaseRecordInfo>();
        private readonly Dictionary<string, AssetSaveState> _assetSaves = new Dictionary<string, AssetSaveState>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, AssetDiskWrite> _selfAuthoredAssetDiskChanges = new Dictionary<string, AssetDiskWrite>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, AssetDatabaseRecordInfo> _sourceAssetRecords = new Dictionary<string, AssetDatabaseRecordInfo>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, List<AssetDatabaseRecordInfo>> _subAssetRecordsByFolder = new Dictionary<string, List<AssetDatabaseRecordInfo>>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<Guid, AssetDatabaseRecordInfo> _assetRecordsById = new Dictionary<Guid, AssetDatabaseRecordInfo>();
        private DateTime _lastAssetDiskChangeTime;
        private long _nextAssetSaveGeneration;
        private int _assetDatabaseAutoRefreshDepth;

        internal void SuspendAssetDatabaseAutoRefresh()
        {
            _assetDatabaseAutoRefreshDepth++;
        }

        internal void ResumeAssetDatabaseAutoRefresh()
        {
            if (_assetDatabaseAutoRefreshDepth == 0)
                throw new InvalidOperationException("Asset database auto-refresh suppression is not balanced.");
            _assetDatabaseAutoRefreshDepth--;
        }

        private sealed class AssetSaveState
        {
            public readonly long Generation;
            public int Depth;
            public bool Failed;

            public AssetSaveState(long generation)
            {
                Generation = generation;
                Depth = 1;
            }
        }

        internal sealed class AssetSaveScope : IDisposable
        {
            private ContentDatabaseModule _owner;
            private readonly string _path;
            private bool _succeeded;

            internal AssetSaveScope(ContentDatabaseModule owner, string path)
            {
                _owner = owner;
                _path = path;
                owner.BeginAssetSave(path);
            }

            public void Complete(bool succeeded)
            {
                _succeeded = succeeded;
            }

            public void Dispose()
            {
                var owner = _owner;
                if (owner == null)
                    return;
                _owner = null;
                owner.EndAssetSave(_path, _succeeded);
            }
        }

        private readonly struct AssetDiskWrite
        {
            public readonly DateTime LastWriteTimeUtc;
            public readonly long Length;
            public readonly DateTime ExpiresAtUtc;
            public readonly long Generation;
            public readonly string ContentHash;

            public AssetDiskWrite(DateTime lastWriteTimeUtc, long length, DateTime expiresAtUtc, long generation, string contentHash)
            {
                LastWriteTimeUtc = lastWriteTimeUtc;
                Length = length;
                ExpiresAtUtc = expiresAtUtc;
                Generation = generation;
                ContentHash = contentHash;
            }
        }

        private static string ComputeAssetDiskHash(string path)
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var sha256 = SHA256.Create();
            return Convert.ToBase64String(sha256.ComputeHash(stream));
        }

        /// <summary>
        /// The project directory.
        /// </summary>
        public ProjectFolderTreeNode Game { get; private set; }

        /// <summary>
        /// The engine directory.
        /// </summary>
        public ProjectFolderTreeNode Engine { get; private set; }

        /// <summary>
        /// The list of all projects workspace directories (including game, engine and plugins projects).
        /// </summary>
        public readonly List<ProjectFolderTreeNode> Projects = new List<ProjectFolderTreeNode>();

        /// <summary>
        /// The list with all content items proxy objects. Use <see cref="AddProxy"/> and <see cref="RemoveProxy"/> to modify this or <see cref="Rebuild"/> to refresh database when adding new item proxy types.
        /// </summary>
        public readonly List<ContentProxy> Proxy = new List<ContentProxy>(128);

        /// <summary>
        /// Occurs when new items is added to the workspace content database.
        /// </summary>
        public event Action<ContentItem> ItemAdded;

        /// <summary>
        /// Occurs when new items is removed from the workspace content database.
        /// </summary>
        public event Action<ContentItem> ItemRemoved;

        /// <summary>
        /// Occurs when workspace has been modified.
        /// </summary>
        public event Action WorkspaceModified;

        /// <summary>
        /// Occurs when workspace will be rebuilt.
        /// </summary>
        public event Action WorkspaceRebuilding;

        /// <summary>
        /// Occurs when workspace has been rebuilt.
        /// </summary>
        public event Action WorkspaceRebuilt;

        /// <summary>
        /// Gets the amount of created items.
        /// </summary>
        public int ItemsCreated => _itemsCreated;

        /// <summary>
        /// Gets the amount of deleted items.
        /// </summary>
        public int ItemsDeleted => _itemsDeleted;

        internal ContentDatabaseModule(Editor editor)
        : base(editor)
        {
            // Init content database after UI module
            InitOrder = -80;

            // Register AssetItems serialization helper (serialize ref ID only)
            FlaxEngine.Json.JsonSerializer.Settings.Converters.Add(new AssetItemConverter());

            ScriptsBuilder.ScriptsReload += OnScriptsReload;
            ScriptsBuilder.ScriptsReloadEnd += OnScriptsReloadEnd;
        }

        private void RefreshAssetDatabaseRecords(ulong revision)
        {
            var records = AssetDatabaseFacade.GetRecords();
            _sourceAssetRecords.Clear();
            _subAssetRecordsByFolder.Clear();
            _assetRecordsById.Clear();
            for (int i = 0; i < records.Length; i++)
            {
                var record = records[i];
                _assetRecordsById[record.ID] = record;
                if (record.IsMain && record.SourceKind != AssetSourceKind.LegacyBinary)
                    _sourceAssetRecords[ContentMutationPathUtils.Normalize(record.SourcePath)] = record;
                else if (!record.IsMain && record.SourceKind != AssetSourceKind.LegacyBinary && record.Status != AssetRecordStatus.MissingSource)
                {
                    var folder = ContentMutationPathUtils.Normalize(Path.GetDirectoryName(record.SourcePath));
                    if (!_subAssetRecordsByFolder.TryGetValue(folder, out var subAssets))
                    {
                        subAssets = new List<AssetDatabaseRecordInfo>();
                        _subAssetRecordsByFolder.Add(folder, subAssets);
                    }
                    subAssets.Add(record);
                }
            }
            _assetDatabaseRevision = revision;
        }

        private void QueueCanonicalSourceRefresh(params string[] paths)
        {
            if (paths == null || paths.Length == 0)
                return;
            lock (_assetDiskChangesLock)
            {
                for (int i = 0; i < paths.Length; i++)
                {
                    if (string.IsNullOrEmpty(paths[i]))
                        continue;
                    var path = GetCanonicalSourcePathForDiskEvent(paths[i]);
                    if (string.IsNullOrEmpty(path))
                        continue;

                    // A recursive folder refresh includes every descendant. File watchers emit a
                    // notification for the copied folder and for each item below it, so retaining
                    // all of them turns a large folder copy into repeated recursive scans on the
                    // editor thread. Keep only the highest changed roots in each quiet-period batch.
                    if (_pendingSourceRefresh.Any(root => ContentMutationPathUtils.IsWithinRoot(path, root)))
                        continue;
                    var nested = _pendingSourceRefresh.Where(candidate => ContentMutationPathUtils.IsWithinRoot(candidate, path, false)).ToArray();
                    for (int j = 0; j < nested.Length; j++)
                        _pendingSourceRefresh.Remove(nested[j]);
                    _pendingSourceRefresh.Add(path);
                }
            }
        }

        private void MarkSourceFolderDirty(string fileOrFolderPath)
        {
            if (string.IsNullOrEmpty(fileOrFolderPath))
                return;
            fileOrFolderPath = StringUtils.NormalizePath(fileOrFolderPath);
            var directory = Directory.Exists(fileOrFolderPath) ? fileOrFolderPath : Path.GetDirectoryName(fileOrFolderPath);
            while (!string.IsNullOrEmpty(directory))
            {
                if (Find(directory) is ContentFolder folder && folder.Node != null)
                {
                    lock (_dirtyNodes)
                        _dirtyNodes.Add(folder.Node);
                    return;
                }
                directory = Path.GetDirectoryName(directory);
            }
        }

        private void MarkAllContentRootsDirty()
        {
            lock (_dirtyNodes)
            {
                foreach (var project in Projects)
                {
                    if (project.Content != null)
                        _dirtyNodes.Add(project.Content);
                }
            }
        }

        private void OnAssetDatabaseChanged(ulong revision)
        {
            var change = AssetDatabaseFacade.GetLastChange();

            // Native keeps only the most recent batch, so a publish from a build worker can overwrite
            // it between the event being raised and this read. Scoping the tree update to another
            // revision's identifiers would leave stale items behind, so reconcile everything instead.
            if (change.Revision != revision)
            {
                RefreshAssetDatabaseRecords(revision);
                if (_enableEvents)
                    MarkAllContentRootsDirty();
                return;
            }

            var previousPaths = new Dictionary<Guid, string>();
            void Capture(Guid[] ids)
            {
                if (ids == null)
                    return;
                for (int i = 0; i < ids.Length; i++)
                {
                    if (_assetRecordsById.TryGetValue(ids[i], out var record) && !string.IsNullOrEmpty(record.SourcePath))
                        previousPaths[ids[i]] = record.SourcePath;
                }
            }
            Capture(change.Removed);
            Capture(change.Changed);
            Capture(change.StatusChanged);

            RefreshAssetDatabaseRecords(revision);
            if (!_enableEvents)
                return;

            if (change.Removed != null)
            {
                for (int i = 0; i < change.Removed.Length; i++)
                {
                    if (previousPaths.TryGetValue(change.Removed[i], out var removedPath))
                        MarkSourceFolderDirty(removedPath);
                }
            }
            void Visit(Guid[] ids, bool allowInPlace)
            {
                if (ids == null)
                    return;
                for (int i = 0; i < ids.Length; i++)
                {
                    if (!_assetRecordsById.TryGetValue(ids[i], out var record))
                        continue;
                    previousPaths.TryGetValue(ids[i], out var previousPath);
                    var samePath = !string.IsNullOrEmpty(previousPath) &&
                        string.Equals(ContentMutationPathUtils.Normalize(previousPath), ContentMutationPathUtils.Normalize(record.SourcePath), StringComparison.OrdinalIgnoreCase);
                    if (allowInPlace && samePath && Find(record.ID) is AssetItem item &&
                        string.Equals(ContentMutationPathUtils.Normalize(item.Path), ContentMutationPathUtils.Normalize(record.SourcePath), StringComparison.OrdinalIgnoreCase))
                    {
                        item.SetAssetDatabaseRecord(record);
                        continue;
                    }
                    if (!string.IsNullOrEmpty(previousPath) && !samePath)
                        MarkSourceFolderDirty(previousPath);
                    MarkSourceFolderDirty(record.SourcePath);
                }
            }
            Visit(change.Added, false);
            Visit(change.Changed, true);
            Visit(change.StatusChanged, true);
        }

        private void QueueMissingMetadataRegistrations()
        {
            var diagnostics = AssetDatabaseFacade.GetDiagnostics();
            for (int i = 0; i < diagnostics.Length; i++)
            {
                var diagnostic = diagnostics[i];
                if (string.IsNullOrWhiteSpace(diagnostic.SourcePath))
                    continue;
                var sourcePath = diagnostic.SourcePath.EndsWith(".meta", StringComparison.OrdinalIgnoreCase)
                    ? diagnostic.SourcePath.Substring(0, diagnostic.SourcePath.Length - 5)
                    : diagnostic.SourcePath;
                if (!ContentMutationPathUtils.IsWithinRoot(sourcePath, Globals.ProjectContentFolder, false))
                    continue;
                if (diagnostic.Code == AssetPipelineDiagnosticCode.MissingMeta)
                {
                    _pendingMissingMetadataRegistrations.Add(ContentMutationPathUtils.Normalize(sourcePath));
                }
                else if (diagnostic.Code == AssetPipelineDiagnosticCode.MetaParseError && diagnostic.SourcePath.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                {
                    _pendingMissingMetadataRegistrations.Add(ContentMutationPathUtils.Normalize(sourcePath));
                }
            }
        }

        private void QueueRecoveredCanonicalImports(List<string> sourcePaths)
        {
            for (int i = 0; i < sourcePaths.Count; i++)
            {
                var sourcePath = ContentMutationPathUtils.Normalize(sourcePaths[i]);
                if (!_sourceAssetRecords.TryGetValue(sourcePath, out var record) || record.Status != AssetRecordStatus.Ready || !CanBuildCanonicalRecord(record))
                {
                    Editor.LogWarning($"Recovered imported source '{sourcePath}' requires metadata reconciliation before it can be rebuilt.");
                    continue;
                }

                var failed = IsTextureRecord(record)
                    ? AssetDatabaseFacade.BuildTexture(record.ID)
                    : IsModelRecord(record)
                        ? AssetDatabaseFacade.BuildModel(record.ID)
                        : AssetDatabaseFacade.BuildGraph(record.ID);
                if (failed)
                    Editor.LogError($"Cannot queue recovered canonical asset rebuild: {sourcePath}");
                else
                    Editor.Log($"Recovered interrupted import and queued exact artifact resolution: {sourcePath}");
            }
        }

        private void OnArtifactPublished(Guid assetId)
        {
            if (Find(assetId) is AssetItem item)
            {
                if (item.ReferencesCount > 0)
                    Editor.Thumbnails.RequestPreview(item);
            }
            else
                Editor.Thumbnails.DeletePreview(assetId);
        }

        /// <summary>Gets the immutable canonical database record for an asset identity.</summary>
        public bool TryGetAssetDatabaseRecord(Guid id, out AssetDatabaseRecordInfo record)
        {
            return _assetRecordsById.TryGetValue(id, out record);
        }

        /// <summary>Gets the immutable main canonical database record for a source path.</summary>
        public bool TryGetAssetDatabaseRecord(string path, out AssetDatabaseRecordInfo record)
        {
            return _sourceAssetRecords.TryGetValue(ContentMutationPathUtils.Normalize(path), out record);
        }

        internal static bool UseContentBackendForFileOperation(ContentItem item)
        {
            return item != null && (item.IsAsset || item.ItemType == ContentItemType.Scene) && !(item is AssetItem assetItem && assetItem.IsCanonicalSource);
        }

        internal static bool UseContentBackendForCopy(ContentItem item)
        {
            // Existing JSON assets embed their identity in the document. Other canonical
            // sources keep identity only in metadata and must retain their source bytes.
            if (item is AssetItem { IsCanonicalSource: true } assetItem)
                return string.Equals(assetItem.ProcessorID, "Flax.ExistingJson", StringComparison.Ordinal);
            return UseContentBackendForFileOperation(item);
        }

        internal static bool ShouldRemoveMissingContentItem(ContentItem item)
        {
            return item is not NewItem && item?.Exists == false;
        }

        private void OnContentAssetDisposing(Asset asset)
        {
            // Handle deleted asset
            if (asset.ShouldDeleteFileOnUnload)
            {
                var item = Find(asset.ID);
                if (item != null)
                {
                    // Close all asset editors
                    Editor.Windows.CloseAllEditors(item);

                    // Dispose
                    item.Dispose();
                }
            }
        }

        /// <summary>
        /// Gets the project workspace used by the given project.
        /// </summary>
        /// <param name="project">The project.</param>
        /// <returns>The project workspace or null if not loaded into database.</returns>
        public ProjectFolderTreeNode GetProjectWorkspace(ProjectInfo project)
        {
            return Projects.FirstOrDefault(x => x.Project == project);
        }

        /// <summary>
        /// Gets the proxy object for the given content item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <returns>Content proxy for that item or null if cannot find.</returns>
        public ContentProxy GetProxy(ContentItem item)
        {
            if (item != null)
            {
                for (int i = 0; i < Proxy.Count; i++)
                {
                    if (Proxy[i].IsProxyFor(item))
                        return Proxy[i];
                }
            }
            return null;
        }

        /// <summary>
        /// Gets the proxy object for the given asset type.
        /// </summary>
        /// <returns>Content proxy for that asset type or null if cannot find.</returns>
        public ContentProxy GetProxy<T>() where T : Asset
        {
            for (int i = 0; i < Proxy.Count; i++)
            {
                if (Proxy[i].IsProxyFor<T>())
                    return Proxy[i];
            }
            return null;
        }

        /// <summary>
        /// Gets the proxy object for the given file extension. Warning! Different asset types may share the same file extension.
        /// </summary>
        /// <param name="extension">The file extension.</param>
        /// <returns>Content proxy for that item or null if cannot find.</returns>
        public ContentProxy GetProxy(string extension)
        {
            if (string.IsNullOrEmpty(extension))
                return null;
            extension = StringUtils.NormalizeExtension(extension);
            for (int i = 0; i < Proxy.Count; i++)
            {
                if (string.Equals(Proxy[i].FileExtension, extension, StringComparison.Ordinal))
                    return Proxy[i];
            }
            return null;
        }

        /// <summary>
        /// Gets the proxy object for the given asset type id.
        /// </summary>
        /// <param name="typeName">The asset type name.</param>
        /// <param name="path">The asset path.</param>
        /// <returns>Asset proxy or null if cannot find.</returns>
        public AssetProxy GetAssetProxy(string typeName, string path)
        {
            for (int i = 0; i < Proxy.Count; i++)
            {
                if (Proxy[i] is AssetProxy proxy && proxy.AcceptsAsset(typeName, path))
                    return proxy;
            }
            if (!path.EndsWith(".flax", StringComparison.OrdinalIgnoreCase))
            {
                for (int i = 0; i < Proxy.Count; i++)
                {
                    if (Proxy[i] is AssetProxy proxy && proxy.AcceptsAsset(typeName, "canonical.flax"))
                        return proxy;
                }
            }
            return null;
        }

        private AssetItem ConstructCanonicalSourceItem(string path, AssetDatabaseRecordInfo record)
        {
            var id = record.ID;
            var proxy = GetAssetProxy(record.TypeName, path);
            var item = proxy?.ConstructItem(path, record.TypeName, ref id);
            if (item == null)
            {
                var type = TypeUtils.GetType(record.TypeName).Type;
                if (type != null && typeof(Asset).IsAssignableFrom(type))
                    item = new BinaryAssetItem(path, ref id, record.TypeName, type, ContentItemSearchFilter.Other);
            }
            item?.SetAssetDatabaseRecord(record);
            return item;
        }

        private AssetItem ConstructCanonicalSubAssetItem(AssetDatabaseRecordInfo record)
        {
            var virtualPath = record.SourcePath + "." + record.ID.ToString("N") + ".subasset";
            var item = ConstructCanonicalSourceItem(virtualPath, record);
            if (item == null)
                return null;

            item.ShortName = Path.GetFileNameWithoutExtension(record.SourcePath) + " / " + item.CanonicalSubAssetName + " [" + item.TypeDescription + "]";
            return item;
        }

        private void LoadCanonicalSubAssets(ContentFolderTreeNode parent)
        {
            var folderPath = ContentMutationPathUtils.Normalize(parent.Folder.Path);
            if (!_subAssetRecordsByFolder.TryGetValue(folderPath, out var records))
                return;

            for (int i = 0; i < records.Count; i++)
            {
                var record = records[i];
                var virtualPath = record.SourcePath + "." + record.ID.ToString("N") + ".subasset";
                if (parent.Folder.ContainsChild(virtualPath))
                    continue;
                var item = ConstructCanonicalSubAssetItem(record);
                if (item == null)
                    continue;
                item.ParentFolder = parent.Folder;
                if (_enableEvents)
                {
                    ItemAdded?.Invoke(item);
                    WorkspaceModified?.Invoke();
                }
                _itemsCreated++;
            }
        }

        /// <summary>
        /// Gets the virtual proxy object from given path.
        /// </summary>
        /// <param name="path">The asset path.</param>
        /// <returns>Asset proxy or null if cannot find.</returns>
        public AssetProxy GetAssetVirtualProxy(string path)
        {
            for (int i = 0; i < Proxy.Count; i++)
            {
                if (Proxy[i] is AssetProxy proxy && proxy.IsVirtualProxy() && path.EndsWith(proxy.FileExtension, StringComparison.OrdinalIgnoreCase))
                    return proxy;
            }
            return null;
        }

        /// <summary>
        /// Refreshes the given item folder. Tries to find new content items and remove not existing ones.
        /// </summary>
        /// <param name="item">Folder to refresh</param>
        /// <param name="checkSubDirs">True if search for changes inside a subdirectories, otherwise only top-most folder will be updated</param>
        public void RefreshFolder(ContentItem item, bool checkSubDirs)
        {
            // Peek folder to refresh
            ContentFolder folder = item.IsFolder ? item as ContentFolder : item.ParentFolder;
            if (folder == null)
                return;

            // Update
            LoadFolder(folder.Node, checkSubDirs);
        }

        /// <summary>
        /// Tries to find item at the specified path.
        /// </summary>
        /// <param name="path">The path.</param>
        /// <returns>Found item or null if cannot find it.</returns>
        public ContentItem Find(string path)
        {
            if (string.IsNullOrEmpty(path))
                return null;

            // Ensure path is normalized to the Flax format
            path = StringUtils.NormalizePath(path);

            // TODO: if it's a bottleneck try to optimize searching by spiting path

            foreach (var project in Projects)
            {
                var result = project.Folder.Find(path);
                if (result != null)
                    return result;
            }

            return null;
        }

        /// <summary>
        /// Tries to find item with the specified ID.
        /// </summary>
        /// <param name="id">The item ID.</param>
        /// <returns>Found item or null if cannot find it.</returns>
        public ContentItem Find(Guid id)
        {
            if (id == Guid.Empty)
                return null;

            // TODO: use AssetInfo via Content manager to get asset path very quickly (it's O(1))

            // TODO: if it's a bottleneck try to optimize searching by caching items IDs

            foreach (var project in Projects)
            {
                var result = project.Folder.Find(id);
                if (result != null)
                    return result;
            }
            return null;
        }

        /// <summary>
        /// Tries to find asset with the specified ID.
        /// </summary>
        /// <param name="id">The asset ID.</param>
        /// <returns>Found asset item or null if cannot find it.</returns>
        public AssetItem FindAsset(Guid id)
        {
            if (id == Guid.Empty)
                return null;

            // TODO: use AssetInfo via Content manager to get asset path very quickly (it's O(1))

            // TODO: if it's a bottleneck try to optimize searching by caching items IDs

            foreach (var project in Projects)
            {
                if (project.Content?.Folder.Find(id) is AssetItem result)
                    return result;
            }
            return null;
        }

        /// <summary>
        /// Tries to find script item at the specified path.
        /// </summary>
        /// <param name="path">The path.</param>
        /// <returns>Found script or null if cannot find it.</returns>
        public ScriptItem FindScript(string path)
        {
            foreach (var project in Projects)
            {
                if (project.Source?.Folder.Find(path) is ScriptItem result)
                    return result;
            }
            return null;
        }

        /// <summary>
        /// Tries to find script item with the specified ID.
        /// </summary>
        /// <param name="id">The item ID.</param>
        /// <returns>Found script or null if cannot find it.</returns>
        public ScriptItem FindScript(Guid id)
        {
            if (id == Guid.Empty)
                return null;
            foreach (var project in Projects)
            {
                if (project.Source?.Folder.Find(id) is ScriptItem result)
                    return result;
            }
            return null;
        }

        /// <summary>
        /// Tries to find script item with the specified name.
        /// </summary>
        /// <param name="scriptName">The name of the script.</param>
        /// <returns>Found script or null if cannot find it.</returns>
        public ScriptItem FindScriptWitScriptName(string scriptName)
        {
            foreach (var project in Projects)
            {
                if (project.Source?.Folder.FindScriptWitScriptName(scriptName) is ScriptItem result)
                    return result;
            }
            return null;
        }

        /// <summary>
        /// Tries to find script item that is used by the specified script object.
        /// </summary>
        /// <param name="script">The instance of the script.</param>
        /// <returns>Found script or null if cannot find it.</returns>
        public ScriptItem FindScriptWitScriptName(Script script)
        {
            return FindScriptWitScriptName(TypeUtils.GetObjectType(script));
        }

        /// <summary>
        /// Tries to find script item that is used by the specified script type.
        /// </summary>
        /// <param name="scriptType">The type of the script.</param>
        /// <returns>Found script or null if cannot find it.</returns>
        public ScriptItem FindScriptWitScriptName(ScriptType scriptType)
        {
            if (scriptType != ScriptType.Null)
            {
                var className = scriptType.Name;
                var scriptName = ScriptItem.CreateScriptName(className);
                return FindScriptWitScriptName(scriptName);
            }
            return null;
        }

        private static void UpdateAssetNewNameTree(ContentItem el)
        {
            if (el is AssetItem { IsCanonicalSubAsset: true })
                return;
            string extension = Path.GetExtension(el.Path);
            string newPath = StringUtils.CombinePaths(el.ParentFolder.Path, el.ShortName + extension);

            // The whole folder has already been moved on disk and in the native content cache.
            // Only synchronize the managed content item paths here.
            el.UpdatePath(newPath);
            if (el is ContentFolder folder)
                for (int i = 0; i < folder.Children.Count; i++)
                    UpdateAssetNewNameTree(folder.Children[i]);
        }

        internal ContentMutationResult CreatePath(string destinationPath, bool isDirectory, Action create, bool allowDeferred = false)
        {
            if (create == null)
                throw new ArgumentNullException(nameof(create));
            destinationPath = ContentMutationPathUtils.Normalize(destinationPath);
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Create);
            plan.Entries.Add(new ContentMutationEntry(destinationPath, destinationPath, ContentMutationPathRole.Main, isDirectory)
            {
                SourceRequired = false,
            });
            var step = new ContentMutationStep(
                "create",
                new[] { 0 },
                () =>
                {
                    try
                    {
                        create();
                        if (ContentMutationPathUtils.Exists(destinationPath) || allowDeferred)
                            return ContentMutationResult.Success(null, destinationPath);
                        return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, null, destinationPath, $"Content creation did not produce '{destinationPath}'.");
                    }
                    catch (UnauthorizedAccessException ex)
                    {
                        return ContentMutationResult.Fail(ContentMutationFailure.PermissionDenied, null, destinationPath, ex.Message);
                    }
                    catch (IOException ex)
                    {
                        return ContentMutationResult.Fail(ContentMutationFailure.LockedStorage, null, destinationPath, ex.Message);
                    }
                    catch (Exception ex)
                    {
                        return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, null, destinationPath, ex.Message);
                    }
                },
                () => DeleteCreatedPath(destinationPath),
                () => allowDeferred && !ContentMutationPathUtils.Exists(destinationPath) ||
                      (isDirectory ? Directory.Exists(destinationPath) : File.Exists(destinationPath)));
            var result = new ContentMutationTransaction(plan).Execute(new[] { step });
            ContentMutationDiagnostics.Log(result.Succeeded ? "mutation.create.transaction-committed" : "mutation.create.transaction-failed", $"transaction={plan.Id:N}; destination='{destinationPath}'; directory={isDirectory}; deferred={allowDeferred && !ContentMutationPathUtils.Exists(destinationPath)}; failure={result.Failure}; recovery={result.RequiresRecovery}");
            return result;
        }

        private static bool DeleteCreatedPath(string path)
        {
            try
            {
                var metadataPath = path.EndsWith(".meta", StringComparison.OrdinalIgnoreCase) ? null : path + ".meta";
                if (Directory.Exists(path))
                    Directory.Delete(path, true);
                else if (File.Exists(path))
                {
                    if (FlaxEngine.Content.GetAssetInfo(path, out _))
                        FlaxEngine.Content.DeleteAsset(path);
                    if (File.Exists(path))
                        File.Delete(path);
                }
                if (metadataPath != null && File.Exists(metadataPath))
                    File.Delete(metadataPath);
                var sidecar = ContentMutationPathUtils.GetExternalActorsSidecarPath(path, false, string.Equals(Path.GetExtension(path), ".scene", StringComparison.OrdinalIgnoreCase));
                if (sidecar != null && Directory.Exists(sidecar))
                    Directory.Delete(sidecar, true);
                return !ContentMutationPathUtils.Exists(path) &&
                       (metadataPath == null || !File.Exists(metadataPath)) &&
                       (sidecar == null || !Directory.Exists(sidecar));
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to roll back created Content path '" + path + "': " + ex.Message);
                return false;
            }
        }

        /// <summary>
        /// Moves the specified items to the different location. Handles moving whole directories and single assets.
        /// </summary>
        /// <param name="items">The items.</param>
        /// <param name="newParent">The new parent.</param>
        /// <returns>True if all items were moved, otherwise false.</returns>
        public bool Move(List<ContentItem> items, ContentFolder newParent)
        {
            if (items == null || newParent == null)
                throw new ArgumentNullException();
            var moves = new List<(ContentItem Item, string Destination)>();
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i] ?? throw new ArgumentNullException(nameof(items));
                if (item.ParentFolder == newParent)
                    continue;
                moves.Add((item, StringUtils.CombinePaths(newParent.Path, item.FileName)));
            }
            if (!ConfirmWorkspaceMoveIfNeeded(moves))
                return false;
            var result = TryMove(moves);
            if (!result.Succeeded)
                ShowMoveFailure(result);
            return result.Succeeded;
        }

        /// <summary>
        /// Moves the specified item to the different location. Handles moving whole directories and single assets.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="newParent">The new parent.</param>
        /// <returns>True if the item was moved, otherwise false.</returns>
        public bool Move(ContentItem item, ContentFolder newParent)
        {
            if (newParent == null || item == null)
                throw new ArgumentNullException();

            if (item.ParentFolder == newParent)
                return true;

            var newPath = StringUtils.CombinePaths(newParent.Path, item.FileName);
            return Move(item, newPath);
        }

        /// <summary>
        /// Moves the specified item to the different location. Handles moving whole directories and single assets.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="newPath">The new path.</param>
        /// <returns>True if the item was moved, otherwise false.</returns>
        public bool Move(ContentItem item, string newPath)
        {
            if (item == null || string.IsNullOrEmpty(newPath))
                throw new ArgumentNullException();
            var moves = new List<(ContentItem Item, string Destination)> { (item, newPath) };
            if (!ConfirmWorkspaceMoveIfNeeded(moves))
                return false;
            var result = TryMove(moves);
            if (!result.Succeeded)
                ShowMoveFailure(result);
            return result.Succeeded;
        }

        internal ContentMutationResult TryMove(IReadOnlyList<(ContentItem Item, string Destination)> requestedMoves)
        {
            if (requestedMoves == null || requestedMoves.Count == 0)
                return ContentMutationResult.Prepared(null, null);

            var moves = new List<(ContentItem Item, string Source, string Destination, ContentFolder OldParent, ContentFolder NewParent)>();
            for (int i = 0; i < requestedMoves.Count; i++)
            {
                var item = requestedMoves[i].Item;
                if (item == null)
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, null, requestedMoves[i].Destination, "The source item is missing.");
                if (item is AssetItem { IsCanonicalSubAsset: true })
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, item.Path, requestedMoves[i].Destination, "Canonical subassets cannot be moved independently of their source asset.");
                if (requestedMoves.Any(x => x.Item != item && x.Item is ContentFolder selectedFolder && selectedFolder.Find(item)))
                    continue;
                var source = ContentMutationPathUtils.Normalize(item.Path);
                var destination = ContentMutationPathUtils.Normalize(requestedMoves[i].Destination);
                if (string.Equals(source, destination, StringComparison.Ordinal))
                    continue;
                var newParent = Find(Path.GetDirectoryName(destination)) as ContentFolder;
                if (newParent == null)
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, source, destination, "The target Content folder is missing.");
                moves.Add((item, source, destination, item.ParentFolder, newParent));
            }
            if (moves.Count == 0)
                return ContentMutationResult.Prepared(requestedMoves[0].Item?.Path, requestedMoves[0].Destination);

            var operation = moves.Count == 1 && moves[0].OldParent == moves[0].NewParent ? ContentMutationOperationKind.Rename : ContentMutationOperationKind.Move;
            var plan = new ContentMutationPlan(operation);
            var steps = new List<ContentMutationStep>();
            for (int i = 0; i < moves.Count; i++)
                AddMoveSteps(plan, steps, moves[i].Item, moves[i].Source, moves[i].Destination);

            ContentMutationDiagnostics.Log("mutation.move.begin", $"transaction={plan.Id:N}; operation={operation}; items={moves.Count}; entries={plan.Entries.Count}");
            var transaction = new ContentMutationTransaction(plan);
            var result = transaction.Execute(steps);
            if (!result.Succeeded)
            {
                ContentMutationDiagnostics.Log("mutation.move.failed", $"transaction={plan.Id:N}; failure={result.Failure}; recovery={result.RequiresRecovery}; message='{ContentMutationDiagnostics.Sanitize(result.Message)}'");
                return result;
            }

            // Reconcile the managed database only after every filesystem/native leg commits.
            for (int i = 0; i < moves.Count; i++)
            {
                var move = moves[i];
                if (move.Item is ContentFolder folder)
                {
                    move.Item.UpdatePath(move.Destination);
                    for (int j = 0; j < folder.Children.Count; j++)
                        UpdateAssetNewNameTree(folder.Children[j]);
                }
                else
                {
                    move.Item.UpdatePath(move.Destination);
                }
                move.Item.ParentFolder = move.NewParent;
                move.OldParent?.Node.SortChildren();
                if (move.NewParent != move.OldParent)
                    move.NewParent.Node.SortChildren();
            }

            if (_enableEvents)
                WorkspaceModified?.Invoke();
            if (_useNewAssetDatabase)
            {
                var refresh = new string[moves.Count * 2];
                for (int i = 0; i < moves.Count; i++)
                {
                    refresh[i * 2] = moves[i].Source;
                    refresh[i * 2 + 1] = moves[i].Destination;
                }
                QueueCanonicalSourceRefresh(refresh);
            }
            ContentMutationDiagnostics.Log("mutation.move.committed", $"transaction={plan.Id:N}; operation={operation}; items={moves.Count}; entries={plan.Entries.Count}");
            return result;
        }

        internal ContentMutationResult PreflightMove(IReadOnlyList<(ContentItem Item, string Destination)> requestedMoves)
        {
            if (requestedMoves == null || requestedMoves.Count == 0)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, null, null, "No Content items were selected for the move.");

            var plan = new ContentMutationPlan(ContentMutationOperationKind.Move);
            int topLevelCount = 0;
            for (int i = 0; i < requestedMoves.Count; i++)
            {
                var item = requestedMoves[i].Item;
                if (item == null || !item.Exists)
                    return ContentMutationResult.Fail(ContentMutationFailure.MissingSource, item?.Path, requestedMoves[i].Destination, "A selected Content item is missing.", transactionId: plan.Id);
                if (requestedMoves.Any(x => x.Item != item && x.Item is ContentFolder selectedFolder && selectedFolder.Find(item)))
                    continue;
                var source = ContentMutationPathUtils.Normalize(item.Path);
                var destination = ContentMutationPathUtils.Normalize(requestedMoves[i].Destination);
                if (string.Equals(source, destination, StringComparison.Ordinal))
                    continue;
                if (Find(Path.GetDirectoryName(destination)) is not ContentFolder)
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, source, destination, "The target Content folder is missing.", transactionId: plan.Id);
                AddMoveSteps(plan, new List<ContentMutationStep>(), item, source, destination);
                topLevelCount++;
            }
            return topLevelCount == 0
                ? ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, requestedMoves[0].Item?.Path, requestedMoves[0].Destination, "The selected items are already in the target folder.", transactionId: plan.Id)
                : plan.Preflight();
        }

        private static void AddMoveSteps(ContentMutationPlan plan, List<ContentMutationStep> steps, ContentItem item, string sourcePath, string destinationPath)
        {
            if (ContentMutationPathUtils.IsCaseOnlyRename(sourcePath, destinationPath))
            {
                var temporaryPath = ContentMutationPathUtils.CreateTemporarySibling(sourcePath, "flax-case-rename");
                var sidecarPath = ContentMutationPathUtils.GetExternalActorsSidecarPath(sourcePath, item.IsFolder, item.ItemType == ContentItemType.Scene);
                var hasSidecar = sidecarPath != null && Directory.Exists(sidecarPath);
                var firstIndices = AddMovePlanEntries(plan, item, sourcePath, temporaryPath, false, false, hasSidecar);
                var secondIndices = AddMovePlanEntries(plan, item, temporaryPath, destinationPath, true, true, hasSidecar);
                steps.Add(CreateMoveStep(item, "case-rename-stage", firstIndices, sourcePath, temporaryPath, plan));
                steps.Add(CreateMoveStep(item, "case-rename-commit", secondIndices, temporaryPath, destinationPath, plan));
            }
            else
            {
                var sidecarPath = ContentMutationPathUtils.GetExternalActorsSidecarPath(sourcePath, item.IsFolder, item.ItemType == ContentItemType.Scene);
                var indices = AddMovePlanEntries(plan, item, sourcePath, destinationPath, false, false, sidecarPath != null && Directory.Exists(sidecarPath));
                steps.Add(CreateMoveStep(item, "move", indices, sourcePath, destinationPath, plan));
            }
        }

        private static int[] AddMovePlanEntries(ContentMutationPlan plan, ContentItem item, string sourcePath, string destinationPath, bool sourceProduced, bool destinationReleased, bool includeSidecar)
        {
            int first = plan.Entries.Count;
            AddMoveTreeEntries(plan, item, sourcePath, destinationPath, false, sourceProduced, destinationReleased);
            var sourceSidecar = ContentMutationPathUtils.GetExternalActorsSidecarPath(sourcePath, item.IsFolder, item.ItemType == ContentItemType.Scene);
            if (includeSidecar && sourceSidecar != null)
            {
                var destinationSidecar = ContentMutationPathUtils.GetExternalActorsSidecarPath(destinationPath, item.IsFolder, item.ItemType == ContentItemType.Scene);
                plan.Entries.Add(new ContentMutationEntry(sourceSidecar, destinationSidecar, ContentMutationPathRole.ExternalActorSidecar, true)
                {
                    SourceProducedByTransaction = sourceProduced,
                    DestinationReleasedByTransaction = destinationReleased,
                    DestinationParentProducedByTransaction = true,
                });
            }
            return Enumerable.Range(first, plan.Entries.Count - first).ToArray();
        }

        private static void AddMoveTreeEntries(ContentMutationPlan plan, ContentItem item, string sourcePath, string destinationPath, bool descendant, bool sourceProduced, bool destinationReleased)
        {
            if (item is AssetItem { IsCanonicalSubAsset: true })
                return;
            plan.Entries.Add(new ContentMutationEntry(sourcePath, destinationPath, descendant ? ContentMutationPathRole.Descendant : ContentMutationPathRole.Main, item.IsFolder)
            {
                SourceProducedByTransaction = sourceProduced,
                DestinationReleasedByTransaction = destinationReleased,
                DestinationParentProducedByTransaction = descendant,
            });
            if ((item.IsFolder || item is AssetItem { IsCanonicalSource: true }) && (sourceProduced || File.Exists(sourcePath + ".meta")))
            {
                plan.Entries.Add(new ContentMutationEntry(sourcePath + ".meta", destinationPath + ".meta", ContentMutationPathRole.MetadataSidecar, false)
                {
                    SourceProducedByTransaction = sourceProduced,
                    DestinationReleasedByTransaction = destinationReleased,
                    DestinationParentProducedByTransaction = descendant,
                });
            }
            if (item is ContentFolder folder)
            {
                for (int i = 0; i < folder.Children.Count; i++)
                {
                    var child = folder.Children[i];
                    AddMoveTreeEntries(plan, child, Path.Combine(sourcePath, child.FileName), Path.Combine(destinationPath, child.FileName), true, sourceProduced, destinationReleased);
                }
            }
        }

        private static ContentMutationStep CreateMoveStep(ContentItem item, string name, int[] entryIndices, string sourcePath, string destinationPath, ContentMutationPlan plan)
        {
            return new ContentMutationStep(
                name,
                entryIndices,
                () => MoveBackend(item, sourcePath, destinationPath),
                () => RollbackMoveBackend(item, sourcePath, destinationPath),
                () => VerifyMoveEntries(plan, entryIndices));
        }

        private static ContentMutationResult MoveBackend(ContentItem item, string sourcePath, string destinationPath)
        {
            try
            {
                bool failed;
                if (item.IsFolder)
                {
                    failed = FlaxEngine.Content.RenameAssetFolder(sourcePath, destinationPath);
                    if (!failed && File.Exists(sourcePath + ".meta"))
                        File.Move(sourcePath + ".meta", destinationPath + ".meta");
                }
                else if (UseContentBackendForFileOperation(item))
                    failed = FlaxEngine.Content.RenameAsset(sourcePath, destinationPath);
                else
                {
                    File.Move(sourcePath, destinationPath);
                    if (item is AssetItem assetItem && assetItem.IsCanonicalSource && File.Exists(sourcePath + ".meta"))
                        File.Move(sourcePath + ".meta", destinationPath + ".meta");
                    failed = false;
                }
                return failed
                    ? ContentMutationResult.Fail(ContentMutationFailure.MoveFailed, sourcePath, destinationPath, $"The Content backend failed to move '{sourcePath}'.")
                    : ContentMutationResult.Success(sourcePath, destinationPath);
            }
            catch (UnauthorizedAccessException ex)
            {
                return ContentMutationResult.Fail(ContentMutationFailure.PermissionDenied, sourcePath, destinationPath, ex.Message);
            }
            catch (IOException ex)
            {
                return ContentMutationResult.Fail(ContentMutationFailure.LockedStorage, sourcePath, destinationPath, ex.Message);
            }
            catch (Exception ex)
            {
                return ContentMutationResult.Fail(ContentMutationFailure.MoveFailed, sourcePath, destinationPath, ex.Message);
            }
        }

        private static bool RollbackMoveBackend(ContentItem item, string sourcePath, string destinationPath)
        {
            if (ContentMutationPathUtils.Exists(sourcePath) && !ContentMutationPathUtils.Exists(destinationPath))
                return true;
            if (ContentMutationPathUtils.Exists(sourcePath) || !ContentMutationPathUtils.Exists(destinationPath))
                return false;
            return MoveBackend(item, destinationPath, sourcePath).Succeeded;
        }

        private static bool VerifyMoveEntries(ContentMutationPlan plan, int[] entryIndices)
        {
            for (int i = 0; i < entryIndices.Length; i++)
            {
                var entry = plan.Entries[entryIndices[i]];
                if (ContentMutationPathUtils.Exists(entry.SourcePath) || !ContentMutationPathUtils.Exists(entry.DestinationPath))
                    return false;
            }
            return true;
        }

        private bool ConfirmWorkspaceMoveIfNeeded(IReadOnlyList<(ContentItem Item, string Destination)> moves)
        {
            bool crossesWorkspaceKind = false;
            for (int i = 0; i < moves.Count && !crossesWorkspaceKind; i++)
            {
                for (int j = 0; j < Projects.Count && !crossesWorkspaceKind; j++)
                {
                    var project = Projects[j];
                    var sourceInContent = project.Content != null && ContentMutationPathUtils.IsWithinRoot(moves[i].Item.Path, project.Content.Path);
                    var sourceInSource = project.Source != null && ContentMutationPathUtils.IsWithinRoot(moves[i].Item.Path, project.Source.Path);
                    var destinationInContent = project.Content != null && ContentMutationPathUtils.IsWithinRoot(moves[i].Destination, project.Content.Path);
                    var destinationInSource = project.Source != null && ContentMutationPathUtils.IsWithinRoot(moves[i].Destination, project.Source.Path);
                    crossesWorkspaceKind = sourceInContent && destinationInSource || sourceInSource && destinationInContent;
                }
            }
            if (!crossesWorkspaceKind)
                return true;
            return MessageBox.Show(Editor.Windows.MainWindow, "Moving items between Content and Source may lose asset database references.\nDo you want to continue?", "Moving item", MessageBoxButtons.OKCancel) == DialogResult.OK;
        }

        private static void ShowMoveFailure(ContentMutationResult result)
        {
            var message = result.RequiresRecovery
                ? "Cannot move Content item. Recovery data was preserved; see the log for exact paths."
                : result.Failure == ContentMutationFailure.DestinationCollision
                    ? "Cannot move Content item because the target already exists."
                    : result.Failure == ContentMutationFailure.PathCycle
                        ? "Cannot move a folder into itself or one of its descendants."
                        : result.Failure == ContentMutationFailure.InvalidDestination
                            ? "Cannot move Content item because the target folder is missing."
                            : "Cannot move Content item. See the log for details.";
            MessageBox.Show(message);
        }

        /// <summary>
        /// Copies the specified item to the target location. Handles copying whole directories and single assets.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="targetPath">The target item path.</param>
        /// <returns>The exact copy result.</returns>
        internal ContentMutationResult Copy(ContentItem item, string targetPath)
        {
            string sourcePath = item?.Path;
            ContentMutationDiagnostics.Log("mutation.copy.begin", $"source='{sourcePath}'; destination='{targetPath}'; folder={item?.IsFolder}");
            if (item == null || !item.Exists)
            {
                ContentMutationDiagnostics.Log("mutation.copy.rejected", $"reason=invalid-source; source='{sourcePath}'; destination='{targetPath}'");
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, sourcePath, targetPath, "The source item is missing.");
            }

            return Copy(new[] { (item, targetPath) });
        }

        internal ContentMutationResult Copy(IReadOnlyList<(ContentItem Item, string Destination)> requests)
        {
            if (requests == null || requests.Count == 0)
                return ContentMutationResult.Prepared(null, null);

            var plan = new ContentMutationPlan(ContentMutationOperationKind.Copy);
            var steps = new List<ContentMutationStep>(requests.Count);
            for (int i = 0; i < requests.Count; i++)
            {
                var item = requests[i].Item;
                var targetPath = ContentMutationPathUtils.Normalize(requests[i].Destination);
                if (item == null || !item.Exists)
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, item?.Path, targetPath, "The source item is missing.", transactionId: plan.Id);
                if (item is AssetItem { IsCanonicalSubAsset: true })
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, item.Path, targetPath, "Canonical subassets cannot be copied independently of their source asset.", transactionId: plan.Id);

                var itemPlan = BuildCopyPlan(item, targetPath);
                var databasePreflight = PreflightCopy(item, targetPath, itemPlan);
                if (!databasePreflight.Succeeded)
                {
                    ContentMutationDiagnostics.Log("mutation.copy.rejected", $"reason={databasePreflight.Failure}; source='{item.Path}'; destination='{targetPath}'; message='{databasePreflight.Message}'");
                    return databasePreflight;
                }
                int firstEntry = plan.Entries.Count;
                plan.Entries.AddRange(itemPlan.Entries);
                var entryIndices = Enumerable.Range(firstEntry, itemPlan.Entries.Count).ToArray();
                var clonedAssets = new List<string>();
                steps.Add(new ContentMutationStep(
                    requests.Count == 1 ? "copy" : "copy-" + i,
                    entryIndices,
                    () => CommitCopy(item, targetPath, clonedAssets),
                    () => RollbackCopy(plan, entryIndices, clonedAssets),
                    () => VerifyCopy(plan, entryIndices)));
            }

            var transaction = new ContentMutationTransaction(plan);
            var result = transaction.Execute(steps);
            if (result.Succeeded)
            {
                if (_useNewAssetDatabase)
                {
                    var refresh = new string[requests.Count];
                    for (int i = 0; i < requests.Count; i++)
                        refresh[i] = requests[i].Destination;
                    QueueCanonicalSourceRefresh(refresh);
                }
                ContentMutationDiagnostics.Log("mutation.copy.committed", $"transaction={result.TransactionId:N}; items={requests.Count}; entries={plan.Entries.Count}");
            }
            else
                ContentMutationDiagnostics.Log("mutation.copy.failed", $"transaction={result.TransactionId:N}; items={requests.Count}; entries={plan.Entries.Count}; failure={result.Failure}; recovery={result.RequiresRecovery}; message='{ContentMutationDiagnostics.Sanitize(result.Message)}'");
            return result;
        }

        internal ContentMutationResult PreflightCopy(ContentItem item, string targetPath)
        {
            return PreflightCopy(item, targetPath, item != null ? BuildCopyPlan(item, targetPath) : null);
        }

        private ContentMutationResult PreflightCopy(ContentItem item, string targetPath, ContentMutationPlan plan)
        {
            var sourcePath = item?.Path;
            if (item == null || !item.Exists)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, sourcePath, targetPath, "The source item is missing.");
            var planResult = plan.Preflight();
            if (!planResult.Succeeded)
                return planResult;
            targetPath = ContentMutationPathUtils.Normalize(targetPath);
            var comparer = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            var plannedPaths = new HashSet<string>(comparer);
            ContentMutationResult failure = default;
            return PreflightCopyTree(item, targetPath, plannedPaths, comparer, ref failure)
                ? ContentMutationResult.Prepared(sourcePath, targetPath, plan.Id)
                : failure;
        }

        private static ContentMutationPlan BuildCopyPlan(ContentItem item, string targetPath)
        {
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Copy);
            targetPath = ContentMutationPathUtils.Normalize(targetPath);
            AddCopyPlanEntries(plan, item, targetPath, false);

            var sourceSidecar = ContentMutationPathUtils.GetExternalActorsSidecarPath(item.Path, item.IsFolder, item.ItemType == ContentItemType.Scene);
            if (sourceSidecar != null && Directory.Exists(sourceSidecar))
            {
                var targetSidecar = ContentMutationPathUtils.GetExternalActorsSidecarPath(targetPath, item.IsFolder, item.ItemType == ContentItemType.Scene);
                plan.Entries.Add(new ContentMutationEntry(sourceSidecar, targetSidecar, ContentMutationPathRole.ExternalActorSidecar, true)
                {
                    DestinationParentProducedByTransaction = true,
                });
            }
            return plan;
        }

        private static void AddCopyPlanEntries(ContentMutationPlan plan, ContentItem item, string targetPath, bool descendant)
        {
            if (item is AssetItem { IsCanonicalSubAsset: true })
                return;
            var entry = new ContentMutationEntry(item.Path, targetPath, descendant ? ContentMutationPathRole.Descendant : ContentMutationPathRole.Main, item.IsFolder)
            {
                DestinationParentProducedByTransaction = descendant,
                AssetCloneExpected = !item.IsFolder && UseContentBackendForCopy(item),
            };
            plan.Entries.Add(entry);
            if ((item.IsFolder || item is AssetItem { IsCanonicalSource: true }) && File.Exists(item.Path + ".meta"))
            {
                plan.Entries.Add(new ContentMutationEntry(item.Path + ".meta", targetPath + ".meta", ContentMutationPathRole.MetadataSidecar, false)
                {
                    DestinationParentProducedByTransaction = descendant,
                });
            }
            if (item is ContentFolder folder)
            {
                for (int i = 0; i < folder.Children.Count; i++)
                {
                    var child = folder.Children[i];
                    AddCopyPlanEntries(plan, child, Path.Combine(targetPath, child.FileName), true);
                }
            }
        }

        private ContentMutationResult CommitCopy(ContentItem item, string targetPath, List<string> clonedAssets)
        {
            try
            {
                if (item.IsFolder)
                {
                    Directory.CreateDirectory(targetPath);
                    CloneFolderMetadata(item.Path, targetPath);
                    CopyFolderChildren((ContentFolder)item, targetPath, clonedAssets);
                }
                else
                {
                    CopyFileItem(item, targetPath, clonedAssets);
                }
                return ContentMutationResult.Success(item.Path, targetPath);
            }
            catch (UnauthorizedAccessException ex)
            {
                Editor.LogWarning(ex);
                return ContentMutationResult.Fail(ContentMutationFailure.PermissionDenied, item.Path, targetPath, ex.Message);
            }
            catch (IOException ex)
            {
                Editor.LogWarning(ex);
                return ContentMutationResult.Fail(ContentMutationFailure.CopyFailed, item.Path, targetPath, ex.Message);
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                return ContentMutationResult.Fail(ContentMutationFailure.CopyFailed, item.Path, targetPath, ex.Message);
            }
        }

        private static bool VerifyCopy(ContentMutationPlan plan, int[] entryIndices)
        {
            for (int i = 0; i < entryIndices.Length; i++)
            {
                var entry = plan.Entries[entryIndices[i]];
                if (!ContentMutationPathUtils.Exists(entry.DestinationPath))
                    return false;
                if (entry.Role == ContentMutationPathRole.MetadataSidecar)
                    continue;
                if (!entry.IsDirectory && entry.AssetCloneExpected)
                {
                    // Asset cloning assigns a new ID and may rewrite the storage, so byte
                    // length is not an identity invariant. Validate its identity and type.
                    if (!entry.SourceWasAsset ||
                        !FlaxEngine.Content.GetAssetInfo(entry.DestinationPath, out var assetInfo) ||
                        assetInfo.ID == Guid.Empty ||
                        assetInfo.ID == entry.SourceAssetId ||
                        !string.Equals(assetInfo.TypeName, entry.SourceAssetType, StringComparison.Ordinal))
                    {
                        return false;
                    }
                }
                else if (!entry.IsDirectory && entry.SourceLength >= 0 && new FileInfo(entry.DestinationPath).Length != entry.SourceLength)
                {
                    return false;
                }
            }
            return true;
        }

        private static bool RollbackCopy(ContentMutationPlan plan, int[] entryIndices, List<string> clonedAssets)
        {
            bool succeeded = true;
            for (int i = clonedAssets.Count - 1; i >= 0; i--)
            {
                try
                {
                    FlaxEngine.Content.DeleteAsset(clonedAssets[i]);
                }
                catch
                {
                    succeeded = false;
                }
            }
            for (int i = entryIndices.Length - 1; i >= 0; i--)
            {
                var path = plan.Entries[entryIndices[i]].DestinationPath;
                try
                {
                    if (Directory.Exists(path))
                        Directory.Delete(path, true);
                    else if (File.Exists(path))
                        File.Delete(path);
                }
                catch
                {
                    succeeded = false;
                }
            }
            return succeeded && entryIndices.All(x => !ContentMutationPathUtils.Exists(plan.Entries[x].DestinationPath));
        }

        private static bool PreflightCopyTree(ContentItem item, string targetPath, HashSet<string> plannedPaths, StringComparer comparer, ref ContentMutationResult failure)
        {
            targetPath = Path.GetFullPath(targetPath);
            if (!plannedPaths.Add(targetPath) || File.Exists(targetPath) || Directory.Exists(targetPath))
            {
                failure = ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, item.Path, targetPath, "A copy destination is duplicated or already exists.");
                return false;
            }
            if (item is ContentFolder folder)
            {
                var indexedSourcePaths = new HashSet<string>(folder.Children.Select(x => Path.GetFullPath(x.Path)), comparer);
                foreach (var sourceEntry in Directory.EnumerateFileSystemEntries(folder.Path))
                {
                    if (sourceEntry.EndsWith(".meta", StringComparison.OrdinalIgnoreCase) && indexedSourcePaths.Contains(Path.GetFullPath(sourceEntry.Substring(0, sourceEntry.Length - 5))))
                        continue;
                    if (!indexedSourcePaths.Contains(Path.GetFullPath(sourceEntry)))
                    {
                        failure = ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, item.Path, targetPath, $"The Content database has not indexed source entry '{sourceEntry}'.");
                        return false;
                    }
                }
                for (int i = 0; i < folder.Children.Count; i++)
                {
                    var child = folder.Children[i];
                    if (child is AssetItem { IsCanonicalSubAsset: true })
                        continue;
                    if (!child.Exists)
                    {
                        failure = ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, child.Path, targetPath, "A source child is missing.");
                        return false;
                    }
                    var childTargetPath = Path.Combine(targetPath, child.FileName);
                    if (!PreflightCopyTree(child, childTargetPath, plannedPaths, comparer, ref failure))
                        return false;
                }
            }
            return true;
        }

        private void CopyFolderChildren(ContentFolder folder, string targetPath, List<string> clonedAssets)
        {
            for (int i = 0; i < folder.Children.Count; i++)
            {
                var child = folder.Children[i];
                if (child is AssetItem { IsCanonicalSubAsset: true })
                    continue;
                var childTargetPath = Path.Combine(targetPath, child.FileName);
                if (File.Exists(childTargetPath) || Directory.Exists(childTargetPath))
                    throw new IOException($"Copy destination '{childTargetPath}' appeared after preflight.");
                if (child is ContentFolder childFolder)
                {
                    Directory.CreateDirectory(childTargetPath);
                    CloneFolderMetadata(child.Path, childTargetPath);
                    CopyFolderChildren(childFolder, childTargetPath, clonedAssets);
                }
                else
                {
                    CopyFileItem(child, childTargetPath, clonedAssets);
                }
            }
        }

        private static void CloneFolderMetadata(string sourcePath, string targetPath)
        {
            var sourceMetaPath = sourcePath + ".meta";
            if (!File.Exists(sourceMetaPath))
                return;
            if (AssetDatabaseFacade.CloneMetadata(sourceMetaPath, targetPath + ".meta"))
                throw new IOException($"Cannot clone folder metadata for '{sourcePath}'.");
        }

        private void CopyFileItem(ContentItem item, string targetPath, List<string> clonedAssets)
        {
            if (item is AssetItem assetItem && assetItem.IsCanonicalSource)
            {
                var targetMetaPath = targetPath + ".meta";
                if (AssetDatabaseFacade.CloneMetadata(item.Path + ".meta", targetMetaPath))
                    throw new IOException($"Cannot clone metadata for canonical source '{item.Path}'.");
                if (UseContentBackendForCopy(item))
                {
                    var cloneId = ReadCanonicalMetadataGuid(targetMetaPath);
                    if (Editor.ContentEditing.CloneAssetFile(item.Path, targetPath, cloneId))
                        throw new IOException($"The Content backend failed to clone canonical asset '{item.Path}'.");
                    clonedAssets.Add(targetPath);
                }
                else
                {
                    File.Copy(item.Path, targetPath, false);
                }
            }
            else if (UseContentBackendForFileOperation(item))
            {
                if (Editor.ContentEditing.CloneAssetFile(item.Path, targetPath, Guid.NewGuid()))
                    throw new IOException($"The Content backend failed to clone '{item.Path}'.");
                clonedAssets.Add(targetPath);
            }
            else
            {
                File.Copy(item.Path, targetPath, false);
            }
        }

        internal static Guid ReadCanonicalMetadataGuid(string metadataPath)
        {
            try
            {
                var document = JObject.Parse(File.ReadAllText(metadataPath));
                var value = document.Value<string>("guid");
                if (value?.Length == 32 && Guid.TryParseExact(value, "N", out _))
                {
                    var id = FlaxEngine.Json.JsonSerializer.ParseID(value);
                    if (id != Guid.Empty)
                        return id;
                }
            }
            catch (Exception ex) when (ex is IOException || ex is JsonException)
            {
                throw new IOException($"Cannot read cloned canonical metadata '{metadataPath}'.", ex);
            }
            throw new IOException($"Cloned canonical metadata '{metadataPath}' has no valid root GUID.");
        }

        /// <summary>
        /// Deletes the specified item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="deletedByUser">If the file was deleted by the user and not outside the editor.</param>
        public void Delete(ContentItem item, bool deletedByUser = false)
        {
            if (item == null)
                throw new ArgumentNullException();

            if (deletedByUser && item is AssetItem { IsCanonicalSubAsset: true })
            {
                Editor.LogWarning("Canonical subassets cannot be deleted independently of their source asset.");
                return;
            }

            ContentMutationDiagnostics.Log("mutation.delete.begin", $"path='{item.Path}'; folder={item.IsFolder}; user={deletedByUser}; type={item.GetType().Name}");

            if (deletedByUser && item is AssetItem canonicalItem && canonicalItem.IsCanonicalSource && !DeleteCanonicalAssetPair(canonicalItem))
                return;

            // Fire events
            if (_enableEvents)
                ItemRemoved?.Invoke(item);
            item.OnDelete();
            _itemsDeleted++;

            var path = item.Path;

            // Special case for folders
            if (item is ContentFolder folder)
            {
                // Delete all children
                if (folder.Children.Count > 0)
                {
                    var children = folder.Children.ToArray();
                    for (int i = 0; i < children.Length; i++)
                    {
                        var childDeletedByUser = deletedByUser && children[i] is not AssetItem { IsCanonicalSubAsset: true };
                        Delete(children[i], childDeletedByUser);
                    }
                }

                // Remove directory
                if (deletedByUser && Directory.Exists(path))
                {
                    // Flush files removal before removing folder (loaded assets remove file during object destruction in Asset::OnDeleteObject)
                    FlaxEngine.Scripting.FlushRemovedObjects();

                    try
                    {
                        var metadataPath = path + ".meta";
                        if (File.Exists(metadataPath))
                        {
                            if (!DeleteCanonicalFolderPair(path))
                                return;
                        }
                        else
                        {
                            Directory.Delete(path, true);
                        }
                    }
                    catch (Exception ex)
                    {
                        Editor.LogWarning(ex);
                        Editor.LogWarning(string.Format("Cannot remove folder \'{0}\'", path));
                        return;
                    }
                }

                // Unlink from the parent
                item.ParentFolder = null;

                // Delete tree node
                folder.Node.Dispose();
            }

            else
            {
                // Try to remove module if build.cs file is being deleted
                if (item.Path.Contains(".Build.cs", StringComparison.Ordinal) && item.ItemType == ContentItemType.Script)
                    Editor.Instance.CodeEditing.RemoveModule(item.Path);

                // Check if it's an asset
                if (item.IsAsset)
                {
                    if (item is AssetItem assetItem)
                        Editor.Windows.CloseAllEditors(assetItem);

                    // Delete asset by using content pool
                    if (item is not AssetItem canonicalAsset || !canonicalAsset.IsCanonicalSource)
                        FlaxEngine.Content.DeleteAsset(path);
                }
                else if (item is ScriptItem)
                {
                    FlaxEngine.Content.DeleteScript(path);
                }
                else if (deletedByUser)
                {
                    // Delete file
                    if (File.Exists(path))
                        File.Delete(path);
                }

                // Unlink from the parent
                item.ParentFolder = null;

                // Delete item
                item.Dispose();
            }

            ContentMutationDiagnostics.Log("mutation.delete.committed", $"path='{path}'; folder={item.IsFolder}; user={deletedByUser}");
            if (_enableEvents)
                WorkspaceModified?.Invoke();
        }

        private bool DeleteCanonicalFolderPair(string folderPath)
        {
            var metadataPath = folderPath + ".meta";
            var trashRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "ContentMutationDelete", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(trashRoot);
            var stagedFolder = Path.Combine(trashRoot, Path.GetFileName(folderPath));
            var stagedMeta = stagedFolder + ".meta";
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Delete);
            plan.Entries.Add(new ContentMutationEntry(folderPath, stagedFolder, ContentMutationPathRole.Main, true));
            plan.Entries.Add(new ContentMutationEntry(metadataPath, stagedMeta, ContentMutationPathRole.MetadataSidecar, false));
            var step = new ContentMutationStep(
                "delete-canonical-folder-pair",
                new[] { 0, 1 },
                () =>
                {
                    Directory.Move(folderPath, stagedFolder);
                    File.Move(metadataPath, stagedMeta);
                    return ContentMutationResult.Success(folderPath, stagedFolder);
                },
                () =>
                {
                    try
                    {
                        if (File.Exists(stagedMeta) && !File.Exists(metadataPath))
                            File.Move(stagedMeta, metadataPath);
                        if (Directory.Exists(stagedFolder) && !Directory.Exists(folderPath))
                            Directory.Move(stagedFolder, folderPath);
                        return Directory.Exists(folderPath) && File.Exists(metadataPath) && !Directory.Exists(stagedFolder) && !File.Exists(stagedMeta);
                    }
                    catch
                    {
                        return false;
                    }
                },
                () => !Directory.Exists(folderPath) && !File.Exists(metadataPath) && Directory.Exists(stagedFolder) && File.Exists(stagedMeta));
            var result = new ContentMutationTransaction(plan).Execute(new[] { step });
            if (!result.Succeeded)
            {
                Editor.LogError($"Cannot delete canonical folder pair '{folderPath}': {result.Message}");
                return false;
            }
            try
            {
                Directory.Delete(stagedFolder, true);
                File.Delete(stagedMeta);
                Directory.Delete(trashRoot);
            }
            catch (Exception ex)
            {
                Editor.LogWarning($"Canonical folder was deleted but its recovery staging data could not be cleaned: {ex.Message}");
            }
            QueueCanonicalSourceRefresh(folderPath);
            return true;
        }

        private bool DeleteCanonicalAssetPair(AssetItem item)
        {
            var sourcePath = ContentMutationPathUtils.Normalize(item.Path);
            var metaPath = sourcePath + ".meta";
            if (!File.Exists(sourcePath) || !File.Exists(metaPath))
            {
                Editor.LogError($"Cannot delete canonical source pair because '{sourcePath}' or its metadata sidecar is missing.");
                return false;
            }

            var trashRoot = StringUtils.CombinePaths(Globals.ProjectCacheFolder, "ContentMutationDelete", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(trashRoot);
            var stagedSource = Path.Combine(trashRoot, Path.GetFileName(sourcePath));
            var stagedMeta = stagedSource + ".meta";
            var plan = new ContentMutationPlan(ContentMutationOperationKind.Delete);
            plan.Entries.Add(new ContentMutationEntry(sourcePath, stagedSource, ContentMutationPathRole.Main, false));
            plan.Entries.Add(new ContentMutationEntry(metaPath, stagedMeta, ContentMutationPathRole.MetadataSidecar, false));
            var indices = new[] { 0, 1 };
            var step = new ContentMutationStep(
                "delete-canonical-pair",
                indices,
                () =>
                {
                    File.Move(sourcePath, stagedSource);
                    File.Move(metaPath, stagedMeta);
                    return ContentMutationResult.Success(sourcePath, stagedSource);
                },
                () =>
                {
                    try
                    {
                        if (File.Exists(stagedMeta) && !File.Exists(metaPath))
                            File.Move(stagedMeta, metaPath);
                        if (File.Exists(stagedSource) && !File.Exists(sourcePath))
                            File.Move(stagedSource, sourcePath);
                        return File.Exists(sourcePath) && File.Exists(metaPath) && !File.Exists(stagedSource) && !File.Exists(stagedMeta);
                    }
                    catch
                    {
                        return false;
                    }
                },
                () => !File.Exists(sourcePath) && !File.Exists(metaPath) && File.Exists(stagedSource) && File.Exists(stagedMeta));
            var result = new ContentMutationTransaction(plan).Execute(new[] { step });
            if (!result.Succeeded)
            {
                Editor.LogError($"Cannot delete canonical source pair '{sourcePath}': {result.Message}");
                return false;
            }
            try
            {
                File.Delete(stagedMeta);
                File.Delete(stagedSource);
                Directory.Delete(trashRoot);
            }
            catch (Exception ex)
            {
                Editor.LogWarning($"Canonical source was deleted but its recovery staging data could not be cleaned: {ex.Message}");
            }
            QueueCanonicalSourceRefresh(sourcePath);
            return true;
        }

        /// <summary>
        /// Removes the specified item from the content database without deleting its backing files.
        /// </summary>
        /// <param name="item">The item.</param>
        internal void RemoveFromDatabase(ContentItem item)
        {
            if (item == null)
                throw new ArgumentNullException();

            RemoveFromDatabaseInternal(item, false);

            if (_enableEvents)
                WorkspaceModified?.Invoke();
        }

        /// <summary>
        /// Removes an item from the visible Content database while keeping loaded native asset
        /// instances alive. Undoable deletion uses this after moving the backing data into the
        /// Editor trash so references held by open assets survive until the history entry is
        /// restored or permanently discarded.
        /// </summary>
        internal void RemoveFromDatabasePreservingAssets(ContentItem item)
        {
            if (item == null)
                throw new ArgumentNullException();

            RemoveFromDatabaseInternal(item, true);

            if (_enableEvents)
                WorkspaceModified?.Invoke();
        }

        private void RemoveFromDatabaseInternal(ContentItem item, bool preserveLoadedAssets)
        {
            if (_enableEvents)
                ItemRemoved?.Invoke(item);
            item.OnDelete();
            _itemsDeleted++;

            if (item is ContentFolder folder)
            {
                if (folder.Children.Count > 0)
                {
                    var children = folder.Children.ToArray();
                    for (int i = 0; i < children.Length; i++)
                        RemoveFromDatabaseInternal(children[i], preserveLoadedAssets);
                }

                item.ParentFolder = null;
                folder.Node.Dispose();
            }
            else
            {
                if (item.Path.Contains(".Build.cs", StringComparison.Ordinal) && item.ItemType == ContentItemType.Script)
                    Editor.Instance.CodeEditing.RemoveModule(item.Path);

                if (item is AssetItem assetItem)
                {
                    Editor.Windows.CloseAllEditors(assetItem);
                    if (!preserveLoadedAssets)
                    {
                        var asset = FlaxEngine.Content.GetAsset(assetItem.ID);
                        if (asset)
                        {
                            FlaxEngine.Content.UnloadAsset(asset);
                            FlaxEngine.Scripting.FlushRemovedObjects();
                        }
                    }
                }

                item.ParentFolder = null;
                item.Dispose();
            }
        }

        /// <summary>
        /// Adds the proxy.
        /// </summary>
        /// <param name="proxy">The proxy type.</param>
        /// <param name="rebuild">Should rebuild entire database after addition.</param>
        public void AddProxy(ContentProxy proxy, bool rebuild = false)
        {
            var oldProxy = Proxy.Find(x => x.GetType().ToString().Equals(proxy.GetType().ToString(), StringComparison.Ordinal));
            if (oldProxy != null)
                RemoveProxy(oldProxy);
            Proxy.Insert(0, proxy);
            if (rebuild)
                Rebuild(true);
        }

        /// <summary>
        /// Removes the proxy.
        /// </summary>
        /// <param name="proxy">The proxy type.</param>
        /// <param name="rebuild">Should rebuild entire database after removal.</param>
        public void RemoveProxy(ContentProxy proxy, bool rebuild = false)
        {
            Proxy.Remove(proxy);
            if (rebuild)
                Rebuild(true);
        }

        /// <summary>
        /// Rebuilds the whole database (eg. when adding custom item types from plugin).
        /// </summary>
        /// <param name="immediate">True if rebuild now, otherwise will be scheduled for the next editor update (eg. to batch multiple rebuilds within a frame).</param>
        public void Rebuild(bool immediate = false)
        {
            _rebuildFlag = true;
            if (immediate)
                RebuildInternal();
        }

        private void RebuildInternal()
        {
            var enableEvents = _enableEvents;
            if (enableEvents)
            {
                WorkspaceRebuilding?.Invoke();
            }

            Profiler.BeginEvent("ContentDatabase.Rebuild");
            var startTime = Platform.TimeSeconds;
            _rebuildFlag = false;
            _rebuildInitFlag = false;
            _enableEvents = false;

            // Load all folders
            // TODO: we should create async task for gathering content and whole workspace contents if it takes too long
            // TODO: create progress bar in content window and after end we should enable events and update it
            _isDuringFastSetup = true;
            var startItems = _itemsCreated;
            foreach (var project in Projects)
            {
                if (project.Content != null)
                    LoadFolder(project.Content, true);
                if (project.Source != null)
                    LoadFolder(project.Source, true);
            }
            _isDuringFastSetup = false;

            _enableEvents = enableEvents;
            var endTime = Platform.TimeSeconds;
            Editor.Log(string.Format("Project database created in {0} ms. Items count: {1}", (int)((endTime - startTime) * 1000.0), _itemsCreated - startItems));
            Profiler.EndEvent();

            if (enableEvents)
            {
                WorkspaceModified?.Invoke();
                WorkspaceRebuilt?.Invoke();
            }
        }

        private void Dispose(ContentItem item)
        {
            if (_enableEvents)
                ItemRemoved?.Invoke(item);

            if (item is ContentFolder folder)
            {
                if (folder.Children.Count > 0)
                {
                    var children = folder.Children.ToArray();
                    for (int i = 0; i < children.Length; i++)
                        Dispose(children[i]);
                }

                item.ParentFolder = null;
                folder.Node.Dispose();
            }
            else
            {
                item.ParentFolder = null;
                item.Dispose();
            }
        }

        private void LoadFolder(ContentFolderTreeNode node, bool checkSubDirs)
        {
            if (node == null)
                return;
            var folder = node.Folder;
            var path = folder.Path;
            var canHaveAssets = node.CanHaveAssets;

            if (_isDuringFastSetup)
            {
                // Remove any spawned children
                for (int i = 0; i < folder.Children.Count; i++)
                {
                    Dispose(folder.Children[i]);
                    i--;
                }
            }
            else
            {
                // Check for missing files/folders (skip it during fast tree setup)
                for (int i = 0; i < folder.Children.Count; i++)
                {
                    var child = folder.Children[i];
                    if (ShouldRemoveMissingContentItem(child))
                    {
                        // Item doesn't exist anymore
                        Editor.Log($"Content item \'{child.Path}\' has been removed");
                        Delete(child);
                        i--;
                    }
                    else if (canHaveAssets && child is AssetItem childAsset)
                    {
                        if (childAsset.IsCanonicalSource)
                        {
                            var recordExists = _assetRecordsById.TryGetValue(childAsset.ID, out var record);
                            var recordMatchesItem = recordExists &&
                                                    record.TypeName == childAsset.TypeName &&
                                                    record.IsMain != childAsset.IsCanonicalSubAsset &&
                                                    string.Equals(ContentMutationPathUtils.Normalize(record.SourcePath), ContentMutationPathUtils.Normalize(childAsset.SourcePath), StringComparison.OrdinalIgnoreCase);
                            if (!recordMatchesItem)
                            {
                                // The identity, type, or source mapping changed. Replace only
                                // this item and leave unrelated open asset editors untouched.
                                Dispose(childAsset);
                                i--;
                                continue;
                            }

                            childAsset.SetAssetDatabaseRecord(record);
                            continue;
                        }

                        if (_sourceAssetRecords.TryGetValue(ContentMutationPathUtils.Normalize(childAsset.Path), out var sourceRecord))
                        {
                            if (sourceRecord.ID == childAsset.ID && sourceRecord.TypeName == childAsset.TypeName)
                            {
                                // A copied source can be discovered by the legacy cache before its sidecar
                                // registration completes. Promote the existing item once canonical ownership
                                // becomes available, and discard only a failed object that tried to parse the
                                // source bytes as a legacy .flax container.
                                if (FlaxEngine.Content.GetAsset(childAsset.ID) is BinaryAsset staleAsset &&
                                    staleAsset.LastLoadFailed &&
                                    string.Equals(ContentMutationPathUtils.Normalize(staleAsset.StoragePath), ContentMutationPathUtils.Normalize(sourceRecord.SourcePath), StringComparison.OrdinalIgnoreCase))
                                {
                                    FlaxEngine.Content.UnloadAsset(staleAsset);
                                }
                                childAsset.SetAssetDatabaseRecord(sourceRecord);
                                continue;
                            }

                            Dispose(childAsset);
                            i--;
                            continue;
                        }

                        // Check if asset type doesn't match the item proxy (eg. item reimported as Material Instance instead of Material)
                        if (FlaxEngine.Content.GetAssetInfo(child.Path, out var assetInfo))
                        {
                            bool changed = assetInfo.ID != childAsset.ID;
                            if (!changed && assetInfo.TypeName != childAsset.TypeName)
                            {
                                // Use proxy check (eg. scene asset might accept different typename than AssetInfo reports)
                                var proxy = GetAssetProxy(childAsset.TypeName, child.Path);
                                if (proxy == null)
                                    proxy = GetAssetProxy(assetInfo.TypeName, child.Path);
                                changed = !proxy.AcceptsAsset(assetInfo.TypeName, child.Path);
                            }
                            if (changed)
                            {
                                OnAssetTypeInfoChanged(childAsset, ref assetInfo);
                                i--;
                            }
                        }
                    }
                    else if (canHaveAssets && child is FileItem && _sourceAssetRecords.TryGetValue(ContentMutationPathUtils.Normalize(child.Path), out var sourceRecord))
                    {
                        var item = ConstructCanonicalSourceItem(child.Path, sourceRecord);
                        if (item != null)
                        {
                            var index = folder.Children.IndexOf(child);
                            Dispose(child);
                            item.ParentFolder = folder;
                            if (index >= 0 && index < folder.Children.Count)
                            {
                                folder.Children.Remove(item);
                                folder.Children.Insert(index, item);
                            }
                            if (_enableEvents)
                            {
                                ItemAdded?.Invoke(item);
                                WorkspaceModified?.Invoke();
                            }
                        }
                    }
                    else if (canHaveAssets && child is FileItem && FlaxEngine.Content.GetAssetInfo(child.Path, out var assetInfo))
                    {
                        // The asset info can become available after the file system notification that created
                        // a temporary generic file item. Upgrade it into the proper asset item when refreshing.
                        var proxy = GetAssetProxy(assetInfo.TypeName, child.Path);
                        var item = proxy?.ConstructItem(child.Path, assetInfo.TypeName, ref assetInfo.ID);
                        if (item != null)
                        {
                            var index = folder.Children.IndexOf(child);
                            Dispose(child);
                            item.ParentFolder = folder;
                            if (index >= 0 && index < folder.Children.Count)
                            {
                                folder.Children.Remove(item);
                                folder.Children.Insert(index, item);
                            }
                            if (_enableEvents)
                            {
                                ItemAdded?.Invoke(item);
                                WorkspaceModified?.Invoke();
                            }
                        }
                    }
                }
            }

            // Find files
            var files = Directory.GetFiles(path, "*.*", SearchOption.TopDirectoryOnly);
            if (canHaveAssets)
            {
                LoadAssets(node, files);
                LoadCanonicalSubAssets(node);
            }
            if (node.CanHaveScripts)
            {
                LoadScripts(node, files);
            }

            // Get child directories
            var childFolders = Directory.GetDirectories(path);

            // Load child folders
            bool sortChildren = false;
            for (int i = 0; i < childFolders.Length; i++)
            {
                var childPath = StringUtils.NormalizePath(childFolders[i]);

                // Check if node already has that element (skip during init when we want to walk project dir very fast)
                ContentFolder childFolderNode = _isDuringFastSetup ? null : node.Folder.FindChild(childPath) as ContentFolder;
                if (childFolderNode == null)
                {
                    // Create node
                    ContentFolderTreeNode n = new ContentFolderTreeNode(node, childPath);
                    if (!_isDuringFastSetup)
                        sortChildren = true;

                    // Load child folder
                    LoadFolder(n, true);

                    // Fire event
                    if (_enableEvents)
                    {
                        ItemAdded?.Invoke(n.Folder);
                        WorkspaceModified?.Invoke();
                    }
                    _itemsCreated++;
                }
                else if (checkSubDirs)
                {
                    // Update child folder
                    LoadFolder(childFolderNode.Node, true);
                }
            }
            if (sortChildren)
                node.SortChildren();

            // Ignore some special folders
            if (node is MainContentFolderTreeNode mainNode && mainNode.Folder.ShortName == "Source")
            {
                var mainNodeChild = mainNode.Folder.Find(StringUtils.CombinePaths(mainNode.Path, "obj")) as ContentFolder;
                if (mainNodeChild != null)
                {
                    mainNodeChild.Visible = false;
                    mainNodeChild.Node.Visible = false;
                }
                mainNodeChild = mainNode.Folder.Find(StringUtils.CombinePaths(mainNode.Path, "Properties")) as ContentFolder;
                if (mainNodeChild != null)
                {
                    mainNodeChild.Visible = false;
                    mainNodeChild.Node.Visible = false;
                }
            }
        }

        private void LoadScripts(ContentFolderTreeNode parent, string[] files)
        {
            for (int i = 0; i < files.Length; i++)
            {
                var path = StringUtils.NormalizePath(files[i]);

                if (path.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                    continue;

                // Check if node already has that element (skip during init when we want to walk project dir very fast)
                if (_isDuringFastSetup || !parent.Folder.ContainsChild(path))
                {
#if PLATFORM_MAC
                    if (path.EndsWith(".DS_Store", StringComparison.Ordinal))
                        continue;
#endif

                    // Create file item
                    ContentItem item;
                    if (path.EndsWith(".cs"))
                        item = new CSharpScriptItem(path);
                    else if (path.EndsWith(".cpp") || path.EndsWith(".h") || path.EndsWith(".c") || path.EndsWith(".hpp"))
                        item = new CppScriptItem(path);
                    else if (path.EndsWith(".shader") || path.EndsWith(".hlsl"))
                        item = new ShaderSourceItem(path);
                    else
                        item = new FileItem(path);

                    // Link
                    item.ParentFolder = parent.Folder;

                    // Fire event
                    if (_enableEvents)
                    {
                        ItemAdded?.Invoke(item);
                        WorkspaceModified?.Invoke();
                        if (!path.EndsWith(".Gen.cs"))
                        {
                            if (item is ScriptItem)
                                ScriptsBuilder.MarkWorkspaceDirty();
                            if (item is ScriptItem || item is ShaderSourceItem)
                                Editor.CodeEditing.SelectedEditor.OnFileAdded(path);
                        }
                    }
                    _itemsCreated++;
                }
            }
        }

        private void LoadAssets(ContentFolderTreeNode parent, string[] files)
        {
            for (int i = 0; i < files.Length; i++)
            {
                var path = StringUtils.NormalizePath(files[i]);

                if (path.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                    continue;

                // Check if node already has that element (skip during init when we want to walk project dir very fast)
                if (_isDuringFastSetup || !parent.Folder.ContainsChild(path))
                {
#if PLATFORM_MAC
                    if (path.EndsWith(".DS_Store", StringComparison.Ordinal))
                        continue;
#endif

                    // Create file item
                    ContentItem item = null;
                    AssetInfo assetInfo = default;
                    if (_sourceAssetRecords.TryGetValue(path, out var sourceRecord))
                    {
                        item = ConstructCanonicalSourceItem(path, sourceRecord);
                    }
                    else if (FlaxEngine.Content.GetAssetInfo(path, out assetInfo))
                    {
                        var proxy = GetAssetProxy(assetInfo.TypeName, path);
                        item = proxy?.ConstructItem(path, assetInfo.TypeName, ref assetInfo.ID);
                    }
                    if (item == null)
                    {
                        var proxy = GetAssetVirtualProxy(path);
                        item = proxy?.ConstructItem(path, assetInfo.TypeName, ref assetInfo.ID);
                        if (item == null)
                        {
                            item = GetProxy(Path.GetExtension(path))?.ConstructItem(path);
                            if (item == null)
                                item = new FileItem(path);
                        }
                    }

                    // Link
                    item.ParentFolder = parent.Folder;

                    // Fire event
                    if (_enableEvents)
                    {
                        ItemAdded?.Invoke(item);
                        WorkspaceModified?.Invoke();
                    }
                    _itemsCreated++;
                }
            }
        }

        private void LoadProjects(ProjectInfo project)
        {
            var workspace = GetProjectWorkspace(project);
            if (workspace == null)
            {
                workspace = new ProjectFolderTreeNode(project);
                Projects.Add(workspace);

                var contentFolder = StringUtils.CombinePaths(project.ProjectFolderPath, "Content");
                if (Directory.Exists(contentFolder))
                {
                    workspace.Content = new MainContentFolderTreeNode(workspace, ContentFolderType.Content, contentFolder);
                    workspace.Content.Folder.ParentFolder = workspace.Folder;
                }

                var sourceFolder = StringUtils.CombinePaths(project.ProjectFolderPath, "Source");
                if (Directory.Exists(sourceFolder))
                {
                    workspace.Source = new MainContentFolderTreeNode(workspace, ContentFolderType.Source, sourceFolder);
                    workspace.Source.Folder.ParentFolder = workspace.Folder;
                }
            }

            foreach (var reference in project.References)
            {
                LoadProjects(reference.Project);
            }
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            FlaxEngine.Content.AssetDisposing += OnContentAssetDisposing;

            // Recover or surface any mutation that was interrupted before the previous Editor process exited.
            var recoveredImportSources = new List<string>();
            var recoveryRequired = ContentMutationTransaction.RecoverPendingTransactions(recoveredImportSources);
            if (recoveryRequired != 0)
                Editor.LogError($"{recoveryRequired} interrupted Content transaction(s) require manual recovery. See the log for exact paths.");

            _useNewAssetDatabase = true;
            if (_useNewAssetDatabase)
            {
                AssetDatabaseFacade.DatabaseChanged += OnAssetDatabaseChanged;
                AssetDatabaseFacade.ArtifactPublished += OnArtifactPublished;
                if (AssetDatabaseFacade.LoadOrScan(true))
                    Editor.LogError("Failed to initialize the canonical asset database. See asset pipeline diagnostics.");
                RefreshAssetDatabaseRecords(AssetDatabaseFacade.Revision);
                QueueRecoveredCanonicalImports(recoveredImportSources);
                QueueMissingMetadataRegistrations();
            }

            // Setup content proxies
            Proxy.Add(new TextureProxy());
            Proxy.Add(new ModelProxy());
            Proxy.Add(new SkinnedModelProxy());
            Proxy.Add(new MaterialProxy());
            Proxy.Add(new MaterialInstanceProxy());
            Proxy.Add(new MaterialFunctionProxy());
            Proxy.Add(new SpriteAtlasProxy());
            Proxy.Add(new CubeTextureProxy());
            Proxy.Add(new PreviewsCacheProxy());
            Proxy.Add(new FontProxy());
            Proxy.Add(new ShaderProxy());
            Proxy.Add(new ShaderSourceProxy());
            Proxy.Add(new ShaderHeaderProxy());
            Proxy.Add(new ParticleEmitterProxy());
            Proxy.Add(new ParticleEmitterFunctionProxy());
            Proxy.Add(new ParticleSystemProxy());
            Proxy.Add(new SceneAnimationProxy());
            Proxy.Add(new CSharpScriptProxy());
            Proxy.Add(new CSharpEmptyProxy());
            Proxy.Add(new CSharpEmptyClassProxy());
            Proxy.Add(new CSharpEmptyStructProxy());
            Proxy.Add(new CSharpEmptyInterfaceProxy());
            Proxy.Add(new CSharpActorProxy());
            Proxy.Add(new CSharpGamePluginProxy());
            Proxy.Add(new CppAssetProxy());
            Proxy.Add(new CppStaticClassProxy());
            Proxy.Add(new CppScriptProxy());
            Proxy.Add(new CppActorProxy());
            Proxy.Add(new SceneProxy());
            Proxy.Add(new PrefabProxy());
            Proxy.Add(new IESProfileProxy());
            Proxy.Add(new CollisionDataProxy());
            Proxy.Add(new AudioClipProxy());
            Proxy.Add(new AnimationGraphProxy());
            Proxy.Add(new AnimationGraphFunctionProxy());
            Proxy.Add(new AnimationProxy());
            Proxy.Add(new SkeletonMaskProxy());
            Proxy.Add(new GameplayGlobalsProxy());
            Proxy.Add(new VisualScriptProxy());
            Proxy.Add(new BehaviorTreeProxy());
            Proxy.Add(new LocalizedStringTableProxy());
            Proxy.Add(new VideoProxy("mp4"));
            Proxy.Add(new WidgetProxy());
            Proxy.Add(new FileProxy());
            Proxy.Add(new SpawnableJsonAssetProxy<PhysicalMaterial>());

            // Settings
            Proxy.Add(new SettingsProxy(typeof(GameSettings), Editor.Instance.Icons.GameSettings128));
            Proxy.Add(new SettingsProxy(typeof(TimeSettings), Editor.Instance.Icons.TimeSettings128));
            Proxy.Add(new SettingsProxy(typeof(LayersAndTagsSettings), Editor.Instance.Icons.LayersTagsSettings128));
            Proxy.Add(new SettingsProxy(typeof(PhysicsSettings), Editor.Instance.Icons.PhysicsSettings128));
            Proxy.Add(new SettingsProxy(typeof(GraphicsSettings), Editor.Instance.Icons.GraphicsSettings128));
            Proxy.Add(new SettingsProxy(typeof(NetworkSettings), Editor.Instance.Icons.Document128));
            Proxy.Add(new SettingsProxy(typeof(NavigationSettings), Editor.Instance.Icons.NavigationSettings128));
            Proxy.Add(new SettingsProxy(typeof(LocalizationSettings), Editor.Instance.Icons.LocalizationSettings128));
            Proxy.Add(new SettingsProxy(typeof(AudioSettings), Editor.Instance.Icons.AudioSettings128));
            Proxy.Add(new SettingsProxy(typeof(BuildSettings), Editor.Instance.Icons.BuildSettings128));
            Proxy.Add(new SettingsProxy(typeof(InputSettings), Editor.Instance.Icons.InputSettings128));
            Proxy.Add(new SettingsProxy(typeof(StreamingSettings), Editor.Instance.Icons.BuildSettings128));
            Proxy.Add(new SettingsProxy(typeof(WindowsPlatformSettings), Editor.Instance.Icons.WindowsSettings128));
            Proxy.Add(new SettingsProxy(typeof(UWPPlatformSettings), Editor.Instance.Icons.UWPSettings128));
            Proxy.Add(new SettingsProxy(typeof(LinuxPlatformSettings), Editor.Instance.Icons.LinuxSettings128));
            Proxy.Add(new SettingsProxy(typeof(AndroidPlatformSettings), Editor.Instance.Icons.AndroidSettings128));
            Proxy.Add(new SettingsProxy(typeof(MacPlatformSettings), Editor.Instance.Icons.AppleSettings128));
            Proxy.Add(new SettingsProxy(typeof(iOSPlatformSettings), Editor.Instance.Icons.AppleSettings128));
            Proxy.Add(new SettingsProxy(typeof(WebPlatformSettings), Editor.Instance.Icons.WebSettings128));

            var typePS4PlatformSettings = TypeUtils.GetManagedType(GameSettings.PS4PlatformSettingsTypename);
            if (typePS4PlatformSettings != null)
                Proxy.Add(new SettingsProxy(typePS4PlatformSettings, Editor.Instance.Icons.PlaystationSettings128));

            var typeXboxOnePlatformSettings = TypeUtils.GetManagedType(GameSettings.XboxOnePlatformSettingsTypename);
            if (typeXboxOnePlatformSettings != null)
                Proxy.Add(new SettingsProxy(typeXboxOnePlatformSettings, Editor.Instance.Icons.XBOXSettings128));

            var typeXboxScarlettPlatformSettings = TypeUtils.GetManagedType(GameSettings.XboxScarlettPlatformSettingsTypename);
            if (typeXboxScarlettPlatformSettings != null)
                Proxy.Add(new SettingsProxy(typeXboxScarlettPlatformSettings, Editor.Instance.Icons.XBOXSettings128));

            var typeSwitchPlatformSettings = TypeUtils.GetManagedType(GameSettings.SwitchPlatformSettingsTypename);
            if (typeSwitchPlatformSettings != null)
                Proxy.Add(new SettingsProxy(typeSwitchPlatformSettings, Editor.Instance.Icons.SwitchSettings128));

            var typePS5PlatformSettings = TypeUtils.GetManagedType(GameSettings.PS5PlatformSettingsTypename);
            if (typePS5PlatformSettings != null)
                Proxy.Add(new SettingsProxy(typePS5PlatformSettings, Editor.Instance.Icons.PlaystationSettings128));

            // Last add generic json (won't override other json proxies)
            Proxy.Add(new GenericJsonAssetProxy());

            // Create content folders nodes
            Engine = new ProjectFolderTreeNode(Editor.EngineProject)
            {
                Content = new MainContentFolderTreeNode(Engine, ContentFolderType.Content, Globals.EngineContentFolder),
            };
            if (Editor.GameProject != Editor.EngineProject)
            {
                Game = new ProjectFolderTreeNode(Editor.GameProject)
                {
                    Content = new MainContentFolderTreeNode(Game, ContentFolderType.Content, Globals.ProjectContentFolder),
                    Source = new MainContentFolderTreeNode(Game, ContentFolderType.Source, Globals.ProjectSourceFolder),
                };
                // TODO: why it's required? the code above should work for linking the nodes hierarchy
                Game.Content.Folder.ParentFolder = Game.Folder;
                Game.Source.Folder.ParentFolder = Game.Folder;
                Projects.Add(Game);
            }
            Engine.Content.Folder.ParentFolder = Engine.Folder;
            Projects.Add(Engine);
            if (Editor.GameProject != Editor.EngineProject)
            {
                LoadProjects(Game.Project);
            }

            Editor.ContentImporting.ImportFileEnd += (obj, failed) =>
            {
                var path = obj.ResultUrl;
                if (!failed)
                    FlaxEngine.Scripting.InvokeOnUpdate(() => OnImportFileDone(path));
            };
            _enableEvents = true;
            _rebuildInitFlag = true;
        }

        /// <inheritdoc />
        public override void OnEndInit()
        {
            // Handle init when project was loaded without scripts loading ()
            if (_rebuildInitFlag)
                RebuildInternal();
        }

        private void OnImportFileDone(string path)
        {
            var item = Find(path);
            if (item is BinaryAssetItem binaryAssetItem)
            {
                // Get asset info from the registry (content layer will update cache it just after import)
                if (FlaxEngine.Content.GetAssetInfo(binaryAssetItem.Path, out var assetInfo))
                {
                    // If asset type id has been changed we HAVE TO close all windows that use it
                    // For eg. change texture to sprite atlas on reimport
                    if (binaryAssetItem.TypeName != assetInfo.TypeName)
                    {
                        var toRefresh = binaryAssetItem.ParentFolder;
                        OnAssetTypeInfoChanged(binaryAssetItem, ref assetInfo);

                        // Refresh the parent folder to find the new asset (it should have different type or some other format)
                        RefreshFolder(toRefresh, false);
                    }
                    else
                    {
                        // Refresh element data that could change during importing
                        binaryAssetItem.OnReimport(ref assetInfo.ID);
                    }
                }
            }
        }

        private void OnAssetTypeInfoChanged(AssetItem assetItem, ref AssetInfo assetInfo)
        {
            // Asset type has been changed!
            Editor.LogWarning(string.Format("Asset \'{0}\' changed type from {1} to {2}", assetItem.Path, assetItem.TypeName, assetInfo.TypeName));
            Editor.Windows.CloseAllEditors(assetItem);

            // Remove this item from the database and some related data
            assetItem.Dispose();
            assetItem.ParentFolder.Children.Remove(assetItem);

            // Delete old thumbnail and remove it from the cache
            if (!assetItem.HasDefaultThumbnail)
            {
                Editor.Instance.Thumbnails.DeletePreview(assetItem);
            }
        }

        private static string GetCanonicalSourcePathForDiskEvent(string path)
        {
            path = ContentMutationPathUtils.Normalize(path);
            return path.EndsWith(".meta", StringComparison.OrdinalIgnoreCase) ? path.Substring(0, path.Length - 5) : path;
        }

        private static bool IsTextureRecord(AssetDatabaseRecordInfo record)
        {
            return record.IsMain && string.Equals(record.ProcessorID, "Flax.Texture", StringComparison.Ordinal);
        }

        private static bool IsModelRecord(AssetDatabaseRecordInfo record)
        {
            return record.IsMain && string.Equals(record.ProcessorID, "Flax.Model", StringComparison.Ordinal);
        }

        private static bool IsDocumentOrImportedSourceRecord(AssetDatabaseRecordInfo record)
        {
            if (!record.IsMain)
                return false;
            return string.Equals(record.ProcessorID, "Flax.GraphDocument", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.MaterialInstance", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.SkeletonMask", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.SceneAnimation", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.ParticleSystem", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.CollisionData", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.Audio", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.Font", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.ShaderSource", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.Video", StringComparison.Ordinal) ||
                   string.Equals(record.ProcessorID, "Flax.Text", StringComparison.Ordinal);
        }

        private static bool CanBuildCanonicalRecord(AssetDatabaseRecordInfo record)
        {
            return IsTextureRecord(record) || IsModelRecord(record) || IsDocumentOrImportedSourceRecord(record);
        }

        private void QueueAssetDiskChange(string path, bool renameOnly)
        {
            if (string.IsNullOrEmpty(path))
                return;
            path = ContentMutationPathUtils.Normalize(path);
            var sourcePath = GetCanonicalSourcePathForDiskEvent(path);
            lock (_assetDiskChangesLock)
            {
                _pendingAssetDiskChanges.Add(path);
                _pendingTextureBuildSources.Add(sourcePath);
                _lastAssetDiskChangeTime = DateTime.UtcNow;
                if (_sourceAssetRecords.TryGetValue(sourcePath, out var record) && CanBuildCanonicalRecord(record))
                {
                    _pendingTextureBuildIds.Add(record.ID);
                    _textureRecordsBeforeWatcherScan[record.ID] = record;
                    if (renameOnly)
                        _renameOnlyTextureIds.Add(record.ID);
                    else
                        _renameOnlyTextureIds.Remove(record.ID);
                }
            }
        }

        private void SchedulePendingTextureBuilds()
        {
            HashSet<Guid> ids;
            HashSet<Guid> renameOnlyIds;
            Dictionary<Guid, AssetDatabaseRecordInfo> previousRecords;
            lock (_assetDiskChangesLock)
            {
                ids = new HashSet<Guid>(_pendingTextureBuildIds);
                foreach (var sourcePath in _pendingTextureBuildSources)
                {
                    if (_sourceAssetRecords.TryGetValue(sourcePath, out var record) && CanBuildCanonicalRecord(record))
                        ids.Add(record.ID);
                }
                renameOnlyIds = new HashSet<Guid>(_renameOnlyTextureIds);
                previousRecords = new Dictionary<Guid, AssetDatabaseRecordInfo>(_textureRecordsBeforeWatcherScan);
                _pendingTextureBuildSources.Clear();
                _pendingTextureBuildIds.Clear();
                _renameOnlyTextureIds.Clear();
                _textureRecordsBeforeWatcherScan.Clear();
            }

            foreach (var id in ids)
            {
                if (!_assetRecordsById.TryGetValue(id, out var record) || !CanBuildCanonicalRecord(record) || record.Status != AssetRecordStatus.Ready)
                    continue;
                if (renameOnlyIds.Contains(id) && previousRecords.TryGetValue(id, out var previous) && previous.MetaSemanticHash == record.MetaSemanticHash)
                    continue;
                var failed = IsTextureRecord(record)
                    ? AssetDatabaseFacade.BuildTexture(id)
                    : IsModelRecord(record)
                        ? AssetDatabaseFacade.BuildModel(id)
                        : AssetDatabaseFacade.RebuildGraph(id);
                if (failed)
                    Editor.LogError($"Cannot queue canonical asset build after disk change: {record.SourcePath}");
            }
        }

        private bool TrySuppressSelfAuthoredDiskChange(string path, DateTime now)
        {
            if (!_selfAuthoredAssetDiskChanges.TryGetValue(path, out var write))
                return false;
            FileInfo fileInfo = null;
            try
            {
                if (File.Exists(path))
                    fileInfo = new FileInfo(path);
            }
            catch
            {
                // A locked or replaced file is an external change and must not be suppressed.
            }
            var exactSelfWrite = false;
            if (now <= write.ExpiresAtUtc && fileInfo != null && fileInfo.LastWriteTimeUtc == write.LastWriteTimeUtc && fileInfo.Length == write.Length)
            {
                try
                {
                    exactSelfWrite = string.Equals(ComputeAssetDiskHash(path), write.ContentHash, StringComparison.Ordinal);
                }
                catch
                {
                    // A locked or replaced file is an external change and must not be suppressed.
                }
            }
            if (exactSelfWrite)
            {
                ContentMutationDiagnostics.Log("watcher.self-save-suppressed", $"path='{path}'; generation={write.Generation}; length={fileInfo.Length}; writeTime={fileInfo.LastWriteTimeUtc:O}");
                return true;
            }
            ContentMutationDiagnostics.Log("watcher.self-save-mismatch", $"path='{path}'; generation={write.Generation}; exists={fileInfo != null}; expectedLength={write.Length}; actualLength={fileInfo?.Length}; expectedWriteTime={write.LastWriteTimeUtc:O}; actualWriteTime={fileInfo?.LastWriteTimeUtc:O}");
            _selfAuthoredAssetDiskChanges.Remove(path);
            return false;
        }

        internal void OnDirectoryEvent(MainContentFolderTreeNode node, FileSystemEventArgs e)
        {
            // Ensure to be ready for external events
            if (_isDuringFastSetup)
                return;

            ContentMutationDiagnostics.Log("watcher.event", $"change={e.ChangeType}; path='{e.FullPath}'; root='{node?.Path}'");

            Editor.Scene.QueueSceneDiskChange(e);

            // TODO: maybe we could make it faster! since we have a path so it would be easy to just create or delete given file. but remember about subdirectories

            // Switch type
            switch (e.ChangeType)
            {
            case WatcherChangeTypes.Created:
            case WatcherChangeTypes.Deleted:
            case WatcherChangeTypes.Renamed:
            {
                var path = StringUtils.NormalizePath(e.FullPath);
                var suppressed = false;
                lock (_assetDiskChangesLock)
                {
                    // A delete is never a self-authored write, so it must always reach the database.
                    suppressed = e.ChangeType != WatcherChangeTypes.Deleted &&
                                 (_assetSaves.ContainsKey(path) || TrySuppressSelfAuthoredDiskChange(path, DateTime.UtcNow));
                    if (!suppressed)
                        QueueAssetDiskChange(path, e.ChangeType == WatcherChangeTypes.Renamed);
                }
                if (suppressed)
                    break;
                if (e is RenamedEventArgs renamed)
                    QueueAssetDiskChange(renamed.OldFullPath, true);
                MarkSourceFolderDirty(path);
                if (e is RenamedEventArgs renamedOld)
                    MarkSourceFolderDirty(renamedOld.OldFullPath);
                break;
            }
            case WatcherChangeTypes.Changed:
            {
                var path = StringUtils.NormalizePath(e.FullPath);
                var now = DateTime.UtcNow;
                lock (_assetDiskChangesLock)
                {
                    // An in-flight save writes the document itself, so its own notifications are not external changes.
                    var saveInProgress = _assetSaves.TryGetValue(path, out var saveState);
                    if (saveInProgress)
                    {
                        ContentMutationDiagnostics.Log("watcher.change-suppressed", $"path='{path}'; generation={saveState.Generation}; depth={saveState.Depth}");
                        break;
                    }
                    if (TrySuppressSelfAuthoredDiskChange(path, now))
                        break;
                    QueueAssetDiskChange(path, false);
                    ContentMutationDiagnostics.Log("watcher.change-queued", $"path='{path}'; pending={_pendingAssetDiskChanges.Count}");
                }
                break;
            }
            }
        }

        internal void BeginAssetSave(string path)
        {
            path = StringUtils.NormalizePath(path);
            long generation;
            int depth;
            lock (_assetDiskChangesLock)
            {
                if (_assetSaves.TryGetValue(path, out var state))
                {
                    state.Depth++;
                }
                else
                {
                    state = new AssetSaveState(++_nextAssetSaveGeneration);
                    _assetSaves.Add(path, state);
                }
                generation = state.Generation;
                depth = state.Depth;
            }
            ContentMutationDiagnostics.Log("save.begin", $"path='{path}'; generation={generation}; depth={depth}");
        }

        internal AssetSaveScope TrackAssetSave(string path)
        {
            return new AssetSaveScope(this, path);
        }

        internal bool SaveAsset(Asset asset)
        {
            if (asset == null)
                throw new ArgumentNullException(nameof(asset));
            if (TryGetAssetDatabaseRecord(asset.ID, out var record) && record.SourceKind == AssetSourceKind.TextDocument)
            {
                if (asset is Material or MaterialInstance or SkeletonMask or SceneAnimation or ParticleSystem)
                {
                    using var canonicalScope = TrackAssetSave(record.SourcePath);
                    var canonicalFailed = asset is Material material
                        ? AssetDatabaseFacade.SaveMaterialDocument(material, asset.ID)
                        : AssetDatabaseFacade.SaveAuthoredDocument((BinaryAsset)asset, asset.ID);
                    canonicalScope.Complete(!canonicalFailed);
                    return canonicalFailed;
                }
            }
            if (!ConvertedTypePolicy.AllowsLegacyBinaryAuthoring(asset.GetType().FullName, asset.Path))
            {
                Editor.LogError("Legacy .flax saving is disabled for converted asset types. Migrate the asset before editing it.");
                return true;
            }
            using var scope = TrackAssetSave(asset.Path);
            var failed = asset.Save();
            scope.Complete(!failed);
            return failed;
        }

        internal bool SaveAsset(string path, Func<bool> save)
        {
            if (save == null)
                throw new ArgumentNullException(nameof(save));
            using var scope = TrackAssetSave(path);
            var failed = save();
            scope.Complete(!failed);
            return failed;
        }

        /// <summary>
        /// Saves a canonical authored document from an editor that works on a clone, whose asset identifier
        /// differs from the tracked item. Tracking the write keeps the watcher from reprocessing it as external.
        /// </summary>
        internal bool SaveCanonicalAuthoredDocument(BinaryAsset asset, AssetItem item)
        {
            if (asset == null)
                throw new ArgumentNullException(nameof(asset));
            if (item == null)
                throw new ArgumentNullException(nameof(item));
            using var scope = TrackAssetSave(item.Path);
            var failed = AssetDatabaseFacade.SaveAuthoredDocument(asset, item.ID);
            scope.Complete(!failed);
            return failed;
        }

        internal bool IsAssetSaveInProgress(string path)
        {
            path = StringUtils.NormalizePath(path);
            lock (_assetDiskChangesLock)
                return _assetSaves.ContainsKey(path);
        }

        internal void EndAssetSave(string path, bool succeeded)
        {
            path = StringUtils.NormalizePath(path);
            long generation;
            int depth;
            bool finalSucceeded;
            lock (_assetDiskChangesLock)
            {
                if (!_assetSaves.TryGetValue(path, out var state))
                {
                    ContentMutationDiagnostics.Log("save.end-unmatched", $"path='{path}'; succeeded={succeeded}");
                    return;
                }

                state.Failed |= !succeeded;
                state.Depth--;
                generation = state.Generation;
                depth = state.Depth;
                finalSucceeded = depth == 0 && !state.Failed;
                if (depth == 0)
                    _assetSaves.Remove(path);
            }
            ContentMutationDiagnostics.Log("save.end", $"path='{path}'; generation={generation}; depth={depth}; scopeSucceeded={succeeded}; finalSucceeded={finalSucceeded}");
            if (depth != 0 || !finalSucceeded)
                return;

            try
            {
                var fileInfo = new FileInfo(path);
                var now = DateTime.UtcNow;
                var write = new AssetDiskWrite(fileInfo.LastWriteTimeUtc, fileInfo.Length, now.AddSeconds(SelfAuthoredAssetDiskChangeLifetimeSeconds), generation, ComputeAssetDiskHash(path));
                lock (_assetDiskChangesLock)
                {
                    // A watcher event can race the save completion. Remove anything queued during the write,
                    // then ignore trailing notifications only while the file still matches this exact save.
                    _pendingAssetDiskChanges.Remove(path);
                    var sourcePath = GetCanonicalSourcePathForDiskEvent(path);
                    _pendingTextureBuildSources.Remove(sourcePath);
                    if (_sourceAssetRecords.TryGetValue(sourcePath, out var record))
                    {
                        _pendingTextureBuildIds.Remove(record.ID);
                        _renameOnlyTextureIds.Remove(record.ID);
                        _textureRecordsBeforeWatcherScan.Remove(record.ID);
                    }
                    _selfAuthoredAssetDiskChanges[path] = write;
                }
                ContentMutationDiagnostics.Log("save.self-write-recorded", $"path='{path}'; generation={generation}; length={fileInfo.Length}; writeTime={fileInfo.LastWriteTimeUtc:O}; expires={write.ExpiresAtUtc:O}");
            }
            catch
            {
                // A failed or immediately replaced file will flow through the normal external-change path.
                ContentMutationDiagnostics.Log("save.self-write-unavailable", $"path='{path}'; generation={generation}");
            }
        }

        private void ProcessPendingAssetDiskChanges()
        {
            if (_assetDatabaseAutoRefreshDepth != 0)
                return;

            // Importing assets produces its own file watcher notifications and refreshes
            // the affected content items through OnImportFileDone.
            if (Editor.ContentImporting.IsImporting)
                return;

            var now = DateTime.UtcNow;
            var readyPaths = new List<string>();
            lock (_assetDiskChangesLock)
            {
                if (_selfAuthoredAssetDiskChanges.Count != 0)
                {
                    var expiredPaths = _selfAuthoredAssetDiskChanges.Where(x => now > x.Value.ExpiresAtUtc).Select(x => x.Key).ToArray();
                    for (int i = 0; i < expiredPaths.Length; i++)
                        _selfAuthoredAssetDiskChanges.Remove(expiredPaths[i]);
                }
                // Wait until filesystem notifications stop, then process the whole deduplicated burst.
                if (_pendingAssetDiskChanges.Count == 0 || (now - _lastAssetDiskChangeTime).TotalSeconds < AssetDiskChangeQuietPeriodSeconds)
                    return;

                readyPaths.AddRange(_pendingAssetDiskChanges);
                _pendingAssetDiskChanges.Clear();
            }

            if (_useNewAssetDatabase)
                QueueCanonicalSourceRefresh(readyPaths.ToArray());

            bool workspaceModified = false;
            for (int i = 0; i < readyPaths.Count; i++)
            {
                var item = Find(readyPaths[i]) as AssetItem;
                if (item == null || item.ItemType == ContentItemType.Scene)
                    continue;

                var asset = FlaxEngine.Content.GetAsset(item.ID);
                if (asset == null || (!asset.IsLoaded && !asset.LastLoadFailed))
                    continue;

                Editor.Log("Reloading asset changed on disk: " + item.Path);
                item.Reload();
                item.NotifyReloaded();
                workspaceModified = true;
            }
            if (workspaceModified && _enableEvents)
                WorkspaceModified?.Invoke();
        }

        private void OnScriptsReload()
        {
            var enabledEvents = _enableEvents;
            _enableEvents = false;
            _isDuringFastSetup = true;
            var startItems = _itemsCreated;
            foreach (var project in Projects)
            {
                if (project.Content != null)
                {
                    //Dispose(project.Content.Folder);
                    for (int i = 0; i < project.Content.Folder.Children.Count; i++)
                    {
                        Dispose(project.Content.Folder.Children[i]);
                        i--;
                    }
                }
                if (project.Source != null)
                {
                    //Dispose(project.Source.Folder);
                    for (int i = 0; i < project.Source.Folder.Children.Count; i++)
                    {
                        Dispose(project.Source.Folder.Children[i]);
                        i--;
                    }
                }
            }

            List<ContentProxy> removeProxies = new List<ContentProxy>();
            foreach (var proxy in Editor.Instance.ContentDatabase.Proxy)
            {
                if (proxy.GetType().IsCollectible)
                    removeProxies.Add(proxy);
            }
            foreach (var proxy in removeProxies)
                RemoveProxy(proxy, false);

            _isDuringFastSetup = false;
            _enableEvents = enabledEvents;
        }

        private void OnScriptsReloadEnd()
        {
            RebuildInternal();
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            ProcessPendingAssetDiskChanges();

            if (_assetDatabaseAutoRefreshDepth == 0 && _pendingMissingMetadataRegistrations.Count != 0 && !Editor.ContentImporting.IsImporting)
            {
                var sourcePaths = _pendingMissingMetadataRegistrations.ToArray();
                _pendingMissingMetadataRegistrations.Clear();
                Editor.ContentImporting.RegisterInPlaceCanonicalSources(sourcePaths);
            }

            // Only drain once the refresh can actually run, otherwise queued paths are lost for good.
            if (_assetDatabaseAutoRefreshDepth == 0 && !Editor.ContentImporting.IsImporting)
            {
                string[] refreshPaths;
                lock (_assetDiskChangesLock)
                {
                    refreshPaths = _pendingSourceRefresh.Count == 0 ? Array.Empty<string>() : _pendingSourceRefresh.ToArray();
                    _pendingSourceRefresh.Clear();
                }
                if (refreshPaths.Length != 0)
                {
                    if (AssetDatabaseFacade.RefreshSources(refreshPaths))
                        Editor.LogError("Canonical asset database refresh failed. See asset pipeline diagnostics.");
                    else
                        SchedulePendingTextureBuilds();
                    QueueMissingMetadataRegistrations();
                }
            }

            // Update all dirty content tree nodes
            lock (_dirtyNodes)
            {
                foreach (var node in _dirtyNodes)
                {
                    LoadFolder(node, true);

                    if (_enableEvents)
                        WorkspaceModified?.Invoke();
                }
                _dirtyNodes.Clear();
            }

            // Lazy-rebuilds
            if (_rebuildFlag)
            {
                RebuildInternal();
            }
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            FlaxEngine.Content.AssetDisposing -= OnContentAssetDisposing;
            if (_useNewAssetDatabase)
            {
                AssetDatabaseFacade.DatabaseChanged -= OnAssetDatabaseChanged;
                AssetDatabaseFacade.ArtifactPublished -= OnArtifactPublished;
            }
            ScriptsBuilder.ScriptsReload -= OnScriptsReload;
            ScriptsBuilder.ScriptsReloadEnd -= OnScriptsReloadEnd;

            // Disable events
            _enableEvents = false;
            lock (_assetDiskChangesLock)
            {
                _pendingAssetDiskChanges.Clear();
                _pendingTextureBuildSources.Clear();
                _pendingTextureBuildIds.Clear();
                _renameOnlyTextureIds.Clear();
                _textureRecordsBeforeWatcherScan.Clear();
                _assetSaves.Clear();
                _selfAuthoredAssetDiskChanges.Clear();
            }
            _sourceAssetRecords.Clear();
            _assetRecordsById.Clear();

            // Cleanup
            Proxy.ForEach(x => x.Dispose());
            if (Game != null)
            {
                Game.Dispose();
                Game = null;
            }
            if (Engine != null)
            {
                Engine.Dispose();
                Engine = null;
            }
            Proxy.Clear();
        }
    }
}
