// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using FlaxEditor.Actions;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Scripting;
using FlaxEngine;
using FlaxEngine.Utilities;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using FlaxJsonSerializer = FlaxEngine.Json.JsonSerializer;
using Object = FlaxEngine.Object;

namespace FlaxEditor
{
    /// <summary>
    /// Describes an Actor to create with <c>actors.create-batch</c>.
    /// </summary>
    public sealed class CliActorCreateOptions
    {
        /// <summary>The full Actor type name.</summary>
        public string Type { get; set; } = typeof(EmptyActor).FullName;

        /// <summary>The Actor name.</summary>
        public string Name { get; set; }

        /// <summary>The parent Actor ID. Omit to use the first loaded scene.</summary>
        public Guid? Parent { get; set; }

        /// <summary>The local position.</summary>
        public Vector3? Position { get; set; }

        /// <summary>The local Euler rotation in degrees.</summary>
        public Float3? Rotation { get; set; }

        /// <summary>The local scale.</summary>
        public Float3? Scale { get; set; }
    }

    internal static class CliAuthoringCommands
    {
        [CliCommand("scenes.list", Description = "List loaded scenes.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandOperation ListScenes(CliCommandContext context = null)
        {
            return new ListScenesOperation(Level.Scenes, context);
        }

        private sealed class ListScenesOperation : CliCommandOperation
        {
            private readonly Scene[] _scenes;
            private readonly CliCommandContext _context;
            private readonly Stack<Actor> _pending = new Stack<Actor>();
            private readonly List<object> _descriptions = new List<object>();
            private CliCommandResult _result;
            private int _sceneIndex = -1;
            private int _actorCount;

            public ListScenesOperation(Scene[] scenes, CliCommandContext context)
            {
                _scenes = scenes;
                _context = context;
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                var timer = System.Diagnostics.Stopwatch.StartNew();
                do
                {
                    _context?.CancellationToken.ThrowIfCancellationRequested();
                    if (_pending.Count == 0)
                    {
                        if (_sceneIndex >= 0)
                        {
                            var completedScene = _scenes[_sceneIndex];
                            FlaxEngine.Content.GetAssetInfo(completedScene.ID, out var info);
                            _descriptions.Add(new
                            {
                                id = completedScene.ID,
                                name = completedScene.Name,
                                path = info.Path,
                                dirty = Editor.Instance.Scene.IsEdited(completedScene),
                                actorCount = _actorCount,
                            });
                        }

                        _sceneIndex++;
                        if (_sceneIndex >= _scenes.Length)
                        {
                            _context?.ReportProgress("Scene list complete", 1.0f);
                            _result = CliCommandResult.Success(_descriptions.ToArray());
                            return;
                        }

                        _actorCount = 0;
                        var scene = _scenes[_sceneIndex];
                        for (var childIndex = scene.ChildrenCount - 1; childIndex >= 0; childIndex--)
                            _pending.Push(scene.GetChild(childIndex));
                        continue;
                    }

                    var actor = _pending.Pop();
                    _actorCount++;
                    for (var childIndex = actor.ChildrenCount - 1; childIndex >= 0; childIndex--)
                        _pending.Push(actor.GetChild(childIndex));
                } while (timer.Elapsed < timeBudget);
            }
        }

        [CliCommand("scenes.create", Description = "Create a scene asset under the project Content root.", Access = CliCommandAccess.MutatesProject)]
        public static object CreateScene([CliOption("path", Description = "Content-relative scene path.", Required = true)] string path, [CliOption("open", Description = "Open the scene after creating it.")] bool open = false)
        {
            var outputPath = ResolveAuthoringPath(path, ".scene", true);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            Editor.Instance.Scene.CreateSceneFile(outputPath);
            Editor.Instance.ContentDatabase.RefreshFolder(Editor.Instance.ContentDatabase.Find(Path.GetDirectoryName(outputPath)), false);

            Guid sceneId = Guid.Empty;
            if (FlaxEngine.Content.GetAssetInfo(outputPath, out var info))
                sceneId = info.ID;
            if (open && sceneId != Guid.Empty)
            {
                SaveEditedScenes();
                Editor.Instance.Scene.OpenScene(sceneId);
            }
            return new { path = outputPath, sceneId, opened = open && sceneId != Guid.Empty, saved = true, dirty = false };
        }

        [CliCommand("scenes.open", Description = "Open a scene asset by ID or Content-relative path.", Access = CliCommandAccess.MutatesProject)]
        public static object OpenScene([CliOption("scene", Description = "Scene ID or Content-relative path.", Required = true)] string scene, [CliOption("additive")] bool additive = false)
        {
            var id = ResolveAssetId(scene, ".scene");
            var autoSavedSceneIds = additive ? Array.Empty<Guid>() : SaveEditedScenes();
            Editor.Instance.Scene.OpenScene(id, additive);
            return new { sceneId = id, additive, requested = true, autoSavedSceneIds, saved = autoSavedSceneIds.Length != 0, dirty = Editor.Instance.Scene.IsEdited() };
        }

        [CliCommand("scenes.save", Description = "Save one or all loaded scenes.", Access = CliCommandAccess.MutatesProject)]
        public static object SaveScenes([CliOption("scene", Description = "Optional loaded scene ID.")] Guid? scene = null)
        {
            if (scene.HasValue)
            {
                var value = RequireScene(scene.Value);
                var saved = SaveSceneIfEdited(value);
                return new { scenes = new[] { DescribeScene(value) }, saved, dirty = Editor.Instance.Scene.IsEdited(value) };
            }
            var changed = SaveEditedScenes();
            return new { sceneIds = changed, saved = changed.Length != 0, dirty = Editor.Instance.Scene.IsEdited() };
        }

        [CliCommand("scenes.hierarchy", Description = "Return the loaded scene Actor hierarchy.", Access = CliCommandAccess.ReadOnly)]
        public static CliCommandOperation SceneHierarchy([CliOption("scene", Description = "Optional loaded scene ID.")] Guid? scene = null, CliCommandContext context = null)
        {
            var scenes = scene.HasValue ? new[] { RequireScene(scene.Value) } : Level.Scenes;
            return new SceneHierarchyOperation(scenes, context);
        }

        private sealed class SceneHierarchyOperation : CliCommandOperation
        {
            private sealed class SceneDescription
            {
                [JsonProperty("id")]
                public Guid Id;

                [JsonProperty("name")]
                public string Name;

                [JsonProperty("path")]
                public string Path;

                [JsonProperty("dirty")]
                public bool Dirty;

                [JsonProperty("actorCount")]
                public int ActorCount;
            }

            private sealed class ActorTreeDescription
            {
                [JsonProperty("handle")]
                public object Handle;

                [JsonProperty("children")]
                public readonly List<ActorTreeDescription> Children = new List<ActorTreeDescription>();
            }

            private sealed class SceneTreeDescription
            {
                [JsonProperty("scene")]
                public SceneDescription Scene;

                [JsonProperty("actors")]
                public readonly List<ActorTreeDescription> Actors = new List<ActorTreeDescription>();
            }

            private struct PendingActor
            {
                public Actor Actor;
                public SceneTreeDescription Scene;
                public List<ActorTreeDescription> Destination;
            }

            private readonly Stack<PendingActor> _pending = new Stack<PendingActor>();
            private readonly SceneTreeDescription[] _scenes;
            private readonly CliCommandContext _context;
            private CliCommandResult _result;
            private int _visited;

            public SceneHierarchyOperation(Scene[] scenes, CliCommandContext context)
            {
                _context = context;
                _scenes = new SceneTreeDescription[scenes.Length];
                for (var sceneIndex = 0; sceneIndex < scenes.Length; sceneIndex++)
                {
                    var scene = scenes[sceneIndex];
                    FlaxEngine.Content.GetAssetInfo(scene.ID, out var info);
                    var description = new SceneTreeDescription
                    {
                        Scene = new SceneDescription
                        {
                            Id = scene.ID,
                            Name = scene.Name,
                            Path = info.Path,
                            Dirty = Editor.Instance.Scene.IsEdited(scene),
                        },
                    };
                    _scenes[sceneIndex] = description;
                }

                for (var sceneIndex = scenes.Length - 1; sceneIndex >= 0; sceneIndex--)
                {
                    var scene = scenes[sceneIndex];
                    for (var childIndex = scene.ChildrenCount - 1; childIndex >= 0; childIndex--)
                    {
                        _pending.Push(new PendingActor
                        {
                            Actor = scene.GetChild(childIndex),
                            Scene = _scenes[sceneIndex],
                            Destination = _scenes[sceneIndex].Actors,
                        });
                    }
                }
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                var timer = System.Diagnostics.Stopwatch.StartNew();
                do
                {
                    _context?.CancellationToken.ThrowIfCancellationRequested();
                    if (_pending.Count == 0)
                    {
                        _context?.ReportProgress("Scene hierarchy complete", 1.0f);
                        _result = CliCommandResult.Success(_scenes);
                        return;
                    }

                    var pending = _pending.Pop();
                    var actor = pending.Actor;
                    var node = new ActorTreeDescription { Handle = DescribeActor(actor) };
                    pending.Destination.Add(node);
                    pending.Scene.Scene.ActorCount++;
                    _visited++;
                    for (var childIndex = actor.ChildrenCount - 1; childIndex >= 0; childIndex--)
                    {
                        _pending.Push(new PendingActor
                        {
                            Actor = actor.GetChild(childIndex),
                            Scene = pending.Scene,
                            Destination = node.Children,
                        });
                    }
                } while (timer.Elapsed < timeBudget);

                if ((_visited & 255) == 0)
                    _context?.ReportProgress($"Read {_visited} Actors", 0.0f);
            }
        }

        [CliCommand("actors.find", Description = "Find Actors using stable filters.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static CliCommandOperation FindActors([CliOption("name")] string name = null, [CliOption("type")] string type = null, [CliOption("scene")] Guid? scene = null, [CliOption("active")] bool? active = null, CliCommandContext context = null)
        {
            var actorType = string.IsNullOrWhiteSpace(type) ? null : RequireType(type, typeof(Actor));
            var roots = scene.HasValue ? new Actor[] { RequireScene(scene.Value) } : Level.Scenes.Cast<Actor>().ToArray();
            return new FindActorsOperation(roots, name, actorType, active, context);
        }

        private sealed class FindActorsOperation : CliCommandOperation
        {
            private readonly Stack<Actor> _pending = new Stack<Actor>();
            private readonly List<object> _matches = new List<object>();
            private readonly string _name;
            private readonly Type _type;
            private readonly bool? _active;
            private readonly CliCommandContext _context;
            private CliCommandResult _result;
            private int _visited;

            public FindActorsOperation(Actor[] roots, string name, Type type, bool? active, CliCommandContext context)
            {
                _name = name;
                _type = type;
                _active = active;
                _context = context;
                for (var i = roots.Length - 1; i >= 0; i--)
                {
                    var root = roots[i];
                    for (var childIndex = root.ChildrenCount - 1; childIndex >= 0; childIndex--)
                        _pending.Push(root.GetChild(childIndex));
                }
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                var timer = System.Diagnostics.Stopwatch.StartNew();
                do
                {
                    _context?.CancellationToken.ThrowIfCancellationRequested();
                    if (_pending.Count == 0)
                    {
                        _context?.ReportProgress("Actor search complete", 1.0f);
                        _result = CliCommandResult.Success(_matches.ToArray());
                        return;
                    }

                    var actor = _pending.Pop();
                    for (var i = actor.ChildrenCount - 1; i >= 0; i--)
                        _pending.Push(actor.GetChild(i));
                    _visited++;

                    if ((string.IsNullOrWhiteSpace(_name) || string.Equals(actor.Name, _name, StringComparison.OrdinalIgnoreCase)) &&
                        (_type == null || _type.IsAssignableFrom(actor.GetType())) &&
                        (!_active.HasValue || actor.IsActive == _active.Value))
                    {
                        _matches.Add(DescribeActor(actor));
                    }
                } while (timer.Elapsed < timeBudget);

                if ((_visited & 255) == 0)
                    _context?.ReportProgress($"Searched {_visited} Actors", 0.0f);
            }
        }

        [CliCommand("actors.get", Description = "Get an Actor and its attached scripts.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GetActor([CliOption("actor", Required = true)] Guid actor)
        {
            return DescribeActorDetails(RequireActor(actor));
        }

        [CliCommand("actors.create", Description = "Create an Actor with one undo action.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreateActor([CliOption("type")] string type = "FlaxEngine.EmptyActor", [CliOption("name")] string name = null, [CliOption("parent")] Guid? parent = null, [CliOption("position")] Vector3? position = null, [CliOption("rotation")] Float3? rotation = null, [CliOption("scale")] Float3? scale = null)
        {
            var options = new CliActorCreateOptions { Type = type, Name = name, Parent = parent, Position = position, Rotation = rotation, Scale = scale };
            ValidateCreate(options);
            return MutationResult(CreateActor(options));
        }

        [CliCommand("actors.create-batch", Description = "Atomically validate and create multiple Actors in one undo group.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreateActors([CliOption("actors", Required = true)] CliActorCreateOptions[] actors)
        {
            if (actors == null || actors.Length == 0)
                throw new ArgumentException("At least one Actor description is required.", nameof(actors));
            foreach (var actor in actors)
                ValidateCreate(actor);

            var created = new List<Actor>(actors.Length);
            var undo = Editor.Instance.Undo;
            var undoEnabled = undo.Enabled;
            undo.Enabled = false;
            try
            {
                foreach (var actor in actors)
                    created.Add(CreateActor(actor));
            }
            catch
            {
                for (var i = created.Count - 1; i >= 0; i--)
                {
                    var createdActor = created[i];
                    Object.Destroy(ref createdActor);
                }
                throw;
            }
            finally
            {
                undo.Enabled = undoEnabled;
            }
            var nodes = created.Select(x => Editor.Instance.Scene.GetActorNode(x)).Where(x => x != null).Cast<SceneGraphNode>().ToList();
            if (nodes.Count != created.Count)
                throw new InvalidOperationException("One or more created Actors are missing from the Editor scene graph.");
            undo.AddAction(new DeleteActorsAction(nodes, true));
            var saved = SaveActorScenes(created);
            return new { actors = created.Select(DescribeActor).ToArray(), saved, dirty = created.Any(x => Editor.Instance.Scene.IsEdited(x.Scene)) };
        }

        [CliCommand("actors.spawn-layout", Description = "Spawn model or Prefab instances using a ring, grid, line, or stack layout.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SpawnLayout(
            [CliOption("count", Required = true)] int count,
            [CliOption("center-x", Required = true)] float centerX,
            [CliOption("center-y", Required = true)] float centerY,
            [CliOption("center-z", Required = true)] float centerZ,
            [CliOption("layout")] string layout = "ring",
            [CliOption("plane")] string plane = "xz",
            [CliOption("radius")] float radius = 300.0f,
            [CliOption("spacing")] float spacing = 150.0f,
            [CliOption("columns")] int columns = 0,
            [CliOption("vertical-offset")] float verticalOffset = 0.0f,
            [CliOption("size")] float size = 100.0f,
            [CliOption("model")] string model = null,
            [CliOption("prefab")] string prefab = null,
            [CliOption("physics")] string physics = "none",
            [CliOption("collider")] string collider = "none",
            [CliOption("name-prefix")] string namePrefix = "Spawned",
            [CliOption("parent")] Guid? parent = null)
        {
            if (count < 1 || count > 1000)
                throw new ArgumentOutOfRangeException(nameof(count), "Spawn count must be between 1 and 1000.");
            if (radius < 0.0f)
                throw new ArgumentOutOfRangeException(nameof(radius));
            if (spacing < 0.0f)
                throw new ArgumentOutOfRangeException(nameof(spacing));
            if (size <= 0.0f)
                throw new ArgumentOutOfRangeException(nameof(size));
            if (columns < 0)
                throw new ArgumentOutOfRangeException(nameof(columns));
            if (string.IsNullOrWhiteSpace(model) == string.IsNullOrWhiteSpace(prefab))
                throw new ArgumentException("Specify exactly one of --model or --prefab.");
            if (string.IsNullOrWhiteSpace(namePrefix))
                throw new ArgumentException("Name prefix cannot be empty.", nameof(namePrefix));

            var center = new Vector3(centerX, centerY, centerZ);
            layout = layout?.Trim().ToLowerInvariant();
            plane = plane?.Trim().ToLowerInvariant();
            physics = physics?.Trim().ToLowerInvariant();
            collider = collider?.Trim().ToLowerInvariant();
            if (layout != "ring" && layout != "grid" && layout != "line" && layout != "stack")
                throw new ArgumentException("Layout must be ring, grid, line, or stack.", nameof(layout));
            if (plane != "xy" && plane != "xz" && plane != "yz")
                throw new ArgumentException("Plane must be xy, xz, or yz.", nameof(plane));
            if (physics != "none" && physics != "static" && physics != "dynamic")
                throw new ArgumentException("Physics must be none, static, or dynamic.", nameof(physics));
            if (collider != "none" && collider != "box" && collider != "sphere")
                throw new ArgumentException("Collider must be none, box, or sphere.", nameof(collider));
            if (physics == "none" && collider != "none")
                throw new ArgumentException("A collider requires static or dynamic physics.");

            Model modelAsset = null;
            Prefab prefabAsset = null;
            if (!string.IsNullOrWhiteSpace(model))
            {
                var modelId = ResolveAssetId(model, ".flax");
                modelAsset = FlaxEngine.Content.Load<Model>(modelId) ?? throw new InvalidOperationException($"Cannot load Model '{model}'.");
                if (modelAsset.WaitForLoaded())
                    throw new InvalidOperationException($"Failed to load Model '{model}'.");
            }
            else
            {
                var prefabId = ResolveAssetId(prefab, ".prefab");
                prefabAsset = FlaxEngine.Content.Load<Prefab>(prefabId) ?? throw new InvalidOperationException($"Cannot load Prefab '{prefab}'.");
                if (prefabAsset.WaitForLoaded())
                    throw new InvalidOperationException($"Failed to load Prefab '{prefab}'.");
            }

            var spawnParent = parent.HasValue ? RequireActor(parent.Value) : Level.GetScene(0);
            var created = new List<Actor>(count);
            try
            {
                for (var i = 0; i < count; i++)
                {
                    var offset = LayoutOffset(layout, plane, i, count, radius, spacing, columns);
                    var position = center + offset + new Vector3(0.0f, verticalOffset, 0.0f);
                    Actor root;
                    if (prefabAsset != null)
                    {
                        root = PrefabManager.SpawnPrefab(prefabAsset, null) ?? throw new InvalidOperationException("Failed to instantiate the Prefab.");
                        root.Name = $"{namePrefix} {i + 1}";
                        Editor.Instance.SceneEditing.Spawn(root, spawnParent, -1, false);
                    }
                    else if (physics == "dynamic")
                    {
                        var body = new RigidBody
                        {
                            Name = $"{namePrefix} {i + 1}",
                            StaticFlags = StaticFlags.None,
                        };
                        Editor.Instance.SceneEditing.Spawn(body, spawnParent, -1, false);
                        var visual = new StaticModel
                        {
                            Name = "Model",
                            Model = modelAsset,
                            LocalScale = Float3.One * (size / 100.0f),
                        };
                        Editor.Instance.SceneEditing.Spawn(visual, body, -1, false);
                        SpawnCollider(collider, size, body);
                        root = body;
                    }
                    else
                    {
                        var visual = new StaticModel
                        {
                            Name = $"{namePrefix} {i + 1}",
                            Model = modelAsset,
                            LocalScale = Float3.One * (size / 100.0f),
                        };
                        Editor.Instance.SceneEditing.Spawn(visual, spawnParent, -1, false);
                        if (physics == "static")
                            SpawnCollider(collider, size, visual);
                        root = visual;
                    }
                    root.LocalPosition = position;
                    created.Add(root);
                }
            }
            catch
            {
                for (var i = created.Count - 1; i >= 0; i--)
                {
                    var createdActor = created[i];
                    Object.Destroy(ref createdActor);
                }
                throw;
            }

            var nodes = created.Select(x => Editor.Instance.Scene.GetActorNode(x)).Where(x => x != null).Cast<SceneGraphNode>().ToList();
            if (nodes.Count != created.Count)
                throw new InvalidOperationException("One or more spawned Actors are missing from the Editor scene graph.");
            var saved = SaveActorScenes(created);
            return new
            {
                layout,
                actors = created.Select(x => new
                {
                    actor = DescribeActor(x),
                    position = new { x = x.LocalPosition.X, y = x.LocalPosition.Y, z = x.LocalPosition.Z },
                }).ToArray(),
                saved,
                dirty = created.Any(x => Editor.Instance.Scene.IsEdited(x.Scene)),
            };
        }

        private static Vector3 LayoutOffset(string layout, string plane, int index, int count, float radius, float spacing, int columns)
        {
            if (layout == "stack")
                return new Vector3(0.0f, (index - (count - 1) * 0.5f) * spacing, 0.0f);

            float first;
            float second;
            if (layout == "ring")
            {
                var angle = Math.PI * 2.0 * index / count;
                first = (float)Math.Cos(angle) * radius;
                second = (float)Math.Sin(angle) * radius;
            }
            else if (layout == "grid")
            {
                var columnCount = columns > 0 ? columns : (int)Math.Ceiling(Math.Sqrt(count));
                var rowCount = (int)Math.Ceiling((double)count / columnCount);
                var column = index % columnCount;
                var row = index / columnCount;
                first = (column - (columnCount - 1) * 0.5f) * spacing;
                second = (row - (rowCount - 1) * 0.5f) * spacing;
            }
            else
            {
                first = (index - (count - 1) * 0.5f) * spacing;
                second = 0.0f;
            }

            switch (plane)
            {
            case "xy": return new Vector3(first, second, 0.0f);
            case "yz": return new Vector3(0.0f, first, second);
            default: return new Vector3(first, 0.0f, second);
            }
        }

        private static void SpawnCollider(string collider, float size, Actor parent)
        {
            if (collider == "box")
            {
                var box = new BoxCollider { Name = "Box Collider" };
                Editor.Instance.SceneEditing.Spawn(box, parent, -1, false);
                box.Size = new Float3(size);
            }
            else if (collider == "sphere")
            {
                var sphere = new SphereCollider { Name = "Sphere Collider" };
                Editor.Instance.SceneEditing.Spawn(sphere, parent, -1, false);
                sphere.Radius = size * 0.5f;
            }
        }

        [CliCommand("actors.delete", Description = "Delete an Actor hierarchy with undo.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object DeleteActor([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            if (value is Scene)
                throw new InvalidOperationException("Use scene lifecycle commands for scene roots.");
            var handle = DescribeActor(value);
            var scene = value.Scene;
            var node = Editor.Instance.Scene.GetActorNode(value) ?? throw new InvalidOperationException("The Actor is missing from the Editor scene graph.");
            var action = new DeleteActorsAction(node.BuildAllNodes().Where(x => x.CanDelete).ToList());
            action.Do();
            Editor.Instance.Undo.AddAction(action);
            var saved = SaveSceneIfEdited(scene);
            return new { actor = handle, deleted = true, saved, dirty = Editor.Instance.Scene.IsEdited(scene) };
        }

        [CliCommand("actors.rename", Description = "Rename an Actor with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object RenameActor([CliOption("actor", Required = true)] Guid actor, [CliOption("name", Required = true)] string name)
        {
            if (string.IsNullOrWhiteSpace(name))
                throw new ArgumentException("Actor name cannot be empty.", nameof(name));
            var value = RequireActor(actor);
            using (new UndoBlock(Editor.Instance.Undo, value, "Rename actor"))
                value.Name = name;
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.transform", Description = "Set only the supplied local transform channels.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object TransformActor([CliOption("actor", Required = true)] Guid actor, [CliOption("position")] Vector3? position = null, [CliOption("rotation")] Float3? rotation = null, [CliOption("scale")] Float3? scale = null)
        {
            if (!position.HasValue && !rotation.HasValue && !scale.HasValue)
                throw new ArgumentException("At least one transform channel is required.");
            var value = RequireActor(actor);
            using (new UndoBlock(Editor.Instance.Undo, value, "Transform actor"))
            {
                if (position.HasValue) value.LocalPosition = position.Value;
                if (rotation.HasValue) value.LocalEulerAngles = rotation.Value;
                if (scale.HasValue) value.LocalScale = scale.Value;
            }
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.parent", Description = "Reparent an Actor with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ParentActor([CliOption("actor", Required = true)] Guid actor, [CliOption("parent", Required = true)] Guid parent, [CliOption("world-position-stays")] bool worldPositionStays = true, [CliOption("order")] int order = -1)
        {
            var value = RequireActor(actor);
            var oldScene = value.Scene;
            var newParent = RequireActor(parent);
            if (value is Scene || value == newParent || IsDescendant(newParent, value))
                throw new InvalidOperationException("The requested parent relationship is invalid.");
            var action = new ParentActorsAction(new SceneObject[] { value }, newParent, order, worldPositionStays);
            action.Do();
            Editor.Instance.Undo.AddAction(action);
            var saved = SaveActorScenes(new[] { value, oldScene });
            return new { actor = DescribeActor(value), saved, dirty = Editor.Instance.Scene.IsEdited(value.Scene) || Editor.Instance.Scene.IsEdited(oldScene) };
        }

        [CliCommand("actors.active", Description = "Set Actor active state with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetActorActive([CliOption("actor", Required = true)] Guid actor, [CliOption("active", Required = true)] bool active)
        {
            var value = RequireActor(actor);
            using (new UndoBlock(Editor.Instance.Undo, value, "Set actor active"))
                value.IsActive = active;
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.tag", Description = "Replace the Actor tag collection.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetActorTags([CliOption("actor", Required = true)] Guid actor, [CliOption("tags", Required = true)] string[] tags)
        {
            var value = RequireActor(actor);
            tags = tags ?? Array.Empty<string>();
            using (new UndoBlock(Editor.Instance.Undo, value, "Set actor tags"))
                value.Tags = tags.Where(x => !string.IsNullOrWhiteSpace(x)).Distinct(StringComparer.OrdinalIgnoreCase).Select(Tags.Get).ToArray();
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.layer", Description = "Set the Actor layer by index.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetActorLayer([CliOption("actor", Required = true)] Guid actor, [CliOption("layer", Required = true)] int layer)
        {
            if (layer < 0 || layer > 31)
                throw new ArgumentOutOfRangeException(nameof(layer), "Layer must be in the range 0 through 31.");
            var value = RequireActor(actor);
            using (new UndoBlock(Editor.Instance.Undo, value, "Set actor layer"))
                value.Layer = layer;
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.component.add", Description = "Add a Script component to an Actor with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object AddComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("type", Required = true)] string type)
        {
            var value = RequireActor(actor);
            var scriptType = TypeUtils.GetType(type);
            if (!scriptType || !new ScriptType(typeof(Script)).IsAssignableFrom(scriptType) || !scriptType.CanCreateInstance)
                throw new ArgumentException($"Type '{type}' is not a creatable Script type.", nameof(type));
            var action = AddRemoveScript.Add(value, scriptType);
            action.Do();
            Editor.Instance.Undo.AddAction(action);
            var script = value.Scripts.LastOrDefault(x => string.Equals(x.TypeName, scriptType.TypeName, StringComparison.Ordinal));
            var saved = SaveSceneIfEdited(value.Scene);
            return new { actor = DescribeActor(value), component = DescribeScript(script), saved, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        [CliCommand("actors.component.remove", Description = "Remove a Script component with undo.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object RemoveComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("component", Required = true)] Guid component)
        {
            var value = RequireActor(actor);
            var script = RequireScript(value, component);
            var description = DescribeScript(script);
            var action = AddRemoveScript.Remove(script);
            action.Do();
            Editor.Instance.Undo.AddAction(action);
            var saved = SaveSceneIfEdited(value.Scene);
            return new { actor = DescribeActor(value), component = description, deleted = true, saved, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        [CliCommand("actors.component.get", Description = "Get a Script component or one public property.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GetComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("component", Required = true)] Guid component, [CliOption("property")] string property = null)
        {
            var script = RequireScript(RequireActor(actor), component);
            if (string.IsNullOrWhiteSpace(property))
                return new { component = DescribeScript(script), properties = JObject.FromObject(script, JsonSerializer.Create(FlaxJsonSerializer.Settings)) };
            var member = RequireWritableProperty(script, property, false);
            return new { component = DescribeScript(script), property = member.Name, value = JToken.FromObject(member.GetValue(script), JsonSerializer.Create(FlaxJsonSerializer.Settings)) };
        }

        [CliCommand("actors.component.set", Description = "Set one public Script property with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("component", Required = true)] Guid component, [CliOption("property", Required = true)] string property, [CliOption("value", Required = true)] JToken value)
        {
            var actorValue = RequireActor(actor);
            var script = RequireScript(actorValue, component);
            var member = RequireWritableProperty(script, property, true);
            var converted = value.ToObject(member.PropertyType, JsonSerializer.Create(FlaxJsonSerializer.Settings));
            using (new UndoBlock(Editor.Instance.Undo, script, "Set component property"))
                member.SetValue(script, converted);
            MarkEdited(actorValue);
            var saved = SaveSceneIfEdited(actorValue.Scene);
            return new { actor = DescribeActor(actorValue), component = DescribeScript(script), property = member.Name, value, saved, dirty = Editor.Instance.Scene.IsEdited(actorValue.Scene) };
        }

        [CliCommand("prefabs.create", Description = "Create a Prefab from an Actor and link the Actor to it.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreatePrefab([CliOption("actor", Required = true)] Guid actor, [CliOption("path", Required = true)] string path)
        {
            var value = RequireActor(actor);
            var outputPath = ResolveAuthoringPath(path, ".prefab", true);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            if (PrefabManager.CreatePrefab(value, outputPath, true))
                throw new InvalidOperationException($"Failed to create Prefab '{outputPath}'.");
            Editor.Instance.ContentDatabase.RefreshFolder(Editor.Instance.ContentDatabase.Find(Path.GetDirectoryName(outputPath)), false);
            FlaxEngine.Content.GetAssetInfo(outputPath, out var info);
            SaveSceneIfEdited(value.Scene);
            return new { prefabId = info.ID, path = outputPath, actor = DescribeActor(value), saved = true, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        [CliCommand("prefabs.variant", Description = "Create a Prefab variant from an existing Prefab.", Access = CliCommandAccess.MutatesProject)]
        public static object CreatePrefabVariant([CliOption("prefab", Required = true)] string prefab, [CliOption("path", Required = true)] string path)
        {
            var prefabId = ResolveAssetId(prefab, ".prefab");
            var asset = FlaxEngine.Content.Load<Prefab>(prefabId) ?? throw new InvalidOperationException($"Cannot load Prefab '{prefab}'.");
            if (asset.WaitForLoaded())
                throw new InvalidOperationException($"Failed to load Prefab '{prefab}'.");
            var outputPath = ResolveAuthoringPath(path, ".prefab", true);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            var root = PrefabManager.SpawnPrefab(asset, null) ?? throw new InvalidOperationException("Failed to create the Prefab variant instance.");
            try
            {
                if (PrefabManager.CreatePrefab(root, outputPath, true))
                    throw new InvalidOperationException($"Failed to create Prefab variant '{outputPath}'.");
            }
            finally
            {
                Object.Destroy(ref root);
            }
            Editor.Instance.ContentDatabase.RefreshFolder(Editor.Instance.ContentDatabase.Find(Path.GetDirectoryName(outputPath)), false);
            FlaxEngine.Content.GetAssetInfo(outputPath, out var info);
            return new { prefabId = info.ID, sourcePrefabId = prefabId, path = outputPath, saved = true, dirty = false };
        }

        [CliCommand("prefabs.instantiate", Description = "Instantiate a Prefab into a loaded scene with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object InstantiatePrefab([CliOption("prefab", Required = true)] string prefab, [CliOption("parent")] Guid? parent = null, [CliOption("position")] Vector3? position = null)
        {
            var prefabId = ResolveAssetId(prefab, ".prefab");
            var asset = FlaxEngine.Content.Load<Prefab>(prefabId) ?? throw new InvalidOperationException($"Cannot load Prefab '{prefab}'.");
            if (asset.WaitForLoaded())
                throw new InvalidOperationException($"Failed to load Prefab '{prefab}'.");
            var instance = PrefabManager.SpawnPrefab(asset, null) ?? throw new InvalidOperationException("Failed to instantiate the Prefab.");
            if (position.HasValue)
                instance.LocalPosition = position.Value;
            Editor.Instance.SceneEditing.Spawn(instance, parent.HasValue ? RequireActor(parent.Value) : Level.GetScene(0), -1, false);
            var saved = SaveSceneIfEdited(instance.Scene);
            return new { prefabId, actor = DescribeActor(instance), saved, dirty = Editor.Instance.Scene.IsEdited(instance.Scene) };
        }

        [CliCommand("prefabs.apply", Description = "Apply all changes from a linked Prefab instance.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ApplyPrefab([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            Editor.Instance.Prefabs.ApplyAll(value);
            SaveEditedScenes();
            return new { prefabId = value.PrefabID, actor = DescribeActor(value), saved = true, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        [CliCommand("prefabs.save", Description = "Save a linked Prefab instance by applying all changes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SavePrefab([CliOption("actor", Required = true)] Guid actor)
        {
            return ApplyPrefab(actor);
        }

        [CliCommand("prefabs.revert", Description = "Replace a linked Prefab instance with a clean instance using one undo action.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object RevertPrefab([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            if (!value.HasPrefabLink)
                throw new InvalidOperationException("The Actor is not linked to a Prefab.");
            var prefabId = value.PrefabID;
            var asset = FlaxEngine.Content.Load<Prefab>(prefabId) ?? throw new InvalidOperationException($"Cannot load Prefab '{prefabId}'.");
            if (asset.WaitForLoaded())
                throw new InvalidOperationException($"Failed to load Prefab '{prefabId}'.");

            var parent = value.Parent;
            var order = value.OrderInParent;
            var transform = value.LocalTransform;
            var replacement = PrefabManager.SpawnPrefab(asset, null) ?? throw new InvalidOperationException("Failed to instantiate a clean Prefab.");
            replacement.LocalTransform = transform;
            var oldNode = Editor.Instance.Scene.GetActorNode(value) ?? throw new InvalidOperationException("The Actor is missing from the Editor scene graph.");
            var removeOld = new DeleteActorsAction(oldNode);
            removeOld.Do();
            var undo = Editor.Instance.Undo;
            var undoEnabled = undo.Enabled;
            undo.Enabled = false;
            try
            {
                Editor.Instance.SceneEditing.Spawn(replacement, parent, order, false);
            }
            catch
            {
                removeOld.Undo();
                Object.Destroy(ref replacement);
                throw;
            }
            finally
            {
                undo.Enabled = undoEnabled;
            }
            var replacementNode = Editor.Instance.Scene.GetActorNode(replacement) ?? throw new InvalidOperationException("The replacement Actor is missing from the Editor scene graph.");
            undo.AddAction(new MultiUndoAction(new IUndoAction[] { removeOld, new DeleteActorsAction(replacementNode, true) }, "Revert Prefab"));
            var saved = SaveSceneIfEdited(replacement.Scene);
            return new { prefabId, removedActorId = actor, actor = DescribeActor(replacement), saved, dirty = Editor.Instance.Scene.IsEdited(replacement.Scene) };
        }

        [CliCommand("prefabs.unpack", Description = "Break the Prefab link for an Actor hierarchy with undo.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object UnpackPrefab([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            if (!value.HasPrefabLink)
                throw new InvalidOperationException("The Actor is not linked to a Prefab.");
            var prefabId = value.PrefabID;
            var action = BreakPrefabLinkAction.Break(value);
            action.Do();
            Editor.Instance.Undo.AddAction(action);
            MarkEdited(value);
            var saved = SaveSceneIfEdited(value.Scene);
            return new { prefabId, actor = DescribeActor(value), unpacked = true, saved, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        private static Actor CreateActor(CliActorCreateOptions options)
        {
            var type = RequireType(options.Type, typeof(Actor));
            var actor = (Actor)Object.New(type);
            if (!string.IsNullOrWhiteSpace(options.Name)) actor.Name = options.Name;
            if (options.Position.HasValue) actor.LocalPosition = options.Position.Value;
            if (options.Rotation.HasValue) actor.LocalEulerAngles = options.Rotation.Value;
            if (options.Scale.HasValue) actor.LocalScale = options.Scale.Value;
            var parent = options.Parent.HasValue ? RequireActor(options.Parent.Value) : Level.GetScene(0);
            Editor.Instance.SceneEditing.Spawn(actor, parent, -1, false);
            return actor;
        }

        private static void ValidateCreate(CliActorCreateOptions options)
        {
            if (options == null)
                throw new ArgumentNullException(nameof(options));
            RequireType(options.Type, typeof(Actor));
            if (options.Parent.HasValue)
                RequireActor(options.Parent.Value);
        }

        private static Type RequireType(string typeName, Type baseType)
        {
            var type = TypeUtils.GetType(typeName);
            if (!type || type.Type == null || !baseType.IsAssignableFrom(type.Type) || type.Type.IsAbstract)
                throw new ArgumentException($"Type '{typeName}' is not a concrete {baseType.Name} type.");
            return type.Type;
        }

        private static Actor RequireActor(Guid id)
        {
            var value = Object.Find<Actor>(ref id);
            if (value == null || !value.HasScene)
                throw new KeyNotFoundException($"Actor '{id}' was not found in a loaded scene.");
            return value;
        }

        private static Scene RequireScene(Guid id)
        {
            var scene = Level.Scenes.FirstOrDefault(x => x.ID == id);
            return scene ?? throw new KeyNotFoundException($"Scene '{id}' is not loaded.");
        }

        private static Script RequireScript(Actor actor, Guid id)
        {
            for (var i = 0; i < actor.ScriptsCount; i++)
            {
                var script = actor.GetScript(i);
                if (script.ID == id)
                    return script;
            }
            throw new KeyNotFoundException($"Script component '{id}' is not attached to Actor '{actor.ID}'.");
        }

        private static PropertyInfo RequireWritableProperty(object target, string name, bool writable)
        {
            if (string.IsNullOrWhiteSpace(name) || name.Contains(".") || name.Contains("[") || name.Contains("]"))
                throw new ArgumentException("Phase 5 component properties currently require a direct public property name.", nameof(name));
            var property = target.GetType().GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase);
            if (property == null || !property.CanRead || (writable && !property.CanWrite) || property.GetIndexParameters().Length != 0)
                throw new ArgumentException($"Property '{name}' is not a {(writable ? "writable " : string.Empty)}public property on '{target.GetType().FullName}'.", nameof(name));
            return property;
        }

        private static string ResolveAuthoringPath(string path, string extension, bool requireNew)
        {
            if (string.IsNullOrWhiteSpace(path))
                throw new ArgumentException("An authoring path is required.", nameof(path));
            if (string.IsNullOrEmpty(Path.GetExtension(path)))
                path += extension;
            var contentRoot = Path.GetFullPath(Globals.ProjectContentFolder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var root = GetAuthoringRoot(contentRoot);
            var relative = path.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
            if (relative.Equals("Content", StringComparison.OrdinalIgnoreCase) || relative.StartsWith("Content" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                relative = Path.GetFullPath(relative, Globals.ProjectFolder);
            var result = Path.GetFullPath(relative, root);
            if (!result.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The path escapes the project Content authoring root.");
            if (!string.Equals(Path.GetExtension(result), extension, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException($"Expected a '{extension}' path.");
            if (requireNew && (File.Exists(result) || Directory.Exists(result) || Editor.Instance.ContentDatabase.Find(result) != null))
                throw new IOException($"Authoring path '{result}' already exists.");
            ValidateNoLinks(contentRoot, result);
            return result;
        }

        private static string GetAuthoringRoot(string contentRoot)
        {
            var result = contentRoot;
            var configPath = Path.Combine(Globals.ProjectFolder, ".flax", "cli.json");
            if (File.Exists(configPath))
            {
                var config = JObject.Parse(File.ReadAllText(configPath));
                var configured = config.Value<string>("authoringRoot");
                if (!string.IsNullOrWhiteSpace(configured))
                {
                    var relative = configured.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
                    result = Path.GetFullPath(relative, Globals.ProjectFolder);
                }
            }
            result = result.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (!result.Equals(contentRoot, StringComparison.OrdinalIgnoreCase) && !result.StartsWith(contentRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("The configured authoring root escapes the project Content folder.");
            ValidateNoLinks(contentRoot, result);
            return result;
        }

        private static void ValidateNoLinks(string contentRoot, string path)
        {
            var current = new DirectoryInfo(Directory.Exists(path) ? path : Path.GetDirectoryName(path));
            while (current != null && current.FullName.Length >= contentRoot.Length)
            {
                if (current.Exists && (current.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidOperationException($"Authoring path '{path}' traverses link '{current.FullName}'.");
                if (current.FullName.Equals(contentRoot, StringComparison.OrdinalIgnoreCase))
                    return;
                current = current.Parent;
            }
            throw new InvalidOperationException("The authoring path is outside the project Content folder.");
        }

        private static Guid ResolveAssetId(string value, string extension)
        {
            if (Guid.TryParse(value, out var id))
            {
                if (FlaxEngine.Content.GetAssetInfo(id, out var idInfo) && string.Equals(Path.GetExtension(idInfo.Path), extension, StringComparison.OrdinalIgnoreCase))
                    return id;
                throw new FileNotFoundException($"Asset '{value}' was not found or is not a '{extension}' asset.");
            }
            var path = ResolveAuthoringPath(value, extension, false);
            if (FlaxEngine.Content.GetAssetInfo(path, out var info) && info.ID != Guid.Empty)
                return info.ID;
            var item = Editor.Instance.ContentDatabase.Find(path) as FlaxEditor.Content.AssetItem;
            if (item != null)
                return item.ID;
            throw new FileNotFoundException($"Asset '{value}' was not found.", path);
        }

        private static IEnumerable<Actor> EnumerateActors(Actor root)
        {
            yield return root;
            for (var i = 0; i < root.ChildrenCount; i++)
            foreach (var child in EnumerateActors(root.GetChild(i)))
                yield return child;
        }

        private static IEnumerable<Actor> EnumerateChildren(Actor root)
        {
            for (var i = 0; i < root.ChildrenCount; i++)
                yield return root.GetChild(i);
        }

        private static bool IsDescendant(Actor candidate, Actor ancestor)
        {
            for (var current = candidate.Parent; current != null; current = current.Parent)
                if (current == ancestor)
                    return true;
            return false;
        }

        private static object DescribeScene(Scene scene)
        {
            FlaxEngine.Content.GetAssetInfo(scene.ID, out var info);
            return new { id = scene.ID, name = scene.Name, path = info.Path, dirty = Editor.Instance.Scene.IsEdited(scene), actorCount = EnumerateActors(scene).Count() - 1 };
        }

        private static object DescribeActor(Actor actor)
        {
            return new
            {
                sceneId = actor.Scene?.ID ?? Guid.Empty,
                actorId = actor.ID,
                path = ActorPath(actor),
                type = actor.TypeName,
                name = actor.Name,
            };
        }

        private static object DescribeActorDetails(Actor actor)
        {
            return new
            {
                handle = DescribeActor(actor),
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
                prefab = actor.HasPrefabLink ? new { id = actor.PrefabID, objectId = actor.PrefabObjectID, root = actor.IsPrefabRoot } : null,
                components = actor.Scripts.Select(DescribeScript).ToArray(),
                children = EnumerateChildren(actor).Select(DescribeActor).ToArray(),
            };
        }

        private static object DescribeScript(Script script)
        {
            return script == null ? null : new { componentId = script.ID, actorId = script.Actor?.ID ?? Guid.Empty, type = script.TypeName, script.Enabled, order = script.OrderInParent };
        }

        private static object MutationResult(Actor actor)
        {
            var saved = SaveSceneIfEdited(actor.Scene);
            return new { actor = DescribeActor(actor), saved, dirty = Editor.Instance.Scene.IsEdited(actor.Scene) };
        }

        private static bool SaveActorScenes(IEnumerable<Actor> actors)
        {
            var saved = false;
            foreach (var scene in actors.Where(x => x != null).Select(x => x.Scene).Where(x => x != null).Distinct())
                saved |= SaveSceneIfEdited(scene);
            return saved;
        }

        private static Guid[] SaveEditedScenes()
        {
            if (Editor.IsPlayMode)
                return Array.Empty<Guid>();
            var edited = Level.Scenes.Where(x => Editor.Instance.Scene.IsEdited(x)).ToArray();
            foreach (var scene in edited)
                SaveSceneSynchronously(scene);
            return edited.Select(x => x.ID).ToArray();
        }

        private static bool SaveSceneIfEdited(Scene scene)
        {
            if (scene == null || Editor.IsPlayMode || !Editor.Instance.Scene.IsEdited(scene))
                return false;
            SaveSceneSynchronously(scene);
            return true;
        }

        private static void SaveSceneSynchronously(Scene scene)
        {
            if (Level.SaveScene(scene))
                throw new IOException($"Failed to save scene '{scene.Name}'. See the Editor log for details.");
            var node = Editor.Instance.Scene.GetActorNode(scene) as SceneNode;
            if (node != null)
                node.IsEdited = false;
            if (!Editor.Instance.Scene.IsEdited())
                Editor.Instance.Undo.MarkScenesSaved();
        }

        private static void MarkEdited(Actor actor)
        {
            Editor.Instance.Scene.MarkSceneEdited(actor.Scene);
        }

        private static string ActorPath(Actor actor)
        {
            var names = new Stack<string>();
            for (var current = actor; current != null; current = current.Parent)
                names.Push(current.Name);
            return "/" + string.Join("/", names);
        }
    }
}
