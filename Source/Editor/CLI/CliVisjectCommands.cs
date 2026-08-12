// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.Content;
using FlaxEditor.Surface;
using FlaxEditor.Surface.Elements;
using FlaxEngine;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace FlaxEditor
{
    /// <summary>Typed, asset-backed Visject graph authoring for material and animation graphs.</summary>
    internal static class CliVisjectCommands
    {
        [CliCommand("visject.groups.list", Description = "List the specialized Visject node groups and archetypes.", Access = CliCommandAccess.ReadOnly)]
        public static object ListGroups()
        {
            return NodeFactory.DefaultGroups.Select(group => new
            {
                id = group.GroupID,
                name = group.Name,
                nodes = (group.Archetypes ?? Enumerable.Empty<NodeArchetype>()).Select(node => new
                {
                    id = node.TypeID,
                    title = node.Title,
                    description = node.Description,
                    signature = node.Signature,
                    flags = node.Flags.ToString(),
                    values = SafeValues(node.DefaultValues),
                }).ToArray(),
            }).ToArray();
        }

        [CliCommand("visject.asset.inspect", Description = "Inspect a material or animation graph through the Editor Visject implementation.", Access = CliCommandAccess.ReadOnly)]
        public static object Inspect([CliOption("asset", Required = true)] string asset, [CliOption("kind", Description = "material or animation")] string kind = null)
        {
            using var state = Open(asset, kind, writable: false);
            return Describe(state);
        }

        [CliCommand("visject.validate", Description = "Validate a material or animation graph by loading it through Visject.", Access = CliCommandAccess.ReadOnly)]
        public static object Validate([CliOption("asset", Required = true)] string asset, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: false);
            return new { valid = true, kind = state.Kind, assetId = state.AssetId, nodes = state.Surface.Nodes?.Count ?? 0, parameters = state.Surface.Parameters?.Count ?? 0 };
        }

        [CliCommand("visject.node.add", Description = "Add a node to a material or animation graph and persist it.", Access = CliCommandAccess.MutatesProject)]
        public static object AddNode([CliOption("asset", Required = true)] string asset, [CliOption("group", Required = true)] ushort group, [CliOption("type", Required = true)] ushort type, [CliOption("x")] float x = 0, [CliOption("y")] float y = 0, [CliOption("values-json")] JToken valuesJson = null, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: true);
            var values = ParseValues(valuesJson);
            var node = state.Surface.Context.SpawnNode(group, type, new Float2(x, y), values);
            if (node == null) throw new InvalidOperationException($"Visject node group={group}, type={type} is not valid for {state.Kind}.");
            state.Save();
            return DescribeNode(node);
        }

        [CliCommand("visject.node.remove", Description = "Remove a node from a material or animation graph.", Access = CliCommandAccess.Destructive)]
        public static object RemoveNode([CliOption("asset", Required = true)] string asset, [CliOption("node", Required = true)] uint node, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: true);
            var target = state.Surface.FindNode(node) ?? throw new InvalidOperationException($"Visject node '{node}' was not found.");
            state.Surface.Delete(target, false);
            state.Save();
            return new { removed = true, node };
        }

        [CliCommand("visject.node.set", Description = "Set one serialized Visject node value and persist it.", Access = CliCommandAccess.MutatesProject)]
        public static object SetNodeValue([CliOption("asset", Required = true)] string asset, [CliOption("node", Required = true)] uint node, [CliOption("index", Required = true)] int index, [CliOption("value-json", Required = true)] JToken valueJson, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: true);
            var target = state.Surface.FindNode(node) ?? throw new InvalidOperationException($"Visject node '{node}' was not found.");
            if (target.Values == null || index < 0 || index >= target.Values.Length) throw new ArgumentOutOfRangeException(nameof(index));
            target.SetValue(index, ParseValue(valueJson));
            state.Save();
            return DescribeNode(target);
        }

        [CliCommand("visject.connect", Description = "Connect an output box to an input box in a material or animation graph.", Access = CliCommandAccess.MutatesProject)]
        public static object Connect([CliOption("asset", Required = true)] string asset, [CliOption("from-node", Required = true)] uint fromNode, [CliOption("from-box", Required = true)] int fromBox, [CliOption("to-node", Required = true)] uint toNode, [CliOption("to-box", Required = true)] int toBox, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: true);
            var source = RequireBox(state.Surface, fromNode, fromBox);
            var target = RequireBox(state.Surface, toNode, toBox);
            if (!source.CanConnectWith(target))
                throw new InvalidOperationException($"Visject boxes {fromNode}:{fromBox} and {toNode}:{toBox} cannot be connected (direction or value type mismatch).");
            source.CreateConnection(target);
            state.Surface.MarkAsEdited(true);
            state.Save();
            return new { connected = true, fromNode, fromBox, toNode, toBox };
        }

        [CliCommand("visject.disconnect", Description = "Disconnect two Visject boxes in a material or animation graph.", Access = CliCommandAccess.MutatesProject)]
        public static object Disconnect([CliOption("asset", Required = true)] string asset, [CliOption("from-node", Required = true)] uint fromNode, [CliOption("from-box", Required = true)] int fromBox, [CliOption("to-node", Required = true)] uint toNode, [CliOption("to-box", Required = true)] int toBox, [CliOption("kind")] string kind = null)
        {
            using var state = Open(asset, kind, writable: true);
            var source = RequireBox(state.Surface, fromNode, fromBox);
            var target = RequireBox(state.Surface, toNode, toBox);
            if (!source.AreConnected(target))
                throw new InvalidOperationException($"Visject boxes {fromNode}:{fromBox} and {toNode}:{toBox} are not connected.");
            source.BreakConnection(target);
            state.Surface.MarkAsEdited(true);
            state.Save();
            return new { disconnected = true, fromNode, fromBox, toNode, toBox };
        }

        private static SurfaceState Open(string value, string kind, bool writable)
        {
            var item = ResolveAssetItem(value, writable);
            if (writable && Editor.Instance.Windows.FindEditor(item) is FlaxEditor.Windows.Assets.AssetEditorWindow)
                throw new InvalidOperationException($"Graph asset '{item.Path}' is open in an asset editor. Save or close that editor before mutating it through the CLI.");
            var normalized = (kind ?? string.Empty).Trim().ToLowerInvariant();
            if (string.IsNullOrEmpty(normalized)) normalized = item.TypeName?.Contains("AnimationGraph", StringComparison.OrdinalIgnoreCase) == true ? "animation" : "material";
            var owner = new GraphOwner(item, normalized, new Undo());
            VisjectSurface surface;
            switch (normalized)
            {
            case "material":
                // Resolve and load through the authoritative AssetItem. Loading
                // the same physical file by differently normalized path strings
                // can register a duplicate asset and rewrite its on-disk ID.
                owner.Material = item.LoadAsync() as Material ?? throw new InvalidOperationException("The asset is not a Material.");
                if (owner.Material.WaitForLoaded())
                    throw new InvalidOperationException($"Material '{item.Path}' failed to load from disk.");
                // Newly created materials do not contain a surface chunk yet;
                // ask the native asset to materialize Flax's default graph.
                owner.SurfaceData = owner.Material.LoadSurface(true);
                surface = new MaterialSurface(owner, null, owner.Undo);
                break;
            case "animation":
            case "animationgraph":
                owner.Animation = item.LoadAsync() as AnimationGraph ?? throw new InvalidOperationException("The asset is not an AnimationGraph.");
                if (owner.Animation.WaitForLoaded())
                    throw new InvalidOperationException($"Animation Graph '{item.Path}' failed to load from disk.");
                owner.SurfaceData = owner.Animation.LoadSurface();
                surface = new AnimGraphSurface(owner, null, owner.Undo);
                break;
            default: throw new InvalidOperationException("Visject kind must be material or animation.");
            }
            if (surface.Load()) { surface.Dispose(); throw new InvalidOperationException("The Visject graph failed to load."); }
            return new SurfaceState(owner, surface, writable);
        }

        private static AssetItem ResolveAssetItem(string value, bool writable)
        {
            AssetItem item;
            if (Guid.TryParse(value, out var id))
            {
                item = Editor.Instance.ContentDatabase.FindAsset(id);
                if (item == null && FlaxEngine.Content.GetAssetInfo(id, out var info) && !string.IsNullOrEmpty(info.Path))
                    item = FindAssetItem(info.Path);
            }
            else
            {
                var projectRoot = Path.GetFullPath(Globals.ProjectFolder);
                var contentRoot = Path.GetFullPath(Globals.ProjectContentFolder);
                var normalized = value.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
                string path;
                if (Path.IsPathRooted(normalized))
                {
                    path = Path.GetFullPath(normalized);
                }
                else if (normalized.Equals("Content", StringComparison.OrdinalIgnoreCase) || normalized.StartsWith("Content" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                {
                    path = Path.GetFullPath(normalized, projectRoot);
                }
                else
                {
                    path = Path.GetFullPath(normalized, contentRoot);
                }
                item = FindAssetItem(path);
            }
            if (item == null || item.ID == Guid.Empty)
                throw new FileNotFoundException($"Graph asset '{value}' was not found in the Content database.");
            if (writable && !IsProjectContentPath(item.Path))
                throw new InvalidOperationException($"Graph asset mutations are confined to project Content. Resolved path: '{item.Path}'.");
            return item;
        }

        private static AssetItem FindAssetItem(string path)
        {
            var database = Editor.Instance.ContentDatabase;
            var item = database.Find(path) as AssetItem;
            if (item != null)
                return item;
            var current = Directory.Exists(path) ? path : Path.GetDirectoryName(path);
            while (!string.IsNullOrEmpty(current))
            {
                var parent = database.Find(current);
                if (parent != null)
                {
                    database.RefreshFolder(parent, true);
                    break;
                }
                current = Path.GetDirectoryName(current);
            }
            return database.Find(path) as AssetItem;
        }

        private static bool IsProjectContentPath(string path)
        {
            var root = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            path = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            return string.Equals(path, root, comparison) || path.StartsWith(root + Path.DirectorySeparatorChar, comparison);
        }

        private static bool PathEquals(string a, string b)
        {
            var comparison = Path.DirectorySeparatorChar == '\\' ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            return string.Equals(Path.GetFullPath(a).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                                 Path.GetFullPath(b).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar), comparison);
        }

        private static object[] ParseValues(JToken json)
        {
            if (json == null || json.Type == JTokenType.Null) return null;
            if (json.Type == JTokenType.Array) return json.Children().Select(ParseValue).ToArray();
            if (json.Type == JTokenType.String) return ParseValues(JToken.Parse(json.Value<string>()));
            throw new ArgumentException("Visject values-json must be an array or a JSON string containing an array.");
        }

        private static object ParseValue(JToken json)
        {
            if (json == null || json.Type == JTokenType.Null) return null;
            if (json.Type == JTokenType.String)
            {
                var value = json.Value<string>();
                if (Guid.TryParse(value, out var guid))
                    return guid;
                try
                {
                    var nested = JToken.Parse(value);
                    if (nested.Type == JTokenType.String)
                    {
                        value = nested.Value<string>();
                        return Guid.TryParse(value, out guid) ? guid : value;
                    }
                    return ParseValue(nested);
                }
                catch (JsonReaderException)
                {
                    return value;
                }
            }
            return json.Type switch
            {
                JTokenType.Float => json.Value<float>(),
                JTokenType.Integer => json.Value<long>() is var integer && integer <= int.MaxValue && integer >= int.MinValue ? (object)(int)integer : integer,
                JTokenType.Boolean => json.Value<bool>(),
                _ => json.ToObject<object>(),
            };
        }

        private static Box RequireBox(VisjectSurface surface, uint nodeId, int boxId)
        {
            var node = surface.FindNode(nodeId) ?? throw new InvalidOperationException($"Visject node '{nodeId}' was not found.");
            return node.GetBox(boxId) ?? throw new InvalidOperationException($"Visject box '{boxId}' was not found on node '{nodeId}'.");
        }

        private static object Describe(SurfaceState state) => new
        {
            kind = state.Kind,
            assetId = state.AssetId,
            path = state.Path,
            dataBytes = state.Owner.SurfaceData?.Length ?? 0,
            nodes = (state.Surface.Nodes ?? new List<SurfaceNode>()).Select(DescribeNode).ToArray(),
            parameters = state.Surface.Parameters?.Select(x => new { x.ID, x.Name, type = x.Type.ToString() }).ToArray(),
        };

        private static object DescribeNode(SurfaceNode node) => new
        {
            id = node.ID,
            group = node.GroupArchetype.GroupID,
            type = node.Archetype.TypeID,
            nodeType = node.Type,
            title = node.Title,
            x = node.Location.X,
            y = node.Location.Y,
            values = SafeValues(node.Values),
            boxes = node.Elements.OfType<Box>().Select(x => new { id = x.ID, text = x.Text, isOutput = x.IsOutput, connections = x.Connections.Select(c => new { node = c.ParentNode.ID, box = c.ID }).ToArray() }).ToArray(),
        };

        private static object[] SafeValues(object[] values) => values?.Select(SafeValue).ToArray();

        private static object SafeValue(object value)
        {
            if (value == null || value is string || value is bool || value is Guid || value is byte[] || value is int || value is uint || value is long || value is ulong || value is float || value is double || value is decimal)
                return value;
            if (value is Array array)
            {
                var result = new object[array.Length];
                for (var i = 0; i < array.Length; i++) result[i] = SafeValue(array.GetValue(i));
                return result;
            }
            return value.ToString();
        }

        private sealed class GraphOwner : IVisjectSurfaceOwner
        {
            public readonly AssetItem Item;
            public readonly Guid AssetId;
            public readonly string Path;
            public readonly string Kind;
            private readonly Undo _undo;
            public Material Material;
            public AnimationGraph Animation;
            public byte[] SurfaceData { get; set; }
            public Asset SurfaceAsset => Material ?? (Asset)Animation;
            public string SurfaceName => Path;
            public Undo Undo => _undo;
            public VisjectSurfaceContext ParentContext => null;
            public GraphOwner(AssetItem item, string kind, Undo undo) { Item = item; AssetId = item.ID; Path = item.Path; Kind = kind; _undo = undo; }
            public void OnContextCreated(VisjectSurfaceContext context) { }
            public void OnSurfaceEditedChanged() { }
            public void OnSurfaceGraphEdited() { }
            public void OnSurfaceClose() { }
            public void Save()
            {
                if (Editor.Instance.ContentEditing.FastTempAssetClone(Path, out var backupPath))
                    throw new IOException($"Failed to create a rollback copy for graph asset '{Path}'.");
                try
                {
                    if (Material != null)
                    {
                        if (Material.SaveSurface(SurfaceData, Material.Info))
                            throw new IOException("Material surface save failed.");
                    }
                    else if (Animation != null)
                    {
                        if (Animation.SaveSurface(SurfaceData))
                            throw new IOException("Animation graph surface save failed.");
                    }
                    VerifyPersistedAsset();
                }
                catch (Exception ex)
                {
                    var rollbackFailed = Editor.Instance.ContentEditing.CloneAssetFile(backupPath, Path, AssetId);
                    Item.Reload();
                    var rollbackAsset = Item.LoadAsync();
                    var rollbackLoadFailed = rollbackAsset == null || rollbackAsset.WaitForLoaded();
                    if (rollbackFailed || rollbackLoadFailed)
                        throw new IOException($"Graph save failed and the rollback copy could not be restored for '{Path}'.", ex);
                    throw new IOException($"Graph save failed persistence validation; the original asset was restored for '{Path}'.", ex);
                }
            }

            private void VerifyPersistedAsset()
            {
                Item.Reload();
                var asset = Item.LoadAsync();
                if (asset == null || asset.WaitForLoaded())
                    throw new IOException($"Graph asset '{Path}' failed to reload after saving.");
                if (asset.ID != AssetId)
                    throw new IOException($"Graph asset '{Path}' changed ID while saving. Expected {AssetId}, loaded {asset.ID}.");
                if (!PathEquals(asset.Path, Path))
                    throw new IOException($"Graph asset '{Path}' reloaded from an unexpected path '{asset.Path}'.");
                if (Material != null)
                {
                    Material = asset as Material ?? throw new IOException($"Graph asset '{Path}' reloaded as '{asset.GetType().FullName}' instead of Material.");
                    SurfaceData = Material.LoadSurface(false);
                }
                else
                {
                    Animation = asset as AnimationGraph ?? throw new IOException($"Graph asset '{Path}' reloaded as '{asset.GetType().FullName}' instead of AnimationGraph.");
                    SurfaceData = Animation.LoadSurface();
                }
                if (SurfaceData == null || SurfaceData.Length == 0)
                    throw new IOException($"Graph asset '{Path}' reloaded without persisted surface data.");
            }
        }

        private sealed class SurfaceState(GraphOwner owner, VisjectSurface surface, bool writable) : IDisposable
        {
            public readonly GraphOwner Owner = owner;
            public readonly VisjectSurface Surface = surface;
            public readonly bool Writable = writable;
            public string Kind => owner.Kind;
            public Guid AssetId => owner.AssetId;
            public string Path => owner.Path;
            public void Save() { if (!Writable) throw new InvalidOperationException("The graph was opened read-only."); if (Surface.Save()) throw new IOException("Visject surface serialization failed."); Owner.Save(); }
            public void Dispose() { Surface.Dispose(); Owner.Undo?.Dispose(); }
        }
    }
}
