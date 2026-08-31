// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using FlaxEngine;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Context-owned immutable file bytes with no final artifact path exposure.</summary>
    public sealed class AssetImportReadOnlyFile
    {
        private readonly byte[] _data;

        internal AssetImportReadOnlyFile(string name, byte[] data)
        {
            Name = name;
            _data = data ?? throw new ArgumentNullException(nameof(data));
        }

        /// <summary>Logical display name of the observed input.</summary>
        public string Name { get; }
        /// <summary>Observed byte length.</summary>
        public long Length => _data.LongLength;
        /// <summary>Opens an independent read-only in-memory stream.</summary>
        public Stream OpenRead() => new MemoryStream(_data, false);
        /// <summary>Returns a copy of the observed bytes.</summary>
        public byte[] ReadAllBytes() => (byte[])_data.Clone();
    }

    /// <summary>Verified immutable bytes from one exact published artifact output.</summary>
    public sealed class AssetImportReadOnlyArtifact
    {
        private readonly byte[] _data;

        internal AssetImportReadOnlyArtifact(Guid assetGuid, string outputKind, string exactArtifactKey, byte[] data)
        {
            AssetGuid = assetGuid;
            OutputKind = outputKind;
            ExactArtifactKey = exactArtifactKey;
            _data = data;
        }

        /// <summary>Persistent source identity owning the artifact.</summary>
        public Guid AssetGuid { get; }
        /// <summary>Logical output kind within the immutable artifact.</summary>
        public string OutputKind { get; }
        /// <summary>Exact canonical SHA-256 output key.</summary>
        public string ExactArtifactKey { get; }
        /// <summary>Verified byte length.</summary>
        public long Length => _data.LongLength;
        /// <summary>Opens an independent read-only in-memory stream.</summary>
        public Stream OpenRead() => new MemoryStream(_data, false);
        /// <summary>Returns a copy of the verified bytes.</summary>
        public byte[] ReadAllBytes() => (byte[])_data.Clone();
    }

    /// <summary>One staged object declaration produced by a scripted importer.</summary>
    public sealed class ImportedObjectDeclaration
    {
        internal ImportedObjectDeclaration(string identifier, FlaxEngine.Object obj, Texture thumbnail)
        {
            Identifier = identifier;
            Object = obj;
            Thumbnail = thumbnail;
        }

        /// <summary>Importer-defined stable identifier.</summary>
        public string Identifier { get; internal set; }
        /// <summary>Staged object.</summary>
        public FlaxEngine.Object Object { get; }
        /// <summary>Optional derived preview.</summary>
        public Texture Thumbnail { get; }
    }

    /// <summary>Restricted managed importer context. It records outputs and dependencies without exposing final artifact paths.</summary>
    public sealed class AssetImportContext
    {
        private readonly Dictionary<string, ImportedObjectDeclaration> _objects = new Dictionary<string, ImportedObjectDeclaration>(StringComparer.Ordinal);
        private readonly HashSet<string> _sourceDependencies = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<Guid> _sourceGuidDependencies = new HashSet<Guid>();
        private readonly HashSet<string> _artifactDependencies = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<Guid> _artifactGuidDependencies = new HashSet<Guid>();
        private readonly HashSet<ArtifactKey> _exactArtifactDependencies = new HashSet<ArtifactKey>();
        private readonly HashSet<string> _customDependencies = new HashSet<string>(StringComparer.Ordinal);
        private readonly Dictionary<string, Hash128> _globalDependencies = new Dictionary<string, Hash128>(StringComparer.Ordinal);
        private readonly Dictionary<string, Hash128> _toolDependencies = new Dictionary<string, Hash128>(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _observedSourceHashes = new Dictionary<string, string>(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _observedArtifactKeys = new Dictionary<string, string>(StringComparer.Ordinal);
        private readonly Dictionary<string, byte[]> _outputData = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _identityRenames = new Dictionary<string, string>(StringComparer.Ordinal);
        private readonly List<string> _warnings = new List<string>();
        private readonly List<string> _errors = new List<string>();
        private readonly Content.Settings.BuildTarget _selectedBuildTarget;
        private readonly Func<bool> _isCancelled;
        private FlaxEngine.Object _mainObject;
        private bool _selectedTargetObserved;
        private bool _logicalPathObserved;

        internal AssetImportContext(string assetPath, Content.Settings.BuildTarget selectedBuildTarget, Func<bool> isCancelled = null)
        {
            if (string.IsNullOrWhiteSpace(assetPath))
                throw new ArgumentException("A canonical source path is required.", nameof(assetPath));
            this.assetPath = assetPath;
            _selectedBuildTarget = selectedBuildTarget;
            _isCancelled = isCancelled ?? (() => false);
        }

        /// <summary>Canonical source path being imported.</summary>
        public string assetPath { get; }

        /// <summary>Persistent source GUID being imported.</summary>
        public Guid assetGuid
        {
            get
            {
                var value = AssetDatabase.AssetPathToGUID(assetPath);
                return Guid.TryParse(value, out var result) ? result : Guid.Empty;
            }
        }

        /// <summary>Static source name input without extension.</summary>
        public string assetName => Path.GetFileNameWithoutExtension(assetPath);

        /// <summary>Currently selected staged main object.</summary>
        public FlaxEngine.Object mainObject => _mainObject;

        /// <summary>Selected target profile. Access records a target/global dependency.</summary>
        public Content.Settings.BuildTarget selectedBuildTarget
        {
            get
            {
                _selectedTargetObserved = true;
                return _selectedBuildTarget;
            }
        }

        /// <summary>Reads the canonical source through a context-owned immutable snapshot.</summary>
        public AssetImportReadOnlyFile OpenSource()
        {
            var physicalPath = AssetDatabase.ResolvePhysicalPathInternal(assetPath);
            return ReadSource("source", assetPath, physicalPath);
        }

        /// <summary>Reads a declared source dependency through a context-owned immutable snapshot.</summary>
        public AssetImportReadOnlyFile OpenSourceDependency(Guid guid)
        {
            if (guid == Guid.Empty)
                throw new ArgumentException("A valid dependency GUID is required.", nameof(guid));
            DependsOnSourceAsset(guid);
            var path = AssetDatabase.GUIDToAssetPath(guid.ToString("N"));
            if (string.IsNullOrEmpty(path))
                throw new FileNotFoundException($"Source dependency '{guid:N}' is not registered.");
            return ReadSource("guid:" + guid.ToString("N"), path, AssetDatabase.ResolvePhysicalPathInternal(path));
        }

        /// <summary>Reads a declared source dependency path through a context-owned immutable snapshot.</summary>
        public AssetImportReadOnlyFile OpenSourceDependency(string path)
        {
            DependsOnSourceAsset(path);
            var normalized = path.Replace('\\', '/');
            return ReadSource("path:" + normalized, normalized, AssetDatabase.ResolvePhysicalPathInternal(normalized));
        }

        /// <summary>Reads and exactly pins another source's current immutable artifact output.</summary>
        public AssetImportReadOnlyArtifact OpenArtifactDependency(Guid guid, string outputKind = "runtime")
        {
            if (guid == Guid.Empty)
                throw new ArgumentException("A valid dependency GUID is required.", nameof(guid));
            ValidateOutputName(outputKind);
            DependsOnArtifact(guid);
            var json = ScriptedImporterFacade.ReadArtifactOutput(guid, outputKind);
            if (string.IsNullOrEmpty(json))
                throw new InvalidOperationException(ScriptedImporterFacade.GetLastError());
            var envelope = JObject.Parse(json);
            var key = (string)envelope["artifactKey"];
            var contentHash = (string)envelope["contentHash"];
            var data = Convert.FromBase64String((string)envelope["data"] ?? string.Empty);
            var actualHash = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
            if (key?.Length != 64 || contentHash?.Length != 64 || !string.Equals(contentHash, actualHash, StringComparison.Ordinal))
                throw new InvalidDataException("The artifact-read response failed exact key or content verification.");
            _observedArtifactKeys[$"guid:{guid:N}/{outputKind}"] = key;
            return new AssetImportReadOnlyArtifact(guid, outputKind, key, data);
        }

        /// <summary>Returns verified auxiliary artifact bytes and records the exact dependency.</summary>
        public byte[] GetArtifactData(Guid guid, string outputKind)
        {
            return OpenArtifactDependency(guid, outputKind).ReadAllBytes();
        }

        /// <summary>Adds a staged output object under a unique stable identifier.</summary>
        public void AddObjectToAsset(string identifier, FlaxEngine.Object obj)
        {
            AddObjectToAsset(identifier, obj, null);
        }

        /// <summary>Adds a staged output object with an auxiliary thumbnail.</summary>
        public void AddObjectToAsset(string identifier, FlaxEngine.Object obj, Texture thumbnail)
        {
            ValidateIdentifier(identifier);
            if (obj == null)
                throw new ArgumentNullException(nameof(obj));
            if (_objects.ContainsKey(identifier))
                throw new InvalidOperationException($"The importer added stable identifier '{identifier}' more than once.");
            _objects.Add(identifier, new ImportedObjectDeclaration(identifier, obj, thumbnail));
        }

        /// <summary>Selects one previously added output as the main object.</summary>
        public void SetMainObject(FlaxEngine.Object obj)
        {
            if (obj == null)
                throw new ArgumentNullException(nameof(obj));
            foreach (var value in _objects.Values)
            {
                if (ReferenceEquals(value.Object, obj))
                {
                    _mainObject = obj;
                    return;
                }
            }
            throw new InvalidOperationException("The main object must be added to the import context first.");
        }

        /// <summary>Preserves the local identity of a renamed stable output key.</summary>
        public void PreserveObjectIdentity(string oldIdentifier, string newIdentifier)
        {
            ValidateIdentifier(oldIdentifier);
            ValidateIdentifier(newIdentifier);
            if (!_objects.TryGetValue(newIdentifier, out var declaration))
                throw new InvalidOperationException($"The new identifier '{newIdentifier}' has not been added.");
            if (_objects.ContainsKey(oldIdentifier))
                throw new InvalidOperationException($"The previous identifier '{oldIdentifier}' is still present in this import.");
            if (_identityRenames.ContainsKey(oldIdentifier) || _identityRenames.ContainsValue(newIdentifier))
                throw new InvalidOperationException("An output identity can participate in only one rename declaration per import.");
            _identityRenames.Add(oldIdentifier, declaration.Identifier);
        }

        /// <summary>Returns staged object declarations in deterministic identifier order.</summary>
        public IReadOnlyList<ImportedObjectDeclaration> GetObjects()
        {
            var result = new List<ImportedObjectDeclaration>(_objects.Values);
            result.Sort((a, b) => string.CompareOrdinal(a.Identifier, b.Identifier));
            return result;
        }

        /// <summary>Declares a source dependency. The declaration remains meaningful while missing.</summary>
        public void DependsOnSourceAsset(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new ArgumentException("A canonical dependency path is required.", nameof(path));
            _sourceDependencies.Add(path.Replace('\\', '/'));
        }

        /// <summary>Declares a source dependency by persistent identity.</summary>
        public void DependsOnSourceAsset(Guid guid)
        {
            if (guid == Guid.Empty)
                throw new ArgumentException("A valid dependency GUID is required.", nameof(guid));
            _sourceGuidDependencies.Add(guid);
        }

        /// <summary>Declares a dependency on another source's current target artifact.</summary>
        public void DependsOnArtifact(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new ArgumentException("A canonical dependency path is required.", nameof(path));
            _artifactDependencies.Add(path.Replace('\\', '/'));
        }

        /// <summary>Declares an artifact dependency by persistent source identity.</summary>
        public void DependsOnArtifact(Guid guid)
        {
            if (guid == Guid.Empty)
                throw new ArgumentException("A valid dependency GUID is required.", nameof(guid));
            _artifactGuidDependencies.Add(guid);
        }

        /// <summary>Declares an exact artifact dependency.</summary>
        public void DependsOnArtifact(ArtifactKey key)
        {
            if (!key.IsValid)
                throw new ArgumentException("A valid exact artifact key is required.", nameof(key));
            _exactArtifactDependencies.Add(key);
        }

        /// <summary>Declares a named custom dependency.</summary>
        public void DependsOnCustomDependency(string dependency)
        {
            if (string.IsNullOrWhiteSpace(dependency))
                throw new ArgumentException("A dependency name is required.", nameof(dependency));
            _customDependencies.Add(dependency);
        }

        /// <summary>Declares a normalized global-state value.</summary>
        public void DependsOnGlobalState(string key, Hash128 value)
        {
            if (string.IsNullOrWhiteSpace(key))
                throw new ArgumentException("A global-state key is required.", nameof(key));
            _globalDependencies[key] = value;
        }

        /// <summary>Declares an importer tool or external toolchain version input.</summary>
        public void DependsOnTool(string id, Hash128 versionHash)
        {
            if (string.IsNullOrWhiteSpace(id))
                throw new ArgumentException("A stable tool identifier is required.", nameof(id));
            if (versionHash.IsZero)
                throw new ArgumentException("A tool version hash must be nonzero.", nameof(versionHash));
            _toolDependencies[id] = versionHash;
        }

        /// <summary>Declares that the canonical logical source path is a semantic input.</summary>
        public void DependsOnLogicalPath()
        {
            _logicalPathObserved = true;
        }

        /// <summary>Returns true after the worker coordinator requests cancellation.</summary>
        public bool IsCancelled()
        {
            return _isCancelled();
        }

        /// <summary>Gets another asset's main object and records its artifact dependency.</summary>
        public FlaxEngine.Object GetReferenceToAssetMainObject(string path)
        {
            DependsOnArtifact(path);
            return AssetDatabase.LoadMainAssetAtPath(path);
        }

        /// <summary>Writes named auxiliary output bytes to private context-owned staging memory.</summary>
        public void SetOutputArtifactData(string name, byte[] data)
        {
            ValidateOutputName(name);
            if (data == null)
                throw new ArgumentNullException(nameof(data));
            _outputData[name] = (byte[])data.Clone();
        }

        /// <summary>Reads context-owned auxiliary output bytes.</summary>
        public byte[] GetOutputArtifactData(string name)
        {
            ValidateOutputName(name);
            return _outputData.TryGetValue(name, out var data) ? (byte[])data.Clone() : null;
        }

        /// <summary>Emits a structured import error. Any error prevents publication.</summary>
        public void LogImportError(string message, FlaxEngine.Object context = null)
        {
            if (string.IsNullOrWhiteSpace(message))
                throw new ArgumentException("A diagnostic message is required.", nameof(message));
            _errors.Add(message);
            Debug.LogError(message, context);
        }

        /// <summary>Emits a structured non-fatal import warning.</summary>
        public void LogImportWarning(string message, FlaxEngine.Object context = null)
        {
            if (string.IsNullOrWhiteSpace(message))
                throw new ArgumentException("A diagnostic message is required.", nameof(message));
            _warnings.Add(message);
            Debug.LogWarning(message, context);
        }

        /// <summary>Emits a structured informational import message.</summary>
        public void LogImportInfo(string message, FlaxEngine.Object context = null)
        {
            if (string.IsNullOrWhiteSpace(message))
                throw new ArgumentException("A diagnostic message is required.", nameof(message));
            Debug.Log(message, context);
        }

        internal bool SelectedTargetObserved => _selectedTargetObserved;
        internal IReadOnlyCollection<string> SourceDependencies => _sourceDependencies;
        internal IReadOnlyCollection<Guid> SourceGuidDependencies => _sourceGuidDependencies;
        internal IReadOnlyCollection<string> ArtifactDependencies => _artifactDependencies;
        internal IReadOnlyCollection<Guid> ArtifactGuidDependencies => _artifactGuidDependencies;
        internal IReadOnlyCollection<ArtifactKey> ExactArtifactDependencies => _exactArtifactDependencies;
        internal IReadOnlyCollection<string> CustomDependencies => _customDependencies;
        internal IReadOnlyDictionary<string, Hash128> GlobalDependencies => _globalDependencies;
        internal IReadOnlyDictionary<string, Hash128> ToolDependencies => _toolDependencies;
        internal IReadOnlyDictionary<string, string> ObservedSourceHashes => _observedSourceHashes;
        internal IReadOnlyDictionary<string, string> ObservedArtifactKeys => _observedArtifactKeys;
        internal bool LogicalPathObserved => _logicalPathObserved;
        internal IReadOnlyList<string> Warnings => _warnings;
        internal IReadOnlyList<string> Errors => _errors;
        internal IReadOnlyDictionary<string, string> IdentityRenames => _identityRenames;
        internal IReadOnlyDictionary<string, byte[]> OutputData => _outputData;

        private AssetImportReadOnlyFile ReadSource(string observationKey, string logicalPath, string physicalPath)
        {
            if (IsCancelled())
                throw new OperationCanceledException("Scripted import was cancelled.");
            if (string.IsNullOrEmpty(physicalPath) || !File.Exists(physicalPath))
                throw new FileNotFoundException($"Declared source input '{logicalPath}' is missing.", physicalPath);
            var data = File.ReadAllBytes(physicalPath);
            _observedSourceHashes[observationKey] = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
            return new AssetImportReadOnlyFile(Path.GetFileName(logicalPath), data);
        }

        private static void ValidateIdentifier(string identifier)
        {
            if (string.IsNullOrWhiteSpace(identifier) || identifier.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                throw new ArgumentException("Stable object identifiers must be non-empty portable strings.", nameof(identifier));
        }

        private static void ValidateOutputName(string name)
        {
            if (string.IsNullOrWhiteSpace(name) || Path.IsPathRooted(name) || name.Contains("..") || name.IndexOfAny(Path.GetInvalidPathChars()) >= 0)
                throw new ArgumentException("Artifact output names must be relative normalized names.", nameof(name));
        }
    }
}
