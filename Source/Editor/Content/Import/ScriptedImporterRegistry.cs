// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;

namespace FlaxEditor.Content.Import
{
    /// <summary>Deterministic managed importer registry rebuilt after successful script reloads.</summary>
    internal static class ScriptedImporterRegistry
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int InvokeImporterDelegate(IntPtr importerId, int importerIdLength);

        private sealed class Entry
        {
            public Type Type;
            public ScriptedImporterAttribute Attribute;
            public string ID;
            public string ImplementationHash;
        }

        private sealed class ActiveEntry
        {
            public string AssemblyName;
            public string TypeName;
        }

        private static readonly InvokeImporterDelegate InvokeCallback = InvokeImporter;
        private static IReadOnlyDictionary<string, ActiveEntry> _entries = new Dictionary<string, ActiveEntry>(StringComparer.Ordinal);
        private static bool _initialized;

        internal static void Initialize()
        {
            if (_initialized)
                return;
            _initialized = true;
            ScriptsBuilder.ScriptsReloadEnd += OnScriptsReloadEnd;
            Reconstruct();
        }

        internal static void Shutdown()
        {
            if (!_initialized)
                return;
            ScriptsBuilder.ScriptsReloadEnd -= OnScriptsReloadEnd;
            _initialized = false;
        }

        private static void OnScriptsReloadEnd()
        {
            Reconstruct();
        }

        internal static bool Reconstruct()
        {
            try
            {
                var assemblies = Utils.GetAssemblies()
                    .Where(x => !x.IsDynamic)
                    .OrderBy(x => x.FullName, StringComparer.Ordinal)
                    .ToArray();
                var assemblyHashes = new Dictionary<Assembly, byte[]>();
                var candidates = new List<Entry>();
                var postprocessors = new List<Type>();
                foreach (var assembly in assemblies)
                {
                    var types = GetTypesOrThrow(assembly);
                    foreach (var type in types.OrderBy(x => x.FullName, StringComparer.Ordinal))
                    {
                        if (type != null && !type.IsAbstract && typeof(AssetPostprocessor).IsAssignableFrom(type))
                            postprocessors.Add(type);
                        if (type == null || type.IsAbstract || !typeof(ScriptedImporter).IsAssignableFrom(type))
                            continue;
                        var attribute = type.GetCustomAttribute<ScriptedImporterAttribute>(false);
                        if (attribute == null)
                            throw new InvalidOperationException($"Scripted importer '{type.FullName}' has no ScriptedImporterAttribute.");
                        Validate(type, attribute);
                        candidates.Add(new Entry
                        {
                            Type = type,
                            Attribute = attribute,
                            ID = attribute.Id,
                        });
                    }
                }
                var postprocessorHash = HashPostprocessors(postprocessors, assemblyHashes);
                candidates.Sort((a, b) => string.CompareOrdinal(a.ID, b.ID));
                for (var i = 1; i < candidates.Count; i++)
                {
                    if (string.Equals(candidates[i - 1].ID, candidates[i].ID, StringComparison.Ordinal))
                        throw new InvalidOperationException($"Scripted importer ID '{candidates[i].ID}' is declared by both '{candidates[i - 1].Type.FullName}' and '{candidates[i].Type.FullName}'.");
                }
                foreach (var entry in candidates)
                    entry.ImplementationHash = HashImporter(entry, postprocessorHash, assemblyHashes);

                var next = candidates.ToDictionary(x => x.ID, x => new ActiveEntry
                {
                    AssemblyName = x.Type.Assembly.GetName().Name,
                    TypeName = x.Type.FullName,
                }, StringComparer.Ordinal);
                var previous = _entries;
                var callback = Marshal.GetFunctionPointerForDelegate(InvokeCallback);
                if (ScriptedImporterInterop.BeginRegistration(callback))
                    throw new InvalidOperationException(ScriptedImporterInterop.GetLastError());
                try
                {
                    foreach (var entry in candidates)
                    {
                        var attribute = entry.Attribute;
                        var flags = (attribute.SupportsOverride ? 1 : 0) |
                                    (attribute.ProducesMainObject ? 2 : 0) |
                                    (attribute.ProducesSubObjects ? 4 : 0) |
                                    (attribute.SupportsParallelImport ? 8 : 0) |
                                    (attribute.RequiresMainThread ? 16 : 0);
                        if (ScriptedImporterInterop.AddRegistration(entry.ID, attribute.Version, attribute.SettingsVersion,
                            entry.ImplementationHash, string.Join(";", attribute.Extensions), attribute.Priority, flags))
                            throw new InvalidOperationException(ScriptedImporterInterop.GetLastError());
                    }
                    _entries = next;
                    if (ScriptedImporterInterop.CommitRegistration())
                    {
                        _entries = previous;
                        throw new InvalidOperationException(ScriptedImporterInterop.GetLastError());
                    }
                }
                catch
                {
                    ScriptedImporterInterop.AbortRegistration();
                    _entries = previous;
                    throw;
                }
                return true;
            }
            catch (Exception ex)
            {
                Editor.LogError("Scripted importer registry reload failed. The previous registry remains active.");
                Editor.LogWarning(ex);
                return false;
            }
        }

