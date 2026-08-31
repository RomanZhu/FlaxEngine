// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Content tree node used for main directories.
    /// </summary>
    /// <seealso cref="ContentFolderTreeNode" />
    public class MainContentFolderTreeNode : ContentFolderTreeNode
    {
        private const int WatcherBufferSize = 64 * 1024;
        private FileSystemWatcher _watcher;
        private DateTime _watchRootCreationTimeUtc;
        private bool _watchRootExists;
        private volatile bool _watcherRestartRequested;
        private bool _isDestroyed;

        /// <inheritdoc />
        public override bool CanDelete => false;

        /// <inheritdoc />
        public override bool CanDuplicate => false;

        /// <summary>
        /// Initializes a new instance of the <see cref="MainContentFolderTreeNode"/> class.
        /// </summary>
        /// <param name="parent">The parent project.</param>
        /// <param name="type">The folder type.</param>
        /// <param name="path">The folder path.</param>
        public MainContentFolderTreeNode(ProjectFolderTreeNode parent, ContentFolderType type, string path)
        : base(parent, type, path)
        {
            RestartWatcher();
        }

        private void RestartWatcher()
        {
            if (_watcher != null)
            {
                _watcher.EnableRaisingEvents = false;
                _watcher.Changed -= OnEvent;
                _watcher.Created -= OnEvent;
                _watcher.Deleted -= OnEvent;
                _watcher.Renamed -= OnEvent;
                _watcher.Error -= OnError;
                _watcher.Dispose();
                _watcher = null;
            }

            _watchRootExists = Directory.Exists(Path);
            _watchRootCreationTimeUtc = _watchRootExists ? Directory.GetCreationTimeUtc(Path) : DateTime.MinValue;
            _watcherRestartRequested = false;
            if (!_watchRootExists || _isDestroyed)
                return;

            _watcher = new FileSystemWatcher(Path)
            {
                IncludeSubdirectories = true,
                InternalBufferSize = WatcherBufferSize,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size,
            };
            _watcher.Changed += OnEvent;
            _watcher.Created += OnEvent;
            _watcher.Deleted += OnEvent;
            _watcher.Renamed += OnEvent;
            _watcher.Error += OnError;
            _watcher.EnableRaisingEvents = true;
        }

        private void OnEvent(object sender, FileSystemEventArgs e)
        {
            Editor.Instance?.ContentDatabase?.OnDirectoryEvent(this, e);
        }

        private void OnError(object sender, ErrorEventArgs e)
        {
            _watcherRestartRequested = true;
            Editor.Instance?.ContentDatabase?.OnDirectoryWatcherError(this, e.GetException());
        }

        /// <summary>
        /// Checks whether the watched root was removed, replaced, or the watcher requested a restart.
        /// Must be called from the editor thread.
        /// </summary>
        internal string ValidateDirectoryWatcher()
        {
            if (_isDestroyed)
                return null;

            var exists = Directory.Exists(Path);
            var creationTimeUtc = exists ? Directory.GetCreationTimeUtc(Path) : DateTime.MinValue;
            if (!_watcherRestartRequested && exists == _watchRootExists && (!exists || creationTimeUtc == _watchRootCreationTimeUtc))
                return null;

            var reason = _watcherRestartRequested
                ? "watcher error or overflow"
                : !_watchRootExists && exists
                    ? "watched root restored"
                    : _watchRootExists && !exists
                        ? "watched root removed"
                        : "watched root replaced";
            RestartWatcher();
            return reason;
        }

        /// <inheritdoc />
        protected override void DoDragDrop()
        {
            // No drag for root nodes
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _isDestroyed = true;
            if (_watcher != null)
            {
                _watcher.EnableRaisingEvents = false;
                _watcher.Dispose();
                _watcher = null;
            }

            base.OnDestroy();
        }
    }
}
