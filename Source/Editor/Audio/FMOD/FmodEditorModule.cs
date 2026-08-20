// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Modules;
using FlaxEngine;

namespace FlaxEditor.FMOD
{
    /// <summary>
    /// FMOD authoring integration. Runtime initialization remains owned by the engine audio backend.
    /// </summary>
    public sealed class FmodEditorModule : EditorModule
    {
        private FmodBankWatcher _watcher;

        public event Action BanksChanged;

        public FmodEditorModule(Editor editor)
        : base(editor)
        {
            InitOrder = 100;
        }

        public override void OnInit()
        {
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            _watcher = new FmodBankWatcher(banks);
            if (_watcher != null)
                _watcher.Changed += OnBanksChanged;
        }

        private void OnBanksChanged()
        {
            Editor.Log("FMOD banks changed; metadata and preview instances can be reloaded.");
            BanksChanged?.Invoke();
        }

        public override void OnExit()
        {
            if (_watcher != null)
            {
                _watcher.Changed -= OnBanksChanged;
                _watcher.Dispose();
                _watcher = null;
            }
        }

        public bool SynchronizeMetadata()
        {
            var banks = Path.Combine(Globals.ProjectContentFolder, "Audio", "Banks");
            if (FmodMetadataImporter.TryRead(banks, out _, out var error))
            {
                Editor.Log("FMOD metadata synchronized from sidecar.");
                return true;
            }
            if (!string.IsNullOrEmpty(error))
                Editor.LogError("FMOD metadata validation failed: " + error);
            else
                Editor.LogWarning("FMOD metadata sidecar is missing; using metadata-only fallback.");
            return string.IsNullOrEmpty(error);
        }
    }
}
