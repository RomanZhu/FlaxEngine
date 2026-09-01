// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using FlaxEngine;
using FlaxEngine.Interop;
using Newtonsoft.Json;

namespace FlaxEditor.Content.Import
{
    [Flags]
    public enum ArtifactTargetDimensions : uint
    {
        None = 0,
        Platform = 1u << 0,
        Architecture = 1u << 1,
        Graphics = 1u << 2,
        Configuration = 1u << 3,
        Quality = 1u << 4,
        TextureCompression = 1u << 5,
        AudioCodec = 1u << 6,
        ShaderCompiler = 1u << 7,
        Role = 1u << 8,
        FeatureFlags = 1u << 9,
        All = (1u << 10) - 1,
    }

    /// <summary>Exact immutable artifact digest used for dependency invalidation.</summary>
    public readonly struct ArtifactKey : IEquatable<ArtifactKey>
    {
        private readonly string _value;

        public ArtifactKey(string value)
        {
            if (!IsCanonical(value))
                throw new ArgumentException("Artifact keys must contain exactly 64 lowercase or uppercase hexadecimal characters.", nameof(value));
            _value = value.ToLowerInvariant();
        }

        public bool IsValid => _value != null;
        public override string ToString() => _value ?? string.Empty;
        public bool Equals(ArtifactKey other) => string.Equals(_value, other._value, StringComparison.Ordinal);
        public override bool Equals(object obj) => obj is ArtifactKey other && Equals(other);
        public override int GetHashCode() => _value == null ? 0 : StringComparer.Ordinal.GetHashCode(_value);

        public static bool TryParse(string value, out ArtifactKey result)
        {
            if (IsCanonical(value))
            {
                result = new ArtifactKey(value);
                return true;
            }
            result = default;
            return false;
        }

        private static bool IsCanonical(string value)
        {
            if (value == null || value.Length != 64)
                return false;
            for (var i = 0; i < value.Length; i++)
            {
                var c = value[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    return false;
            }
            return true;
        }
    }

    /// <summary>Structured immutable target snapshot supplied to a scripted importer.</summary>
    public sealed class AssetImportTarget
    {
        public string Platform { get; }
        public string Architecture { get; }
        public string Graphics { get; }
        public string Configuration { get; }
        public string Quality { get; }
        public string TextureCompression { get; }
        public string AudioCodec { get; }
        public string ShaderCompiler { get; }
        public string Role { get; }
        public IReadOnlyList<string> FeatureFlags { get; }

        internal AssetImportTarget()
        {
            Platform = ScriptedImporterInterop.GetTargetDimension(0);
            Architecture = ScriptedImporterInterop.GetTargetDimension(1);
            Graphics = ScriptedImporterInterop.GetTargetDimension(2);
            Configuration = ScriptedImporterInterop.GetTargetDimension(3);
            Quality = ScriptedImporterInterop.GetTargetDimension(4);
            TextureCompression = ScriptedImporterInterop.GetTargetDimension(5);
            AudioCodec = ScriptedImporterInterop.GetTargetDimension(6);
            ShaderCompiler = ScriptedImporterInterop.GetTargetDimension(7);
            Role = ScriptedImporterInterop.GetTargetDimension(8);
            var flags = new string[ScriptedImporterInterop.GetTargetFeatureFlagCount()];
            for (var i = 0; i < flags.Length; i++)
                flags[i] = ScriptedImporterInterop.GetTargetFeatureFlag(i);
            FeatureFlags = flags;
        }
    }

    /// <summary>Declares one managed source importer.</summary>
    [AttributeUsage(AttributeTargets.Class, Inherited = false)]
    public sealed class ScriptedImporterAttribute : Attribute
    {
        public string Id { get; }
        public int Version { get; }
        public string[] Extensions { get; }
        public int Priority { get; }
        public int SettingsVersion { get; set; } = 1;
        public bool SupportsOverride { get; set; } = true;
        public bool ProducesMainObject { get; set; } = true;
        public bool ProducesSubObjects { get; set; } = true;
        public bool SupportsParallelImport { get; set; }
        public bool RequiresMainThread { get; set; } = true;
        public bool PathSensitive { get; set; } = true;

        public ScriptedImporterAttribute(string id, int version, string[] extensions, int priority = 0)
        {
            Id = id;
            Version = version;
            Extensions = extensions ?? Array.Empty<string>();
            Priority = priority;
        }
    }

    /// <summary>Base type for project-defined managed importers.</summary>
    public abstract class ScriptedImporter
    {
        public abstract void OnImportAsset(AssetImportContext context);
    }

    /// <summary>Deterministically ordered managed import callback set.</summary>
    public abstract class AssetPostprocessor
    {
        public virtual int Version => 1;
        public virtual int Order => 0;
        public virtual void OnPreprocessAsset(AssetImportContext context) { }
        public virtual void OnPostprocessAsset(AssetImportContext context) { }
        public virtual void OnPostprocessAllAssets(IReadOnlyList<AssetGuid> importedAssets) { }
        public virtual void OnWillCreateAsset(string path) { }
        public virtual void OnWillMoveAsset(string sourcePath, string destinationPath) { }
        public virtual void OnWillDeleteAsset(string path) { }
        public virtual void OnWillSaveAssets(IReadOnlyList<string> paths) { }
    }

    /// <summary>Immutable handle returned when an importer declares an object.</summary>
    public readonly struct ImportedObjectHandle : IEquatable<ImportedObjectHandle>
    {
        internal readonly int Index;
        internal ImportedObjectHandle(int index) => Index = index;
        public bool IsValid => Index >= 0;
        public bool Equals(ImportedObjectHandle other) => Index == other.Index;
        public override bool Equals(object obj) => obj is ImportedObjectHandle other && Equals(other);
        public override int GetHashCode() => Index;
    }

    /// <summary>Description of one main or sub-object emitted by an importer.</summary>
    public sealed class ImportedObjectDescriptor
    {
        public string TypeName { get; set; }
        public string DisplayName { get; set; }

        public ImportedObjectDescriptor(string typeName, string displayName = null)
        {
            TypeName = typeName ?? throw new ArgumentNullException(nameof(typeName));
            DisplayName = displayName;
        }

        public ImportedObjectDescriptor(Type type, string displayName = null)
            : this(type?.FullName ?? throw new ArgumentNullException(nameof(type)), displayName)
        {
        }
    }

    /// <summary>Description of one immutable output written into job staging.</summary>
    public sealed class ArtifactOutputDescriptor
    {
        public string Kind { get; set; }
        public string Extension { get; set; }
        public ArtifactTargetDimensions TargetDimensions { get; set; } = ArtifactTargetDimensions.All;

        public ArtifactOutputDescriptor(string kind, string extension)
        {
            Kind = kind ?? throw new ArgumentNullException(nameof(kind));
            Extension = extension ?? throw new ArgumentNullException(nameof(extension));
        }
    }

    /// <summary>Explicit query dependency including the exact result set observed by the importer.</summary>
    public sealed class AssetQuery
    {
        public string Expression { get; }
        public IReadOnlyList<AssetObjectId> Results { get; }

        public AssetQuery(string expression, IReadOnlyList<AssetObjectId> results)
        {
            Expression = expression ?? throw new ArgumentNullException(nameof(expression));
            Results = results ?? Array.Empty<AssetObjectId>();
        }
    }

    public enum ImportDiagnosticSeverity
    {
        Info,
        Warning,
        Error,
    }

    public sealed class ImportDiagnostic
    {
        public ImportDiagnosticSeverity Severity { get; set; } = ImportDiagnosticSeverity.Error;
        public string Message { get; set; }
        public string File { get; set; }
        public int Line { get; set; } = -1;
        public int Column { get; set; } = -1;

        public ImportDiagnostic(string message) => Message = message;
    }

    /// <summary>Read-only source capability returned by the parent import context.</summary>
    public sealed class SourceReadHandle : MemoryStream
    {
        internal SourceReadHandle(byte[] data)
            : base(data, false)
        {
        }
    }

    /// <summary>Write-only staging capability for one declared artifact output.</summary>
    public sealed class ArtifactOutputWriter : MemoryStream
    {
        private readonly int _outputIndex;
        private bool _committed;

        internal ArtifactOutputWriter(int outputIndex) => _outputIndex = outputIndex;

        internal void Commit()
        {
            if (_committed)
                return;
            _committed = true;
            if (ScriptedImporterInterop.WriteOutput(_outputIndex, ToArray()))
                throw new InvalidOperationException(ScriptedImporterInterop.GetLastError());
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
                Commit();
            base.Dispose(disposing);
        }
    }

    /// <summary>Controlled importer input, dependency, object, and output surface.</summary>
    public sealed class AssetImportContext
    {
        private readonly List<ArtifactOutputWriter> _outputs = new List<ArtifactOutputWriter>();
        private readonly AssetImportTarget _target = new AssetImportTarget();

        public AssetGuid AssetGuid
        {
            get
            {
                ScriptedImporterInterop.GetAsset(out var value);
                return new AssetGuid(value);
            }
        }

        public string AssetPath => ScriptedImporterInterop.GetSourcePath();
        public AssetImportTarget Target => _target;
        public string SettingsJson => ScriptedImporterInterop.GetSettings();
        public T GetSettings<T>() => JsonConvert.DeserializeObject<T>(SettingsJson);

        public SourceReadHandle OpenSourceFile() => Read(null);
        public SourceReadHandle OpenDependencyFile(string path) => Read(path ?? throw new ArgumentNullException(nameof(path)));

        private static SourceReadHandle Read(string path)
        {
            var bytes = ScriptedImporterInterop.Read(path, out _, out var failed);
            if (failed)
                throw new IOException(ScriptedImporterInterop.GetLastError());
            return new SourceReadHandle(bytes ?? Array.Empty<byte>());
        }

        public void DependsOnSourceAsset(AssetGuid guid) => DependsOnSourceAsset(AssetObjectId.Main(guid));
        public void DependsOnSourceAsset(AssetObjectId objectId)
        {
            var guid = objectId.Asset.Value;
            ScriptedImporterInterop.DependsOnObject(ref guid, objectId.LocalId, 0);
        }

        public void DependsOnArtifact(AssetObjectId objectId)
        {
            var guid = objectId.Asset.Value;
            ScriptedImporterInterop.DependsOnObject(ref guid, objectId.LocalId, 1);
        }

        public void DependsOnArtifact(ArtifactKey artifact)
        {
            if (!artifact.IsValid)
                throw new ArgumentException("Artifact dependency key is invalid.", nameof(artifact));
            if (ScriptedImporterInterop.DependsOnExactArtifact(artifact.ToString()))
                throw new InvalidOperationException(ScriptedImporterInterop.GetLastError());
        }
        public void DependsOnCustomDependency(string name) => ScriptedImporterInterop.DependsOnNamed(0, name, null);
        public void DependsOnFolder(string path)
        {
            if (ScriptedImporterInterop.DependsOnFolder(path))
                throw new IOException(ScriptedImporterInterop.GetLastError());
        }

        public void DependsOnFolder(string path, string contentHash) => ScriptedImporterInterop.DependsOnNamed(1, path, contentHash);
        public void DependsOnQuery(AssetQuery query) => ScriptedImporterInterop.DependsOnNamed(2, query.Expression, ScriptedImporterRegistry.HashQuery(query));
        public void DependsOnToolchain(string tool, string versionHash) => ScriptedImporterInterop.DependsOnNamed(3, tool, versionHash);
        public void DependsOnProjectSetting(string setting, string valueHash) => ScriptedImporterInterop.DependsOnNamed(4, setting, valueHash);

        public ImportedObjectHandle AddObjectToAsset(string stableIdentifier, ImportedObjectDescriptor descriptor)
        {
            if (descriptor == null)
                throw new ArgumentNullException(nameof(descriptor));
            var index = ScriptedImporterInterop.AddObject(stableIdentifier, descriptor.TypeName, descriptor.DisplayName);
            if (index < 0)
                throw new InvalidOperationException("The imported object identifier or type is invalid or duplicated.");
            return new ImportedObjectHandle(index);
        }

        public void SetMainObject(ImportedObjectHandle handle)
        {
            if (!handle.IsValid || ScriptedImporterInterop.SetMainObject(handle.Index))
                throw new InvalidOperationException("The scripted importer selected an invalid main object.");
        }

        public ArtifactOutputWriter CreateOutput(string outputName, ArtifactOutputDescriptor descriptor)
        {
            if (descriptor == null)
                throw new ArgumentNullException(nameof(descriptor));
            var index = ScriptedImporterInterop.CreateOutput(outputName, descriptor.Kind, descriptor.Extension,
                (uint)descriptor.TargetDimensions);
            if (index < 0)
                throw new InvalidOperationException("The artifact output declaration is invalid or duplicated.");
            var writer = new ArtifactOutputWriter(index);
            _outputs.Add(writer);
            return writer;
        }

        public void LogDiagnostic(ImportDiagnostic diagnostic)
        {
            if (diagnostic == null)
                throw new ArgumentNullException(nameof(diagnostic));
            ScriptedImporterInterop.LogDiagnostic((int)diagnostic.Severity, diagnostic.Message, diagnostic.File, diagnostic.Line, diagnostic.Column);
        }

        internal void CommitOutputs()
        {
            for (var i = 0; i < _outputs.Count; i++)
                _outputs[i].Commit();
        }
    }

    internal static partial class ScriptedImporterInterop
    {
        private const string Library = "FlaxEngine";

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_BeginRegistration")]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool BeginRegistration(IntPtr callback);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_AddRegistration", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool AddRegistration(string id, int importerVersion, int settingsVersion, string implementationHash, string extensions, int priority, int flags);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_CommitRegistration")]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool CommitRegistration();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_AbortRegistration")]
        internal static partial void AbortRegistration();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_GetLastError", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial string GetLastError();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterInternal_RunWorker")]
        internal static partial int RunWorker();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetAsset")]
        internal static partial void GetAsset(out Guid asset);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetSourcePath", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial string GetSourcePath();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetTargetDimension", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial string GetTargetDimension(int dimension);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetTargetFeatureFlagCount")]
        internal static partial int GetTargetFeatureFlagCount();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetTargetFeatureFlag", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial string GetTargetFeatureFlag(int index);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_GetSettings", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial string GetSettings();

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_Read", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        [return: MarshalUsing(typeof(FlaxEngine.Interop.ArrayMarshaller<,>), CountElementName = "count")]
        internal static partial byte[] Read(string path, out int count, [MarshalAs(UnmanagedType.U1)] out bool failed);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_DependsOnObject")]
        internal static partial void DependsOnObject(ref Guid asset, long localId, int kind);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_DependsOnExactArtifact", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool DependsOnExactArtifact(string artifactKey);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_DependsOnNamed", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial void DependsOnNamed(int kind, string identity, string hash);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_DependsOnFolder", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool DependsOnFolder(string path);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_AddObject", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial int AddObject(string stableIdentifier, string typeName, string displayName);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_SetMainObject")]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool SetMainObject(int objectIndex);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_CreateOutput", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial int CreateOutput(string outputName, string kind, string extension, uint targetDimensions);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_WriteOutput")]
        [return: MarshalAs(UnmanagedType.U1)]
        internal static partial bool WriteOutput(int outputIndex, [In, MarshalUsing(typeof(FlaxEngine.Interop.ArrayMarshaller<,>))] byte[] data);

        [LibraryImport(Library, EntryPoint = "ScriptedImporterContextInternal_LogDiagnostic", StringMarshalling = StringMarshalling.Custom, StringMarshallingCustomType = typeof(StringMarshaller))]
        internal static partial void LogDiagnostic(int severity, string message, string file, int line, int column);
    }
}