        private static Type[] GetTypesOrThrow(Assembly assembly)
        {
            try
            {
                return assembly.GetTypes();
            }
            catch (ReflectionTypeLoadException ex)
            {
                var details = string.Join(Environment.NewLine, ex.LoaderExceptions.Where(x => x != null).Select(x => x.Message));
                throw new InvalidOperationException($"Cannot inspect importer assembly '{assembly.FullName}'.{Environment.NewLine}{details}", ex);
            }
        }

        private static void Validate(Type type, ScriptedImporterAttribute attribute)
        {
            if (!type.IsClass || type.IsGenericTypeDefinition || type.GetConstructor(Type.EmptyTypes) == null)
                throw new InvalidOperationException($"Scripted importer '{type.FullName}' must be a non-generic class with a parameterless constructor.");
            if (!IsStableToken(attribute.Id))
                throw new InvalidOperationException($"Scripted importer '{type.FullName}' has an invalid stable ID '{attribute.Id}'.");
            if (attribute.Version < 1 || attribute.SettingsVersion < 1 || attribute.Extensions.Length == 0)
                throw new InvalidOperationException($"Scripted importer '{type.FullName}' has an invalid version or extension claim.");
            if (attribute.RequiresMainThread && attribute.SupportsParallelImport)
                throw new InvalidOperationException($"Scripted importer '{type.FullName}' cannot require the main thread and parallel execution.");
            var extensions = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var extensionValue in attribute.Extensions)
            {
                var extension = extensionValue?.Trim().TrimStart('.');
                if (string.IsNullOrEmpty(extension) || extension.Any(x => !char.IsLetterOrDigit(x) && x != '-' && x != '_') || !extensions.Add(extension))
                    throw new InvalidOperationException($"Scripted importer '{type.FullName}' has an invalid or duplicated extension claim.");
            }
        }

        private static bool IsStableToken(string value)
        {
            if (string.IsNullOrEmpty(value) || value[0] == '.' || value[value.Length - 1] == '.')
                return false;
            var previousDot = false;
            foreach (var character in value)
            {
                if (character == '.' && previousDot)
                    return false;
                if (!(character >= 'a' && character <= 'z') && !(character >= 'A' && character <= 'Z') &&
                    !(character >= '0' && character <= '9') && character != '.' && character != '-' && character != '_')
                    return false;
                previousDot = character == '.';
            }
            return true;
        }

        private static byte[] GetAssemblyHash(Assembly assembly, Dictionary<Assembly, byte[]> cache)
        {
            if (cache.TryGetValue(assembly, out var result))
                return result;
            var location = assembly.Location;
            result = !string.IsNullOrEmpty(location) && File.Exists(location)
                ? SHA256.HashData(File.ReadAllBytes(location))
                : SHA256.HashData(Encoding.UTF8.GetBytes(assembly.ManifestModule.ModuleVersionId.ToString("N")));
            cache.Add(assembly, result);
            return result;
        }

        private static byte[] HashPostprocessors(IEnumerable<Type> types, Dictionary<Assembly, byte[]> assemblyHashes)
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var entries = types.Select(type => (Type: type, Instance: (AssetPostprocessor)Activator.CreateInstance(type)))
                .OrderBy(x => x.Instance.Order)
                .ThenBy(x => x.Type.FullName, StringComparer.Ordinal)
                .ToArray();
            foreach (var entry in entries)
            {
                var type = entry.Type;
                hash.AppendData(GetAssemblyHash(type.Assembly, assemblyHashes));
                hash.AppendData(Encoding.UTF8.GetBytes(type.AssemblyQualifiedName));
                hash.AppendData(BitConverter.GetBytes(entry.Instance.Version));
                hash.AppendData(BitConverter.GetBytes(entry.Instance.Order));
            }
            return hash.GetHashAndReset();
        }

        private static AssetPostprocessor[] CreatePostprocessors()
        {
            return Utils.GetAssemblies()
                .Where(x => !x.IsDynamic)
                .OrderBy(x => x.FullName, StringComparer.Ordinal)
                .SelectMany(GetTypesOrThrow)
                .Where(x => x != null && !x.IsAbstract && typeof(AssetPostprocessor).IsAssignableFrom(x))
                .Select(x => (Type: x, Instance: (AssetPostprocessor)Activator.CreateInstance(x)))
                .OrderBy(x => x.Instance.Order)
                .ThenBy(x => x.Type.FullName, StringComparer.Ordinal)
                .Select(x => x.Instance)
                .ToArray();
        }

        /// <summary>Runs the all-assets callback in the parent editor for one committed publication batch.</summary>
        internal static void OnAssetsPublished(Guid[] publishedAssets)
        {
            if (publishedAssets == null || publishedAssets.Length == 0)
                return;
            try
            {
                var ordered = publishedAssets.OrderBy(x => x.ToString(), StringComparer.Ordinal).ToArray();
                var imported = new AssetGuid[ordered.Length];
                for (var i = 0; i < ordered.Length; i++)
                    imported[i] = new AssetGuid(ordered[i]);
                var postprocessors = CreatePostprocessors();
                for (var i = 0; i < postprocessors.Length; i++)
                    postprocessors[i].OnPostprocessAllAssets(imported);
            }
            catch (Exception ex)
            {
                Editor.LogError("Asset postprocessor batch failed after artifact publication.");
                Editor.LogWarning(ex);
            }
        }

        private static string HashImporter(Entry entry, byte[] postprocessorHash, Dictionary<Assembly, byte[]> assemblyHashes)
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            hash.AppendData(GetAssemblyHash(entry.Type.Assembly, assemblyHashes));
            hash.AppendData(Encoding.UTF8.GetBytes(entry.Type.AssemblyQualifiedName));
            hash.AppendData(BitConverter.GetBytes(entry.Attribute.Version));
            hash.AppendData(BitConverter.GetBytes(entry.Attribute.SettingsVersion));
            hash.AppendData(BitConverter.GetBytes(entry.Attribute.Priority));
            foreach (var extension in entry.Attribute.Extensions.OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                hash.AppendData(Encoding.UTF8.GetBytes(extension.ToLowerInvariant()));
            hash.AppendData(postprocessorHash);
            return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
        }

        private static int InvokeImporter(IntPtr importerId, int importerIdLength)
        {
            try
            {
                var id = Marshal.PtrToStringUni(importerId, importerIdLength);
                if (id == null || !_entries.TryGetValue(id, out var entry))
                    throw new InvalidOperationException($"Scripted importer '{id}' is not in the active registry generation.");
                var assembly = Utils.GetAssemblies().FirstOrDefault(x => string.Equals(x.GetName().Name, entry.AssemblyName, StringComparison.Ordinal));
                var type = assembly?.GetType(entry.TypeName, false, false);
                if (type == null)
                    throw new InvalidOperationException($"Scripted importer type '{entry.TypeName}' is unavailable in assembly '{entry.AssemblyName}'.");
                var importer = (ScriptedImporter)Activator.CreateInstance(type);
                var context = new AssetImportContext();
                var postprocessors = CreatePostprocessors();
                for (var i = 0; i < postprocessors.Length; i++)
                    postprocessors[i].OnPreprocessAsset(context);
                importer.OnImportAsset(context);
                for (var i = 0; i < postprocessors.Length; i++)
                    postprocessors[i].OnPostprocessAsset(context);
                context.CommitOutputs();
                return 0;
            }
            catch (Exception ex)
            {
                try
                {
                    ScriptedImporterInterop.LogDiagnostic((int)ImportDiagnosticSeverity.Error, ex.ToString(), null, -1, -1);
                }
                catch
                {
                }
                return 1;
            }
        }

        internal static string HashQuery(AssetQuery query)
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            hash.AppendData(Encoding.UTF8.GetBytes(query.Expression));
            foreach (var result in query.Results.OrderBy(x => x.ToString(), StringComparer.Ordinal))
                hash.AppendData(Encoding.UTF8.GetBytes(result.ToString()));
            return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
        }
    }

    /// <summary>One-shot managed entry point invoked only by the restricted editor worker host.</summary>
    internal static class ScriptedImporterWorker
    {
        internal static int Run()
        {
            return ScriptedImporterRegistry.Reconstruct() ? ScriptedImporterInterop.RunWorker() : 5;
        }
    }
}
