// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using FlaxEditor.Content;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;

namespace FlaxEditor
{
    internal sealed class CliAssetOptions
    {
        [JsonProperty("action")]
        public string Action { get; set; }

        [JsonProperty("path")]
        public string Path { get; set; }

        [JsonProperty("destination")]
        public string Destination { get; set; }

        [JsonProperty("sources")]
        public string[] Sources { get; set; }

        [JsonProperty("assetType")]
        public string AssetType { get; set; }

        [JsonProperty("propertyPath")]
        public string PropertyPath { get; set; }

        [JsonProperty("value")]
        public JToken Value { get; set; }

        [JsonProperty("recursive")]
        public bool Recursive { get; set; }

        [JsonProperty("force")]
        public bool Force { get; set; }

        [JsonProperty("save")]
        public bool Save { get; set; } = true;
    }

    internal sealed partial class CliRequestService
    {
        private readonly List<object> _importedAssets = new List<object>();
        private bool _assetImportFailed;

        private void ExecuteAsset()
        {
            var options = _request.Asset ?? throw new InvalidOperationException("The asset request payload is missing.");
            if (string.IsNullOrWhiteSpace(options.Action))
                throw new InvalidOperationException("The asset action is missing.");

            TryWriteEvent(new { type = "started", requestId = _request.RequestId, operation = _request.Operation, action = options.Action });
            switch (options.Action)
            {
            case "list":
                ListAssets(options);
                break;
            case "types":
                ListAssetTypes(options);
                break;
            case "info":
                CompleteAsset(DescribeAsset(RequireItem(options.Path)));
                break;
            case "create":
                CreateAsset(options);
                break;
            case "mkdir":
                CreateFolder(options);
                break;
            case "import":
                ImportAssets(options);
                break;
            case "duplicate":
                DuplicateAsset(options);
                break;
            case "move":
                MoveAsset(options);
                break;
            case "delete":
                DeleteAsset(options);
                break;
            case "reimport":
                ReimportAsset(options);
                break;
            case "export":
                ExportAsset(options);
                break;
            case "get":
                GetAssetProperty(options);
                break;
            case "set":
                SetAssetProperty(options);
                break;
            case "save":
                SaveAsset(options);
                break;
            default:
                throw new InvalidOperationException($"Unsupported asset action '{options.Action}'.");
            }
        }

        private void ListAssets(CliAssetOptions options)
        {
            var path = string.IsNullOrWhiteSpace(options.Path) ? Globals.ProjectContentFolder : options.Path;
            var item = RequireItem(path);
            IEnumerable<ContentItem> items;
            if (item is ContentFolder folder)
            {
                items = options.Recursive ? EnumerateChildren(folder) : folder.Children;
            }
            else
            {
                items = new[] { item };
            }
            CompleteAsset(items.Select(DescribeAsset).ToArray());
        }

        private void ListAssetTypes(CliAssetOptions options)
        {
            var folder = RequireFolder(string.IsNullOrWhiteSpace(options.Path) ? Globals.ProjectContentFolder : options.Path);
            var types = Editor.Instance.ContentDatabase.Proxy
                .Where(x => x.IsAsset && x.CanCreate(folder))
                .Select(x => new { name = x.Name, extension = x.FileExtension, defaultName = x.NewItemName })
                .OrderBy(x => x.name, StringComparer.OrdinalIgnoreCase)
                .ToArray();
            CompleteAsset(types);
        }

        private void CreateAsset(CliAssetOptions options)
        {
            if (string.IsNullOrWhiteSpace(options.AssetType))
                throw new InvalidOperationException("Asset creation requires an asset type.");
            var requestedPath = options.Path;
            if (string.IsNullOrEmpty(System.IO.Path.GetExtension(requestedPath)))
                requestedPath += ".flax";
            var path = RequireNewPath(requestedPath);
            RequireExistingParent(path);
            if (Editor.CreateAsset(options.AssetType, path))
                throw new InvalidOperationException($"Failed to create {options.AssetType} asset '{path}'.");
            Editor.Instance.ContentDatabase.RefreshFolder(Editor.Instance.ContentDatabase.Find(System.IO.Path.GetDirectoryName(path)), false);
            TryWriteEvent(new { type = "artifact", requestId = _request.RequestId, kind = "asset", path });
            CompleteAsset(DescribePath(path, options.AssetType));
        }

        private void CreateFolder(CliAssetOptions options)
        {
            var path = RequireNewPath(options.Path);
            var parent = RequireExistingParent(path);
            Directory.CreateDirectory(path);
            Editor.Instance.ContentDatabase.RefreshFolder(parent, true);
            CompleteAsset(DescribePath(path, "Folder"));
        }

