// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using FlaxEditor.Content;
using FlaxEditor.Windows.Assets;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;
using Object = FlaxEngine.Object;

namespace FlaxEditor
{
    /// <summary>
    /// Describes one operation in an atomic prefab-asset edit batch.
    /// </summary>
    public sealed class CliPrefabAssetOperation
    {
        [JsonProperty("action")]
        public string Action { get; set; }

        [JsonProperty("actor")]
        public string Actor { get; set; }

        [JsonProperty("parent")]
        public string Parent { get; set; }

        [JsonProperty("type")]
        public string Type { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("component")]
        public string Component { get; set; }

        [JsonProperty("index")]
        public int Index { get; set; }

        [JsonProperty("property")]
        public string Property { get; set; }

        [JsonProperty("value")]
        public JToken Value { get; set; }

        [JsonProperty("referenceActor")]
        public string ReferenceActor { get; set; }

        [JsonProperty("referenceComponent")]
        public string ReferenceComponent { get; set; }

        [JsonProperty("referenceAsset")]
        public string ReferenceAsset { get; set; }

        [JsonProperty("clear")]
        public bool Clear { get; set; }

        [JsonProperty("active")]
        public bool? Active { get; set; }

        [JsonProperty("layer")]
        public int? Layer { get; set; }

        [JsonProperty("tags")]
        public string[] Tags { get; set; }

        [JsonProperty("position")]
        public Vector3? Position { get; set; }

        [JsonProperty("rotation")]
        public Float3? Rotation { get; set; }

        [JsonProperty("scale")]
        public Float3? Scale { get; set; }
    }

    /// <summary>
    /// Edits Prefab assets through a transient off-scene hierarchy. These commands never
    /// load, create, dirty, or save a gameplay Scene.
    /// </summary>
    public static class CliPrefabAssetCommands
    {
        [CliCommand("prefab-assets.hierarchy", Description = "Read a Prefab asset hierarchy without opening or modifying a Scene.", Access = CliCommandAccess.ReadOnly)]
        public static object Hierarchy(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("max-depth")] int maxDepth = 32)
        {
            if (maxDepth < 0 || maxDepth > 256)
                throw new ArgumentOutOfRangeException(nameof(maxDepth), "Prefab hierarchy depth must be between 0 and 256.");
            using (var session = Open(prefab))
            {
                return new
                {
                    prefabId = session.Asset.ID,
                    path = session.Asset.Path,
                    hierarchy = DescribeTree(session.Root, session.Root, 0, maxDepth),
                    sceneTouched = false,
                };
            }
        }

        [CliCommand("prefab-assets.actor.get", Description = "Read an Actor inside a Prefab asset without opening a Scene.", Access = CliCommandAccess.ReadOnly)]
        public static object GetActor(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".")
        {
            using (var session = Open(prefab))
                return Result(session, DescribeActorDetails(session.Root, session.RequireActor(actor)), false);
        }

