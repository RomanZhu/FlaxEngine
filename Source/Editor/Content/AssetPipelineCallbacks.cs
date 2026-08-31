// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>Requires this postprocessor to run before the specified postprocessor types.</summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
    public sealed class AssetPostprocessorRunBeforeAttribute : Attribute
    {
        /// <summary>Creates an ordering constraint.</summary>
        public AssetPostprocessorRunBeforeAttribute(params Type[] types)
        {
            Types = types ?? throw new ArgumentNullException(nameof(types));
        }

        internal Type[] Types { get; }
    }

    /// <summary>Requires this postprocessor to run after the specified postprocessor types.</summary>
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
    public sealed class AssetPostprocessorRunAfterAttribute : Attribute
    {
        /// <summary>Creates an ordering constraint.</summary>
        public AssetPostprocessorRunAfterAttribute(params Type[] types)
        {
            Types = types ?? throw new ArgumentNullException(nameof(types));
        }

        internal Type[] Types { get; }
    }

    /// <summary>Base type for deterministic pre/post import callbacks.</summary>
    public abstract class AssetPostprocessor
    {
        /// <summary>Canonical source path for a per-asset callback.</summary>
        protected string assetPath { get; private set; }
        /// <summary>Importer-settings proxy for a per-asset callback.</summary>
        protected AssetImporter assetImporter { get; private set; }

        /// <summary>Orders postprocessors of the same phase.</summary>
        public virtual int GetPostprocessOrder() => 0;
        /// <summary>Contributes to the affected import input key.</summary>
        public virtual uint GetVersion() => 0;
        /// <summary>Runs before an importer reads settings and produces outputs.</summary>
        public virtual void OnPreprocessAsset()
        {
        }

        internal void Bind(string path)
        {
            assetPath = path;
            assetImporter = AssetImporter.GetAtPath(path);
        }

        internal void CommitImporterSettings()
        {
            assetImporter?.WriteImportSettingsIfDirty();
        }
    }

    /// <summary>Result returned by a pre-delete modification callback.</summary>
    public enum AssetDeleteResult
    {
        /// <summary>The processor did not perform or reject the delete.</summary>
        DidNotDelete,
        /// <summary>The processor rejects the delete.</summary>
        FailedDelete,
        /// <summary>The processor completed the delete itself.</summary>
        DidDelete,
    }

    /// <summary>Result returned by a pre-move modification callback.</summary>
    public enum AssetMoveResult
    {
        /// <summary>The processor did not perform or reject the move.</summary>
        DidNotMove,
        /// <summary>The processor rejects the move.</summary>
        FailedMove,
        /// <summary>The processor completed the move itself.</summary>
        DidMove,
    }

    /// <summary>Marker base type for static source-mutation callbacks.</summary>
    public abstract class AssetModificationProcessor
    {
    }

    internal static class AssetPipelineCallbacks
    {
        private sealed class PostprocessorEntry
        {
            public Type Type;
            public AssetPostprocessor Instance;
            public string Identity;
            public int Order;
            public uint Version;
            public string[] Before;
            public string[] After;
        }

        public static string Preprocess(string path)
        {
            string fingerprint;
            using (AssetDatabase.EnterCallbackScope())
            {
                var processors = GetPostprocessors();
                fingerprint = GetFingerprint(processors);
                foreach (var entry in processors)
                {
                    if (entry.Instance == null)
                        continue;
                    try
                    {
                        entry.Instance.Bind(path);
                        entry.Instance.OnPreprocessAsset();
                        entry.Instance.CommitImporterSettings();
                    }
                    catch (Exception ex)
                    {
                        Debug.LogException(ex);
                    }
                }
            }
            return fingerprint;
        }

        public static string ValidateMove(string oldPath, string newPath, out bool handled)
        {
            handled = false;
            foreach (var type in FindProcessors(typeof(AssetModificationProcessor)))
            {
                var method = type.GetMethod("OnWillMoveAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new[] { typeof(string), typeof(string) }, null);
                if (method == null)
                    continue;
                var value = Invoke(method, null, new object[] { oldPath, newPath });
                if (!(value is AssetMoveResult result))
                    throw new InvalidOperationException($"{type.FullName}.OnWillMoveAsset must return {nameof(AssetMoveResult)}.");
                if (result == AssetMoveResult.FailedMove)
                    return $"Move rejected by {type.FullName}.OnWillMoveAsset.";
                if (result == AssetMoveResult.DidMove)
                {
                    handled = true;
                    return string.Empty;
                }
            }
            return string.Empty;
        }

        public static bool ValidateDelete(string path, out bool handled)
        {
            handled = false;
            foreach (var type in FindProcessors(typeof(AssetModificationProcessor)))
            {
                var method = type.GetMethod("OnWillDeleteAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new[] { typeof(string) }, null);
                if (method == null)
                    continue;
                var value = Invoke(method, null, new object[] { path });
                if (!(value is AssetDeleteResult result))
                    throw new InvalidOperationException($"{type.FullName}.OnWillDeleteAsset must return {nameof(AssetDeleteResult)}.");
                if (result == AssetDeleteResult.FailedDelete)
                    return false;
                if (result == AssetDeleteResult.DidDelete)
                {
                    handled = true;
                    return true;
                }
            }
            return true;
        }

        public static void WillCreate(string path)
        {
            foreach (var type in FindProcessors(typeof(AssetModificationProcessor)))
            {
                var method = type.GetMethod("OnWillCreateAsset", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new[] { typeof(string) }, null);
                if (method != null)
                    Invoke(method, null, new object[] { path });
            }
        }

        public static string[] WillSave(string[] paths)
        {
            var result = paths ?? Array.Empty<string>();
            foreach (var type in FindProcessors(typeof(AssetModificationProcessor)))
            {
                var method = type.GetMethod("OnWillSaveAssets", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null, new[] { typeof(string[]) }, null);
                if (method == null)
                    continue;
                var returned = Invoke(method, null, new object[] { result });
                if (!(returned is string[] filtered))
                    throw new InvalidOperationException($"{type.FullName}.OnWillSaveAssets must return a string array.");
                result = filtered ?? Array.Empty<string>();
            }
            return result;
        }

        public static void PostprocessAll(string[] imported, string[] deleted, string[] moved, string[] movedFrom, bool didDomainReload)
        {
            imported = imported ?? Array.Empty<string>();
            deleted = deleted ?? Array.Empty<string>();
            moved = moved ?? Array.Empty<string>();
            movedFrom = movedFrom ?? Array.Empty<string>();
            if (moved.Length != movedFrom.Length)
                throw new ArgumentException("Moved and moved-from arrays must have matching indexes.");
            using (AssetDatabase.EnterCallbackScope())
            {
                foreach (var entry in GetPostprocessors())
                {
                    var type = entry.Type;
                    var method = type.GetMethod("OnPostprocessAllAssets", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null,
                        new[] { typeof(string[]), typeof(string[]), typeof(string[]), typeof(string[]), typeof(bool) }, null);
                    var parameters = new object[] { imported, deleted, moved, movedFrom, didDomainReload };
                    if (method == null)
                    {
                        method = type.GetMethod("OnPostprocessAllAssets", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic, null,
                            new[] { typeof(string[]), typeof(string[]), typeof(string[]), typeof(string[]) }, null);
                        parameters = new object[] { imported, deleted, moved, movedFrom };
                    }
                    if (method != null)
                        Invoke(method, null, parameters);
                }
            }
        }

        private static PostprocessorEntry[] GetPostprocessors()
        {
            var result = new List<PostprocessorEntry>();
            foreach (var type in FindProcessors(typeof(AssetPostprocessor)))
            {
                var entry = new PostprocessorEntry
                {
                    Type = type,
                    Identity = (type.Assembly.FullName ?? type.Assembly.GetName().Name) + ":" + type.FullName,
                };
                try
                {
                    entry.Instance = (AssetPostprocessor)Activator.CreateInstance(type);
                }
                catch (Exception ex)
                {
                    Debug.LogException(ex);
                }
                if (entry.Instance != null)
                {
                    try
                    {
                        entry.Order = entry.Instance.GetPostprocessOrder();
                    }
                    catch (Exception ex)
                    {
                        Debug.LogException(ex);
                    }
                    try
                    {
                        entry.Version = entry.Instance.GetVersion();
                    }
                    catch (Exception ex)
                    {
                        Debug.LogException(ex);
                    }
                }
                result.Add(entry);
            }
            var entries = result.ToArray();
            var byType = entries.ToDictionary(x => x.Type);
            foreach (var entry in entries)
            {
                entry.Before = ReadConstraints<AssetPostprocessorRunBeforeAttribute>(entry.Type, x => x.Types, byType);
                entry.After = ReadConstraints<AssetPostprocessorRunAfterAttribute>(entry.Type, x => x.Types, byType);
            }
            return TopologicalSort(entries);
        }

        private static string[] ReadConstraints<T>(Type owner, Func<T, Type[]> selector, Dictionary<Type, PostprocessorEntry> entries)
            where T : Attribute
        {
            var result = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var attribute in owner.GetCustomAttributes<T>(false))
            {
                foreach (var target in selector(attribute))
                {
                    if (target == null || !entries.TryGetValue(target, out var entry))
                        throw new InvalidOperationException($"Postprocessor '{owner.FullName}' has an ordering constraint on an unregistered type '{target?.FullName ?? "<null>"}'.");
                    result.Add(entry.Identity);
                }
            }
            return result.ToArray();
        }

        private static PostprocessorEntry[] TopologicalSort(PostprocessorEntry[] entries)
        {
            var edges = entries.ToDictionary(x => x, _ => new HashSet<PostprocessorEntry>());
            var incoming = entries.ToDictionary(x => x, _ => 0);
            foreach (var entry in entries)
            {
                foreach (var identity in entry.Before)
                {
                    var target = entries.First(x => x.Identity == identity);
                    if (edges[entry].Add(target))
                        incoming[target]++;
                }
                foreach (var identity in entry.After)
                {
                    var source = entries.First(x => x.Identity == identity);
                    if (edges[source].Add(entry))
                        incoming[entry]++;
                }
            }
            var comparer = Comparer<PostprocessorEntry>.Create((a, b) =>
            {
                var order = a.Order.CompareTo(b.Order);
                return order != 0 ? order : string.CompareOrdinal(a.Identity, b.Identity);
            });
            var ready = new SortedSet<PostprocessorEntry>(comparer);
            foreach (var entry in entries)
            {
                if (incoming[entry] == 0)
                    ready.Add(entry);
            }
            var result = new List<PostprocessorEntry>(entries.Length);
            while (ready.Count != 0)
            {
                var entry = ready.Min;
                ready.Remove(entry);
                result.Add(entry);
                foreach (var target in edges[entry].OrderBy(x => x.Identity, StringComparer.Ordinal))
                {
                    if (--incoming[target] == 0)
                        ready.Add(target);
                }
            }
            if (result.Count != entries.Length)
            {
                var cycle = incoming.Where(x => x.Value != 0).Select(x => x.Key.Identity).OrderBy(x => x, StringComparer.Ordinal);
                throw new InvalidOperationException("Asset postprocessor ordering contains a cycle: " + string.Join(" -> ", cycle));
            }
            return result.ToArray();
        }

        private static string GetFingerprint(PostprocessorEntry[] processors)
        {
            var value = new StringBuilder();
            value.Append(processors.Length.ToString(CultureInfo.InvariantCulture)).Append('\n');
            foreach (var entry in processors)
            {
                value.Append(entry.Identity.Length.ToString(CultureInfo.InvariantCulture)).Append(':').Append(entry.Identity).Append('|')
                    .Append(entry.Order.ToString(CultureInfo.InvariantCulture)).Append('|')
                    .Append(entry.Version.ToString(CultureInfo.InvariantCulture)).Append('|')
                    .Append(string.Join(",", entry.Before)).Append('|')
                    .Append(string.Join(",", entry.After)).Append('\n');
            }
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value.ToString()))).ToLowerInvariant();
        }

        private static Type[] FindProcessors(Type baseType)
        {
            var result = new List<Type>();
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
                result.AddRange(types.Where(x => x != baseType && !x.IsAbstract && baseType.IsAssignableFrom(x)));
            }
            return result.OrderBy(x => x.Assembly.FullName, StringComparer.Ordinal)
                .ThenBy(x => x.FullName, StringComparer.Ordinal).ToArray();
        }

        private static object Invoke(MethodInfo method, object target, object[] parameters)
        {
            try
            {
                return method.Invoke(target, parameters);
            }
            catch (TargetInvocationException ex)
            {
                Debug.LogException(ex.InnerException ?? ex);
                return method.ReturnType.IsValueType ? Activator.CreateInstance(method.ReturnType) : null;
            }
            catch (Exception ex)
            {
                Debug.LogException(ex);
                return method.ReturnType.IsValueType ? Activator.CreateInstance(method.ReturnType) : null;
            }
        }
    }
}
