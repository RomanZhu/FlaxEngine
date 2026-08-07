// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
            if (!FlaxEngine.Content.GetAssetInfo(ResolvePathOrId(value), out var info) || info.ID == Guid.Empty)
                throw new FileNotFoundException($"Graph asset '{value}' was not found.");
            var normalized = (kind ?? string.Empty).Trim().ToLowerInvariant();
            if (string.IsNullOrEmpty(normalized)) normalized = info.TypeName?.Contains("AnimationGraph", StringComparison.OrdinalIgnoreCase) == true ? "animation" : "material";
            var owner = new GraphOwner(info.ID, info.Path, normalized, new Undo());
            VisjectSurface surface;
            switch (normalized)
            {
            case "material":
                // Load by the resolved path rather than only by GUID. Imported
                // engine assets can legitimately retain a source GUID; loading
                // by ID would then select the engine copy and make project saves
                // target the wrong (often read-only) asset.
                owner.Material = FlaxEngine.Content.LoadAsync<Material>(info.Path) ?? throw new InvalidOperationException("The asset is not a Material.");
                // Keep failed/empty newly-created assets editable: LoadSurface(true)
                // below can materialize the native default graph before saving.
                owner.Material.WaitForLoaded();
                // Newly created materials do not contain a surface chunk yet;
                // ask the native asset to materialize Flax's default graph.
                owner.SurfaceData = owner.Material.LoadSurface(true);
                surface = new MaterialSurface(owner, null, owner.Undo);
                break;
            case "animation":
            case "animationgraph":
                owner.Animation = FlaxEngine.Content.LoadAsync<AnimationGraph>(info.Path) ?? throw new InvalidOperationException("The asset is not an AnimationGraph.");
                owner.Animation.WaitForLoaded();
                owner.SurfaceData = owner.Animation.LoadSurface();
                surface = new AnimGraphSurface(owner, null, owner.Undo);
                break;
            default: throw new InvalidOperationException("Visject kind must be material or animation.");
            }
            if (surface.Load()) { surface.Dispose(); throw new InvalidOperationException("The Visject graph failed to load."); }
            return new SurfaceState(owner, surface, writable);
        }

        private static string ResolvePathOrId(string value)
        {
            if (Guid.TryParse(value, out var id)) return id.ToString();
            var root = Path.GetFullPath(Globals.ProjectFolder);
            var path = Path.IsPathRooted(value) ? Path.GetFullPath(value) : Path.GetFullPath(value, root);
            if (!File.Exists(path) && !path.StartsWith(Path.Combine(root, "Content") + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                path = Path.GetFullPath(Path.Combine(root, "Content", value));
            return path;
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
            if (json.Type == JTokenType.String) return JsonConvert.DeserializeObject<object>(json.Value<string>());
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
            public GraphOwner(Guid id, string path, string kind, Undo undo) { AssetId = id; Path = path; Kind = kind; _undo = undo; }
            public void OnContextCreated(VisjectSurfaceContext context) { }
            public void OnSurfaceEditedChanged() { }
            public void OnSurfaceGraphEdited() { }
            public void OnSurfaceClose() { }
            public void Save()
            {
                if (Material != null) { if (Material.SaveSurface(SurfaceData, Material.Info)) throw new IOException("Material surface save failed."); }
                else if (Animation != null) { if (Animation.SaveSurface(SurfaceData)) throw new IOException("Animation graph surface save failed."); }
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
