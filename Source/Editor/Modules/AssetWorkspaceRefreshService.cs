// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using FlaxEngine;

namespace FlaxEditor.Modules
{
    /// <summary>Owns workspace filesystem hints and forwards them to database reconciliation.</summary>
    internal sealed class AssetWorkspaceRefreshService : IDisposable
    {
        private readonly AssetWorkspaceModule _workspace;
        private readonly Dictionary<string, FileSystemWatcher> _watchers = new Dictionary<string, FileSystemWatcher>(StringComparer.OrdinalIgnoreCase);

        public AssetWorkspaceRefreshService(AssetWorkspaceModule workspace)
        {
            _workspace = workspace;
        }

        public void Watch(string root)
        {
            root = StringUtils.NormalizePath(root);
            if (string.IsNullOrEmpty(root) || !Directory.Exists(root) || _watchers.ContainsKey(root))
                return;
            var watcher = new FileSystemWatcher(root)
            {
                IncludeSubdirectories = true,
                InternalBufferSize = 64 * 1024,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size,
            };
            watcher.Changed += OnEvent;
            watcher.Created += OnEvent;
            watcher.Deleted += OnEvent;
            watcher.Renamed += OnEvent;
            watcher.Error += OnError;
            watcher.EnableRaisingEvents = true;
            _watchers.Add(root, watcher);
        }

        private void OnEvent(object sender, FileSystemEventArgs e)
        {
            _workspace.OnDirectoryEvent(((FileSystemWatcher)sender).Path, e);
        }

        private void OnError(object sender, ErrorEventArgs e)
        {
            _workspace.OnDirectoryWatcherError(((FileSystemWatcher)sender).Path, e);
        }

        public void Dispose()
        {
            foreach (var watcher in _watchers.Values)
            {
                watcher.EnableRaisingEvents = false;
                watcher.Changed -= OnEvent;
                watcher.Created -= OnEvent;
                watcher.Deleted -= OnEvent;
                watcher.Renamed -= OnEvent;
                watcher.Error -= OnError;
                watcher.Dispose();
            }
            _watchers.Clear();
        }
    }
}
