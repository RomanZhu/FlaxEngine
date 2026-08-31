// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.Content.Settings;
using FlaxEditor.SceneEditing;
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

        /// <summary>The parent Actor ID. Omit to use the explicit active Scene.</summary>
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
        private static readonly string[] PrimitiveNames = { "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Capsule" };
        private static int _injectedSaveFailures;
        private static int _injectedMutationFailures;
        private static string _injectedMutationStage;

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

        private sealed class SceneTransitionOperation : CliCommandOperation
        {
            private readonly string _description;
            private readonly Func<bool> _isReady;
            private readonly Func<object> _createResult;
            private readonly CliCommandContext _context;
            private readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
            private CliCommandResult _result;

            public SceneTransitionOperation(string description, Func<bool> isReady, Func<object> createResult, CliCommandContext context)
            {
                _description = description;
                _isReady = isReady;
                _createResult = createResult;
                _context = context;
            }

            public override bool IsCompleted => _result != null;

            public override CliCommandResult Result => _result;

            public override void Update(TimeSpan timeBudget)
            {
                if (_result != null)
                    return;
                _context?.CancellationToken.ThrowIfCancellationRequested();
                if (_isReady())
                {
                    _context?.ReportProgress(_description + " complete", 1.0f);
                    _result = CliCommandResult.Success(_createResult());
                }
                else if (_clock.Elapsed.TotalSeconds >= 30.0)
                {
                    _result = CliCommandResult.Failure("FLX-SCENE-LIFECYCLE-0006", "Timed out waiting for " + _description + ".", new { elapsedSeconds = _clock.Elapsed.TotalSeconds });
                }
                else
                {
                    _context?.ReportProgress("Waiting for " + _description, (float)Math.Min(0.99, _clock.Elapsed.TotalSeconds / 30.0));
                }
            }

            public override void Cancel()
            {
                if (_result == null)
                    _result = CliCommandResult.Failure("FLX-SCENE-LIFECYCLE-0005", _description + " was cancelled.");
            }
        }

        [CliCommand("scenes.create", Description = "Create a scene asset under the project Content root.", Access = CliCommandAccess.MutatesProject)]
        public static object CreateScene([CliOption("path", Description = "Content-relative scene path.", Required = true)] string path, [CliOption("open", Description = "Open the scene after creating it.")] bool open = false, CliCommandContext context = null)
        {
            var outputPath = ResolveAuthoringPath(path, ".scene", true);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            Editor.Instance.Scene.CreateSceneFile(outputPath);
            RefreshCreatedContent(outputPath);

            Guid sceneId = Guid.Empty;
            if (FlaxEngine.Content.GetAssetInfo(outputPath, out var info))
                sceneId = info.ID;
            if (open && sceneId != Guid.Empty)
            {
                EnsureScenesClean("open the new scene");
                Editor.Instance.Scene.OpenScene(sceneId);
                return new SceneTransitionOperation("opening the new Scene", () => Level.FindScene(sceneId) != null && Editor.Instance.Undo.Enabled, () => new { path = outputPath, sceneId, opened = true, saved = true, dirty = false }, context);
            }
            return new { path = outputPath, sceneId, opened = open && sceneId != Guid.Empty, saved = true, dirty = false };
        }

        [CliCommand("scenes.open", Description = "Open a scene asset by ID or Content-relative path.", Access = CliCommandAccess.MutatesProject)]
        public static object OpenScene([CliOption("scene", Description = "Scene ID or Content-relative path.", Required = true)] string scene, [CliOption("additive")] bool additive = false, CliCommandContext context = null)
        {
            var id = ResolveAssetId(scene, ".scene");
            if (!additive)
                EnsureScenesClean("open another scene");
            var autoSavedSceneIds = Array.Empty<Guid>();
            Editor.Instance.Scene.OpenScene(id, additive);
            return new SceneTransitionOperation("opening the Scene", () =>
            {
                var loaded = Level.FindScene(id) != null;
                if (!additive)
                    loaded &= Level.Scenes.Length == 1 && Level.Scenes[0].ID == id;
                return loaded && Editor.Instance.Undo.Enabled;
            }, () => new { sceneId = id, additive, requested = true, autoSavedSceneIds, saved = false, dirty = Editor.Instance.Scene.IsEdited() }, context);
        }

        [CliCommand("scenes.close", Description = "Close one or all loaded scenes, refusing to discard unsaved changes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CloseScenes([CliOption("scene", Description = "Optional loaded scene ID. Omit to close all loaded scenes.")] Guid? scene = null, CliCommandContext context = null)
        {
            var targets = scene.HasValue ? new[] { RequireScene(scene.Value) } : Level.Scenes;
            var targetIds = targets.Select(x => x.ID).ToArray();
            EnsureScenesClean("close scenes");
            var autoSavedSceneIds = Array.Empty<Guid>();
            if (scene.HasValue)
                Editor.Instance.Scene.CloseScene(targets[0]);
            else
                Editor.Instance.Scene.CloseAllScenes();
            return new SceneTransitionOperation("closing the Scene", () => targetIds.All(x => Level.FindScene(x) == null) && Editor.Instance.Undo.Enabled, () => new { sceneIds = targetIds, requested = targetIds.Length != 0, autoSavedSceneIds, saved = false }, context);
        }

        [CliCommand("scenes.reload", Description = "Reload all loaded scenes from disk, refusing to discard unsaved changes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ReloadScenes(CliCommandContext context = null)
        {
            var scenes = Level.Scenes;
            var sceneIds = scenes.Select(x => x.ID).ToArray();
            EnsureScenesClean("reload scenes");
            var autoSavedSceneIds = Array.Empty<Guid>();
            Editor.Instance.Scene.ReloadScenes();
            return new SceneTransitionOperation("reloading the Scenes", () =>
            {
                if (!Editor.Instance.Undo.Enabled)
                    return false;
                for (int i = 0; i < sceneIds.Length; i++)
                {
                    var loaded = Level.FindScene(sceneIds[i]);
                    if (loaded == null || ReferenceEquals(loaded, scenes[i]))
                        return false;
                }
                return true;
            }, () => new { sceneIds, requested = sceneIds.Length != 0, autoSavedSceneIds, saved = false }, context);
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

        [CliCommand("scenes.save-fault", Description = "Inject a deterministic number of Scene save failures for stability tests; zero disables injection.", Access = CliCommandAccess.MutatesProject)]
        public static object ConfigureSaveFault([CliOption("count", Required = true)] int count)
        {
            if (count < 0 || count > 1000)
                throw new ArgumentOutOfRangeException(nameof(count));
            Interlocked.Exchange(ref _injectedSaveFailures, count);
            SceneSaveFaults.Injector = count == 0
                ? null
                : (sceneId, path) =>
                {
                    while (true)
                    {
                        var remaining = Volatile.Read(ref _injectedSaveFailures);
                        if (remaining <= 0)
                            return false;
                        if (Interlocked.CompareExchange(ref _injectedSaveFailures, remaining - 1, remaining) == remaining)
                            return true;
                    }
                };
            return new { enabled = count != 0, failuresRemaining = count };
        }

        [CliCommand("scenes.mutation-fault", Description = "Inject failures at a paste transaction stage for rollback tests; zero disables injection.", Access = CliCommandAccess.MutatesProject)]
        public static object ConfigureMutationFault([CliOption("count", Required = true)] int count, [CliOption("stage")] string stage = null)
        {
            if (count < 0 || count > 1000)
                throw new ArgumentOutOfRangeException(nameof(count));
            var validStages = new[] { "Preflight", "Construction", "Attachment", "Publication" };
            if (count != 0 && !validStages.Any(x => string.Equals(x, stage, StringComparison.OrdinalIgnoreCase)))
                throw new ArgumentException("Mutation fault stage must be Preflight, Construction, Attachment, or Publication.", nameof(stage));

            _injectedMutationStage = count == 0 ? null : validStages.First(x => string.Equals(x, stage, StringComparison.OrdinalIgnoreCase));
            Interlocked.Exchange(ref _injectedMutationFailures, count);
            SceneMutationFaults.Injector = count == 0
                ? null
                : (transactionId, currentStage) =>
                {
                    if (!string.Equals(currentStage, _injectedMutationStage, StringComparison.Ordinal))
                        return false;
                    while (true)
                    {
                        var remaining = Volatile.Read(ref _injectedMutationFailures);
                        if (remaining <= 0)
                            return false;
                        if (Interlocked.CompareExchange(ref _injectedMutationFailures, remaining - 1, remaining) == remaining)
                            return true;
                    }
                };
            return new { enabled = count != 0, stage = _injectedMutationStage, failuresRemaining = count };
        }

        [CliCommand("scenes.debug", Description = "Enable or disable opt-in structured Scene mutation diagnostics for this Editor session.", Access = CliCommandAccess.MutatesProject)]
        public static object ConfigureSceneDebug([CliOption("enabled", Required = true)] bool enabled)
        {
            SceneDebug.Enabled = enabled;
            return new { enabled };
        }

        [CliCommand("scenes.dirty", Description = "List loaded scenes with unsaved changes.", Access = CliCommandAccess.ReadOnly)]
        public static object DirtyScenes()
        {
            var scenes = Level.Scenes.Where(x => Editor.Instance.Scene.IsEdited(x)).Select(DescribeScene).ToArray();
            return new { any = scenes.Length != 0, count = scenes.Length, scenes };
        }

        [CliCommand("scenes.active.get", Description = "Get the primary loaded scene used as the default authoring target.", Access = CliCommandAccess.ReadOnly)]
        public static object GetActiveScene()
        {
            var scene = Editor.Instance.Scene.ActiveScene;
            return new
            {
                semantics = "The active Scene is selected explicitly by authoring or selection context and is independent of Scene load order.",
                scene = scene == null ? null : DescribeScene(scene),
            };
        }

        [CliCommand("scenes.active.set", Description = "Set the explicit active authoring Scene without changing Scene load order.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetActiveScene([CliOption("scene", Description = "Loaded scene ID.", Required = true)] Guid scene)
        {
            var target = RequireScene(scene);
            var before = Level.Scenes.Select(x => x.ID).ToArray();
            if (Editor.Instance.Scene.ActiveScene == target)
                return new { changed = false, scene = DescribeScene(target), loadedSceneIds = before, savedSceneIds = Array.Empty<Guid>() };

            Editor.Instance.Scene.SetActiveScene(target);
            return new { changed = true, scene = DescribeScene(target), loadedSceneIds = before, savedSceneIds = Array.Empty<Guid>() };
        }

        [CliCommand("scenes.build-list.list", Description = "List the cooked startup scene followed by additional cooked scenes.", Access = CliCommandAccess.ReadOnly)]
        public static object ListBuildScenes()
        {
            return DescribeBuildScenes();
        }

        [CliCommand("scenes.build-list.add", Description = "Add a scene to the cooked scene roots or make it the startup scene.", Access = CliCommandAccess.MutatesProject)]
        public static object AddBuildScene([CliOption("scene", Description = "Scene ID or Content-relative path.", Required = true)] string scene, [CliOption("startup", Description = "Promote this scene to the startup slot.")] bool startup = false)
        {
            var id = ResolveAssetId(scene, ".scene");
            var objectId = AssetObjectId.Main(new AssetGuid(id));
            var game = GameSettings.Load();
            var build = GameSettings.Load<BuildSettings>() ?? new BuildSettings();
            var additional = (build.AdditionalScenes ?? Array.Empty<SceneReference>()).Where(x => x.ID != objectId).ToList();
            var changed = false;

            if (startup || game.FirstScene.ID.IsNull)
            {
                if (game.FirstScene.ID != objectId)
                {
                    if (!game.FirstScene.ID.IsNull)
                        additional.Insert(0, new SceneReference(game.FirstScene.ID));
                    game.FirstScene = new SceneReference(objectId);
                    changed = true;
                }
            }
            else if (game.FirstScene.ID != objectId && !(build.AdditionalScenes ?? Array.Empty<SceneReference>()).Any(x => x.ID == objectId))
            {
                additional.Add(new SceneReference(objectId));
                changed = true;
            }

            build.AdditionalScenes = additional.Distinct().ToArray();
            if (changed)
            {
                SaveSettings(game);
                SaveSettings(build);
            }
            return new { changed, buildList = DescribeBuildScenes() };
        }

        [CliCommand("scenes.build-list.remove", Description = "Remove a cooked scene. Removing the startup scene promotes the first additional scene when available.", Access = CliCommandAccess.MutatesProject)]
        public static object RemoveBuildScene([CliOption("scene", Description = "Scene ID or Content-relative path.", Required = true)] string scene)
        {
            var id = ResolveAssetId(scene, ".scene");
            var objectId = AssetObjectId.Main(new AssetGuid(id));
            var game = GameSettings.Load();
            var build = GameSettings.Load<BuildSettings>() ?? new BuildSettings();
            var additional = (build.AdditionalScenes ?? Array.Empty<SceneReference>()).Where(x => x.ID != objectId).ToList();
            var changed = additional.Count != (build.AdditionalScenes?.Length ?? 0);
            AssetObjectId promoted = default;

            if (game.FirstScene.ID == objectId)
            {
                promoted = additional.Count == 0 ? default : additional[0].ID;
                if (additional.Count != 0)
                    additional.RemoveAt(0);
                game.FirstScene = new SceneReference(promoted);
                changed = true;
            }

            build.AdditionalScenes = additional.ToArray();
            if (changed)
            {
                SaveSettings(game);
                SaveSettings(build);
            }
            return new { changed, promotedStartupSceneId = promoted, buildList = DescribeBuildScenes() };
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

        [CliCommand("actors.find", Description = "Find Actors using stable Actor and attached Script filters.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static CliCommandOperation FindActors([CliOption("name")] string name = null, [CliOption("type")] string type = null, [CliOption("component", Description = "Optional attached Script type name.")] string component = null, [CliOption("scene")] Guid? scene = null, [CliOption("active")] bool? active = null, CliCommandContext context = null)
        {
            var actorType = string.IsNullOrWhiteSpace(type) ? null : RequireType(type, typeof(Actor));
            var componentType = string.IsNullOrWhiteSpace(component) ? null : RequireType(component, typeof(Script));
            var roots = scene.HasValue ? new Actor[] { RequireScene(scene.Value) } : Level.Scenes.Cast<Actor>().ToArray();
            return new FindActorsOperation(roots, name, actorType, componentType, active, context);
        }

        private sealed class FindActorsOperation : CliCommandOperation
        {
            private readonly Stack<Actor> _pending = new Stack<Actor>();
            private readonly List<object> _matches = new List<object>();
            private readonly string _name;
            private readonly Type _type;
            private readonly Type _componentType;
            private readonly bool? _active;
            private readonly CliCommandContext _context;
            private CliCommandResult _result;
            private int _visited;

            public FindActorsOperation(Actor[] roots, string name, Type type, Type componentType, bool? active, CliCommandContext context)
            {
                _name = name;
                _type = type;
                _componentType = componentType;
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

                    var matchingComponents = _componentType == null
                        ? null
                        : actor.Scripts.Where(x => _componentType.IsAssignableFrom(x.GetType())).ToArray();
                    if ((string.IsNullOrWhiteSpace(_name) || string.Equals(actor.Name, _name, StringComparison.OrdinalIgnoreCase)) &&
                        (_type == null || _type.IsAssignableFrom(actor.GetType())) &&
                        (_componentType == null || matchingComponents.Length != 0) &&
                        (!_active.HasValue || actor.IsActive == _active.Value))
                    {
                        _matches.Add(_componentType == null
                            ? DescribeActor(actor)
                            : new { actor = DescribeActor(actor), components = matchingComponents.Select(DescribeScript).ToArray() });
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

        [CliCommand("actors.copy", Description = "Copy complete Actor hierarchies to the validated Actor clipboard without changing selection.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object CopyActors([CliOption("actor", Required = true)] Guid[] actor)
        {
            var nodes = ResolveClipboardNodes(actor);
            var data = Actor.ToBytes(nodes.SelectMany(x => x.BuildAllNodes()).OfType<ActorNode>().Select(x => x.Actor).ToArray());
            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (data == null || data.Length == 0 || objectIds == null || objectIds.Length == 0)
                throw new InvalidOperationException("Actor clipboard serialization produced an invalid payload.");
            Clipboard.RawData = data;
            return new { copied = true, roots = nodes.Select(x => x.ID).ToArray(), objectIds, bytes = data.Length };
        }

        [CliCommand("actors.cut", Description = "Copy complete Actor hierarchies and commit their deletion as one recoverable undo action.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object CutActors([CliOption("actor", Required = true)] Guid[] actor)
        {
            var nodes = ResolveClipboardNodes(actor);
            var allNodes = nodes.SelectMany(x => x.BuildAllNodes()).Where(x => x.CanDelete).ToList();
            var data = Actor.ToBytes(allNodes.OfType<ActorNode>().Select(x => x.Actor).ToArray());
            var objectIds = Actor.TryGetSerializedObjectsIds(data);
            if (data == null || data.Length == 0 || objectIds == null || objectIds.Length == 0)
                throw new InvalidOperationException("Actor cut was rejected because clipboard serialization failed.");

            var action = new DeleteActorsAction(allNodes);
            Clipboard.RawData = data;
            if (!action.TryDo())
                throw new InvalidOperationException($"Actor cut deletion failed: {action.LastResult?.ErrorCode} {action.LastResult?.Message}");
            Editor.Instance.Undo.AddAction(action);
            return new { cut = true, roots = nodes.Select(x => x.ID).ToArray(), objectIds, bytes = data.Length, sceneIds = action.SceneIds };
        }

        [CliCommand("actors.paste", Description = "Paste the Actor clipboard into an explicit loaded Scene and optional parent.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PasteActors([CliOption("scene", Required = true)] Guid scene, [CliOption("parent")] Guid? parent = null)
        {
            var destinationScene = RequireScene(scene);
            Actor destinationParent = null;
            if (parent.HasValue)
            {
                destinationParent = RequireActor(parent.Value);
                if (destinationParent.Scene != destinationScene)
                    throw new InvalidOperationException("The paste parent does not belong to the destination Scene.");
            }

            var action = PasteActorsAction.Paste(Clipboard.RawData, destinationScene, destinationParent);
            if (action == null)
                throw new InvalidOperationException("The clipboard does not contain a valid Actor payload.");
            if (!action.TryDo(out _, out var roots))
                throw new InvalidOperationException($"Actor paste failed: {action.LastResult?.ErrorCode} {action.LastResult?.Message}");
            Editor.Instance.Undo.AddAction(action);
            var result = action.LastResult;
            var created = new List<object>(result.CreatedObjectIds.Length);
            for (int i = 0; i < result.CreatedObjectIds.Length; i++)
            {
                var id = result.CreatedObjectIds[i];
                var value = Object.TryFind<Actor>(ref id);
                if (value != null)
                    created.Add(DescribeActor(value));
            }
            var saved = SaveSceneIfEdited(destinationScene);
            return new
            {
                transactionId = result.TransactionId,
                status = result.Status.ToString(),
                errorCode = result.ErrorCode.ToString(),
                sceneIds = result.SceneIds,
                rootIds = roots.Select(x => x.ID).ToArray(),
                createdObjectIds = result.CreatedObjectIds,
                actors = created.ToArray(),
                saved,
                dirty = Editor.Instance.Scene.IsEdited(destinationScene),
            };
        }

        [CliCommand("history.undo", Description = "Attempt one undo and report whether the history cursor advanced.", Access = CliCommandAccess.MutatesProject)]
        public static object UndoHistory()
        {
            var undo = Editor.Instance.Undo;
            var undoBefore = undo.GetUndoActions().Length;
            var redoBefore = undo.GetRedoActions().Length;
            undo.PerformUndo();
            var undoAfter = undo.GetUndoActions().Length;
            var redoAfter = undo.GetRedoActions().Length;
            return new { applied = undoAfter != undoBefore || redoAfter != redoBefore, undoBefore, undoAfter, redoBefore, redoAfter };
        }

        [CliCommand("history.list", Description = "List undo and redo actions in replay order.", Access = CliCommandAccess.ReadOnly)]
        public static object ListHistory()
        {
            var undo = Editor.Instance.Undo;
            return new
            {
                undo = undo.GetUndoActions().Select(x => new { action = x.ActionString, type = x.GetType().FullName }).ToArray(),
                redo = undo.GetRedoActions().Select(x => new { action = x.ActionString, type = x.GetType().FullName }).ToArray(),
            };
        }

        [CliCommand("history.redo", Description = "Attempt one redo and report whether the history cursor advanced.", Access = CliCommandAccess.MutatesProject)]
        public static object RedoHistory()
        {
            var undo = Editor.Instance.Undo;
            var undoBefore = undo.GetUndoActions().Length;
            var redoBefore = undo.GetRedoActions().Length;
            undo.PerformRedo();
            var undoAfter = undo.GetUndoActions().Length;
            var redoAfter = undo.GetRedoActions().Length;
            return new { applied = undoAfter != undoBefore || redoAfter != redoBefore, undoBefore, undoAfter, redoBefore, redoAfter };
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
            var scenes = created.Select(x => x.Scene).Where(x => x != null).Distinct().ToArray();
            var saved = false;
            foreach (var scene in scenes)
                saved |= SaveSceneIfEdited(scene);
            return new { actors = created.Select(DescribeActor).ToArray(), saved, dirty = created.Any(x => Editor.Instance.Scene.IsEdited(x.Scene)) };
        }

        [CliCommand("actors.primitive.list", Description = "List the built-in Editor primitive models and their native bounds.", Access = CliCommandAccess.ReadOnly)]
        public static object ListPrimitives()
        {
            return PrimitiveNames.Select(name => DescribePrimitive(name, LoadPrimitive(name))).ToArray();
        }

        [CliCommand("actors.primitive.create", Description = "Create a StaticModel Actor using a built-in Editor primitive without copying an asset.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreatePrimitive(
            [CliOption("shape", Description = "Cube, Sphere, Plane, Cylinder, Cone, or Capsule.", Required = true)] string shape,
            [CliOption("name")] string name = null,
            [CliOption("parent")] Guid? parent = null,
            [CliOption("position")] Vector3? position = null,
            [CliOption("rotation")] Float3? rotation = null,
            [CliOption("scale")] Float3? scale = null)
        {
            var primitiveName = RequirePrimitiveName(shape);
            var model = LoadPrimitive(primitiveName);
            var actor = new StaticModel
            {
                Name = string.IsNullOrWhiteSpace(name) ? primitiveName : name,
                Model = model,
            };
            if (position.HasValue) actor.LocalPosition = position.Value;
            if (rotation.HasValue) actor.LocalEulerAngles = rotation.Value;
            if (scale.HasValue) actor.LocalScale = scale.Value;
            var actorParent = parent.HasValue ? RequireActor(parent.Value) : RequireActiveScene();
            Editor.Instance.SceneEditing.Spawn(actor, actorParent, -1, false);
            var saved = SaveSceneIfEdited(actor.Scene);
            return new
            {
                actor = DescribeActor(actor),
                primitive = DescribePrimitive(primitiveName, model),
                saved,
                dirty = Editor.Instance.Scene.IsEdited(actor.Scene),
            };
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
            if (!action.TryDo())
                throw new InvalidOperationException($"Failed to delete Actor: {action.LastResult?.ErrorCode} {action.LastResult?.Message}");
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

        [CliCommand("actors.transform", Description = "Set only the supplied local or world transform channels.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object TransformActor([CliOption("actor", Required = true)] Guid actor, [CliOption("position")] Vector3? position = null, [CliOption("rotation")] Float3? rotation = null, [CliOption("scale")] Float3? scale = null, [CliOption("space", Description = "Transform space: local or world.")] string space = "local")
        {
            if (!position.HasValue && !rotation.HasValue && !scale.HasValue)
                throw new ArgumentException("At least one transform channel is required.");
            var value = RequireActor(actor);
            var world = string.Equals(space, "world", StringComparison.OrdinalIgnoreCase);
            if (!world && !string.Equals(space, "local", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Transform space must be 'local' or 'world'.", nameof(space));
            using (new UndoBlock(Editor.Instance.Undo, value, "Transform actor"))
            {
                if (position.HasValue)
                {
                    if (world) value.Position = position.Value;
                    else value.LocalPosition = position.Value;
                }
                if (rotation.HasValue)
                {
                    if (world) value.EulerAngles = rotation.Value;
                    else value.LocalEulerAngles = rotation.Value;
                }
                if (scale.HasValue)
                {
                    if (world) value.Scale = scale.Value;
                    else value.LocalScale = scale.Value;
                }
            }
            MarkEdited(value);
            return MutationResult(value);
        }

        [CliCommand("actors.place", Description = "Place an Actor against another Actor's world bounds using an explicit spatial relation.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PlaceActor([CliOption("actor", Required = true)] Guid actor, [CliOption("relative-to", Required = true)] Guid relativeTo, [CliOption("relation", Description = "centered-on, beside, left, right, below, above, behind, in-front-of, inside, or at-edge.", Required = true)] string relation, [CliOption("gap", Description = "World-space gap between bounds.")] float gap = 0.0f, [CliOption("edge", Description = "Edge for at-edge: left, right, below, above, behind, or in-front.")] string edge = "right", [CliOption("face", Description = "Optional face-toward or face-away orientation.")] string face = null, [CliOption("dry-run")] bool dryRun = false)
        {
            if (gap < 0.0f)
                throw new ArgumentOutOfRangeException(nameof(gap), "Gap cannot be negative.");
            var value = RequireActor(actor);
            var anchor = RequireActor(relativeTo);
            if (value == anchor)
                throw new InvalidOperationException("An Actor cannot be placed relative to itself.");

            var valueBounds = EffectiveBounds(value);
            var anchorBounds = EffectiveBounds(anchor);
            var destination = anchorBounds.Center;
            var relationName = relation?.Trim().ToLowerInvariant();
            switch (relationName)
            {
            case "center":
            case "centered-on":
            case "inside":
                break;
            case "left":
                destination.X = anchorBounds.Minimum.X - gap - valueBounds.Size.X * 0.5f;
                break;
            case "right":
            case "beside":
                destination.X = anchorBounds.Maximum.X + gap + valueBounds.Size.X * 0.5f;
                break;
            case "below":
                destination.Y = anchorBounds.Minimum.Y - gap - valueBounds.Size.Y * 0.5f;
                break;
            case "above":
                destination.Y = anchorBounds.Maximum.Y + gap + valueBounds.Size.Y * 0.5f;
                break;
            case "behind":
                destination.Z = anchorBounds.Minimum.Z - gap - valueBounds.Size.Z * 0.5f;
                break;
            case "in-front":
            case "in-front-of":
            case "front":
                destination.Z = anchorBounds.Maximum.Z + gap + valueBounds.Size.Z * 0.5f;
                break;
            case "at-edge":
                var edgeName = edge?.Trim().ToLowerInvariant();
                switch (edgeName)
                {
                case "left": destination.X = anchorBounds.Minimum.X + valueBounds.Size.X * 0.5f + gap; break;
                case "right": destination.X = anchorBounds.Maximum.X - valueBounds.Size.X * 0.5f - gap; break;
                case "below": destination.Y = anchorBounds.Minimum.Y + valueBounds.Size.Y * 0.5f + gap; break;
                case "above": destination.Y = anchorBounds.Maximum.Y - valueBounds.Size.Y * 0.5f - gap; break;
                case "behind": destination.Z = anchorBounds.Minimum.Z + valueBounds.Size.Z * 0.5f + gap; break;
                case "in-front": destination.Z = anchorBounds.Maximum.Z - valueBounds.Size.Z * 0.5f - gap; break;
                default: throw new ArgumentException("Edge must be left, right, below, above, behind, or in-front.", nameof(edge));
                }
                break;
            default:
                throw new ArgumentException("Unsupported spatial relation.", nameof(relation));
            }

            var delta = destination - valueBounds.Center;
            var resultPosition = value.Position + delta;
            var resultOrientation = value.Orientation;
            if (!string.IsNullOrWhiteSpace(face))
            {
                var direction = anchorBounds.Center - destination;
                if (string.Equals(face, "face-away", StringComparison.OrdinalIgnoreCase)) direction = -direction;
                else if (!string.Equals(face, "face-toward", StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("Face must be face-toward or face-away.", nameof(face));
                if (direction.LengthSquared > 0.0001f)
                    resultOrientation = Quaternion.LookRotation((Float3)Vector3.Normalize(direction), Float3.Up);
            }
            if (!dryRun)
            {
                using (new UndoBlock(Editor.Instance.Undo, value, "Place actor spatially"))
                {
                    value.Position = resultPosition;
                    value.Orientation = resultOrientation;
                }
                MarkEdited(value);
            }
            var saved = !dryRun && SaveSceneIfEdited(value.Scene);
            var resultingBounds = dryRun ? new BoundingBox(valueBounds.Minimum + delta, valueBounds.Maximum + delta) : EffectiveBounds(value);
            return new
            {
                actor = DescribeActor(value),
                relativeTo = DescribeActor(anchor),
                relation = relationName,
                gap,
                dryRun,
                resultingPosition = DescribeVector(resultPosition),
                worldBounds = DescribeBounds(resultingBounds),
                centerDistance = Vector3.Distance(resultingBounds.Center, anchorBounds.Center),
                saved,
                dirty = Editor.Instance.Scene.IsEdited(value.Scene),
            };
        }

        [CliCommand("actors.distribute", Description = "Distribute Actors along an axis using world bounds and explicit edge-to-edge spacing.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object DistributeActors([CliOption("actor", Required = true)] Guid[] actor, [CliOption("axis", Description = "x, y, or z.")] string axis = "x", [CliOption("spacing")] float spacing = 100.0f, [CliOption("dry-run")] bool dryRun = false)
        {
            if (actor == null || actor.Length < 2) throw new ArgumentException("At least two Actors are required.", nameof(actor));
            if (spacing < 0) throw new ArgumentOutOfRangeException(nameof(spacing));
            var values = actor.Distinct().Select(RequireActor).ToArray();
            if (values.Select(x => x.Scene).Distinct().Count() != 1) throw new InvalidOperationException("All Actors must belong to the same Scene.");
            var axisName = axis?.Trim().ToLowerInvariant();
            int component = axisName == "x" ? 0 : axisName == "y" ? 1 : axisName == "z" ? 2 : throw new ArgumentException("Axis must be x, y, or z.", nameof(axis));
            float Read(Vector3 v) => component == 0 ? (float)v.X : component == 1 ? (float)v.Y : (float)v.Z;
            Vector3 Along(float amount) => component == 0 ? new Vector3(amount, 0, 0) : component == 1 ? new Vector3(0, amount, 0) : new Vector3(0, 0, amount);
            values = values.OrderBy(x => Read(EffectiveBounds(x).Center)).ThenBy(x => x.ID).ToArray();
            var evidence = new List<object>();
            var cursor = Read(EffectiveBounds(values[0]).Maximum);
            evidence.Add(new { actor = values[0].ID, position = DescribeVector(values[0].Position), bounds = DescribeBounds(EffectiveBounds(values[0])) });
            for (int i = 1; i < values.Length; i++)
            {
                var value = values[i];
                var bounds = EffectiveBounds(value);
                var delta = cursor + spacing - Read(bounds.Minimum);
                var position = value.Position + Along(delta);
                if (!dryRun)
                {
                    using (new UndoBlock(Editor.Instance.Undo, value, "Distribute actors")) value.Position = position;
                    MarkEdited(value);
                    bounds = EffectiveBounds(value);
                }
                else bounds = new BoundingBox(bounds.Minimum + Along(delta), bounds.Maximum + Along(delta));
                cursor = Read(bounds.Maximum);
                evidence.Add(new { actor = value.ID, position = DescribeVector(position), bounds = DescribeBounds(bounds), spacing });
            }
            var saved = !dryRun && SaveSceneIfEdited(values[0].Scene);
            return new { axis = axisName, spacing, dryRun, saved, actors = evidence.ToArray() };
        }

        [CliCommand("actors.grid-layout", Description = "Lay Actors out in a bounds-aware grid with explicit horizontal and row spacing.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object GridLayoutActors([CliOption("actor", Required = true)] Guid[] actor, [CliOption("columns")] int columns = 4, [CliOption("column-spacing")] float columnSpacing = 100.0f, [CliOption("row-spacing")] float rowSpacing = 100.0f, [CliOption("dry-run")] bool dryRun = false)
        {
            if (actor == null || actor.Length == 0) throw new ArgumentException("At least one Actor is required.", nameof(actor));
            if (columns < 1) throw new ArgumentOutOfRangeException(nameof(columns));
            if (columnSpacing < 0 || rowSpacing < 0) throw new ArgumentOutOfRangeException("Grid spacing cannot be negative.");
            var values = actor.Distinct().Select(RequireActor).ToArray();
            if (values.Select(x => x.Scene).Distinct().Count() != 1) throw new InvalidOperationException("All Actors must belong to the same Scene.");
            var origin = EffectiveBounds(values[0]).Minimum;
            var columnWidths = new float[columns];
            var rows = (values.Length + columns - 1) / columns;
            var rowDepths = new float[rows];
            for (int i = 0; i < values.Length; i++)
            {
                var size = EffectiveBounds(values[i]).Size;
                columnWidths[i % columns] = Math.Max(columnWidths[i % columns], (float)size.X);
                rowDepths[i / columns] = Math.Max(rowDepths[i / columns], (float)size.Z);
            }
            var evidence = new List<object>();
            for (int i = 0; i < values.Length; i++)
            {
                var column = i % columns;
                var row = i / columns;
                var x = (float)origin.X + columnWidths.Take(column).Sum() + columnSpacing * column;
                var z = (float)origin.Z + rowDepths.Take(row).Sum() + rowSpacing * row;
                var value = values[i];
                var bounds = EffectiveBounds(value);
                var delta = new Vector3(x - bounds.Minimum.X, 0, z - bounds.Minimum.Z);
                var position = value.Position + delta;
                if (!dryRun)
                {
                    using (new UndoBlock(Editor.Instance.Undo, value, "Grid layout actors")) value.Position = position;
                    MarkEdited(value);
                    bounds = EffectiveBounds(value);
                }
                else bounds = new BoundingBox(bounds.Minimum + delta, bounds.Maximum + delta);
                evidence.Add(new { actor = value.ID, row, column, position = DescribeVector(position), bounds = DescribeBounds(bounds) });
            }
            var saved = !dryRun && SaveSceneIfEdited(values[0].Scene);
            return new { columns, columnSpacing, rowSpacing, dryRun, saved, actors = evidence.ToArray() };
        }

        [CliCommand("audio.place-emitter", Description = "Place an emitter at a measured distance from a listener and reject positions outside the authored audible radius.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PlaceAudioEmitter([CliOption("emitter", Required = true)] Guid emitter, [CliOption("listener", Required = true)] Guid listener, [CliOption("distance-meters")] float distanceMeters = 2.0f, [CliOption("maximum-meters")] float maximumMeters = 20.0f, [CliOption("allow-outside")] bool allowOutside = false, [CliOption("dry-run")] bool dryRun = false)
        {
            if (distanceMeters < 0 || maximumMeters <= 0) throw new ArgumentOutOfRangeException("Distances must be positive.");
            if (!allowOutside && distanceMeters > maximumMeters) throw new InvalidOperationException($"Requested distance {distanceMeters:0.###} m exceeds event maximum attenuation {maximumMeters:0.###} m.");
            var source = RequireActor(emitter) as AudioEmitter ?? throw new InvalidOperationException($"Actor '{emitter}' is not an AudioEmitter.");
            var ear = RequireActor(listener);
            var position = ear.Position + Vector3.Forward * (distanceMeters * 100.0f);
            if (!dryRun)
            {
                using (new UndoBlock(Editor.Instance.Undo, source, "Place audible emitter")) source.Position = position;
                MarkEdited(source);
            }
            var saved = !dryRun && SaveSceneIfEdited(source.Scene);
            return new { emitter, listener, distanceMeters, maximumMeters, insideAudibleRadius = distanceMeters <= maximumMeters, position = DescribeVector(position), dryRun, saved };
        }

        [CliCommand("audio.place-trigger-around", Description = "Fit a box trigger around station contents using Actor transform placement and a zero local Center.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PlaceAudioTriggerAround([CliOption("trigger", Required = true)] Guid trigger, [CliOption("content", Required = true)] Guid[] content, [CliOption("margin")] float margin = 100.0f, [CliOption("dry-run")] bool dryRun = false)
        {
            var volume = RequireActor(trigger) as BoxCollider ?? throw new InvalidOperationException($"Actor '{trigger}' is not a BoxCollider.");
            if (content == null || content.Length == 0) throw new ArgumentException("Station content is required.", nameof(content));
            if (margin < 0) throw new ArgumentOutOfRangeException(nameof(margin));
            var bounds = EffectiveBounds(RequireActor(content[0]));
            for (int i = 1; i < content.Length; i++) bounds = BoundingBox.Merge(bounds, EffectiveBounds(RequireActor(content[i])));
            var size = (Float3)(bounds.Size + Vector3.One * (margin * 2.0f));
            if (!dryRun)
            {
                using (new UndoBlock(Editor.Instance.Undo, volume, "Fit station trigger"))
                {
                    volume.Position = bounds.Center;
                    volume.Center = Float3.Zero;
                    volume.Size = size;
                    volume.IsTrigger = true;
                }
                MarkEdited(volume);
            }
            var saved = !dryRun && SaveSceneIfEdited(volume.Scene);
            return new { trigger, content, margin, position = DescribeVector(bounds.Center), localCenter = DescribeVector(Float3.Zero), size = DescribeVector(size), dryRun, saved };
        }

        [CliCommand("audio.place-occluder", Description = "Place an occluder between a source and listener with measured endpoint distances.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PlaceAudioOccluder([CliOption("occluder", Required = true)] Guid occluder, [CliOption("source", Required = true)] Guid source, [CliOption("listener", Required = true)] Guid listener, [CliOption("fraction")] float fraction = 0.5f, [CliOption("dry-run")] bool dryRun = false)
        {
            if (fraction <= 0 || fraction >= 1) throw new ArgumentOutOfRangeException(nameof(fraction), "Fraction must be between zero and one.");
            var wall = RequireActor(occluder);
            var sourceActor = RequireActor(source);
            var listenerActor = RequireActor(listener);
            var position = Vector3.Lerp(sourceActor.Position, listenerActor.Position, fraction);
            var direction = listenerActor.Position - sourceActor.Position;
            var orientation = direction.LengthSquared > 0.0001f ? Quaternion.LookRotation((Float3)Vector3.Normalize(direction), Float3.Up) : wall.Orientation;
            if (!dryRun)
            {
                using (new UndoBlock(Editor.Instance.Undo, wall, "Place audio occluder")) { wall.Position = position; wall.Orientation = orientation; }
                MarkEdited(wall);
            }
            var saved = !dryRun && SaveSceneIfEdited(wall.Scene);
            return new { occluder, source, listener, fraction, position = DescribeVector(position), sourceDistanceMeters = Vector3.Distance(position, sourceActor.Position) * 0.01f, listenerDistanceMeters = Vector3.Distance(position, listenerActor.Position) * 0.01f, dryRun, saved };
        }

        [CliCommand("audio.place-sign", Description = "Place a station sign at an Actor bounds edge and face the route approach Actor.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object PlaceAudioSign([CliOption("sign", Required = true)] Guid sign, [CliOption("station", Required = true)] Guid station, [CliOption("approach", Required = true)] Guid approach, [CliOption("gap")] float gap = 25.0f, [CliOption("dry-run")] bool dryRun = false)
        {
            var value = RequireActor(sign);
            var stationActor = RequireActor(station);
            var approachActor = RequireActor(approach);
            var stationBounds = EffectiveBounds(stationActor);
            var signBounds = EffectiveBounds(value);
            var direction = approachActor.Position - stationBounds.Center;
            var normalized = direction.LengthSquared > 0.0001f ? Vector3.Normalize(direction) : Vector3.Backward;
            var radius = Math.Max((float)stationBounds.Size.X, (float)stationBounds.Size.Z) * 0.5f + Math.Max((float)signBounds.Size.X, (float)signBounds.Size.Z) * 0.5f + gap;
            var destination = stationBounds.Center + normalized * radius;
            var position = value.Position + destination - signBounds.Center;
            var faceDirection = approachActor.Position - destination;
            var orientation = faceDirection.LengthSquared > 0.0001f ? Quaternion.LookRotation((Float3)Vector3.Normalize(faceDirection), Float3.Up) : value.Orientation;
            if (!dryRun)
            {
                using (new UndoBlock(Editor.Instance.Undo, value, "Place station sign")) { value.Position = position; value.Orientation = orientation; }
                MarkEdited(value);
            }
            var saved = !dryRun && SaveSceneIfEdited(value.Scene);
            return new { sign, station, approach, gap, position = DescribeVector(position), approachDistance = Vector3.Distance(destination, approachActor.Position), dryRun, saved };
        }

        [CliCommand("colliders.create", Description = "Create a primitive Collider placed by its Actor transform with a zero local shape center.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreateCollider(
            [CliOption("shape", Description = "Collider shape: box, sphere, or capsule.")] string shape = "box",
            [CliOption("name")] string name = null,
            [CliOption("parent")] Guid? parent = null,
            [CliOption("position", Description = "Actor position in the selected transform space.")] Vector3? position = null,
            [CliOption("rotation", Description = "Actor Euler rotation in degrees in the selected transform space.")] Float3? rotation = null,
            [CliOption("scale", Description = "Actor scale in the selected transform space.")] Float3? scale = null,
            [CliOption("space", Description = "Transform space: world or local.")] string space = "world",
            [CliOption("size", Description = "Box size in local centimeters.")] Float3? size = null,
            [CliOption("radius", Description = "Sphere or capsule radius in local centimeters.")] float? radius = null,
            [CliOption("height", Description = "Capsule cylinder height in local centimeters.")] float? height = null,
            [CliOption("trigger", Description = "Create the collider as a trigger.")] bool trigger = false)
        {
            var shapeName = shape?.Trim().ToLowerInvariant();
            if (shapeName == "box" && (radius.HasValue || height.HasValue))
                throw new ArgumentException("Box colliders use --size; --radius and --height are not applicable.");
            if (shapeName == "sphere" && (size.HasValue || height.HasValue))
                throw new ArgumentException("Sphere colliders use --radius; --size and --height are not applicable.");
            if (shapeName == "capsule" && size.HasValue)
                throw new ArgumentException("Capsule colliders use --radius and --height; --size is not applicable.");
            Collider collider;
            switch (shapeName)
            {
            case "box":
                if (size.HasValue)
                {
                    if (size.Value.X <= 0.0f || size.Value.Y <= 0.0f || size.Value.Z <= 0.0f)
                        throw new ArgumentOutOfRangeException(nameof(size), "Box size components must be positive.");
                }
                collider = new BoxCollider();
                break;
            case "sphere":
                if (radius.HasValue)
                {
                    if (radius.Value <= 0.0f)
                        throw new ArgumentOutOfRangeException(nameof(radius), "Sphere radius must be positive.");
                }
                collider = new SphereCollider();
                break;
            case "capsule":
                if (radius.HasValue)
                {
                    if (radius.Value <= 0.0f)
                        throw new ArgumentOutOfRangeException(nameof(radius), "Capsule radius must be positive.");
                }
                if (height.HasValue)
                {
                    if (height.Value < 0.0f)
                        throw new ArgumentOutOfRangeException(nameof(height), "Capsule height cannot be negative.");
                }
                collider = new CapsuleCollider();
                break;
            default:
                throw new ArgumentException("Collider shape must be box, sphere, or capsule.", nameof(shape));
            }

            var world = string.Equals(space, "world", StringComparison.OrdinalIgnoreCase);
            if (!world && !string.Equals(space, "local", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Transform space must be 'world' or 'local'.", nameof(space));
            collider.Name = string.IsNullOrWhiteSpace(name) ? $"{char.ToUpperInvariant(shapeName[0])}{shapeName.Substring(1)} Collider" : name;
            var actorParent = parent.HasValue ? RequireActor(parent.Value) : RequireActiveScene();
            Editor.Instance.SceneEditing.Spawn(collider, actorParent, -1, false);
            // BoxColliderNode.PostSpawn auto-fits parent bounds and can introduce a
            // local Center offset. Typed CLI creation is transform-first, so restore
            // explicit/default geometry after the Editor spawn hook.
            collider.Center = Vector3.Zero;
            collider.IsTrigger = trigger;
            if (collider is BoxCollider createdBox)
                createdBox.Size = size ?? new Float3(100.0f);
            else if (collider is SphereCollider createdSphere)
                createdSphere.Radius = radius ?? 50.0f;
            else if (collider is CapsuleCollider createdCapsule)
            {
                createdCapsule.Radius = radius ?? 20.0f;
                createdCapsule.Height = height ?? 100.0f;
            }
            if (position.HasValue)
            {
                if (world) collider.Position = position.Value;
                else collider.LocalPosition = position.Value;
            }
            if (rotation.HasValue)
            {
                if (world) collider.EulerAngles = rotation.Value;
                else collider.LocalEulerAngles = rotation.Value;
            }
            if (scale.HasValue)
            {
                if (world) collider.Scale = scale.Value;
                else collider.LocalScale = scale.Value;
            }
            MarkEdited(collider);
            var saved = SaveSceneIfEdited(collider.Scene);
            return new
            {
                collider = DescribeCollider(collider),
                centerPolicy = "Actor transform places the collider; Center remains a local shape offset and defaults to zero.",
                saved,
                dirty = Editor.Instance.Scene.IsEdited(collider.Scene),
            };
        }

        [CliCommand("colliders.inspect", Description = "Inspect Collider transform placement, local center, world shape center, and bounds.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object InspectCollider([CliOption("actor", Required = true)] Guid actor)
        {
            return DescribeCollider(RequireCollider(actor));
        }

        [CliCommand("colliders.offset-local", Description = "Explicitly set a Collider local shape offset without using it as world placement.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object OffsetColliderLocal([CliOption("actor", Required = true)] Guid actor, [CliOption("center", Required = true)] Vector3 center)
        {
            var collider = RequireCollider(actor);
            using (new UndoBlock(Editor.Instance.Undo, collider, "Offset collider shape locally"))
                collider.Center = center;
            MarkEdited(collider);
            var saved = SaveSceneIfEdited(collider.Scene);
            return new
            {
                collider = DescribeCollider(collider),
                intent = "explicit-local-shape-offset",
                saved,
                dirty = Editor.Instance.Scene.IsEdited(collider.Scene),
            };
        }

        [CliCommand("colliders.normalize-center", Description = "Move a Collider Actor to its current world shape center and zero the local Center while preserving collider world bounds.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object NormalizeColliderCenter(
            [CliOption("actor", Required = true)] Guid actor,
            [CliOption("allow-hierarchy-motion", Description = "Allow moving an Actor that has children; child world positions will change.")] bool allowHierarchyMotion = false,
            [CliOption("allow-prefab-override", Description = "Allow creating transform and Center overrides on a Prefab-linked Actor.")] bool allowPrefabOverride = false)
        {
            var collider = RequireCollider(actor);
            if (collider.Center == Vector3.Zero)
            {
                return new
                {
                    collider = DescribeCollider(collider),
                    changed = false,
                    reason = "Center is already zero.",
                    saved = false,
                    dirty = Editor.Instance.Scene.IsEdited(collider.Scene),
                };
            }
            if (collider.ChildrenCount != 0 && !allowHierarchyMotion)
                throw new InvalidOperationException("Collider normalization would move child Actors. Re-run with --allow-hierarchy-motion only after reviewing the hierarchy.");
            if (collider.HasPrefabLink && !allowPrefabOverride)
                throw new InvalidOperationException("Collider normalization would create Prefab overrides. Re-run with --allow-prefab-override only when those overrides are intended.");

            var beforeBounds = EffectiveBounds(collider);
            var oldActorPosition = collider.Position;
            var oldCenter = collider.Center;
            var worldShapeCenter = collider.Transform.LocalToWorld(oldCenter);
            using (new UndoBlock(Editor.Instance.Undo, collider, "Normalize collider center"))
            {
                collider.Position = worldShapeCenter;
                collider.Center = Vector3.Zero;
            }
            MarkEdited(collider);
            var afterBounds = EffectiveBounds(collider);
            var saved = SaveSceneIfEdited(collider.Scene);
            return new
            {
                collider = DescribeCollider(collider),
                changed = true,
                oldActorPosition = DescribeVector(oldActorPosition),
                oldLocalCenter = DescribeVector(oldCenter),
                newActorPosition = DescribeVector(collider.Position),
                beforeWorldBounds = DescribeBounds(beforeBounds),
                afterWorldBounds = DescribeBounds(afterBounds),
                saved,
                dirty = Editor.Instance.Scene.IsEdited(collider.Scene),
            };
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
            if (!action.TryDo())
                throw new InvalidOperationException($"Failed to reparent Actor: {action.LastResult?.ErrorCode} {action.LastResult?.Message}");
            Editor.Instance.Undo.AddAction(action);
            var saved = SaveSceneIfEdited(value.Scene);
            if (oldScene != value.Scene)
                saved |= SaveSceneIfEdited(oldScene);
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
            tags = (tags ?? Array.Empty<string>()).Where(x => !string.IsNullOrWhiteSpace(x)).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
            CliAssetPersistence.PersistTags(tags);
            using (new UndoBlock(Editor.Instance.Undo, value, "Set actor tags"))
                value.Tags = tags.Select(Tags.Get).ToArray();
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

        [CliCommand("actors.property.list", Description = "List direct public Actor fields and properties.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object ListActorProperties([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            var properties = value.GetType().GetProperties(BindingFlags.Instance | BindingFlags.Public)
                .Where(x => x.CanRead && x.GetIndexParameters().Length == 0)
                .Select(x => new { name = x.Name, type = x.PropertyType.FullName, writable = x.CanWrite })
                .Concat(value.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public)
                    .Select(x => new { name = x.Name, type = x.FieldType.FullName, writable = !x.IsInitOnly && !x.IsLiteral }))
                .OrderBy(x => x.name, StringComparer.OrdinalIgnoreCase)
                .ToArray();
            return new { actor = DescribeActor(value), properties };
        }

        [CliCommand("actors.property.get", Description = "Get one direct public Actor field or property.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GetActorProperty([CliOption("actor", Required = true)] Guid actor, [CliOption("property", Required = true)] string property)
        {
            var value = RequireActor(actor);
            var member = RequirePublicMember(value, property, false);
            var memberType = GetMemberType(member);
            return new { actor = DescribeActor(value), property = member.Name, type = memberType.FullName, value = SerializeMemberValue(GetMemberValue(member, value), memberType) };
        }

        [CliCommand("actors.animated-model.node", Description = "Read one animated-model skeleton node transform.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GetAnimatedModelNode([CliOption("actor", Required = true)] Guid actor, [CliOption("node", Required = true)] string node, [CliOption("world-space")] bool worldSpace = false)
        {
            var value = RequireActor(actor) as AnimatedModel ?? throw new ArgumentException($"Actor '{actor}' is not an AnimatedModel.", nameof(actor));
            if (value.SkinnedModel == null || value.SkinnedModel.WaitForLoaded())
                throw new InvalidOperationException($"AnimatedModel '{actor}' has no loaded skinned model.");
            if (value.SkinnedModel.FindNode(node) < 0)
                throw new KeyNotFoundException($"Skeleton node '{node}' was not found.");
            value.GetNodeTransformation(node, out var transform, worldSpace);
            return new
            {
                actor = DescribeActor(value),
                node,
                worldSpace,
                matrix = new[]
                {
                    transform.M11, transform.M12, transform.M13, transform.M14,
                    transform.M21, transform.M22, transform.M23, transform.M24,
                    transform.M31, transform.M32, transform.M33, transform.M34,
                    transform.M41, transform.M42, transform.M43, transform.M44,
                },
            };
        }

        [CliCommand("actors.property.set", Description = "Set one direct public Actor field or property with undo, including Actor and asset references.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetActorProperty([CliOption("actor", Required = true)] Guid actor, [CliOption("property", Required = true)] string property, [CliOption("value", Required = true)] JToken value, [CliOption("persist", Description = "Synchronously save and verify the assigned value.")] bool persist = true)
        {
            var actorValue = RequireActor(actor);
            var member = RequirePublicMember(actorValue, property, true);
            if (actorValue is Collider && string.Equals(member.Name, nameof(Collider.Center), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Collider.Center is a local shape offset, not a placement transform. Use actors.transform or actors.place for placement, or colliders.offset-local for an intentional local offset.");
            var memberType = GetMemberType(member);
            var converted = ConvertMemberValue(value, memberType);
            // Reload verification necessarily destroys live SceneObject instances.
            // A live-object Undo action cannot remain valid across that boundary,
            // so only non-persisting edits enter the current Editor undo stack.
            if (persist)
                SetMemberValue(member, actorValue, converted);
            else
                using (new UndoBlock(Editor.Instance.Undo, actorValue, "Set actor property"))
                    SetMemberValue(member, actorValue, converted);
            MarkEdited(actorValue);
            var observed = SerializeMemberValue(GetMemberValue(member, actorValue), memberType);
            var saved = persist && SaveSceneIfEdited(actorValue.Scene);
            var persisted = !persist;
            JToken reloadedValue = null;
            Actor reloadedActor = actorValue;
            if (persist)
            {
                var scene = actorValue.Scene;
                var sceneId = scene.ID;
                Level.UnloadScene(scene);
                if (Level.LoadScene(sceneId))
                    throw new InvalidOperationException($"Failed to reload Scene '{sceneId}' for persistence verification.");
                reloadedActor = Object.Find<Actor>(ref actor);
                if (reloadedActor != null)
                {
                    var reloadedMember = RequirePublicMember(reloadedActor, member.Name, false);
                    reloadedValue = SerializeMemberValue(GetMemberValue(reloadedMember, reloadedActor), GetMemberType(reloadedMember));
                    persisted = JToken.DeepEquals(observed, reloadedValue);
                }
            }
            return new
            {
                actor = DescribeActor(reloadedActor ?? actorValue),
                property = member.Name,
                type = memberType.FullName,
                value = observed,
                reloadedValue,
                verified = persisted,
                persisted,
                saved,
                dirty = reloadedActor != null && Editor.Instance.Scene.IsEdited(reloadedActor.Scene),
            };
        }

        [CliCommand("actors.component.add", Description = "Add a Script component to an Actor with undo.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object AddComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("type", Required = true)] string type)
        {
            var value = RequireActor(actor);
            var scriptType = TypeUtils.GetType(type);
            if (!scriptType || !new ScriptType(typeof(Script)).IsAssignableFrom(scriptType) || !scriptType.CanCreateInstance)
                throw new ArgumentException($"Type '{type}' is not a creatable Script type.", nameof(type));
            var action = AddRemoveScript.Add(value, scriptType);
            if (!action.TryDo())
                throw new InvalidOperationException("Failed to add the Script without changing history.");
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
            if (!action.TryDo())
                throw new InvalidOperationException("Failed to remove the Script without changing history.");
            Editor.Instance.Undo.AddAction(action);
            var saved = SaveSceneIfEdited(value.Scene);
            return new { actor = DescribeActor(value), component = description, deleted = true, saved, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        [CliCommand("actors.component.get", Description = "Get a Script component or one public field or property.", Access = CliCommandAccess.ReadOnly, RequiresScene = true)]
        public static object GetComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("component", Required = true)] Guid component, [CliOption("property")] string property = null)
        {
            var script = RequireScript(RequireActor(actor), component);
            if (string.IsNullOrWhiteSpace(property))
                return new { component = DescribeScript(script), properties = JToken.Parse(FlaxJsonSerializer.Serialize(script)) };
            var member = RequirePublicMember(script, property, false);
            return new { component = DescribeScript(script), property = member.Name, value = SerializeMemberValue(GetMemberValue(member, script), GetMemberType(member)) };
        }

        [CliCommand("actors.component.set", Description = "Set one public Script field or property with undo and durable persistence by default.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object SetComponent([CliOption("actor", Required = true)] Guid actor, [CliOption("component", Required = true)] Guid component, [CliOption("property", Required = true)] string property, [CliOption("value", Required = true)] JToken value, [CliOption("persist", Description = "Synchronously save and verify the assigned value.")] bool persist = true)
        {
            var actorValue = RequireActor(actor);
            var script = RequireScript(actorValue, component);
            var member = RequirePublicMember(script, property, true);
            var memberType = GetMemberType(member);
            var converted = ConvertMemberValue(value, memberType);
            if (persist)
                SetMemberValue(member, script, converted);
            else
                using (new UndoBlock(Editor.Instance.Undo, script, "Set component property"))
                    SetMemberValue(member, script, converted);
            MarkEdited(actorValue);
            var observed = SerializeMemberValue(GetMemberValue(member, script), memberType);
            var saved = persist && SaveSceneIfEdited(actorValue.Scene);
            var persisted = !persist;
            JToken reloadedValue = null;
            Actor reloadedActor = actorValue;
            Script reloadedScript = script;
            if (persist)
            {
                var scene = actorValue.Scene;
                var sceneId = scene.ID;
                Level.UnloadScene(scene);
                if (Level.LoadScene(sceneId))
                    throw new InvalidOperationException($"Failed to reload Scene '{sceneId}' for persistence verification.");
                reloadedActor = Object.Find<Actor>(ref actor);
                reloadedScript = reloadedActor?.Scripts.FirstOrDefault(x => x.ID == component);
                if (reloadedScript != null)
                {
                    var reloadedMember = RequirePublicMember(reloadedScript, member.Name, false);
                    reloadedValue = SerializeMemberValue(GetMemberValue(reloadedMember, reloadedScript), GetMemberType(reloadedMember));
                    persisted = JToken.DeepEquals(observed, reloadedValue);
                }
            }
            return new
            {
                actor = DescribeActor(reloadedActor ?? actorValue),
                component = DescribeScript(reloadedScript ?? script),
                property = member.Name,
                value = observed,
                reloadedValue,
                verified = persisted,
                persisted,
                saved,
                dirty = reloadedActor != null && Editor.Instance.Scene.IsEdited(reloadedActor.Scene),
            };
        }

        [CliCommand("prefabs.create", Description = "Create a Prefab from an Actor and link the Actor to it.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CreatePrefab([CliOption("actor", Required = true)] Guid actor, [CliOption("path", Required = true)] string path)
        {
            var value = RequireActor(actor);
            var outputPath = ResolveAuthoringPath(path, ".prefab", true);
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
            if (PrefabManager.CreatePrefab(value, outputPath, true))
                throw new InvalidOperationException($"Failed to create Prefab '{outputPath}'.");
            CreateCanonicalJsonMetadata(outputPath);
            RefreshCreatedContent(outputPath);
            FlaxEngine.Content.GetAssetInfo(outputPath, out var info);
            return new { prefabId = info.ID, path = outputPath, actor = DescribeActor(value), assetSaved = true, sceneSaved = false, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
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
                CreateCanonicalJsonMetadata(outputPath);
            }
            finally
            {
                Object.Destroy(ref root);
            }
            RefreshCreatedContent(outputPath);
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
            Editor.Instance.SceneEditing.Spawn(instance, parent.HasValue ? RequireActor(parent.Value) : RequireActiveScene(), -1, false);
            return new { prefabId, actor = DescribeActor(instance), saved = false, dirty = Editor.Instance.Scene.IsEdited(instance.Scene) };
        }

        [CliCommand("prefabs.apply", Description = "Apply all changes from a linked Prefab instance.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ApplyPrefab([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            Editor.Instance.Prefabs.ApplyAll(value);
            return new { prefabId = value.PrefabID, actor = DescribeActor(value), assetSaved = true, sceneSaved = false, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
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
            DeleteActorsAction createReplacement = null;
            try
            {
                Level.SpawnActor(replacement, parent);
                replacement.OrderInParent = order;
                var replacementNode = Editor.Instance.Scene.GetActorNode(replacement) ?? throw new InvalidOperationException("The replacement Actor is missing from the Editor scene graph.");
                replacementNode.PostSpawn();
                createReplacement = new DeleteActorsAction(replacementNode, true);

                if (!removeOld.TryDo())
                    throw new InvalidOperationException($"Failed to remove the old Prefab instance: {removeOld.LastResult?.ErrorCode} {removeOld.LastResult?.Message}");
                replacement.OrderInParent = order;

                Editor.Instance.Undo.AddAction(new MultiUndoAction(new IUndoAction[] { createReplacement, removeOld }, "Revert Prefab"));
                return new { prefabId, removedActorId = actor, actor = DescribeActor(replacement), saved = false, dirty = Editor.Instance.Scene.IsEdited(replacement.Scene) };
            }
            catch
            {
                if (removeOld.LastResult?.Succeeded == true)
                    removeOld.TryUndo();
                if (createReplacement != null)
                    createReplacement.TryUndo();
                else if (replacement)
                    Object.Destroy(ref replacement);
                FlaxEngine.Scripting.FlushRemovedObjects();
                throw;
            }
        }

        [CliCommand("prefabs.unpack", Description = "Break the Prefab link for an Actor hierarchy with undo.", Access = CliCommandAccess.Destructive, RequiresScene = true)]
        public static object UnpackPrefab([CliOption("actor", Required = true)] Guid actor)
        {
            var value = RequireActor(actor);
            if (!value.HasPrefabLink)
                throw new InvalidOperationException("The Actor is not linked to a Prefab.");
            var prefabId = value.PrefabID;
            var action = BreakPrefabLinkAction.Break(value);
            if (!action.TryDo())
                throw new InvalidOperationException("Failed to unpack the Prefab instance without changing history.");
            Editor.Instance.Undo.AddAction(action);
            MarkEdited(value);
            return new { prefabId, actor = DescribeActor(value), unpacked = true, saved = false, dirty = Editor.Instance.Scene.IsEdited(value.Scene) };
        }

        private static Actor CreateActor(CliActorCreateOptions options)
        {
            var type = RequireType(options.Type, typeof(Actor));
            var actor = (Actor)Object.New(type);
            if (!string.IsNullOrWhiteSpace(options.Name)) actor.Name = options.Name;
            if (options.Position.HasValue) actor.LocalPosition = options.Position.Value;
            if (options.Rotation.HasValue) actor.LocalEulerAngles = options.Rotation.Value;
            if (options.Scale.HasValue) actor.LocalScale = options.Scale.Value;
            var parent = options.Parent.HasValue ? RequireActor(options.Parent.Value) : RequireActiveScene();
            Editor.Instance.SceneEditing.Spawn(actor, parent, -1, false);
            if (actor is Collider collider)
            {
                // Editor BoxCollider spawning may auto-fit the parent and offset
                // Center. Generic CLI creation preserves constructor defaults and
                // uses the Actor transform as the spatial authority.
                collider.Center = Vector3.Zero;
                if (collider is BoxCollider box)
                    box.Size = new Float3(100.0f);
            }
            return actor;
        }

        private static Scene RequireActiveScene()
        {
            return Editor.Instance.Scene.ActiveScene ?? throw new InvalidOperationException("No unambiguous active destination Scene is selected. Use scenes.active.set or provide an Actor parent.");
        }

        private static List<ActorNode> ResolveClipboardNodes(Guid[] actorIds)
        {
            if (actorIds == null || actorIds.Length == 0)
                throw new ArgumentException("At least one Actor ID is required.", nameof(actorIds));
            var nodes = new List<ActorNode>(actorIds.Length);
            var unique = new HashSet<Guid>();
            for (int i = 0; i < actorIds.Length; i++)
            {
                if (!unique.Add(actorIds[i]))
                    continue;
                var actor = RequireActor(actorIds[i]);
                if (actor is Scene)
                    throw new InvalidOperationException("Scene roots cannot be copied or cut as Actors.");
                var node = Editor.Instance.Scene.GetActorNode(actor) ?? throw new InvalidOperationException($"Actor '{actorIds[i]}' is missing from the Scene graph.");
                if (!node.CanCopyPaste)
                    throw new InvalidOperationException($"Actor '{actorIds[i]}' cannot be copied.");
                nodes.Add(node);
            }
            return nodes.BuildNodesParents();
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

        private static Collider RequireCollider(Guid id)
        {
            var actor = RequireActor(id);
            return actor as Collider ?? throw new ArgumentException($"Actor '{id}' is a '{actor.TypeName}', not a Collider.", nameof(id));
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

        private static string RequirePrimitiveName(string shape)
        {
            if (string.IsNullOrWhiteSpace(shape))
                throw new ArgumentException("A primitive shape is required.", nameof(shape));
            var result = PrimitiveNames.FirstOrDefault(x => string.Equals(x, shape.Trim(), StringComparison.OrdinalIgnoreCase));
            return result ?? throw new ArgumentException($"Unknown primitive '{shape}'. Expected one of: {string.Join(", ", PrimitiveNames)}.", nameof(shape));
        }

        private static Model LoadPrimitive(string name)
        {
            var path = StringUtils.CombinePaths(Globals.EngineContentFolder, $"Editor/Primitives/{name}.flax");
            var model = FlaxEngine.Content.Load<Model>(path) ?? throw new FileNotFoundException($"Built-in primitive '{name}' was not found.", path);
            if (model.WaitForLoaded())
                throw new InvalidOperationException($"Failed to load built-in primitive '{name}'.");
            return model;
        }

        private static object DescribePrimitive(string name, Model model)
        {
            var bounds = model.GetBox();
            return new
            {
                shape = name.ToLowerInvariant(),
                modelId = model.ID,
                uri = $"engine://Editor/Primitives/{name}.flax",
                bounds = new
                {
                    minimum = new { x = bounds.Minimum.X, y = bounds.Minimum.Y, z = bounds.Minimum.Z },
                    maximum = new { x = bounds.Maximum.X, y = bounds.Maximum.Y, z = bounds.Maximum.Z },
                    size = new { x = bounds.Size.X, y = bounds.Size.Y, z = bounds.Size.Z },
                },
            };
        }

        private static MemberInfo RequirePublicMember(object target, string name, bool writable)
        {
            if (string.IsNullOrWhiteSpace(name) || name.Contains(".") || name.Contains("[") || name.Contains("]"))
                throw new ArgumentException("Component members require a direct public field or property name.", nameof(name));
            var property = target.GetType().GetProperty(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase);
            if (property != null && property.CanRead && (!writable || property.CanWrite) && property.GetIndexParameters().Length == 0)
                return property;
            var field = target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.IgnoreCase);
            if (field != null && (!writable || (!field.IsInitOnly && !field.IsLiteral)))
                return field;
            throw new ArgumentException($"Member '{name}' is not a {(writable ? "writable " : string.Empty)}public field or property on '{target.GetType().FullName}'.", nameof(name));
        }

        private static Type GetMemberType(MemberInfo member)
        {
            return member is PropertyInfo property ? property.PropertyType : ((FieldInfo)member).FieldType;
        }

        private static object GetMemberValue(MemberInfo member, object target)
        {
            return member is PropertyInfo property ? property.GetValue(target) : ((FieldInfo)member).GetValue(target);
        }

        private static void SetMemberValue(MemberInfo member, object target, object value)
        {
            if (member is PropertyInfo property)
                property.SetValue(target, value);
            else
                ((FieldInfo)member).SetValue(target, value);
        }

        private static JToken SerializeMemberValue(object value, Type type)
        {
            if (value == null)
                return JValue.CreateNull();
            if (value is Object flaxObject)
                return new JValue(flaxObject.ID.ToString());
            return JToken.Parse(FlaxJsonSerializer.Serialize(value, type));
        }

        private static object ConvertMemberValue(JToken value, Type type)
        {
            if (type.IsArray)
            {
                if (value.Type == JTokenType.Null)
                    return null;
                // Command-line shells necessarily transport complex option
                // values as strings. Accept a JSON array encoded in that string
                // so typed component arrays can be authored without arbitrary
                // in-process C# evaluation.
                if (value.Type == JTokenType.String)
                {
                    var encoded = value.Value<string>();
                    try
                    {
                        value = JToken.Parse(encoded);
                    }
                    catch (Exception ex)
                    {
                        throw new ArgumentException($"A '{type.FullName}' value requires a JSON array; '{encoded}' is not valid JSON: {ex.Message}", nameof(value));
                    }
                }
                if (value is not JArray values)
                    throw new ArgumentException($"A '{type.FullName}' value requires a JSON array.", nameof(value));
                var elementType = type.GetElementType();
                var result = Array.CreateInstance(elementType, values.Count);
                for (var i = 0; i < values.Count; i++)
                    result.SetValue(ConvertMemberValue(values[i], elementType), i);
                return result;
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(JsonAssetReference<>))
            {
                if (value.Type == JTokenType.Null)
                    return Activator.CreateInstance(type);
                if (value.Type != JTokenType.String)
                    throw new ArgumentException($"A '{type.FullName}' reference requires an asset GUID, asset URI, or null.", nameof(value));
                var reference = value.Value<string>();
                JsonAsset asset;
                if (Guid.TryParse(reference, out var id))
                {
                    asset = FlaxEngine.Content.LoadAsync<JsonAsset>(id);
                }
                else
                {
                    var path = ResolveAssetReference(reference);
                    if (Editor.Instance.ContentDatabase.Find(path) is FlaxEditor.Content.AssetItem item)
                        id = item.ID;
                    else if (FlaxEngine.Content.GetAssetInfo(path, out var info) && info.ID != Guid.Empty)
                        id = info.ID;
                    else
                        throw new KeyNotFoundException($"Asset reference '{reference}' was not found in the Content database.");
                    asset = FlaxEngine.Content.LoadAsync<JsonAsset>(id);
                }
                if (!asset || asset.WaitForLoaded())
                    throw new InvalidOperationException($"Json asset reference '{reference}' failed to load.");
                var result = Activator.CreateInstance(type);
                type.GetField("Asset").SetValue(result, asset);
                return result;
            }
            if (typeof(Object).IsAssignableFrom(type))
            {
                if (value.Type == JTokenType.Null)
                    return null;
                if (value.Type != JTokenType.String)
                    throw new ArgumentException($"A '{type.FullName}' reference requires an object GUID, asset URI, or null.", nameof(value));
                var reference = value.Value<string>();
                Object result;
                if (Guid.TryParse(reference, out var id))
                {
                    result = typeof(Asset).IsAssignableFrom(type)
                        ? FlaxEngine.Content.LoadAsync(id, type)
                        : Object.Find(ref id, type, true);
                }
                else if (typeof(Asset).IsAssignableFrom(type))
                {
                    var path = ResolveAssetReference(reference);
                    if (Editor.Instance.ContentDatabase.Find(path) is FlaxEditor.Content.AssetItem item)
                    {
                        id = item.ID;
                    }
                    else if (FlaxEngine.Content.GetAssetInfo(path, out var info) && info.ID != Guid.Empty)
                    {
                        id = info.ID;
                    }
                    else
                    {
                        throw new KeyNotFoundException($"Asset reference '{reference}' was not found in the Content database.");
                    }
                    result = FlaxEngine.Content.LoadAsync(id, type);
                }
                else
                {
                    throw new ArgumentException($"A '{type.FullName}' object reference requires a GUID string or null.", nameof(value));
                }
                if (result == null)
                    throw new KeyNotFoundException($"Reference '{reference}' was not found or is not assignable to '{type.FullName}'.");
                if (result is Asset asset && asset.WaitForLoaded())
                    throw new InvalidOperationException($"Asset reference '{reference}' failed to load.");
                return result;
            }
            var converted = value.ToObject(type, JsonSerializer.Create(FlaxJsonSerializer.Settings));
            // Newtonsoft cannot resolve Flax object references nested inside
            // authoring structs (for example AudioPhysicsRule.Events). Populate
            // those public members through the same stable-ID conversion path.
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

        private static string ResolveAssetReference(string reference)
        {
            if (string.IsNullOrWhiteSpace(reference))
                throw new ArgumentException("An asset reference cannot be empty.", nameof(reference));
            if (reference.StartsWith("engine://", StringComparison.OrdinalIgnoreCase))
                return StringUtils.CombinePaths(Globals.EngineContentFolder, reference.Substring("engine://".Length));
            if (reference.StartsWith("project://", StringComparison.OrdinalIgnoreCase))
                return StringUtils.CombinePaths(Globals.ProjectContentFolder, reference.Substring("project://".Length));
            if (reference.StartsWith("primitive:", StringComparison.OrdinalIgnoreCase))
            {
                var primitive = RequirePrimitiveName(reference.Substring("primitive:".Length));
                return StringUtils.CombinePaths(Globals.EngineContentFolder, $"Editor/Primitives/{primitive}.flax");
            }
            throw new ArgumentException("Asset references must use a GUID, engine:// URI, project:// URI, or primitive:<shape> alias.", nameof(reference));
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
            if (requireNew && (File.Exists(result) || File.Exists(result + ".meta") || Directory.Exists(result) || Editor.Instance.ContentDatabase.Find(result) != null))
                throw new IOException($"Authoring path '{result}' already exists.");
            ValidateNoLinks(contentRoot, result);
            return result;
        }

        private static void RefreshCreatedContent(string path)
        {
            var database = Editor.Instance.ContentDatabase;
            var folder = database.Find(Path.GetDirectoryName(path));
            if (folder != null)
            {
                database.RefreshFolder(folder, false);
                return;
            }

            // A command may create a nested directory that the content tree has not
            // observed yet. Refresh the project Content root recursively so both the
            // new directory and its asset are registered before resolving the ID.
            var contentRoot = database.Find(Globals.ProjectContentFolder);
            if (contentRoot == null)
                throw new InvalidOperationException("The project Content folder is not available in the content database.");
            database.RefreshFolder(contentRoot, true);
        }

        private static void CreateCanonicalJsonMetadata(string path)
        {
            if (CanonicalGraphDocuments.UseNewAssetDatabase && AssetDatabaseFacade.CreateExistingJsonMetadata(path) == Guid.Empty)
                throw new IOException($"Failed to create canonical metadata for '{path}'.");
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

        private static object DescribeBuildScenes()
        {
            var game = GameSettings.Load();
            var build = GameSettings.Load<BuildSettings>() ?? new BuildSettings();
            var entries = new List<object>();
            var seen = new HashSet<AssetObjectId>();
            if (!game.FirstScene.ID.IsNull)
            {
                entries.Add(DescribeBuildScene(game.FirstScene.ID, 0, true));
                seen.Add(game.FirstScene.ID);
            }
            foreach (var reference in build.AdditionalScenes ?? Array.Empty<SceneReference>())
            {
                if (!reference.ID.IsNull && seen.Add(reference.ID))
                    entries.Add(DescribeBuildScene(reference.ID, entries.Count, false));
            }
            return new
            {
                semantics = "GameSettings.FirstScene is the required startup scene. BuildSettings.AdditionalScenes are extra cooked roots in their persisted order.",
                startupSceneId = game.FirstScene.ID,
                count = entries.Count,
                scenes = entries.ToArray(),
            };
        }

        private static object DescribeBuildScene(AssetObjectId id, int index, bool startup)
        {
            var backing = AssetDatabaseFacade.GetBackingAssetID(id);
            FlaxEngine.Content.GetAssetInfo(backing, out var info);
            return new { index, id, path = info.Path, name = Path.GetFileNameWithoutExtension(info.Path), startup, valid = info.ID != Guid.Empty };
        }

        private static void SaveSettings<T>(T settings) where T : FlaxEditor.Content.Settings.SettingsBase
        {
            if (GameSettings.Save(settings))
                throw new IOException($"Failed to save {typeof(T).Name}. See the Editor log for details.");
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
            var world = actor.Transform;
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
                worldTransform = new
                {
                    position = new { x = world.Translation.X, y = world.Translation.Y, z = world.Translation.Z },
                    rotation = new { x = actor.EulerAngles.X, y = actor.EulerAngles.Y, z = actor.EulerAngles.Z },
                    scale = new { x = world.Scale.X, y = world.Scale.Y, z = world.Scale.Z },
                },
                worldBounds = DescribeBounds(EffectiveBounds(actor)),
                prefab = actor.HasPrefabLink ? new { id = actor.PrefabID, objectId = actor.PrefabObjectID, root = actor.IsPrefabRoot } : null,
                components = actor.Scripts.Select(DescribeScript).ToArray(),
                children = EnumerateChildren(actor).Select(DescribeActor).ToArray(),
            };
        }

        private static object DescribeCollider(Collider collider)
        {
            object shape;
            if (collider is BoxCollider box)
                shape = new { type = "box", size = DescribeVector(box.Size) };
            else if (collider is SphereCollider sphere)
                shape = new { type = "sphere", radius = sphere.Radius };
            else if (collider is CapsuleCollider capsule)
                shape = new { type = "capsule", radius = capsule.Radius, height = capsule.Height };
            else
                shape = new { type = collider.TypeName };
            var center = collider.Center;
            return new
            {
                actor = DescribeActorDetails(collider),
                collider.IsTrigger,
                localCenter = DescribeVector(center),
                worldShapeCenter = DescribeVector(collider.Transform.LocalToWorld(center)),
                usesLocalOffset = center != Vector3.Zero,
                shape,
            };
        }

        private static BoundingBox EffectiveBounds(Actor actor)
        {
            var bounds = actor.Box;
            if (bounds.Minimum.X > bounds.Maximum.X || bounds.Minimum.Y > bounds.Maximum.Y || bounds.Minimum.Z > bounds.Maximum.Z)
                return new BoundingBox(actor.Position, actor.Position);
            return bounds;
        }

        private static object DescribeBounds(BoundingBox bounds)
        {
            return new
            {
                minimum = new { x = bounds.Minimum.X, y = bounds.Minimum.Y, z = bounds.Minimum.Z },
                maximum = new { x = bounds.Maximum.X, y = bounds.Maximum.Y, z = bounds.Maximum.Z },
                center = new { x = bounds.Center.X, y = bounds.Center.Y, z = bounds.Center.Z },
                size = new { x = bounds.Size.X, y = bounds.Size.Y, z = bounds.Size.Z },
            };
        }

        private static object DescribeVector(Vector3 value)
        {
            return new { x = value.X, y = value.Y, z = value.Z };
        }

        private static object DescribeVector(Float3 value)
        {
            return new { x = value.X, y = value.Y, z = value.Z };
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

        private static Guid[] SaveEditedScenes()
        {
            if (Editor.IsPlayMode)
                return Array.Empty<Guid>();
            var edited = Level.Scenes.Where(x => Editor.Instance.Scene.IsEdited(x)).ToArray();
            foreach (var scene in edited)
                SaveSceneSynchronously(scene);
            return edited.Select(x => x.ID).ToArray();
        }

        private static void EnsureScenesClean(string operation)
        {
            var edited = Level.Scenes.Where(x => Editor.Instance.Scene.IsEdited(x)).Select(x => x.ID).ToArray();
            if (edited.Length != 0)
                throw new InvalidOperationException($"Cannot {operation} while loaded scenes have unsaved changes ({string.Join(", ", edited)}). Run 'flax scenes save' or 'flax editor save-all' first.");
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
            if (!Editor.Instance.Scene.SaveSceneSynchronously(scene))
                throw new IOException($"Failed to save scene '{scene.Name}'. See the Editor log for details.");
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
