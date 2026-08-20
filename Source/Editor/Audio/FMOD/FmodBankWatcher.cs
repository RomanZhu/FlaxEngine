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
            if ((DateTime.UtcNow - _lastChange).TotalMilliseconds < 250)
                return;
            _lastChange = DateTime.UtcNow;
            Changed?.Invoke();
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
