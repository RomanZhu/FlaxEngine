// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;
using Newtonsoft.Json;

namespace FlaxEditor.Content.Import
{
    /// <summary>Generic importer settings inspector backed by the canonical asset operation service.</summary>
    public abstract class AssetImporterEditor
    {
        private readonly Type _settingsType;
        private AssetImporterSettingsSnapshot _snapshot;

        public AssetGuid Asset { get; }
        public object Settings { get; private set; }
        public AssetImporterSettingsSaveResult LastSaveResult { get; private set; }
        protected virtual int SettingsVersion => 1;

        protected AssetImporterEditor(AssetGuid asset, Type settingsType)
        {
            if (!asset.IsValid)
                throw new ArgumentException("Importer editor asset identity is invalid.", nameof(asset));
            Asset = asset;
            _settingsType = settingsType ?? throw new ArgumentNullException(nameof(settingsType));
            Settings = ReadSettings();
        }

        protected AssetImporterEditor(AssetGuid asset, object settings)
        {
            if (!asset.IsValid)
                throw new ArgumentException("Importer editor asset identity is invalid.", nameof(asset));
            Asset = asset;
            _settingsType = settings?.GetType() ?? throw new ArgumentNullException(nameof(settings));
            Settings = ReadSettings();
        }

        public abstract void OnInspectorGUI();

        protected virtual string ValidateSettings()
        {
            return Settings == null ? "Importer settings are missing." : null;
        }

        public void Apply()
        {
            var validation = ValidateSettings();
            if (!string.IsNullOrEmpty(validation))
                throw new InvalidOperationException(validation);
            if (SettingsVersion != _snapshot.SettingsSchemaVersion)
                throw new InvalidOperationException("Importer settings schema changed. Revert before applying changes.");
            var settingsJson = JsonConvert.SerializeObject(Settings, Formatting.None);
            LastSaveResult = AssetOperationService.SaveImporterSettingsAndReimportDetailed(_snapshot, settingsJson);
            if (LastSaveResult.WriteOutcome != AssetImporterSettingsWriteOutcome.Committed &&
                LastSaveResult.WriteOutcome != AssetImporterSettingsWriteOutcome.Unchanged)
            {
                var fallback = LastSaveResult.WriteOutcome == AssetImporterSettingsWriteOutcome.Conflict
                    ? "Importer settings changed externally. Revert before applying changes."
                    : "Failed to save importer settings.";
                throw CreateOperationException(fallback, LastSaveResult.Diagnostic);
            }
            if (LastSaveResult.Current.SourceAssetID != Guid.Empty)
                _snapshot = LastSaveResult.Current;
            if (LastSaveResult.ReimportOutcome == AssetImporterSettingsReimportOutcome.Failed ||
                LastSaveResult.ReimportOutcome == AssetImporterSettingsReimportOutcome.Blocked)
            {
                throw CreateOperationException("Importer settings were saved, but reimport did not start.",
                    LastSaveResult.Diagnostic);
            }
        }

        public void Revert()
        {
            Settings = ReadSettings();
        }

        private object ReadSettings()
        {
            _snapshot = ReadSnapshot();
            return JsonConvert.DeserializeObject(_snapshot.SettingsJson, _settingsType);
        }

        private AssetImporterSettingsSnapshot ReadSnapshot()
        {
            if (AssetOperationService.GetImporterSettings(Asset.Value, out var snapshot))
                throw CreateOperationException("Failed to load importer settings.");
            return snapshot;
        }

        private static InvalidOperationException CreateOperationException(string fallback)
        {
            var diagnostics = AssetDatabaseQueryService.GetDiagnostics();
            for (int i = 0; i < diagnostics.Length; i++)
            {
                if (!string.IsNullOrEmpty(diagnostics[i].Message))
                    return new InvalidOperationException(diagnostics[i].Message);
            }
            return new InvalidOperationException(fallback);
        }

        private static InvalidOperationException CreateOperationException(string fallback, AssetPipelineDiagnostic diagnostic)
        {
            return new InvalidOperationException(string.IsNullOrEmpty(diagnostic.Message) ? fallback : diagnostic.Message);
        }
    }
}
