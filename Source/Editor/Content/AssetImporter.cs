// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Stable external-object slot declared by an importer.</summary>
    [Serializable]
    public struct SourceAssetIdentifier : IEquatable<SourceAssetIdentifier>
    {
        /// <summary>Expected object type name.</summary>
        public string TypeName;
        /// <summary>Importer-defined stable slot name.</summary>
        public string Name;

        /// <summary>Creates an external-object identifier.</summary>
        public SourceAssetIdentifier(Type type, string name)
        {
            TypeName = type?.FullName ?? throw new ArgumentNullException(nameof(type));
            Name = string.IsNullOrWhiteSpace(name) ? throw new ArgumentException("A stable slot name is required.", nameof(name)) : name;
        }

        /// <inheritdoc />
        public bool Equals(SourceAssetIdentifier other) => string.Equals(TypeName, other.TypeName, StringComparison.Ordinal) && string.Equals(Name, other.Name, StringComparison.Ordinal);

        /// <inheritdoc />
        public override bool Equals(object obj) => obj is SourceAssetIdentifier other && Equals(other);

        /// <inheritdoc />
        public override int GetHashCode() => unchecked(((TypeName?.GetHashCode() ?? 0) * 397) ^ (Name?.GetHashCode() ?? 0));
    }

    /// <summary>Structured diagnostic history for one imported source.</summary>
    public sealed class ImportLog
    {
        internal ImportLog(string assetPath, AssetPipelineDiagnostic[] diagnostics)
        {
            AssetPath = assetPath;
            Diagnostics = diagnostics ?? Array.Empty<AssetPipelineDiagnostic>();
        }

        /// <summary>Canonical source path.</summary>
        public string AssetPath { get; }
        /// <summary>Current structured diagnostics.</summary>
        public IReadOnlyList<AssetPipelineDiagnostic> Diagnostics { get; }
        /// <summary>Returns true when the source currently has a reported error.</summary>
        public bool HasErrors => Diagnostics.Count != 0;
    }

    /// <summary>Editable importer-settings proxy bound to a source GUID and metadata revision.</summary>
    public abstract class AssetImporter : FlaxEngine.Object
    {
        private string _assetPath;
        private Guid _sourceGuid;
        private ulong _databaseRevision;
        private bool _importSettingsMissing;
        private string _userData;
        private string _assetBundleName;
        private string _assetBundleVariant;
        private string _importerId;
        private int _settingsSchemaVersion;
        private string _settingsJson;
        private readonly Dictionary<SourceAssetIdentifier, AssetObjectId> _externalObjects = new Dictionary<SourceAssetIdentifier, AssetObjectId>();
        private bool _dirty;

        /// <summary>Canonical source path. A moved source is rebound by GUID on access.</summary>
        public string assetPath
        {
            get
            {
                Rebind();
                return _assetPath;
            }
        }

        /// <summary>True when settings were synthesized but never committed.</summary>
        public bool importSettingsMissing => _importSettingsMissing;

        /// <summary>Opaque editor metadata. It is not an import input unless the importer declares it.</summary>
        public string userData
        {
            get => _userData;
            set
            {
                value = value ?? string.Empty;
                if (_userData == value)
                    return;
                _userData = value;
                _dirty = true;
            }
        }

        /// <summary>Optional build-collection assignment.</summary>
        public string assetBundleName
        {
            get => _assetBundleName;
            set
            {
                value = value ?? string.Empty;
                if (_assetBundleName == value)
                    return;
                _assetBundleName = value;
                _dirty = true;
            }
        }

        /// <summary>Optional build-collection variant.</summary>
        public string assetBundleVariant
        {
            get => _assetBundleVariant;
            set
            {
                value = value ?? string.Empty;
                if (_assetBundleVariant == value)
                    return;
                _assetBundleVariant = value;
                _dirty = true;
            }
        }

        /// <summary>Gets the importer proxy for a canonical source path.</summary>
        public static AssetImporter GetAtPath(string path)
        {
            var record = AssetDatabase.GetMainRecord(path);
            if (!record.HasValue || record.Value.SourceKind == AssetSourceKind.Folder)
                return null;
            var importer = (AssetImporter)ScriptedImporterRegistry.CreateSelected(record.Value.ProcessorID) ?? new BuiltInAssetImporter();
            importer.Bind(record.Value);
            return importer;
        }

        /// <summary>Gets the current structured import diagnostics for a source.</summary>
        public static ImportLog GetImportLog(string path)
        {
            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(path);
            var diagnostics = AssetDatabaseFacade.GetDiagnostics()
                .Where(x => string.Equals(Path.GetFullPath(x.SourcePath ?? string.Empty), physicalPath, StringComparison.OrdinalIgnoreCase))
                .ToArray();
            return new ImportLog(AssetDatabase.ToLogicalPathInternal(physicalPath), diagnostics);
        }

        /// <summary>Returns the current external-object remaps.</summary>
        public IReadOnlyDictionary<SourceAssetIdentifier, FlaxEngine.Object> GetExternalObjectMap()
        {
            var result = new Dictionary<SourceAssetIdentifier, FlaxEngine.Object>();
            foreach (var entry in _externalObjects)
            {
                var expectedType = TypeUtils.GetType(entry.Key.TypeName).Type ?? typeof(FlaxEngine.Object);
                result.Add(entry.Key, AssetDatabase.LoadAsset(entry.Value, expectedType));
            }
            return result;
        }

        /// <summary>Adds or replaces an external-object remap.</summary>
        public void AddRemap(SourceAssetIdentifier id, FlaxEngine.Object externalObject)
        {
            if (externalObject == null)
                throw new ArgumentNullException(nameof(externalObject));
            if (!SupportsRemappedAssetType(externalObject.GetType()))
                throw new InvalidOperationException($"Importer '{GetType().FullName}' does not support remaps to '{externalObject.GetType().FullName}'.");
            if (!AssetDatabase.TryGetAssetObjectId(externalObject, out var objectId))
                throw new ArgumentException("External remaps require a persistent asset object.", nameof(externalObject));
            _externalObjects[id] = objectId;
            _dirty = true;
        }

        /// <summary>Removes an external-object remap.</summary>
        public bool RemoveRemap(SourceAssetIdentifier id)
        {
            if (!_externalObjects.Remove(id))
                return false;
            _dirty = true;
            return true;
        }

        /// <summary>Returns whether the importer supports a remapped object type.</summary>
        public virtual bool SupportsRemappedAssetType(Type type) => false;

        /// <summary>Assigns optional build-collection metadata without making it an import input.</summary>
        public void SetAssetBundleNameAndVariant(string name, string variant)
        {
            assetBundleName = name;
            assetBundleVariant = variant;
        }

        /// <summary>Writes dirty settings, then synchronously imports this source.</summary>
        public void SaveAndReimport()
        {
            WriteImportSettingsIfDirty();
            AssetDatabase.ImportAsset(assetPath, ImportAssetOptions.ForceSynchronousImport | ImportAssetOptions.ForceUpdate);
        }

        /// <summary>Writes dirty importer settings without importing.</summary>
        public bool WriteImportSettingsIfDirty()
        {
            if (CaptureScriptedSettings())
                _dirty = true;
            Rebind();
            if (!_dirty)
                return false;
            var externalObjectsJson = SerializeExternalObjects();
            if (AssetDatabaseFacade.ApplyImporterMetadata(_sourceGuid, _databaseRevision, _importerId,
                    _settingsSchemaVersion, _settingsJson, externalObjectsJson, _userData,
                    _assetBundleName, _assetBundleVariant))
                throw new InvalidOperationException("Importer settings update failed or conflicted with a newer metadata revision.");
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.IsMain && x.SourceAssetID == _sourceGuid);
            if (record.SourceAssetID == Guid.Empty)
                throw new InvalidOperationException("The imported source disappeared while applying settings.");
            Bind(record);
            return true;
        }

        /// <summary>Marks serialized importer state dirty.</summary>
        protected void MarkDirty()
        {
            _dirty = true;
        }

        internal void Bind(AssetDatabaseRecordInfo record)
        {
            var metadata = AssetDatabaseFacade.GetImporterMetadata(record.SourceAssetID);
            if (metadata.SourceAssetID == Guid.Empty)
                throw new InvalidDataException($"Cannot read importer metadata for '{record.CanonicalPath}'.");
            _assetPath = record.CanonicalPath;
            _sourceGuid = record.SourceAssetID;
            _databaseRevision = metadata.Revision;
            _importSettingsMissing = record.Status == AssetRecordStatus.MissingMeta;
            _importerId = metadata.ImporterID;
            _settingsSchemaVersion = metadata.SettingsSchemaVersion;
            _settingsJson = metadata.SettingsJson;
            _userData = metadata.UserData ?? string.Empty;
            _assetBundleName = metadata.AssetBundleName ?? string.Empty;
            _assetBundleVariant = metadata.AssetBundleVariant ?? string.Empty;
            LoadExternalObjects(metadata.ExternalObjectsJson);
            var settingsUpgraded = this is ScriptedImporter scriptedImporter &&
                                   scriptedImporter.UpgradeSettingsToCurrent(ref _settingsSchemaVersion, ref _settingsJson);
            LoadScriptedSettings();
            _dirty = settingsUpgraded;
        }

        private void Rebind()
        {
            if (_sourceGuid == Guid.Empty || _databaseRevision == AssetDatabaseFacade.Revision)
                return;
            var record = AssetDatabaseFacade.GetRecords().FirstOrDefault(x => x.IsMain && x.SourceAssetID == _sourceGuid);
            if (record.SourceAssetID == Guid.Empty)
                throw new InvalidOperationException("The source bound to this importer proxy no longer exists.");
            if (_dirty)
            {
                if (record.Revision != _databaseRevision)
                    throw new InvalidOperationException("Importer settings changed externally; reload the importer proxy before saving.");
                _assetPath = record.CanonicalPath;
                return;
            }
            Bind(record);
        }

        private void LoadExternalObjects(string json)
        {
            _externalObjects.Clear();
            foreach (var item in JArray.Parse(string.IsNullOrWhiteSpace(json) ? "[]" : json).OfType<JObject>())
            {
                var typeName = (string)item["type"];
                var name = (string)item["name"];
                if (string.IsNullOrEmpty(typeName) || string.IsNullOrEmpty(name) ||
                    !Guid.TryParseExact((string)item["guid"], "N", out var guid) ||
                    !long.TryParse((string)item["localId"], out var localId) || localId == 0)
                    throw new InvalidDataException("Importer externalObjects contains an invalid remap.");
                var identifier = new SourceAssetIdentifier { TypeName = typeName, Name = name };
                if (_externalObjects.ContainsKey(identifier))
                    throw new InvalidDataException("Importer externalObjects contains a duplicate remap slot.");
                _externalObjects.Add(identifier, new AssetObjectId { Guid = guid, LocalId = localId });
            }
        }

        private string SerializeExternalObjects()
        {
            var result = new JArray();
            foreach (var entry in _externalObjects.OrderBy(x => x.Key.TypeName, StringComparer.Ordinal).ThenBy(x => x.Key.Name, StringComparer.Ordinal))
            {
                result.Add(new JObject
                {
                    ["type"] = entry.Key.TypeName,
                    ["name"] = entry.Key.Name,
                    ["guid"] = entry.Value.Guid.ToString("N"),
                    ["localId"] = entry.Value.LocalId,
                });
            }
            return result.ToString(Newtonsoft.Json.Formatting.None);
        }

        private IEnumerable<MemberInfo> GetScriptedSettingsMembers()
        {
            for (var type = GetType(); type != null && type != typeof(ScriptedImporter) && type != typeof(AssetImporter); type = type.BaseType)
            {
                foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.DeclaredOnly))
                {
                    if (!field.IsInitOnly && !field.IsStatic)
                        yield return field;
                }
                foreach (var property in type.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.DeclaredOnly))
                {
                    if (property.CanRead && property.CanWrite && property.GetIndexParameters().Length == 0)
                        yield return property;
                }
            }
        }

        private void LoadScriptedSettings()
        {
            if (!(this is ScriptedImporter))
                return;
            var settings = JObject.Parse(string.IsNullOrWhiteSpace(_settingsJson) ? "{}" : _settingsJson);
            foreach (var member in GetScriptedSettingsMembers())
            {
                var token = settings[member.Name];
                if (token == null)
                    continue;
                if (member is FieldInfo field)
                    field.SetValue(this, token.ToObject(field.FieldType));
                else if (member is PropertyInfo property)
                    property.SetValue(this, token.ToObject(property.PropertyType));
            }
        }

        private bool CaptureScriptedSettings()
        {
            if (!(this is ScriptedImporter))
                return false;
            var settings = JObject.Parse(string.IsNullOrWhiteSpace(_settingsJson) ? "{}" : _settingsJson);
            var previous = settings.DeepClone();
            foreach (var member in GetScriptedSettingsMembers().OrderBy(x => x.Name, StringComparer.Ordinal))
            {
                object value;
                if (member is FieldInfo field)
                    value = field.GetValue(this);
                else
                    value = ((PropertyInfo)member).GetValue(this);
                settings[member.Name] = value == null ? JValue.CreateNull() : JToken.FromObject(value);
            }
            _settingsJson = settings.ToString(Newtonsoft.Json.Formatting.None);
            return !JToken.DeepEquals(previous, settings);
        }

        private sealed class BuiltInAssetImporter : AssetImporter
        {
        }
    }

    /// <summary>Registers a managed importer for one or more source extensions.</summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
    public sealed class ScriptedImporterAttribute : Attribute
    {
        /// <summary>Creates a scripted importer registration.</summary>
        public ScriptedImporterAttribute(uint version, params string[] fileExtensions)
        {
            if (version == 0)
                throw new ArgumentOutOfRangeException(nameof(version));
            this.version = version;
            this.fileExtensions = NormalizeExtensions(fileExtensions, nameof(fileExtensions));
        }

        /// <summary>Importer implementation version.</summary>
        public uint version { get; }
        /// <summary>Extensions for which this is a default importer candidate.</summary>
        public string[] fileExtensions { get; }
        /// <summary>Extensions for which this importer is an explicit override candidate.</summary>
        public string[] overrideFileExtensions { get; set; } = Array.Empty<string>();
        /// <summary>Ordering priority among otherwise independent imports.</summary>
        public int importQueuePriority { get; set; }
        /// <summary>Whether immutable artifact caching is permitted.</summary>
        public bool AllowCaching { get; set; } = true;
        /// <summary>Current normalized per-asset settings schema version.</summary>
        public uint settingsSchemaVersion { get; set; } = 1;

        internal static string[] NormalizeExtensions(string[] extensions, string parameterName)
        {
            if (extensions == null || extensions.Length == 0)
                throw new ArgumentException("At least one extension is required.", parameterName);
            var result = extensions.Select(x => (x ?? string.Empty).Trim().TrimStart('.').ToLowerInvariant()).ToArray();
            if (result.Any(string.IsNullOrEmpty) || result.Distinct(StringComparer.OrdinalIgnoreCase).Count() != result.Length)
                throw new ArgumentException("Importer extensions must be non-empty and unique.", parameterName);
            return result;
        }
    }

    /// <summary>Base class for managed worker-host importers.</summary>
    public abstract class ScriptedImporter : AssetImporter
    {
        /// <summary>Produces staged objects and dependencies for one source.</summary>
        public abstract void OnImportAsset(AssetImportContext ctx);

        /// <summary>Purely upgrades one settings object from <paramref name="fromVersion"/> to the next schema version.</summary>
        protected virtual JObject UpgradeSettings(uint fromVersion, JObject settings)
        {
            throw new InvalidOperationException($"Importer '{GetType().FullName}' does not implement settings upgrade from schema {fromVersion}.");
        }

        internal bool UpgradeSettingsToCurrent(ref int schemaVersion, ref string settingsJson)
        {
            var attribute = GetType().GetCustomAttribute<ScriptedImporterAttribute>() ??
                            throw new InvalidOperationException($"Importer '{GetType().FullName}' has no registration attribute.");
            if (schemaVersion < 1 || schemaVersion > attribute.settingsSchemaVersion)
                throw new InvalidDataException($"Importer settings schema {schemaVersion} cannot be loaded by '{GetType().FullName}' schema {attribute.settingsSchemaVersion}.");
            if (schemaVersion == attribute.settingsSchemaVersion)
                return false;
            var settings = JObject.Parse(string.IsNullOrWhiteSpace(settingsJson) ? "{}" : settingsJson);
            while (schemaVersion < attribute.settingsSchemaVersion)
            {
                settings = UpgradeSettings((uint)schemaVersion, (JObject)settings.DeepClone()) ??
                           throw new InvalidDataException($"Importer '{GetType().FullName}' returned null while upgrading schema {schemaVersion}.");
                schemaVersion++;
            }
            settingsJson = settings.ToString(Newtonsoft.Json.Formatting.None);
            return true;
        }
    }

    internal static class ScriptedImporterRegistry
    {
        private sealed class Entry
        {
            public Type Type;
            public ScriptedImporterAttribute Attribute;
            public string Id;
        }

        private static readonly object Locker = new object();
        private static Entry[] _entries;

        public static Type[] GetAvailable(string path, bool overridesOnly = false)
        {
            var extension = Path.GetExtension(path ?? string.Empty).TrimStart('.');
            return Entries.Where(x => Matches(x, extension, overridesOnly)).Select(x => x.Type).ToArray();
        }

        public static Type GetDefault(string path)
        {
            var extension = Path.GetExtension(path ?? string.Empty).TrimStart('.');
            return Entries.Where(x => x.Attribute.fileExtensions.Contains(extension, StringComparer.OrdinalIgnoreCase))
                .OrderBy(x => x.Attribute.importQueuePriority)
                .ThenBy(x => x.Id, StringComparer.Ordinal)
                .Select(x => x.Type)
                .FirstOrDefault();
        }

        public static ScriptedImporter CreateSelected(string processorId)
        {
            var entry = Entries.FirstOrDefault(x => string.Equals(x.Id, processorId, StringComparison.Ordinal));
            return entry == null ? null : (ScriptedImporter)Activator.CreateInstance(entry.Type);
        }

        internal static bool TryImport(string path, ImportAssetOptions options, string callbackHash)
        {
            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(path);
            if (!File.Exists(physicalPath))
                return false;
            var record = AssetDatabase.GetMainRecord(physicalPath);
            var selected = record.HasValue ? Entries.FirstOrDefault(x => string.Equals(x.Id, record.Value.ProcessorID, StringComparison.Ordinal)) : null;
            if (selected == null)
            {
                if (record.HasValue && !string.Equals(record.Value.ProcessorID, "Flax.Unsupported", StringComparison.Ordinal))
                    return false;
                var defaultType = GetDefault(physicalPath);
                selected = defaultType == null ? null : Entries.First(x => x.Type == defaultType);
                if (selected == null)
                    return false;
                if (ScriptedImporterFacade.EnsureMetadata(physicalPath, selected.Id, (int)selected.Attribute.settingsSchemaVersion))
                    throw new InvalidOperationException(ScriptedImporterFacade.GetLastError());
                record = AssetDatabase.GetMainRecord(physicalPath);
            }
            if (!record.HasValue)
                throw new InvalidOperationException("Managed importer metadata was published but no canonical record was registered.");
            if (string.IsNullOrWhiteSpace(callbackHash))
                throw new InvalidOperationException("Scripted imports require the exact preprocess callback fingerprint.");

            var settingsProxy = (ScriptedImporter)Activator.CreateInstance(selected.Type);
            settingsProxy.Bind(record.Value);
            if (settingsProxy.WriteImportSettingsIfDirty())
            {
                record = AssetDatabase.GetMainRecord(physicalPath);
                if (!record.HasValue)
                    throw new InvalidOperationException("The source disappeared while upgrading importer settings.");
            }

            var verifyDeterminism = (options & ImportAssetOptions.VerifyDeterminism) != 0;
            var result = ScriptedImporterWorkerCoordinator.Run(record.Value.CanonicalPath, selected.Id, callbackHash, verifyDeterminism);
            if (ScriptedImporterFacade.Publish(physicalPath, result.ToString(Newtonsoft.Json.Formatting.None)))
                throw new InvalidOperationException(ScriptedImporterFacade.GetLastError());
            return true;
        }

        internal static JObject ExecuteWorker(string path, string processorId, string callbackHash, Func<bool> isCancelled)
        {
            if (!AssetDatabase.IsAssetImportWorkerProcess())
                throw new InvalidOperationException("Scripted importer execution is restricted to isolated worker processes.");
            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(path);
            var record = AssetDatabase.GetMainRecord(physicalPath);
            if (!record.HasValue)
                throw new InvalidOperationException("The worker cannot resolve the canonical source record.");
            var selected = Entries.FirstOrDefault(x => string.Equals(x.Id, processorId, StringComparison.Ordinal));
            if (selected == null || !string.Equals(record.Value.ProcessorID, selected.Id, StringComparison.Ordinal))
                throw new InvalidOperationException("The requested scripted importer no longer owns the source record.");

            CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
            CultureInfo.CurrentUICulture = CultureInfo.InvariantCulture;

            var importer = (ScriptedImporter)Activator.CreateInstance(selected.Type);
            importer.Bind(record.Value);
            var target = new Content.Settings.BuildTarget
            {
                Name = "Editor",
                Platform = BuildPlatform.Windows64,
                Mode = BuildConfiguration.Development,
            };
            var context = new AssetImportContext(record.Value.CanonicalPath, target, isCancelled);
            if (context.IsCancelled())
                throw new OperationCanceledException("The scripted import was cancelled before execution.");
            importer.OnImportAsset(context);
            if (context.IsCancelled())
                throw new OperationCanceledException("The scripted import was cancelled before staging completed.");
            if (context.Errors.Count != 0)
                throw new InvalidOperationException(string.Join(Environment.NewLine, context.Errors));
            if (context.mainObject == null || context.GetObjects().Count == 0)
                throw new InvalidOperationException($"Scripted importer '{selected.Id}' did not select a main object.");

            return BuildResult(context, selected, callbackHash);
        }

        private static JObject BuildResult(AssetImportContext context, Entry selected, string callbackHash)
        {
            var objects = new JArray();
            var outputData = new JObject();
            foreach (var declaration in context.GetObjects())
            {
                var serialized = SerializeObject(declaration.Object);
                objects.Add(new JObject
                {
                    ["identifier"] = declaration.Identifier,
                    ["type"] = serialized.TypeName,
                    ["name"] = declaration.Object.GetType().Name,
                    ["format"] = serialized.Format,
                    ["main"] = ReferenceEquals(declaration.Object, context.mainObject),
                    ["transientId"] = declaration.Object.ID.ToString("N"),
                    ["data"] = Convert.ToBase64String(serialized.Data),
                });
                if (declaration.Thumbnail != null)
                {
                    var thumbnail = SerializeObject(declaration.Thumbnail);
                    outputData[$"thumbnail-{declaration.Identifier}"] = Convert.ToBase64String(thumbnail.Data);
                }
            }
            foreach (var entry in context.OutputData.OrderBy(x => x.Key, StringComparer.Ordinal))
                outputData[entry.Key] = Convert.ToBase64String(entry.Value);
            var renames = new JObject();
            foreach (var entry in context.IdentityRenames.OrderBy(x => x.Key, StringComparer.Ordinal))
                renames[entry.Key] = entry.Value;
            return new JObject
            {
                ["implementationVersion"] = selected.Attribute.version,
                ["providerHash"] = GetProviderHash(selected),
                ["postprocessorHash"] = callbackHash ?? string.Empty,
                ["environment"] = new JObject
                {
                    ["framework"] = RuntimeInformation.FrameworkDescription,
                    ["osArchitecture"] = RuntimeInformation.OSArchitecture.ToString(),
                    ["processArchitecture"] = RuntimeInformation.ProcessArchitecture.ToString(),
                    ["processBitness"] = Environment.Is64BitProcess ? 64 : 32,
                    ["culture"] = CultureInfo.CurrentCulture.Name,
                    ["uiCulture"] = CultureInfo.CurrentUICulture.Name,
                    ["timeZone"] = TimeZoneInfo.Local.Id,
                },
                ["objects"] = objects,
                ["outputData"] = outputData,
                ["identityRenames"] = renames,
                ["dependencies"] = new JObject
                {
                    ["sourcePaths"] = new JArray(context.SourceDependencies.OrderBy(x => x, StringComparer.OrdinalIgnoreCase)),
                    ["sourceGuids"] = new JArray(context.SourceGuidDependencies.OrderBy(x => x).Select(x => x.ToString("N"))),
                    ["artifactPaths"] = new JArray(context.ArtifactDependencies.OrderBy(x => x, StringComparer.OrdinalIgnoreCase)),
                    ["artifactGuids"] = new JArray(context.ArtifactGuidDependencies.OrderBy(x => x).Select(x => x.ToString("N"))),
                    ["exactArtifacts"] = new JArray(context.ExactArtifactDependencies.Select(x => x.ToString()).OrderBy(x => x, StringComparer.Ordinal)),
                    ["custom"] = new JArray(context.CustomDependencies.OrderBy(x => x, StringComparer.Ordinal)),
                    ["global"] = JObject.FromObject(context.GlobalDependencies.OrderBy(x => x.Key, StringComparer.Ordinal)
                        .ToDictionary(x => x.Key, x => x.Value.ToString(), StringComparer.Ordinal)),
                    ["tools"] = JObject.FromObject(context.ToolDependencies.OrderBy(x => x.Key, StringComparer.Ordinal)
                        .ToDictionary(x => x.Key, x => x.Value.ToString(), StringComparer.Ordinal)),
                    ["observedSources"] = JObject.FromObject(context.ObservedSourceHashes.OrderBy(x => x.Key, StringComparer.Ordinal)
                        .ToDictionary(x => x.Key, x => x.Value, StringComparer.Ordinal)),
                    ["observedArtifacts"] = JObject.FromObject(context.ObservedArtifactKeys.OrderBy(x => x.Key, StringComparer.Ordinal)
                        .ToDictionary(x => x.Key, x => x.Value, StringComparer.Ordinal)),
                    ["logicalPath"] = context.LogicalPathObserved,
                },
            };
        }

        private static string GetProviderHash(Entry selected)
        {
            var path = selected.Type.Assembly.Location;
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                throw new InvalidOperationException($"Scripted importer assembly '{selected.Type.Assembly.FullName}' has no immutable file identity.");
            return Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
        }

        private static (string Format, string TypeName, byte[] Data) SerializeObject(FlaxEngine.Object obj)
        {
            var isJson = obj is JsonAssetBase;
            if (obj is Asset asset)
            {
                var temporaryPath = Path.Combine(Path.GetTempPath(), $"scripted-import-{Guid.NewGuid():N}{(isJson ? ".json" : ".flax")}");
                try
                {
                    if (asset.Save(temporaryPath))
                        throw new InvalidOperationException($"Scripted importer output '{obj.GetType().FullName}' does not support artifact staging.");
                    var bytes = File.ReadAllBytes(temporaryPath);
                    if (bytes.Length == 0)
                        throw new InvalidDataException("Scripted importer produced an empty staged object.");
                    return isJson ? ("json", typeof(JsonAsset).FullName, bytes) : ("flax", obj.GetType().FullName, bytes);
                }
                finally
                {
                    if (File.Exists(temporaryPath))
                        File.Delete(temporaryPath);
                }
            }

            var data = FlaxEngine.Json.JsonSerializer.Serialize(obj, obj.GetType());
            var json = new JObject
            {
                ["ID"] = obj.ID.ToString("N"),
                ["TypeName"] = obj.GetType().FullName,
                ["EngineBuild"] = Globals.EngineBuildNumber,
                ["Data"] = string.IsNullOrWhiteSpace(data) ? new JObject() : JToken.Parse(data),
            };
            return ("json", typeof(JsonAsset).FullName, Encoding.UTF8.GetBytes(json.ToString(Newtonsoft.Json.Formatting.None)));
        }

        public static string GetId(Type type)
        {
            var entry = Entries.FirstOrDefault(x => x.Type == type);
            if (entry == null)
                throw new InvalidOperationException($"Importer '{type?.FullName}' is not registered.");
            return entry.Id;
        }

        private static Entry[] Entries
        {
            get
            {
                lock (Locker)
                {
                    if (_entries == null)
                        _entries = Discover();
                    return _entries;
                }
            }
        }

        private static Entry[] Discover()
        {
            var entries = new List<Entry>();
            foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies().OrderBy(x => x.FullName, StringComparer.Ordinal))
            {
                Type[] types;
                try
                {
                    types = assembly.GetTypes();
                }
                catch (ReflectionTypeLoadException ex)
                {
                    types = ex.Types.Where(x => x != null).ToArray();
                }
                foreach (var type in types.OrderBy(x => x.FullName, StringComparer.Ordinal))
                {
                    if (type.IsAbstract || !typeof(ScriptedImporter).IsAssignableFrom(type))
                        continue;
                    var attribute = type.GetCustomAttribute<ScriptedImporterAttribute>();
                    if (attribute == null)
                        continue;
                    if (attribute.settingsSchemaVersion == 0 || attribute.settingsSchemaVersion > int.MaxValue)
                        throw new InvalidOperationException($"Scripted importer '{type.FullName}' has an invalid settings schema version.");
                    attribute.overrideFileExtensions = attribute.overrideFileExtensions == null || attribute.overrideFileExtensions.Length == 0
                        ? Array.Empty<string>()
                        : ScriptedImporterAttribute.NormalizeExtensions(attribute.overrideFileExtensions, nameof(attribute.overrideFileExtensions));
                    entries.Add(new Entry { Type = type, Attribute = attribute, Id = type.FullName });
                }
            }
            foreach (var group in entries.SelectMany(x => x.Attribute.fileExtensions.Select(extension => new { extension, entry = x }))
                         .GroupBy(x => x.extension, StringComparer.OrdinalIgnoreCase))
            {
                var ordered = group.OrderBy(x => x.entry.Attribute.importQueuePriority).ThenBy(x => x.entry.Id, StringComparer.Ordinal).ToArray();
                if (ordered.Length > 1 && ordered[0].entry.Attribute.importQueuePriority == ordered[1].entry.Attribute.importQueuePriority)
                    throw new InvalidOperationException($"Scripted importer registration conflict for '.{group.Key}' between '{ordered[0].entry.Id}' and '{ordered[1].entry.Id}'.");
            }
            return entries.OrderBy(x => x.Id, StringComparer.Ordinal).ToArray();
        }

        private static bool Matches(Entry entry, string extension, bool overridesOnly)
        {
            var extensions = overridesOnly ? entry.Attribute.overrideFileExtensions : entry.Attribute.fileExtensions.Concat(entry.Attribute.overrideFileExtensions);
            return extensions.Contains(extension, StringComparer.OrdinalIgnoreCase);
        }
    }
}
