// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using System.Text;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor.Content.Import
{
    /// <summary>Generic importer settings inspector backed by the adjacent tracked metadata file.</summary>
    public abstract class AssetImporterEditor
    {
        private readonly Type _settingsType;

        public AssetGuid Asset { get; }
        public object Settings { get; private set; }
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
            Settings = settings ?? throw new ArgumentNullException(nameof(settings));
            _settingsType = settings.GetType();
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
            var sourcePath = GetSourcePath();
            var metaPath = sourcePath + ".meta";
            var root = JObject.Parse(File.ReadAllText(metaPath, Encoding.UTF8));
            if (!(root["importer"] is JObject importer))
                throw new InvalidDataException($"Asset metadata '{metaPath}' has no importer object.");
            importer["version"] = SettingsVersion;
            importer["settings"] = Settings == null ? new JObject() : JToken.FromObject(Settings, JsonSerializer.CreateDefault());
            var staging = metaPath + ".apply-" + Guid.NewGuid().ToString("N");
            AssetDatabase.StartAssetEditing();
            try
            {
                File.WriteAllText(staging, root.ToString(Formatting.Indented) + Environment.NewLine, new UTF8Encoding(false));
                File.Move(staging, metaPath, true);
                AssetDatabase.ImportAsset(sourcePath, ImportAssetOptions.ForceUpdate);
            }
            finally
            {
                if (File.Exists(staging))
                    File.Delete(staging);
                AssetDatabase.StopAssetEditing();
            }
        }

        public void Revert()
        {
            Settings = ReadSettings();
        }

        private object ReadSettings()
        {
            var metaPath = GetSourcePath() + ".meta";
            var root = JObject.Parse(File.ReadAllText(metaPath, Encoding.UTF8));
            var settings = root["importer"]?["settings"];
            if (settings == null)
                throw new InvalidDataException($"Asset metadata '{metaPath}' has no importer settings.");
            return settings.ToObject(_settingsType, JsonSerializer.CreateDefault());
        }

        private string GetSourcePath()
        {
            var path = AssetDatabaseQueryService.GetCanonicalSourcePath(Asset.Value);
            if (string.IsNullOrEmpty(path))
                throw new FileNotFoundException("The importer editor asset is not registered in the source database.", Asset.ToString());
            return path;
        }
    }
}