        [CliCommand("prefab-assets.actor.add", Description = "Add an Actor directly to a Prefab asset without opening a Scene.", Access = CliCommandAccess.MutatesProject)]
        public static object AddActor(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("type")] string type = "FlaxEngine.EmptyActor",
            [CliOption("name")] string name = null,
            [CliOption("parent")] string parent = ".",
            [CliOption("position")] Vector3? position = null,
            [CliOption("rotation")] Float3? rotation = null,
            [CliOption("scale")] Float3? scale = null)
        {
            using (var session = Open(prefab, true))
            {
                var result = session.AddActor(type, name, parent, position, rotation, scale);
                session.Save();
                return Result(session, DescribeActorDetails(session.Root, result), true);
            }
        }

        [CliCommand("prefab-assets.actor.set", Description = "Set authored Actor state or one public property directly in a Prefab asset.", Access = CliCommandAccess.MutatesProject)]
        public static object SetActor(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("name")] string name = null,
            [CliOption("parent")] string parent = null,
            [CliOption("active")] bool? active = null,
            [CliOption("layer")] int? layer = null,
            [CliOption("tags")] string[] tags = null,
            [CliOption("position")] Vector3? position = null,
            [CliOption("rotation")] Float3? rotation = null,
            [CliOption("scale")] Float3? scale = null,
            [CliOption("property")] string property = null,
            [CliOption("value")] JToken value = null)
        {
            using (var session = Open(prefab, true))
            {
                var result = session.SetActor(actor, name, parent, active, layer, tags,
                    position, rotation, scale, property, value);
                session.Save();
                return Result(session, DescribeActorDetails(session.Root, result), true);
            }
        }

        [CliCommand("prefab-assets.actor.delete", Description = "Delete an Actor hierarchy directly from a Prefab asset.", Access = CliCommandAccess.Destructive)]
        public static object DeleteActor(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor", Required = true)] string actor)
        {
            using (var session = Open(prefab, true))
            {
                var value = session.RequireActor(actor);
                if (value == session.Root)
                    throw new InvalidOperationException("The root Actor of a Prefab asset cannot be deleted.");
                var removed = DescribeActor(session.Root, value);
                Object.Destroy(ref value);
                session.Save();
                return Result(session, new { actor = removed, deleted = true }, true);
            }
        }

        [CliCommand("prefab-assets.component.get", Description = "Read a Script component inside a Prefab asset.", Access = CliCommandAccess.ReadOnly)]
        public static object GetComponent(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("component", Required = true)] string component = null,
            [CliOption("index")] int index = 0,
            [CliOption("property")] string property = null)
        {
            using (var session = Open(prefab))
            {
                var value = session.RequireComponent(session.RequireActor(actor), component, index);
                object data;
                if (string.IsNullOrWhiteSpace(property))
                    data = new { component = DescribeScript(session.Root, value), properties = JToken.Parse(FlaxJsonSerializer.Serialize(value)) };
                else
                {
                    var member = RequirePublicMember(value, property, false);
                    data = new { component = DescribeScript(session.Root, value), property = member.Name, value = SerializeMemberValue(GetMemberValue(member, value), GetMemberType(member)) };
                }
                return Result(session, data, false);
            }
        }

        [CliCommand("prefab-assets.component.add", Description = "Add a Script component directly to a Prefab asset.", Access = CliCommandAccess.MutatesProject)]
        public static object AddComponent(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("type", Required = true)] string type = null)
        {
            using (var session = Open(prefab, true))
            {
                var value = session.AddComponent(actor, type);
                session.Save();
                return Result(session, DescribeScript(session.Root, value), true);
            }
        }

        [CliCommand("prefab-assets.component.set", Description = "Set a public Script field or property directly in a Prefab asset.", Access = CliCommandAccess.MutatesProject)]
        public static object SetComponent(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("component", Required = true)] string component = null,
            [CliOption("index")] int index = 0,
            [CliOption("property", Required = true)] string property = null,
            [CliOption("value", Required = true)] JToken value = null)
        {
            using (var session = Open(prefab, true))
            {
                var script = session.SetComponent(actor, component, index, property, value);
                session.Save();
                return Result(session, DescribeScript(session.Root, script), true);
            }
        }

        [CliCommand("prefab-assets.component.remove", Description = "Remove a Script component directly from a Prefab asset.", Access = CliCommandAccess.Destructive)]
        public static object RemoveComponent(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("component", Required = true)] string component = null,
            [CliOption("index")] int index = 0)
        {
            using (var session = Open(prefab, true))
            {
                var value = session.RequireComponent(session.RequireActor(actor), component, index);
                var removed = DescribeScript(session.Root, value);
                Object.Destroy(ref value);
                session.Save();
                return Result(session, new { component = removed, deleted = true }, true);
            }
        }

        [CliCommand("prefab-assets.reference.set", Description = "Wire an internal object or external asset reference directly in a Prefab asset.", Access = CliCommandAccess.MutatesProject)]
        public static object SetReference(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("actor")] string actor = ".",
            [CliOption("component")] string component = null,
            [CliOption("index")] int index = 0,
            [CliOption("property", Required = true)] string property = null,
            [CliOption("reference-actor")] string referenceActor = null,
            [CliOption("reference-component")] string referenceComponent = null,
            [CliOption("reference-asset")] string referenceAsset = null,
            [CliOption("clear")] bool clear = false)
        {
            using (var session = Open(prefab, true))
            {
                var data = session.SetReference(actor, component, index, property,
                    referenceActor, referenceComponent, referenceAsset, clear);
                session.Save();
                return Result(session, data, true);
            }
        }

        [CliCommand("prefab-assets.batch", Description = "Atomically apply prefab hierarchy, Actor, component, and reference operations without opening a Scene.", Access = CliCommandAccess.Destructive)]
        public static object Batch(
            [CliOption("prefab", Required = true)] string prefab,
            [CliOption("operations", Required = true)] CliPrefabAssetOperation[] operations,
            [CliOption("verify-reload")] bool verifyReload = true)
        {
            if (operations == null || operations.Length == 0)
                throw new ArgumentException("At least one prefab-asset operation is required.", nameof(operations));
            object[] results;
            Guid prefabId;
            string path;
            using (var session = Open(prefab, true))
            {
                prefabId = session.Asset.ID;
                path = session.Asset.Path;
                results = new object[operations.Length];
                for (var i = 0; i < operations.Length; i++)
                    results[i] = session.Execute(operations[i] ?? throw new ArgumentException($"Prefab operation {i} is null."));
                session.Save();
            }

            object verification = null;
            if (verifyReload)
            {
                var asset = FlaxEngine.Content.Load<Prefab>(prefabId) ?? throw new InvalidOperationException("The saved Prefab could not be reloaded.");
                asset.Reload();
                if (asset.WaitForLoaded())
                    throw new InvalidOperationException("The saved Prefab failed reload verification.");
                using (var session = Open(prefab))
                    verification = DescribeTree(session.Root, session.Root, 0, 256);
            }
            return new { prefabId, path, saved = true, verified = verifyReload, results, hierarchy = verification, sceneTouched = false };
        }

        private sealed class PrefabSession : IDisposable
        {
            public readonly Prefab Asset;
            public Actor Root;

            public PrefabSession(Prefab asset)
            {
                Asset = asset;
                Root = PrefabManager.SpawnPrefab(asset, null) ?? throw new InvalidOperationException($"Failed to load Prefab hierarchy '{asset.Path}'.");
            }

            public void Dispose()
            {
                var root = Root;
                Root = null;
                Object.Destroy(ref root);
            }

            public void Save()
            {
                Editor.Instance.Prefabs.ApplyAll(Root);
            }

            public Actor RequireActor(string selector)
            {
                if (string.IsNullOrWhiteSpace(selector) || selector == "." || selector == "/" ||
                    string.Equals(selector, Root.Name, StringComparison.Ordinal))
                    return Root;
                if (Guid.TryParse(selector, out var id))
                {
                    var byId = Enumerate(Root).FirstOrDefault(x => x.ID == id || x.PrefabObjectID == id);
                    return byId ?? throw new KeyNotFoundException($"Actor '{selector}' was not found in Prefab '{Asset.Path}'.");
                }

                var parts = selector.Replace('\\', '/').Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
                var current = Root;
                var first = 0;
                if (parts.Length != 0 && string.Equals(parts[0], Root.Name, StringComparison.Ordinal))
                    first = 1;
                for (var partIndex = first; partIndex < parts.Length; partIndex++)
                {
                    var matches = Enumerable.Range(0, current.ChildrenCount)
                        .Select(current.GetChild)
                        .Where(x => string.Equals(x.Name, parts[partIndex], StringComparison.Ordinal))
                        .ToArray();
                    if (matches.Length == 0)
                        throw new KeyNotFoundException($"Actor path '{selector}' was not found in Prefab '{Asset.Path}'.");
                    if (matches.Length > 1)
                        throw new InvalidOperationException($"Actor path '{selector}' is ambiguous at '{parts[partIndex]}'; use the stable Actor or Prefab object ID.");
                    current = matches[0];
                }
                return current;
            }

            public Script RequireComponent(Actor actor, string selector, int index)
            {
                if (string.IsNullOrWhiteSpace(selector))
                    throw new ArgumentException("A component type or ID is required.", nameof(selector));
                if (index < 0)
                    throw new ArgumentOutOfRangeException(nameof(index));
                if (Guid.TryParse(selector, out var id))
                {
                    var byId = actor.Scripts.FirstOrDefault(x => x.ID == id || x.PrefabObjectID == id);
                    return byId ?? throw new KeyNotFoundException($"Component '{selector}' is not attached to Actor '{ActorPath(Root, actor)}'.");
                }
                var matches = actor.Scripts.Where(x =>
                    string.Equals(x.TypeName, selector, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(x.GetType().Name, selector, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(x.GetType().FullName, selector, StringComparison.OrdinalIgnoreCase)).ToArray();
                if (index >= matches.Length)
                    throw new KeyNotFoundException($"Component '{selector}' at index {index} is not attached to Actor '{ActorPath(Root, actor)}'.");
                return matches[index];
            }

            public Actor AddActor(string type, string name, string parent, Vector3? position, Float3? rotation, Float3? scale)
            {
                var actorType = RequireType(type, typeof(Actor));
                var result = (Actor)Object.New(actorType);
                if (!string.IsNullOrWhiteSpace(name)) result.Name = name;
                if (position.HasValue) result.LocalPosition = position.Value;
                if (rotation.HasValue) result.LocalEulerAngles = rotation.Value;
                if (scale.HasValue) result.LocalScale = scale.Value;
                result.SetParent(RequireActor(parent), false, false);
                return result;
            }

            public Actor SetActor(string actor, string name, string parent, bool? active, int? layer,
                string[] tags, Vector3? position, Float3? rotation, Float3? scale, string property, JToken value)
            {
                var result = RequireActor(actor);
                if (name != null) result.Name = name;
                if (parent != null)
                {
                    if (result == Root)
                        throw new InvalidOperationException("The root Actor of a Prefab asset cannot be reparented.");
                    var newParent = RequireActor(parent);
                    for (var current = newParent; current != null; current = current.Parent)
                        if (current == result)
                            throw new InvalidOperationException("An Actor cannot be parented below itself.");
                    result.SetParent(newParent, true, false);
                }
                if (active.HasValue) result.IsActive = active.Value;
                if (layer.HasValue)
                {
                    if (layer.Value < 0 || layer.Value > 31)
                        throw new ArgumentOutOfRangeException(nameof(layer), "Actor layer must be between 0 and 31.");
                    result.Layer = layer.Value;
                }
                if (tags != null)
                {
                    CliAssetPersistence.PersistTags(tags);
                    result.Tags = tags.Select(Tags.Get).ToArray();
                }
                if (position.HasValue) result.LocalPosition = position.Value;
                if (rotation.HasValue) result.LocalEulerAngles = rotation.Value;
                if (scale.HasValue) result.LocalScale = scale.Value;
                if (!string.IsNullOrWhiteSpace(property))
                {
                    if (value == null)
                        throw new ArgumentException("Actor property assignment requires a value.", nameof(value));
                    var member = RequirePublicMember(result, property, true);
                    SetMemberValue(member, result, ConvertMemberValue(value, GetMemberType(member)));
                }
                else if (value != null)
                {
                    throw new ArgumentException("An Actor value was supplied without a property name.", nameof(value));
                }
                return result;
            }

            public Script AddComponent(string actor, string type)
            {
                var scriptType = RequireType(type, typeof(Script));
                return RequireActor(actor).AddScript(scriptType) ?? throw new InvalidOperationException($"Failed to add Script '{type}'.");
            }

            public Script SetComponent(string actor, string component, int index, string property, JToken value)
            {
                var result = RequireComponent(RequireActor(actor), component, index);
                var member = RequirePublicMember(result, property, true);
                SetMemberValue(member, result, ConvertMemberValue(value, GetMemberType(member)));
                return result;
            }

            public object SetReference(string actor, string component, int index, string property,
                string referenceActor, string referenceComponent, string referenceAsset, bool clear)
            {
                var targetActor = RequireActor(actor);
                object target = string.IsNullOrWhiteSpace(component) ? targetActor : RequireComponent(targetActor, component, index);
                var member = RequirePublicMember(target, property, true);
                var memberType = GetMemberType(member);
                var isJsonAssetReference = memberType.IsGenericType && memberType.GetGenericTypeDefinition() == typeof(JsonAssetReference<>);
                if (!typeof(Object).IsAssignableFrom(memberType) && !isJsonAssetReference)
                    throw new ArgumentException($"Member '{member.Name}' is not a Flax object reference.", nameof(property));

                var choices = (clear ? 1 : 0) + (!string.IsNullOrWhiteSpace(referenceActor) ? 1 : 0) +
                              (!string.IsNullOrWhiteSpace(referenceAsset) ? 1 : 0);
                if (choices != 1)
                    throw new ArgumentException("Choose exactly one of --clear, --reference-actor, or --reference-asset.");
                object assignedValue = null;
                Object reference = null;
                if (!clear && !string.IsNullOrWhiteSpace(referenceActor))
                {
                    if (isJsonAssetReference)
                        throw new ArgumentException($"Member '{member.Name}' accepts an asset reference, not an Actor or Script reference.", nameof(referenceActor));
                    var sourceActor = RequireActor(referenceActor);
                    reference = string.IsNullOrWhiteSpace(referenceComponent)
                        ? sourceActor
                        : RequireComponent(sourceActor, referenceComponent, 0);
                    assignedValue = reference;
                }
                else if (!clear)
                {
                    assignedValue = isJsonAssetReference
                        ? ConvertMemberValue(new JValue(referenceAsset), memberType)
                        : ResolveAssetObject(referenceAsset, memberType);
                    reference = assignedValue as Object;
                    if (isJsonAssetReference)
                        reference = memberType.GetField("Asset")?.GetValue(assignedValue) as Object;
                }
                if (reference != null && !memberType.IsAssignableFrom(reference.GetType()))
                {
                    if (!isJsonAssetReference)
                        throw new ArgumentException($"Reference '{reference.ID}' is '{reference.GetType().FullName}', not assignable to '{memberType.FullName}'.");
                }
                if (clear && isJsonAssetReference)
                    assignedValue = Activator.CreateInstance(memberType);
                SetMemberValue(member, target, assignedValue);
                return new
                {
                    actor = DescribeActor(Root, targetActor),
                    component = target is Script script ? DescribeScript(Root, script) : null,
                    property = member.Name,
                    reference = reference?.ID ?? Guid.Empty,
                    cleared = reference == null,
                };
            }

            public object Execute(CliPrefabAssetOperation operation)
            {
                var action = (operation.Action ?? string.Empty).Trim().ToLowerInvariant();
                switch (action)
                {
                case "actor.get":
                    return DescribeActorDetails(Root, RequireActor(operation.Actor));
                case "actor.add":
                    return DescribeActorDetails(Root, AddActor(operation.Type ?? "FlaxEngine.EmptyActor", operation.Name,
                        operation.Parent ?? ".", operation.Position, operation.Rotation, operation.Scale));
                case "actor.set":
                    return DescribeActorDetails(Root, SetActor(operation.Actor, operation.Name, operation.Parent,
                        operation.Active, operation.Layer, operation.Tags, operation.Position, operation.Rotation,
                        operation.Scale, operation.Property, operation.Value));
                case "actor.delete":
                {
                    var actor = RequireActor(operation.Actor);
                    if (actor == Root) throw new InvalidOperationException("The root Actor of a Prefab asset cannot be deleted.");
                    var description = DescribeActor(Root, actor);
                    Object.Destroy(ref actor);
                    return new { actor = description, deleted = true };
                }
                case "component.get":
                {
                    var script = RequireComponent(RequireActor(operation.Actor), operation.Component, operation.Index);
                    return new { component = DescribeScript(Root, script), properties = JToken.Parse(FlaxJsonSerializer.Serialize(script)) };
                }
                case "component.add":
                    return DescribeScript(Root, AddComponent(operation.Actor, operation.Type));
                case "component.set":
                    return DescribeScript(Root, SetComponent(operation.Actor, operation.Component, operation.Index, operation.Property, operation.Value));
                case "component.remove":
                {
                    var script = RequireComponent(RequireActor(operation.Actor), operation.Component, operation.Index);
                    var description = DescribeScript(Root, script);
                    Object.Destroy(ref script);
                    return new { component = description, deleted = true };
                }
                case "reference.set":
                    return SetReference(operation.Actor, operation.Component, operation.Index, operation.Property,
                        operation.ReferenceActor, operation.ReferenceComponent, operation.ReferenceAsset, operation.Clear);
                default:
                    throw new InvalidOperationException($"Unsupported prefab-asset operation '{operation.Action}'.");
                }
            }
        }

        private static PrefabSession Open(string prefab, bool writable = false)
        {
            var asset = ResolvePrefab(prefab);
            if (writable && Editor.Instance.ContentDatabase.FindAsset(asset.ID) is AssetItem item &&
                Editor.Instance.Windows.FindEditor(item) is PrefabWindow)
                throw new InvalidOperationException($"Close the Prefab editor for '{asset.Path}' before asset-native mutation.");
            return new PrefabSession(asset);
        }

        private static Prefab ResolvePrefab(string reference)
        {
            if (string.IsNullOrWhiteSpace(reference))
                throw new ArgumentException("A Prefab asset path or GUID is required.", nameof(reference));
            Guid id;
            if (!Guid.TryParse(reference, out id))
            {
                var path = reference;
                if (path.StartsWith("project://", StringComparison.OrdinalIgnoreCase))
                    path = path.Substring("project://".Length);
                var contentRoot = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                path = Path.IsPathRooted(path) ? Path.GetFullPath(path) : Path.GetFullPath(path, contentRoot);
                var comparison = StringComparison.OrdinalIgnoreCase;
                if (!path.StartsWith(contentRoot + Path.DirectorySeparatorChar, comparison) ||
                    !string.Equals(Path.GetExtension(path), ".prefab", StringComparison.OrdinalIgnoreCase))
                    throw new ArgumentException("Prefab paths must resolve to a .prefab asset under the project Content folder.", nameof(reference));
                if (Editor.Instance.ContentDatabase.Find(path) is AssetItem item)
                    id = item.ID;
                else if (!FlaxEngine.Content.GetAssetInfo(path, out var info) || info.ID == Guid.Empty)
                    throw new FileNotFoundException($"Prefab asset '{reference}' was not found.", path);
                else
                    id = info.ID;
            }
            var asset = FlaxEngine.Content.Load<Prefab>(id) ?? throw new KeyNotFoundException($"Prefab asset '{reference}' was not found.");
            if (asset.WaitForLoaded())
                throw new InvalidOperationException($"Prefab asset '{reference}' failed to load.");
            return asset;
        }

        private static Type RequireType(string typeName, Type baseType)
        {
            var scriptType = TypeUtils.GetType(typeName);
            if (!scriptType || scriptType.Type == null || !baseType.IsAssignableFrom(scriptType.Type) ||
                scriptType.Type.IsAbstract || !scriptType.CanCreateInstance)
                throw new ArgumentException($"Type '{typeName}' is not a creatable {baseType.Name} type.");
            return scriptType.Type;
        }

        private static IEnumerable<Actor> Enumerate(Actor root)
        {
            var pending = new Stack<Actor>();
            pending.Push(root);
            while (pending.Count != 0)
            {
                var actor = pending.Pop();
                yield return actor;
                for (var i = actor.ChildrenCount - 1; i >= 0; i--)
                    pending.Push(actor.GetChild(i));
            }
        }

        private static string ActorPath(Actor root, Actor actor)
        {
            if (actor == root)
                return ".";
            var names = new Stack<string>();
            for (var current = actor; current != null && current != root; current = current.Parent)
                names.Push(current.Name);
            return string.Join("/", names);
        }

        private static object DescribeActor(Actor root, Actor actor) => new
        {
            actorId = actor.ID,
            prefabObjectId = actor.PrefabObjectID,
            path = ActorPath(root, actor),
            type = actor.TypeName,
            name = actor.Name,
        };

        private static object DescribeActorDetails(Actor root, Actor actor) => new
        {
            handle = DescribeActor(root, actor),
            parentId = actor.Parent?.ID ?? Guid.Empty,
            actor.IsActive,
            actor.Layer,
            tags = actor.Tags.Select(x => x.ToString()).ToArray(),
            transform = new
            {
                position = new { x = actor.LocalPosition.X, y = actor.LocalPosition.Y, z = actor.LocalPosition.Z },
                rotation = new { x = actor.LocalEulerAngles.X, y = actor.LocalEulerAngles.Y, z = actor.LocalEulerAngles.Z },
                scale = new { x = actor.LocalScale.X, y = actor.LocalScale.Y, z = actor.LocalScale.Z },
            },
            components = actor.Scripts.Select(x => DescribeScript(root, x)).ToArray(),
            children = Enumerable.Range(0, actor.ChildrenCount).Select(actor.GetChild).Select(x => DescribeActor(root, x)).ToArray(),
        };

        private static object DescribeScript(Actor root, Script script) => script == null ? null : new
        {
            componentId = script.ID,
            prefabObjectId = script.PrefabObjectID,
            actorId = script.Actor?.ID ?? Guid.Empty,
            actorPath = script.Actor != null ? ActorPath(root, script.Actor) : null,
            type = script.TypeName,
            script.Enabled,
            order = script.OrderInParent,
        };

        private static object DescribeTree(Actor root, Actor actor, int depth, int maxDepth) => new
        {
            actor = DescribeActorDetails(root, actor),
            children = depth >= maxDepth
                ? Array.Empty<object>()
                : Enumerable.Range(0, actor.ChildrenCount).Select(actor.GetChild)
                    .Select(x => DescribeTree(root, x, depth + 1, maxDepth)).ToArray(),
            childrenTruncated = depth >= maxDepth && actor.ChildrenCount != 0,
        };

        private static object Result(PrefabSession session, object data, bool saved) => new
        {
            prefabId = session.Asset.ID,
            path = session.Asset.Path,
            data,
            saved,
            sceneTouched = false,
        };

        private static MemberInfo RequirePublicMember(object target, string name, bool writable)
        {
            if (string.IsNullOrWhiteSpace(name) || name.Contains(".") || name.Contains("[") || name.Contains("]"))
                throw new ArgumentException("Members require a direct public field or property name.", nameof(name));
            var property = target.GetType().GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase);
            if (property != null && property.CanRead && (!writable || property.CanWrite) && property.GetIndexParameters().Length == 0)
                return property;
            var field = target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase);
            if (field != null && (!writable || (!field.IsInitOnly && !field.IsLiteral)))
                return field;
            throw new ArgumentException($"Member '{name}' is not a {(writable ? "writable " : string.Empty)}public field or property on '{target.GetType().FullName}'.", nameof(name));
        }

        private static Type GetMemberType(MemberInfo member) => member is PropertyInfo property ? property.PropertyType : ((FieldInfo)member).FieldType;
        private static object GetMemberValue(MemberInfo member, object target) => member is PropertyInfo property ? property.GetValue(target) : ((FieldInfo)member).GetValue(target);
        private static void SetMemberValue(MemberInfo member, object target, object value)
        {
            if (member is PropertyInfo property) property.SetValue(target, value);
            else ((FieldInfo)member).SetValue(target, value);
        }

        private static JToken SerializeMemberValue(object value, Type type)
        {
            if (value == null) return JValue.CreateNull();
            if (value is Object flaxObject) return new JValue(flaxObject.ID.ToString());
            return JToken.Parse(FlaxJsonSerializer.Serialize(value, type));
        }

        private static object ConvertMemberValue(JToken value, Type type)
        {
            if (value == null)
                throw new ArgumentNullException(nameof(value));
            if (type.IsArray)
            {
                if (value.Type == JTokenType.Null) return null;
                if (value.Type == JTokenType.String) value = JToken.Parse(value.Value<string>());
                if (value is not JArray values) throw new ArgumentException($"A '{type.FullName}' value requires a JSON array.");
                var elementType = type.GetElementType();
                var result = Array.CreateInstance(elementType, values.Count);
                for (var i = 0; i < values.Count; i++) result.SetValue(ConvertMemberValue(values[i], elementType), i);
                return result;
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(JsonAssetReference<>))
            {
                if (value.Type == JTokenType.Null) return Activator.CreateInstance(type);
                if (value.Type != JTokenType.String) throw new ArgumentException($"A '{type.FullName}' reference requires an asset GUID, asset URI, or null.");
                var reference = value.Value<string>();
                JsonAsset asset;
                if (Guid.TryParse(reference, out var id))
                {
                    asset = FlaxEngine.Content.LoadAsync<JsonAsset>(id);
                }
                else
                {
                    var path = ResolveAssetReference(reference);
                    if (Editor.Instance.ContentDatabase.Find(path) is AssetItem item) id = item.ID;
                    else if (FlaxEngine.Content.GetAssetInfo(path, out var info) && info.ID != Guid.Empty) id = info.ID;
                    else throw new KeyNotFoundException($"Asset reference '{reference}' was not found in the Content database.");
                    asset = FlaxEngine.Content.LoadAsync<JsonAsset>(id);
                }
                if (!asset || asset.WaitForLoaded()) throw new InvalidOperationException($"Json asset reference '{reference}' failed to load.");
                var result = Activator.CreateInstance(type);
                type.GetField("Asset").SetValue(result, asset);
                return result;
            }
            if (typeof(Object).IsAssignableFrom(type))
            {
                if (value.Type == JTokenType.Null) return null;
                if (value.Type != JTokenType.String) throw new ArgumentException($"A '{type.FullName}' reference requires an object GUID, project:// URI, or null.");
                var reference = value.Value<string>();
                if (Guid.TryParse(reference, out var id))
                {
                    var result = typeof(Asset).IsAssignableFrom(type) ? FlaxEngine.Content.LoadAsync(id, type) : Object.Find(ref id, type, true);
                    if (result != null) return result;
                }
                if (typeof(Asset).IsAssignableFrom(type)) return ResolveAssetObject(reference, type);
                throw new KeyNotFoundException($"Object reference '{reference}' was not found.");
            }
            var converted = value.ToObject(type, JsonSerializer.Create(FlaxJsonSerializer.Settings));
            if (converted != null && value is JObject objectValue &&
                !type.IsPrimitive && !type.IsEnum && type != typeof(string) && type != typeof(Guid))
            {
                foreach (var property in objectValue.Properties())
                {
                    var member = type.GetMember(property.Name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase).FirstOrDefault();
                    if (member is PropertyInfo memberProperty && memberProperty.CanWrite && memberProperty.GetIndexParameters().Length == 0)
                        memberProperty.SetValue(converted, ConvertMemberValue(property.Value, memberProperty.PropertyType));
                    else if (member is FieldInfo memberField && !memberField.IsInitOnly && !memberField.IsLiteral)
                        memberField.SetValue(converted, ConvertMemberValue(property.Value, memberField.FieldType));
                }
            }
            return converted;
        }

        private static Object ResolveAssetObject(string reference, Type type)
        {
            if (string.IsNullOrWhiteSpace(reference)) throw new ArgumentException("An asset reference is required.");
            Guid id;
            if (!Guid.TryParse(reference, out id))
            {
                var path = ResolveAssetReference(reference);
                if (Editor.Instance.ContentDatabase.Find(path) is AssetItem item) id = item.ID;
                else if (!FlaxEngine.Content.GetAssetInfo(path, out var info) || info.ID == Guid.Empty)
                    throw new KeyNotFoundException($"Asset reference '{reference}' was not found.");
                else id = info.ID;
            }
            var result = FlaxEngine.Content.LoadAsync(id, type);
            if (result is not Asset asset || asset.WaitForLoaded())
                throw new InvalidOperationException($"Asset reference '{reference}' failed to load as '{type.FullName}'.");
            return result;
        }

        private static string ResolveAssetReference(string reference)
        {
            if (string.IsNullOrWhiteSpace(reference))
                throw new ArgumentException("An asset reference cannot be empty.", nameof(reference));
            if (reference.StartsWith("engine://", StringComparison.OrdinalIgnoreCase))
                return StringUtils.CombinePaths(Globals.EngineContentFolder, reference.Substring("engine://".Length));
            if (reference.StartsWith("project://", StringComparison.OrdinalIgnoreCase))
                return StringUtils.CombinePaths(Globals.ProjectContentFolder, reference.Substring("project://".Length));
            return Path.IsPathRooted(reference)
                ? Path.GetFullPath(reference)
                : Path.GetFullPath(reference, Globals.ProjectContentFolder);
        }
    }
}
