// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// Debounced watcher for built FMOD banks and metadata sidecars.
    /// </summary>
    public sealed class FmodBankWatcher : IDisposable
    {
        private readonly FileSystemWatcher _watcher;
        private DateTime _lastChange;
        private bool _pending;

        public event Action Changed;

        public FmodBankWatcher(string directory)
        {
            if (!Directory.Exists(directory))
                return;
            _watcher = new FileSystemWatcher(directory)
            {
                IncludeSubdirectories = true,
                Filter = "*.*",
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite | NotifyFilters.Size,
            };
            _watcher.Changed += OnChanged;
            _watcher.Created += OnChanged;
            _watcher.Deleted += OnChanged;
            _watcher.Renamed += OnChanged;
            _watcher.EnableRaisingEvents = true;
        }

        private void OnChanged(object sender, FileSystemEventArgs args)
        {
            if (!args.Name.EndsWith(".bank", StringComparison.OrdinalIgnoreCase) &&
                !args.Name.EndsWith("metadata.json", StringComparison.OrdinalIgnoreCase))
                return;
            _lastChange = DateTime.UtcNow;
            _pending = true;
        }

        /// <summary>
        /// Dispatches a change after the filesystem has been quiet for the debounce interval.
        /// Call from the editor update thread.
        /// </summary>
        public void Update()
        {
            if (_pending && (DateTime.UtcNow - _lastChange).TotalMilliseconds >= 250)
            {
                _pending = false;
                Changed?.Invoke();
            }
        }

        public void Dispose()
        {
            if (_watcher == null)
                return;
            _watcher.EnableRaisingEvents = false;
            _watcher.Dispose();
        }
    }
}