        private void ImportAssets(CliAssetOptions options)
        {
            if (options.Sources == null || options.Sources.Length == 0)
                throw new InvalidOperationException("Asset import requires at least one source file.");
            var target = RequireFolder(options.Destination);
            var sources = options.Sources.Select(System.IO.Path.GetFullPath).ToArray();
            foreach (var source in sources)
            {
                if (!File.Exists(source) && !Directory.Exists(source))
                    throw new FileNotFoundException($"Import source '{source}' does not exist.", source);
            }

            var importing = Editor.Instance.ContentImporting;
            importing.ImportFileEnd += OnAssetImportFileEnd;
            importing.ImportingQueueEnd += OnAssetImportQueueEnd;
            TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = "Import" });
            importing.Import(sources, target, true);
        }

        private void ReimportAsset(CliAssetOptions options)
        {
            var item = RequireItem(options.Path) as BinaryAssetItem
                       ?? throw new InvalidOperationException("Only binary assets with import metadata can be reimported.");
            var proxy = Editor.Instance.ContentDatabase.GetProxy(item);
            if (proxy == null || !proxy.CanReimport(item))
                throw new InvalidOperationException($"Asset '{item.Path}' does not support reimport.");
            if (item.GetImportPath(out var importPath) || !File.Exists(importPath))
                throw new FileNotFoundException($"The import source for asset '{item.Path}' does not exist.", importPath);

            var importing = Editor.Instance.ContentImporting;
            importing.ImportFileEnd += OnAssetImportFileEnd;
            importing.ImportingQueueEnd += OnAssetImportQueueEnd;
            TryWriteEvent(new { type = "phase", requestId = _request.RequestId, name = "Reimport" });
            importing.Reimport(item, null, true);
        }

        private void OnAssetImportFileEnd(FlaxEditor.Content.IFileEntryAction entry, bool failed)
        {
            _assetImportFailed |= failed;
            _importedAssets.Add(new { source = entry.SourceUrl, path = entry.ResultUrl, success = !failed });
            TryWriteEvent(new
            {
                type = failed ? "diagnostic" : "artifact",
                requestId = _request.RequestId,
                severity = failed ? "error" : null,
                code = failed ? "FLX-ASSET-IMPORT-0006" : null,
                kind = failed ? null : "asset",
                path = entry.ResultUrl,
            });
        }

        private void OnAssetImportQueueEnd()
        {
            var importing = Editor.Instance.ContentImporting;
            importing.ImportFileEnd -= OnAssetImportFileEnd;
            importing.ImportingQueueEnd -= OnAssetImportQueueEnd;
            if (_assetImportFailed)
                CompleteAsset(_importedAssets.ToArray(), false, "FLX-ASSET-IMPORT-0006", "One or more assets failed to import.");
            else
                CompleteAsset(_importedAssets.ToArray());
        }

        private void DuplicateAsset(CliAssetOptions options)
        {
            var item = RequireItem(options.Path);
            var destination = RequireNewPath(options.Destination);
            RequireExistingParent(destination);
            Editor.Instance.ContentDatabase.Copy(item, destination);
            if (!File.Exists(destination) && !Directory.Exists(destination))
                throw new InvalidOperationException($"Failed to duplicate asset '{item.Path}' to '{destination}'.");
            CompleteAsset(DescribePath(destination, item.TypeDescription));
        }

        private void MoveAsset(CliAssetOptions options)
        {
            var item = RequireItem(options.Path);
            var destination = RequireNewPath(options.Destination);
            RequireExistingParent(destination);
            Editor.Instance.ContentDatabase.Move(item, destination);
            if (!PathEquals(item.Path, destination))
                throw new InvalidOperationException($"Failed to move asset to '{destination}'.");
            CompleteAsset(DescribeAsset(item));
        }

        private void DeleteAsset(CliAssetOptions options)
        {
            if (!options.Force)
                throw new InvalidOperationException("Asset deletion requires explicit confirmation.");
            var item = RequireItem(options.Path);
            var path = item.Path;
            if (PathEquals(path, Globals.ProjectContentFolder))
                throw new InvalidOperationException("The project Content root cannot be deleted.");
            Editor.Instance.ContentDatabase.Delete(item, true);
            if (File.Exists(path) || Directory.Exists(path))
                throw new InvalidOperationException($"Failed to delete asset '{path}'.");
            CompleteAsset(new { path, deleted = true });
        }

        private void ExportAsset(CliAssetOptions options)
        {
            var item = RequireItem(options.Path);
            var destination = System.IO.Path.GetFullPath(options.Destination ?? throw new InvalidOperationException("Asset export requires a destination folder."));
            Directory.CreateDirectory(destination);
            var proxy = Editor.Instance.ContentDatabase.GetProxy(item);
            if (proxy != null && proxy.CanExport)
                proxy.Export(item, destination);
            else if (item.IsAsset && Editor.CanExport(item.Path))
            {
                if (Editor.Export(item.Path, destination))
                    throw new InvalidOperationException($"Failed to export asset '{item.Path}'.");
            }
            else
                throw new InvalidOperationException($"Asset '{item.Path}' does not support export.");
            CompleteAsset(new { path = item.Path, destination });
        }

        private void GetAssetProperty(CliAssetOptions options)
        {
            var asset = LoadAsset(options.Path);
            var value = GetMemberPathValue(asset, options.PropertyPath);
            CompleteAsset(new
            {
                path = RequireItem(options.Path).Path,
                property = options.PropertyPath,
                value = value == null ? JValue.CreateNull() : JToken.Parse(FlaxJsonSerializer.Serialize(value)),
                valueType = value?.GetType().FullName,
            });
        }

        private void SetAssetProperty(CliAssetOptions options)
        {
            if (options.Value == null)
                throw new InvalidOperationException("Asset property assignment requires a JSON value.");
            var asset = LoadAsset(options.Path);
            var member = SetMemberPathValue(asset, options.PropertyPath, options.Value);
            if (options.Save && asset.Save())
                throw new InvalidOperationException($"Failed to save asset '{options.Path}'.");
            CompleteAsset(new { path = RequireItem(options.Path).Path, property = options.PropertyPath, value = options.Value, valueType = GetMemberType(member).FullName, saved = options.Save });
        }

        private void SaveAsset(CliAssetOptions options)
        {
            var asset = LoadAsset(options.Path);
            if (asset.Save())
                throw new InvalidOperationException($"Failed to save asset '{options.Path}'.");
            CompleteAsset(new { path = RequireItem(options.Path).Path, saved = true });
        }

        private Asset LoadAsset(string path)
        {
            var item = RequireItem(path) as AssetItem
                       ?? throw new InvalidOperationException($"Content item '{path}' is not an asset.");
            var asset = item.LoadAsync();
            if (asset == null || asset.WaitForLoaded())
                throw new InvalidOperationException($"Failed to load asset '{item.Path}'.");
            return asset;
        }

        private ContentItem RequireItem(string path)
        {
            path = RequireProjectContentPath(path);
            var database = Editor.Instance.ContentDatabase;
            var item = database.Find(path);
            if (item == null && (File.Exists(path) || Directory.Exists(path)))
            {
                var parentPath = System.IO.Path.GetDirectoryName(path);
                ContentFolder parent = null;
                while (!string.IsNullOrEmpty(parentPath))
                {
                    parent = database.Find(parentPath) as ContentFolder;
                    if (parent != null || PathEquals(parentPath, Globals.ProjectContentFolder))
                        break;
                    parentPath = System.IO.Path.GetDirectoryName(parentPath);
                }
                if (parent != null)
                {
                    database.RefreshFolder(parent, true);
                    item = database.Find(path);
                }
            }
            return item ?? throw new FileNotFoundException($"Content item '{path}' was not found.", path);
        }

        private ContentFolder RequireFolder(string path)
        {
            return RequireItem(path) as ContentFolder
                   ?? throw new InvalidOperationException($"Content item '{path}' is not a folder.");
        }

        private ContentFolder RequireExistingParent(string path)
        {
            var parentPath = System.IO.Path.GetDirectoryName(path);
            return RequireFolder(parentPath);
        }

        private string RequireNewPath(string path)
        {
            path = RequireProjectContentPath(path);
            if (PathEquals(path, Globals.ProjectContentFolder))
                throw new InvalidOperationException("The project Content root cannot be replaced.");
            if (File.Exists(path) || Directory.Exists(path) || Editor.Instance.ContentDatabase.Find(path) != null)
                throw new IOException($"The destination '{path}' already exists.");
            return StringUtils.NormalizePath(path);
        }

        private static string RequireProjectContentPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new InvalidOperationException("An asset path is required.");
            path = System.IO.Path.GetFullPath(path);
            var root = System.IO.Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar);
            var comparison = System.IO.Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!string.Equals(path.TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar), root, comparison) &&
                !path.StartsWith(root + System.IO.Path.DirectorySeparatorChar, comparison))
                throw new InvalidOperationException($"Asset path '{path}' is outside the project Content folder '{root}'.");
            return path;
        }

        private static bool PathEquals(string a, string b)
        {
            var comparison = System.IO.Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            return string.Equals(System.IO.Path.GetFullPath(a).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar),
                                 System.IO.Path.GetFullPath(b).TrimEnd(System.IO.Path.DirectorySeparatorChar, System.IO.Path.AltDirectorySeparatorChar), comparison);
        }

        private static IEnumerable<ContentItem> EnumerateChildren(ContentFolder folder)
        {
            foreach (var child in folder.Children)
            {
                yield return child;
                if (child is ContentFolder childFolder)
                {
                    foreach (var descendant in EnumerateChildren(childFolder))
                        yield return descendant;
                }
            }
        }

        private static object DescribeAsset(ContentItem item)
        {
            var info = new FileInfo(item.Path);
            var asset = item as AssetItem;
            string importPath = null;
            if (item is BinaryAssetItem binaryAsset)
                binaryAsset.GetImportPath(out importPath);
            return new
            {
                path = item.Path,
                name = item.ShortName,
                kind = item.IsFolder ? "folder" : item.IsAsset ? "asset" : "file",
                type = asset?.TypeName ?? item.TypeDescription,
                id = asset?.ID,
                size = item.IsFolder || !info.Exists ? (long?)null : info.Length,
                importPath,
            };
        }

        private static object DescribePath(string path, string type)
        {
            return new { path, name = System.IO.Path.GetFileNameWithoutExtension(path), kind = Directory.Exists(path) ? "folder" : "asset", type };
        }

        private static object GetMemberPathValue(object target, string path)
        {
            var names = SplitMemberPath(path);
            for (int i = 0; i < names.Length; i++)
            {
                var name = names[i];
                var member = FindMember(target.GetType(), name, false);
                target = GetMemberValue(target, member);
                if (target == null && i != names.Length - 1)
                    throw new InvalidOperationException($"Property path '{path}' contains a null value at '{name}'.");
            }
            return target;
        }

        private static MemberInfo SetMemberPathValue(object target, string path, JToken token)
        {
            var names = SplitMemberPath(path);
            var parents = new List<(object Owner, MemberInfo Member, bool ValueType)>();
            for (int i = 0; i < names.Length - 1; i++)
            {
                var member = FindMember(target.GetType(), names[i], false);
                var child = GetMemberValue(target, member);
                if (child == null)
                    throw new InvalidOperationException($"Property path '{path}' contains a null value at '{names[i]}'.");
                parents.Add((target, member, GetMemberType(member).IsValueType));
                target = child;
            }

            var leaf = FindMember(target.GetType(), names[names.Length - 1], true);
            var value = JsonConvert.DeserializeObject(token.ToString(Formatting.None), GetMemberType(leaf), FlaxJsonSerializer.Settings);
            SetMemberValue(target, leaf, value);
            for (int i = parents.Count - 1; i >= 0; i--)
            {
                if (parents[i].ValueType)
                    SetMemberValue(parents[i].Owner, parents[i].Member, target);
                target = parents[i].Owner;
            }
            return leaf;
        }

        private static string[] SplitMemberPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new InvalidOperationException("A public asset property path is required.");
            var names = path.Split('.');
            if (names.Any(string.IsNullOrWhiteSpace))
                throw new InvalidOperationException($"Invalid asset property path '{path}'.");
            return names;
        }

        private static MemberInfo FindMember(Type type, string name, bool requireWritable)
        {
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public;
            var property = type.GetProperty(name, flags) ?? type.GetProperties(flags).FirstOrDefault(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
            if (property != null && property.GetIndexParameters().Length == 0 && property.CanRead && (!requireWritable || property.CanWrite))
                return property;
            var field = type.GetField(name, flags) ?? type.GetFields(flags).FirstOrDefault(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
            if (field != null && (!requireWritable || !field.IsInitOnly))
                return field;
            throw new InvalidOperationException($"Public {(requireWritable ? "writable " : string.Empty)}property or field '{name}' was not found on type '{type.FullName}'.");
        }

        private static Type GetMemberType(MemberInfo member)
        {
            return member is PropertyInfo property ? property.PropertyType : ((FieldInfo)member).FieldType;
        }

        private static object GetMemberValue(object target, MemberInfo member)
        {
            return member is PropertyInfo property ? property.GetValue(target) : ((FieldInfo)member).GetValue(target);
        }

        private static void SetMemberValue(object target, MemberInfo member, object value)
        {
            if (member is PropertyInfo property)
                property.SetValue(target, value);
            else
                ((FieldInfo)member).SetValue(target, value);
        }

        private void CompleteAsset(object data, bool success = true, string errorCode = null, string errorMessage = null)
        {
            if (_completed)
                return;
            _completed = true;
            var result = new
            {
                schemaVersion = 1,
                requestId = _request.RequestId,
                success,
                exitCode = success ? 0 : 6,
                data,
                errors = success ? Array.Empty<object>() : new[] { new { code = errorCode, message = errorMessage } },
            };
            WriteResult(result);
            TryWriteEvent(new { type = "result", requestId = _request.RequestId, success, exitCode = success ? 0 : 6 });
            Engine.RequestExit(success ? 0 : 1);
        }
    }
}
