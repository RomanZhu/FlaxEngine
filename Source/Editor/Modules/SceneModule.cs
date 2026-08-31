// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using FlaxEditor.SceneEditing;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEngine;
using FlaxEngine.GUI;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Scenes and actors management module.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class SceneModule : EditorModule
    {
        private struct PendingSceneSave
        {
            public long DirtyGeneration;
            public int UndoState;
        }

        /// <summary>
        /// The root node for the scene graph created for the loaded scenes and actors hierarchy.
        /// </summary>
        /// <seealso cref="FlaxEditor.SceneGraph.RootNode" />
        public class ScenesRootNode : RootNode
        {
            private readonly Editor _editor;

            /// <inheritdoc />
            public ScenesRootNode()
            {
                _editor = Editor.Instance;
            }

            /// <inheritdoc />
            public override void Spawn(Actor actor, Actor parent, int orderInParent = -1)
            {
                _editor.SceneEditing.Spawn(actor, parent, orderInParent);
            }

            /// <inheritdoc />
            public override Undo Undo => Editor.Instance.Undo;

            /// <inheritdoc />
            public override ISceneEditingContext SceneContext => _editor.Windows.EditWin;
        }

        /// <summary>
        /// The root tree node for the whole scene graph.
        /// </summary>
        public ScenesRootNode Root;

        internal bool SuppressUndoDirtyTracking;

        private const double SceneDiskChangePromptDelaySeconds = 0.5;
        private const double SceneDiskChangeIgnoreAfterSaveSeconds = 2.0;
        private const string SceneActorsFolderName = "SceneActors";
        private const string ExternalActorsFolderName = "ExternalActors";
        private const string ExternalActorExtension = ".actor";
        private readonly object _sceneDiskChangesLock = new object();
        private readonly Dictionary<string, DateTime> _pendingSceneDiskChanges = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, DateTime> _ignoredSceneDiskChanges = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<Guid, long> _sceneDirtyGenerations = new Dictionary<Guid, long>();
        private readonly Dictionary<Guid, long> _sceneSavedGenerations = new Dictionary<Guid, long>();
        private readonly Dictionary<Guid, Queue<PendingSceneSave>> _pendingSceneSaveGenerations = new Dictionary<Guid, Queue<PendingSceneSave>>();
        private readonly HashSet<Guid> _capturedSceneEdits = new HashSet<Guid>();
        private int _sceneEditCaptureDepth;
        private Guid _activeSceneId;
        private FileSystemWatcher _sceneActorsWatcher;
        private DateTime _nextSceneActorsWatcherRetry;
        private bool _sceneActorsWatcherError;
        private bool _sceneActorsFolderWasMissing;

        /// <summary>
        /// Occurs when actor gets removed. Editor and all submodules should remove references to that actor.
        /// </summary>
        public event Action<ActorNode> ActorRemoved;

        /// <summary>
        /// Occurs when the editor scene graph changes.
        /// </summary>
        public event Action SceneGraphChanged;

        /// <summary>
        /// Gets the Scene selected by the most recent explicit scene interaction.
        /// Falls back only when exactly one Scene is loaded.
        /// </summary>
        public Scene ActiveScene
        {
            get
            {
                var active = _activeSceneId != Guid.Empty ? Level.FindScene(_activeSceneId) : null;
                if (active != null)
                    return active;
                return Level.ScenesCount == 1 ? Level.GetScene(0) : null;
            }
        }

        internal SceneModule(Editor editor)
        : base(editor)
        {
            // After editor cache but before the windows
            InitOrder = -800;
        }

        /// <summary>
        /// Marks the scene as modified.
        /// </summary>
        /// <param name="scene">The scene.</param>
        public void MarkSceneEdited(Scene scene)
        {
            MarkSceneEdited(GetActorNode(scene) as SceneNode);
        }

        /// <summary>
        /// Marks the scene as modified.
        /// </summary>
        /// <param name="scene">The scene.</param>
        public void MarkSceneEdited(SceneNode scene)
        {
            if (scene == null)
                return;

            var sceneId = scene.Scene.ID;
            if (_sceneEditCaptureDepth != 0)
                _capturedSceneEdits.Add(sceneId);
            _sceneDirtyGenerations.TryGetValue(sceneId, out var generation);
            _sceneDirtyGenerations[sceneId] = generation + 1;
            _activeSceneId = sceneId;
            scene.IsEdited = true;
            if (!SuppressUndoDirtyTracking)
                Editor.Undo.MarkSceneChangedOutsideUndo(sceneId);
        }

        internal void BeginSceneEditCapture()
        {
            if (_sceneEditCaptureDepth++ == 0)
                _capturedSceneEdits.Clear();
        }

        internal Guid[] EndSceneEditCapture()
        {
            if (_sceneEditCaptureDepth == 0 || --_sceneEditCaptureDepth != 0)
                return Array.Empty<Guid>();
            var result = new Guid[_capturedSceneEdits.Count];
            _capturedSceneEdits.CopyTo(result);
            _capturedSceneEdits.Clear();
            return result;
        }

        /// <summary>
        /// Sets the active Scene from explicit selection or authoring context.
        /// </summary>
        /// <param name="scene">The active Scene.</param>
        public void SetActiveScene(Scene scene)
        {
            if (scene != null && Level.FindScene(scene.ID) == scene)
                _activeSceneId = scene.ID;
        }

        /// <summary>
        /// Marks the scenes as modified.
        /// </summary>
        /// <param name="scenes">The scenes.</param>
        public void MarkSceneEdited(IEnumerable<Scene> scenes)
        {
            foreach (var scene in scenes)
                MarkSceneEdited(scene);
        }

        /// <summary>
        /// Marks all the scenes as modified.
        /// </summary>
        public void MarkAllScenesEdited()
        {
            MarkSceneEdited(Level.Scenes);
        }

        /// <summary>
        /// Determines whether the specified scene is edited.
        /// </summary>
        /// <param name="scene">The scene.</param>
        /// <returns><c>true</c> if the specified scene is edited; otherwise, <c>false</c>.</returns>
        public bool IsEdited(Scene scene)
        {
            var node = GetActorNode(scene) as SceneNode;
            return node?.IsEdited ?? false;
        }

        /// <summary>
        /// Determines whether any scene is edited.
        /// </summary>
        /// <returns><c>true</c> if any scene is edited; otherwise, <c>false</c>.</returns>
        public bool IsEdited()
        {
            foreach (var scene in Root.ChildNodes)
            {
                if (scene is SceneNode node && node.IsEdited)
                    return true;
            }
            return false;
        }

        internal void ClearEditedScenes()
        {
            foreach (var scene in Root.ChildNodes)
            {
                if (scene is SceneNode node)
                {
                    node.IsEdited = false;
                    var sceneId = node.Scene.ID;
                    _sceneDirtyGenerations.TryGetValue(sceneId, out var generation);
                    _sceneSavedGenerations[sceneId] = generation;
                }
            }
        }

        /// <summary>
        /// Determines whether every scene is edited.
        /// </summary>
        /// <returns><c>true</c> if every scene is edited; otherwise, <c>false</c>.</returns>
        public bool IsEverySceneEdited()
        {
            foreach (var scene in Root.ChildNodes)
            {
                if (scene is SceneNode node && !node.IsEdited)
                    return false;
            }
            return true;
        }

        /// <summary>
        /// Creates the new scene file. The default scene contains set of simple actors.
        /// </summary>
        /// <param name="path">The path.</param>
        public void CreateSceneFile(string path)
        {
            // Create a sample scene
            var scene = new Scene
            {
                StaticFlags = StaticFlags.FullyStatic
            };

            //
            var sun = scene.AddChild<DirectionalLight>();
            sun.Name = "Sun";
            sun.LocalPosition = new Vector3(40, 160, 0);
            sun.LocalEulerAngles = new Vector3(45, 0, 0);
            sun.StaticFlags = StaticFlags.FullyStatic;
            //
            var sky = scene.AddChild<Sky>();
            sky.Name = "Sky";
            sky.LocalPosition = new Vector3(40, 150, 0);
            sky.SunLight = sun;
            sky.StaticFlags = StaticFlags.FullyStatic;
            //
            var skyLight = scene.AddChild<SkyLight>();
            skyLight.Mode = SkyLight.Modes.CustomTexture;
            skyLight.Brightness = 2.5f;
            skyLight.CustomTexture = FlaxEngine.Content.LoadAsyncInternal<CubeTexture>(EditorAssets.DefaultSkyCubeTexture);
            skyLight.StaticFlags = StaticFlags.FullyStatic;
            //
            var floor = scene.AddChild<StaticModel>();
            floor.Name = "Floor";
            floor.Scale = new Float3(4, 0.5f, 4);
            floor.Model = FlaxEngine.Content.LoadAsync<Model>(StringUtils.CombinePaths(Globals.EngineContentFolder, "Editor/Primitives/Cube.flax"));
            if (floor.Model)
            {
                floor.Model.WaitForLoaded();
                floor.SetMaterial(0, FlaxEngine.Content.LoadAsync<MaterialBase>(StringUtils.CombinePaths(Globals.EngineContentFolder, "Engine/WhiteMaterial.flax")));
            }
            floor.StaticFlags = StaticFlags.FullyStatic;
            //
            var cam = scene.AddChild<Camera>();
            cam.Name = "Camera";
            cam.Position = new Vector3(0, 150, -300);
            //
            var audioListener = cam.AddChild<AudioListener>();
            audioListener.Name = "Audio Listener";

            // Serialize
            var bytes = Level.SaveSceneToBytes(scene);

            // Cleanup
            Object.Destroy(ref scene);

            if (bytes == null || bytes.Length == 0)
                throw new Exception("Failed to serialize scene.");

            // Write to file
            using (var fileStream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read))
                fileStream.Write(bytes, 0, bytes.Length);
        }

        /// <summary>
        /// Saves scene (async).
        /// </summary>
        /// <param name="scene">Scene to save.</param>
        public void SaveScene(Scene scene)
        {
            SaveScene(GetActorNode(scene) as SceneNode);
        }

        /// <summary>
        /// Saves scene (async).
        /// </summary>
        /// <param name="scene">Scene to save.</param>
        public void SaveScene(SceneNode scene)
        {
            if (Editor.MultiplayerPlayMode.IsReplica)
                return;
            if (scene == null || !scene.IsEdited)
                return;

            QueueSaveCompletion(scene);
            if (SceneSaveFaults.ShouldFail(scene.Scene))
            {
                OnSceneSaveError(scene.Scene, scene.Scene.ID);
                return;
            }
            Level.SaveSceneAsync(scene.Scene);
        }

        /// <summary>
        /// Saves a Scene synchronously and only marks the captured generation clean after success.
        /// </summary>
        /// <param name="scene">Scene to save.</param>
        /// <returns>True when the save completed successfully.</returns>
        public bool SaveSceneSynchronously(Scene scene)
        {
            if (Editor.MultiplayerPlayMode.IsReplica || scene == null)
                return false;
            var node = GetActorNode(scene) as SceneNode;
            if (node == null)
                return false;
            if (!node.IsEdited)
                return true;

            QueueSaveCompletion(node);
            if (SceneSaveFaults.ShouldFail(scene))
            {
                OnSceneSaveError(scene, scene.ID);
                return false;
            }
            var failed = Level.SaveScene(scene);
            if (failed)
            {
                node.IsEdited = true;
                return false;
            }
            return !node.IsEdited;
        }

        /// <summary>
        /// Saves all open scenes (async).
        /// </summary>
        public void SaveScenes()
        {
            if (Editor.MultiplayerPlayMode.IsReplica)
                return;
            if (!IsEdited())
                return;

            var queued = 0;
            foreach (var scene in Root.ChildNodes)
            {
                if (scene is SceneNode node && node.IsEdited)
                {
                    QueueSaveCompletion(node);
                    if (SceneSaveFaults.ShouldFail(node.Scene))
                        OnSceneSaveError(node.Scene, node.Scene.ID);
                    else
                        Level.SaveSceneAsync(node.Scene);
                    queued++;
                }
            }
            if (queued != 0)
                Editor.UI.AddStatusMessage("Saving scenes...");
        }

        private void QueueSaveCompletion(SceneNode scene)
        {
            var sceneId = scene.Scene.ID;
            _sceneDirtyGenerations.TryGetValue(sceneId, out var generation);
            if (!_pendingSceneSaveGenerations.TryGetValue(sceneId, out var queue))
            {
                queue = new Queue<PendingSceneSave>();
                _pendingSceneSaveGenerations.Add(sceneId, queue);
            }
            queue.Enqueue(new PendingSceneSave
            {
                DirtyGeneration = generation,
                UndoState = Editor.Undo.GetSceneState(sceneId),
            });
            SceneDebug.Log("SaveRequested", $"Scene={sceneId} DirtyGeneration={generation} Path='{scene.Scene.Path}'");
        }

        private bool TryTakePendingSave(Guid sceneId, out PendingSceneSave save)
        {
            save = default;
            if (!_pendingSceneSaveGenerations.TryGetValue(sceneId, out var queue) || queue.Count == 0)
                return false;
            save = queue.Dequeue();
            if (queue.Count == 0)
                _pendingSceneSaveGenerations.Remove(sceneId);
            return true;
        }

        private void RemovePendingSave(Guid sceneId)
        {
            TryTakePendingSave(sceneId, out _);
        }

        /// <summary>
        /// Opens scene (async).
        /// </summary>
        /// <param name="sceneId">Scene ID</param>
        /// <param name="additive">True if don't close opened scenes and just add new scene to them, otherwise will release current scenes and load single one.</param>
        public void OpenScene(Guid sceneId, bool additive = false)
        {
            if (!Editor.StateMachine.CurrentState.CanChangeScene)
                return;

            // In play-mode Editor mocks the level streaming script
            if (Editor.IsPlayMode)
            {
                if (!additive)
                    Level.UnloadAllScenesAsync();
                Level.LoadSceneAsync(sceneId);
                return;
            }

            if (!additive)
            {
                // Ensure to save all pending changes
                if (CheckSaveBeforeClose())
                    return;
            }

            // Load scene
            Editor.StateMachine.ChangingScenesState.LoadScene(sceneId, additive);
        }

        /// <summary>
        /// Reload all loaded scenes.
        /// </summary>
        public void ReloadScenes()
        {
            if (!Editor.StateMachine.CurrentState.CanChangeScene)
                return;

            if (!Editor.IsPlayMode)
            {
                if (CheckSaveBeforeClose())
                    return;
            }

            // Reload scenes
            foreach (var scene in Level.Scenes)
            {
                var sceneId = scene.ID;
                Level.UnloadScene(scene);
                Level.LoadScene(sceneId);
            }
        }

        internal void QueueSceneDiskChange(FileSystemEventArgs e)
        {
            if (e == null)
                return;

            QueueSceneFileDiskChange(e.FullPath);
            if (e is RenamedEventArgs renamed)
                QueueSceneFileDiskChange(renamed.OldFullPath);
        }

        private void QueueSceneFileDiskChange(string path)
        {
            if (TryGetScenePathFromSceneFilePath(path, out var scenePath))
                QueueSceneDiskChange(scenePath);
        }

        private void QueueExternalActorDiskChange(string path)
        {
            if (TryGetScenePathFromExternalActorPath(path, out var scenePath))
                QueueSceneDiskChange(scenePath);
        }

        private void QueueSceneDiskChange(string scenePath)
        {
            if (string.IsNullOrEmpty(scenePath))
                return;

            scenePath = NormalizeAbsolutePath(scenePath);
            lock (_sceneDiskChangesLock)
            {
                _pendingSceneDiskChanges[scenePath] = DateTime.UtcNow;
            }
        }

        private static bool TryGetScenePathFromSceneFilePath(string path, out string scenePath)
        {
            scenePath = null;
            if (!string.Equals(Path.GetExtension(path), ".scene", StringComparison.OrdinalIgnoreCase))
                return false;

            path = NormalizeAbsolutePath(path);
            var contentFolder = NormalizeAbsolutePath(Globals.ProjectContentFolder).TrimEnd('/', '\\');
            if (path.Length <= contentFolder.Length ||
                !path.StartsWith(contentFolder, StringComparison.OrdinalIgnoreCase) ||
                (path[contentFolder.Length] != '/' && path[contentFolder.Length] != '\\'))
            {
                return false;
            }

            scenePath = path;
            return true;
        }

        internal static bool TryGetScenePathFromExternalActorPath(string path, out string scenePath)
        {
            scenePath = null;
            if (!string.Equals(Path.GetExtension(path), ExternalActorExtension, StringComparison.OrdinalIgnoreCase))
                return false;

            path = NormalizeAbsolutePath(path);
            var sceneActorsFolder = GetSceneActorsRootPath();
            if (path.Length <= sceneActorsFolder.Length ||
                !path.StartsWith(sceneActorsFolder, StringComparison.OrdinalIgnoreCase) ||
                (path[sceneActorsFolder.Length] != '/' && path[sceneActorsFolder.Length] != '\\'))
            {
                return false;
            }

            var relativePath = path.Substring(sceneActorsFolder.Length + 1);
            var marker = "/" + ExternalActorsFolderName + "/";
            var markerIndex = relativePath.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
            if (markerIndex <= 0)
                return false;

            var sceneNamePath = relativePath.Substring(0, markerIndex);
            if (string.IsNullOrEmpty(sceneNamePath))
                return false;

            scenePath = StringUtils.NormalizePath(StringUtils.CombinePaths(Globals.ProjectContentFolder, sceneNamePath + ".scene"));
            return true;
        }

        private static string NormalizeAbsolutePath(string path)
        {
            return StringUtils.NormalizePath(Path.GetFullPath(path));
        }

        private static string GetSceneActorsRootPath()
        {
            return NormalizeAbsolutePath(StringUtils.CombinePaths(Globals.ProjectFolder, SceneActorsFolderName)).TrimEnd('/', '\\');
        }

        /// <summary>
        /// Closes scene (async).
        /// </summary>
        /// <param name="scene">The scene.</param>
        public void CloseScene(Scene scene)
        {
            if (!Editor.StateMachine.CurrentState.CanChangeScene)
                return;

            // In play-mode Editor mocks the level streaming script
            if (Editor.IsPlayMode)
            {
                Level.UnloadSceneAsync(scene);
                return;
            }

            // Ensure to save all pending changes
            if (CheckSaveBeforeClose())
                return;

            // Unload scene
            Editor.StateMachine.ChangingScenesState.UnloadScene(scene);
        }

        /// <summary>
        /// Closes all opened scene (async).
        /// </summary>
        public void CloseAllScenes()
        {
            if (!Editor.StateMachine.CurrentState.CanChangeScene)
                return;

            // In play-mode Editor mocks the level streaming script
            if (Editor.IsPlayMode)
            {
                Level.UnloadAllScenesAsync();
                return;
            }

            // Ensure to save all pending changes
            if (CheckSaveBeforeClose())
                return;

            // Unload scenes
            Editor.StateMachine.ChangingScenesState.UnloadScene(Level.Scenes);
        }

        /// <summary>
        /// Closes all of the scenes except for the specified scene (async).
        /// </summary>
        /// <param name="scene">The scene to not close.</param>
        public void CloseAllScenesExcept(Scene scene)
        {
            if (!Editor.StateMachine.CurrentState.CanChangeScene)
                return;

            var scenes = new List<Scene>();
            foreach (var s in Level.Scenes)
            {
                if (s == scene)
                    continue;
                scenes.Add(s);
            }

            // In play-mode Editor mocks the level streaming script
            if (Editor.IsPlayMode)
            {
                foreach (var s in scenes)
                {
                    Level.UnloadSceneAsync(s);
                }
                return;
            }

            // Ensure to save all pending changes
            if (CheckSaveBeforeClose())
                return;

            // Unload scenes
            Editor.StateMachine.ChangingScenesState.UnloadScene(scenes);
        }

        /// <summary>
        /// Show save before scene load/unload action.
        /// </summary>
        /// <param name="scene">The scene that will be closed.</param>
        /// <returns>True if action has been canceled, otherwise false</returns>
        public bool CheckSaveBeforeClose(SceneNode scene)
        {
            // Check if scene was edited after last saving
            if (scene.IsEdited)
            {
                // Ask user for further action
                var result = MessageBox.Show(
                                             string.Format("Scene \'{0}\' has been edited. Save before closing?", scene.Name),
                                             "Close without saving?",
                                             MessageBoxButtons.YesNoCancel,
                                             MessageBoxIcon.Question
                                            );
                if (result == DialogResult.OK || result == DialogResult.Yes)
                {
                    // Save completion is authoritative. Never fall through to unload on failure.
                    if (!SaveSceneSynchronously(scene.Scene))
                    {
                        SceneDebug.Error(SceneMutationErrorCode.SaveFailed, "CloseBlocked", $"Scene={scene.Scene.ID} Path='{scene.Scene.Path}'");
                        return true;
                    }
                }
                else if (result == DialogResult.Cancel || result == DialogResult.Abort)
                {
                    // Cancel closing
                    return true;
                }
                else
                {
                    Editor.Undo.DiscardSceneChanges(scene.Scene.ID);
                    scene.IsEdited = false;
                }
            }

            ClearRefsToSceneObjects();

            return false;
        }

        /// <summary>
        /// Show save before scene load/unload action.
        /// </summary>
        /// <returns>True if action has been canceled, otherwise false</returns>
        public bool CheckSaveBeforeClose()
        {
            // Check if scene was edited after last saving
            if (IsEdited())
            {
                // Ask user for further action
                var scenes = Level.Scenes;
                var result = MessageBox.Show(
                                             scenes.Length == 1 ? string.Format("Scene \'{0}\' has been edited. Save before closing?", scenes[0].Name) : string.Format("{0} scenes have been edited. Save before closing?", scenes.Length),
                                             "Close without saving?",
                                             MessageBoxButtons.YesNoCancel,
                                             MessageBoxIcon.Question
                                            );
                if (result == DialogResult.OK || result == DialogResult.Yes)
                {
                    // Save completion is authoritative. Never fall through to unload on failure.
                    var scenesToSave = Level.Scenes.Where(IsEdited).ToArray();
                    for (int i = 0; i < scenesToSave.Length; i++)
                    {
                        if (!SaveSceneSynchronously(scenesToSave[i]))
                        {
                            SceneDebug.Error(SceneMutationErrorCode.SaveFailed, "CloseBlocked", $"Scene={scenesToSave[i].ID} Path='{scenesToSave[i].Path}'");
                            return true;
                        }
                    }
                }
                else if (result == DialogResult.Cancel || result == DialogResult.Abort)
                {
                    // Cancel closing
                    return true;
                }
                else
                {
                    for (int i = 0; i < scenes.Length; i++)
                    {
                        Editor.Undo.DiscardSceneChanges(scenes[i].ID);
                        if (GetActorNode(scenes[i]) is SceneNode node)
                            node.IsEdited = false;
                    }
                }
            }

            ClearRefsToSceneObjects();

            return false;
        }

        /// <summary>
        /// Clears references to the scene objects by the editor. Deselects objects.
        /// </summary>
        /// <param name="fullCleanup">True if cleanup all data (including serialized and cached data). Otherwise will just clear living references to the scene objects.</param>
        public void ClearRefsToSceneObjects(bool fullCleanup = false)
        {
            Profiler.BeginEvent("SceneModule.ClearRefsToSceneObjects");
            Editor.SceneEditing.Deselect();

            if (fullCleanup)
            {
                Undo.Clear();
            }
            Profiler.EndEvent();
        }

        private void EnsureSceneActorsWatcher()
        {
            var now = DateTime.UtcNow;
            if (_sceneActorsWatcher != null)
            {
                if (now >= _nextSceneActorsWatcherRetry)
                {
                    _nextSceneActorsWatcherRetry = now.AddSeconds(1.0);
                    if (!Directory.Exists(GetSceneActorsRootPath()))
                    {
                        DisposeSceneActorsWatcher();
                        _sceneActorsFolderWasMissing = true;
                    }
                }
                return;
            }

            if (now < _nextSceneActorsWatcherRetry)
                return;
            _nextSceneActorsWatcherRetry = now.AddSeconds(1.0);

            var sceneActorsFolder = GetSceneActorsRootPath();
            if (!Directory.Exists(sceneActorsFolder))
            {
                _sceneActorsFolderWasMissing = true;
                return;
            }

            _sceneActorsWatcher = new FileSystemWatcher(sceneActorsFolder)
            {
                IncludeSubdirectories = true,
                InternalBufferSize = 64 * 1024,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size,
            };
            _sceneActorsWatcher.Changed += OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Created += OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Deleted += OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Renamed += OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Error += OnSceneActorsWatcherError;
            _sceneActorsWatcher.EnableRaisingEvents = true;

            if (_sceneActorsFolderWasMissing && Level.IsAnySceneLoaded)
            {
                lock (_sceneDiskChangesLock)
                {
                    QueueLoadedExternalActorsScenes(DateTime.UtcNow);
                }
            }
            _sceneActorsFolderWasMissing = false;
        }

        private void DisposeSceneActorsWatcher()
        {
            if (_sceneActorsWatcher == null)
                return;

            _sceneActorsWatcher.EnableRaisingEvents = false;
            _sceneActorsWatcher.Changed -= OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Created -= OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Deleted -= OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Renamed -= OnSceneActorsDiskChanged;
            _sceneActorsWatcher.Error -= OnSceneActorsWatcherError;
            _sceneActorsWatcher.Dispose();
            _sceneActorsWatcher = null;
        }

        private void OnSceneActorsDiskChanged(object sender, FileSystemEventArgs e)
        {
            QueueExternalActorDiskChange(e.FullPath);
            if (e is RenamedEventArgs renamed)
                QueueExternalActorDiskChange(renamed.OldFullPath);
        }

        private void OnSceneActorsWatcherError(object sender, ErrorEventArgs e)
        {
            lock (_sceneDiskChangesLock)
            {
                _sceneActorsWatcherError = true;
            }
        }

        private void IgnoreSceneDiskChangesFromSave(Scene scene)
        {
            if (scene == null)
                return;

            var path = scene.Path;
            if (string.IsNullOrEmpty(path))
                return;

            path = NormalizeAbsolutePath(path);
            lock (_sceneDiskChangesLock)
            {
                _ignoredSceneDiskChanges[path] = DateTime.UtcNow.AddSeconds(SceneDiskChangeIgnoreAfterSaveSeconds);
            }
        }

        private bool ShouldIgnoreSceneDiskChange(string scenePath, DateTime now)
        {
            if (_ignoredSceneDiskChanges.TryGetValue(scenePath, out var ignoreUntil))
            {
                if (now <= ignoreUntil)
                    return true;
                _ignoredSceneDiskChanges.Remove(scenePath);
            }
            return false;
        }

        private void ProcessPendingSceneDiskChanges()
        {
            if ((!Editor.StateMachine.CurrentState.CanChangeScene && !Editor.MultiplayerPlayMode.IsReplica) ||
                (Editor.MultiplayerPlayMode.IsReplica && !Editor.StateMachine.IsEditMode) ||
                Level.IsAnyActionPending)
            {
                return;
            }

            var now = DateTime.UtcNow;
            var readyScenePaths = new List<string>();
            lock (_sceneDiskChangesLock)
            {
                if (_sceneActorsWatcherError)
                {
                    _sceneActorsWatcherError = false;
                    QueueLoadedExternalActorsScenes(now);
                }

                if (_pendingSceneDiskChanges.Count == 0)
                    return;

                foreach (var e in _pendingSceneDiskChanges)
                {
                    if ((now - e.Value).TotalSeconds >= SceneDiskChangePromptDelaySeconds)
                        readyScenePaths.Add(e.Key);
                }
                for (int i = 0; i < readyScenePaths.Count; i++)
                    _pendingSceneDiskChanges.Remove(readyScenePaths[i]);
            }

            for (int i = 0; i < readyScenePaths.Count; i++)
            {
                var scenePath = readyScenePaths[i];
                if (ShouldIgnoreSceneDiskChange(scenePath, now))
                    continue;

                var scene = FindLoadedSceneByPath(scenePath);
                if (scene == null)
                    continue;

                if (Editor.IsCliMode || Editor.MultiplayerPlayMode.IsReplica)
                {
                    ReloadSceneFromDisk(scene);
                    continue;
                }

                var result = MessageBox.Show(
                                             string.Format("Scene '{0}' has changed on disk. Reload it?\n\nUnsaved changes in the loaded scene will be lost.", scene.Name),
                                             "Scene changed on disk",
                                             MessageBoxButtons.YesNo,
                                             MessageBoxIcon.Warning
                                            );
                if (result == DialogResult.Yes || result == DialogResult.OK)
                    ReloadSceneFromDisk(scene);
            }
        }

        private void QueueLoadedExternalActorsScenes(DateTime now)
        {
            for (int i = 0; i < Level.ScenesCount; i++)
            {
                var scene = Level.GetScene(i);
                if (scene && scene.UseExternalActors && !string.IsNullOrEmpty(scene.Path))
                    _pendingSceneDiskChanges[NormalizeAbsolutePath(scene.Path)] = now.AddSeconds(-SceneDiskChangePromptDelaySeconds);
            }
        }

        private Scene FindLoadedSceneByPath(string scenePath)
        {
            scenePath = NormalizeAbsolutePath(scenePath);
            for (int i = 0; i < Level.ScenesCount; i++)
            {
                var scene = Level.GetScene(i);
                if (scene && !string.IsNullOrEmpty(scene.Path) && string.Equals(NormalizeAbsolutePath(scene.Path), scenePath, StringComparison.OrdinalIgnoreCase))
                    return scene;
            }
            return null;
        }

        private void ReloadSceneFromDisk(Scene scene)
        {
            var sceneId = scene.ID;
            var scenePath = scene.Path;
            if (!File.Exists(scenePath))
            {
                Editor.LogWarning("Cannot reload scene changed on disk because the file is missing: " + scenePath);
                return;
            }

            var asset = FlaxEngine.Content.GetRuntimeObject(sceneId);
            if (asset != null && (asset.IsLoaded || asset.LastLoadFailed))
                asset.Reload();

            ClearRefsToSceneObjects();
            Level.UnloadScene(scene);
            if (Level.LoadScene(sceneId))
                Editor.LogWarning("Failed to reload scene changed on disk: " + scenePath);
        }

        private Dictionary<ContainerControl, Float2> _uiRootSizes;

        internal void OnSaveStart(ContainerControl uiRoot)
        {
            // Force viewport UI to have fixed size during scene/prefabs saving to result in stable data (less mess in version control diffs)
            if (_uiRootSizes == null)
                _uiRootSizes = new Dictionary<ContainerControl, Float2>();
            _uiRootSizes[uiRoot] = uiRoot.Size;
            uiRoot.Size = new Float2(1920, 1080);
        }

        internal void OnSaveEnd(ContainerControl uiRoot)
        {
            // Restore cached size of the UI root container
            if (_uiRootSizes != null && _uiRootSizes.Remove(uiRoot, out var size))
            {
                uiRoot.Size = size;
            }
        }

        private void OnSceneSaving(Scene scene, Guid sceneId)
        {
            IgnoreSceneDiskChangesFromSave(scene);
            OnSaveStart(RootControl.GameRoot);
        }
        
        private void OnSceneSaved(Scene scene, Guid sceneId)
        {
            IgnoreSceneDiskChangesFromSave(scene);
            EnsureSceneActorsWatcher();
            OnSaveEnd(RootControl.GameRoot);

            if (TryTakePendingSave(sceneId, out var saved))
            {
                _sceneDirtyGenerations.TryGetValue(sceneId, out var currentGeneration);
                _sceneSavedGenerations[sceneId] = saved.DirtyGeneration;
                Editor.Undo.MarkSceneSaved(sceneId, saved.UndoState);
                if (currentGeneration == saved.DirtyGeneration && GetActorNode(scene) is SceneNode sceneNode)
                    sceneNode.IsEdited = false;
                if (!IsEdited())
                {
                    Editor.UI.AddStatusMessage("Saved!");
                }
                SceneDebug.Log("SaveCompleted", $"Scene={sceneId} SavedGeneration={saved.DirtyGeneration} CurrentGeneration={currentGeneration} Path='{scene?.Path}'");
            }
        }
        
        private void OnSceneSaveError(Scene scene, Guid sceneId)
        {
            IgnoreSceneDiskChangesFromSave(scene);
            OnSaveEnd(RootControl.GameRoot);
            RemovePendingSave(sceneId);
            if (GetActorNode(scene) is SceneNode sceneNode)
                sceneNode.IsEdited = true;
            var path = scene?.Path ?? sceneId.ToString();
            SceneDebug.Error(SceneMutationErrorCode.SaveFailed, "SaveFailed", $"Scene={sceneId} Path='{path}' DirtyPreserved=true");
            Editor.UI.AddStatusMessage("Failed to save scene. The scene remains modified.");
        }

        private void OnSceneLoaded(Scene scene, Guid sceneId)
        {
            EnsureSceneActorsWatcher();

            _activeSceneId = sceneId;
            if (!_sceneDirtyGenerations.ContainsKey(sceneId))
                _sceneDirtyGenerations.Add(sceneId, 0);
            if (!_sceneSavedGenerations.ContainsKey(sceneId))
                _sceneSavedGenerations.Add(sceneId, 0);
            SceneDebug.Log("SceneLoaded", $"Scene={sceneId} Path='{scene?.Path}'");

            var startTime = DateTime.UtcNow;

            // Build scene tree
            var sceneNode = SceneGraphFactory.BuildSceneTree(scene);
            var treeNode = sceneNode.TreeNode;
            treeNode.IsLayoutLocked = true;
            treeNode.Expand(true);

            // Add to the tree
            var rootNode = Root.TreeNode;
            rootNode.IsLayoutLocked = true;
            sceneNode.ParentNode = Root;
            rootNode.SortChildren();
            rootNode.IsLayoutLocked = false;
            treeNode.UnlockChildrenRecursive();
            rootNode.Parent.PerformLayout();

            var endTime = DateTime.UtcNow;
            var milliseconds = (int)(endTime - startTime).TotalMilliseconds;
            Editor.Log($"Created graph for scene \'{scene.Name}\' in {milliseconds} ms");
            SceneGraphChanged?.Invoke();
        }

        private void OnSceneUnloading(Scene scene, Guid sceneId)
        {
            if (_activeSceneId == sceneId)
                _activeSceneId = Guid.Empty;
            _pendingSceneSaveGenerations.Remove(sceneId);
            Editor.Undo.OnSceneUnloading(sceneId);
            SceneDebug.Log("SceneUnloading", $"Scene={sceneId} Path='{scene?.Path}'");

            // Find scene tree node
            var node = Root.FindChildActor(scene);
            if (node != null)
            {
                Editor.Log($"Cleanup graph for scene \'{scene.Name}\'");

                // Cleanup
                var selection = Editor.SceneEditing.Selection;
                var hasSceneSelection = false;
                for (int i = 0; i < selection.Count; i++)
                {
                    if (selection[i].ParentScene == node)
                    {
                        hasSceneSelection = true;
                        break;
                    }
                }
                if (hasSceneSelection)
                {
                    var newSelection = new List<SceneGraphNode>();
                    for (int i = 0; i < selection.Count; i++)
                    {
                        if (selection[i].ParentScene != node)
                            newSelection.Add(selection[i]);
                    }
                    Editor.SceneEditing.Select(newSelection);
                }
                node.Dispose();
                SceneGraphChanged?.Invoke();
            }
        }

        private void OnActorSpawned(Actor actor)
        {
            // Skip for not loaded scenes (spawning actors during scene loading in script Start function)
            var sceneNode = GetActorNode(actor.Scene);
            if (sceneNode == null)
                return;

            // Skip for missing parent
            var parent = actor.Parent;
            if (parent == null)
                return;

            var parentNode = GetActorNode(parent);
            if (parentNode == null)
            {
                // Missing parent node when adding child actor to not spawned or unlinked actor
                return;
            }

            // Skip if already added
            if (SceneGraphFactory.Nodes.ContainsKey(actor.ID))
                return;

            var node = SceneGraphFactory.BuildActorNode(actor);
            if (node != null)
            {
                node.ParentNode = parentNode;
                SceneGraphChanged?.Invoke();
            }
        }

        private void OnActorDeleted(Actor actor)
        {
            var node = GetActorNode(actor);
            if (node != null)
            {
                OnActorDeleted(node);
                SceneGraphChanged?.Invoke();
            }
        }

        private void OnActorDeleted(ActorNode node)
        {
            for (int i = 0; i < node.ChildNodes.Count; i++)
            {
                if (node.ChildNodes[i] is ActorNode child)
                {
                    i--;
                    OnActorDeleted(child);
                }
            }

            ActorRemoved?.Invoke(node);

            // Cleanup part of the graph
            node.Dispose();
        }

        private void OnActorParentChanged(Actor actor, Actor prevParent)
        {
            ActorNode node = null;
            var parentNode = GetActorNode(actor.Parent);

            // Try use previous parent actor to find actor node
            var prevParentNode = GetActorNode(prevParent);
            if (prevParentNode != null)
            {
                // If should be one of the children
                node = prevParentNode.FindChildActor(actor);

                // Search whole tree if node was not found
                if (node == null)
                {
                    node = GetActorNode(actor);
                }
            }
            else if (parentNode != null)
            {
                // Create new node for that actor (user may unlink it from the scene before and now link it)
                node = SceneGraphFactory.BuildActorNode(actor);
            }
            if (node == null)
                return;

            // Get the new parent node (may be missing)
            node.ParentNode = parentNode;
            if (parentNode == null)
            {
                // Check if actor is selected in editor
                if (Editor.SceneEditing.Selection.Contains(node))
                    Editor.SceneEditing.Deselect();

                // Remove node (user may unlink actor from the scene but not destroy the actor)
                node.Dispose();
            }
            SceneGraphChanged?.Invoke();
        }

        private void OnActorOrderInParentChanged(Actor actor)
        {
            ActorNode node = GetActorNode(actor);
            node?.TreeNode.OnOrderInParentChanged();
        }

        private void OnActorNameChanged(Actor actor)
        {
            ActorNode node = GetActorNode(actor);
            if (node != null)
            {
                node.TreeNode.UpdateText();
                SceneGraphChanged?.Invoke();
            }
        }

        private void OnActorActiveChanged(Actor actor)
        {
            //ActorNode node = GetActorNode(actor);
            //node?.TreeNode.OnActiveChanged();
        }

        private void OnActorDestroyChildren(Actor actor)
        {
            // Instead of doing OnActorParentChanged for every child lets remove all of them at once from that actor
            ActorNode node = GetActorNode(actor);
            if (node != null)
            {
                if (Editor.SceneEditing.HasSthSelected)
                {
                    // Clear selection if one of the removed actors is selected
                    var selection = new HashSet<Actor>();
                    foreach (var e in Editor.SceneEditing.Selection)
                    {
                        if (e is ActorNode q && q.Actor)
                            selection.Add(q.Actor);
                    }
                    var count = actor.ChildrenCount;
                    for (int i = 0; i < count; i++)
                    {
                        var child = actor.GetChild(i);
                        if (selection.Contains(child))
                        {
                            Editor.SceneEditing.Deselect();
                            break;
                        }
                    }
                }

                // Remove all child nodes (upfront remove all nodes to run faster)
                for (int i = 0; i < node.ChildNodes.Count; i++)
                {
                    if (node.ChildNodes[i] is ActorNode child)
                        child.parentNode = null;
                }
                node.TreeNode.DisposeChildren();
                for (int i = 0; i < node.ChildNodes.Count; i++)
                {
                    node.ChildNodes[i].Dispose();
                }
                node.ChildNodes.Clear();
                SceneGraphChanged?.Invoke();
            }
        }

        /// <summary>
        /// Gets the actor node.
        /// </summary>
        /// <param name="actor">The actor.</param>
        /// <returns>Found actor node or null if missing. Actor may not be linked to the scene tree so node won't be found in that case.</returns>
        public ActorNode GetActorNode(Actor actor)
        {
            if (actor == null)
                return null;

            // ActorNode has the same ID as actor does
            return SceneGraphFactory.FindNode(actor.ID) as ActorNode;
        }

        /// <summary>
        /// Gets the actor node.
        /// </summary>
        /// <param name="actorId">The actor id.</param>
        /// <returns>Found actor node or null if missing. Actor may not be linked to the scene tree so node won't be found in that case.</returns>
        public ActorNode GetActorNode(Guid actorId)
        {
            // ActorNode has the same ID as actor does
            return SceneGraphFactory.FindNode(actorId) as ActorNode;
        }

        /// <summary>
        /// Executes the custom action on the graph nodes.
        /// </summary>
        /// <param name="callback">The callback.</param>
        public void ExecuteOnGraph(SceneGraphTools.GraphExecuteCallbackDelegate callback)
        {
            Root.ExecuteOnGraph(callback);
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            EnsureSceneActorsWatcher();
            ProcessPendingSceneDiskChanges();
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            Root = new ScenesRootNode();
            EnsureSceneActorsWatcher();

            // Bind events
            Level.SceneSaving += OnSceneSaving;
            Level.SceneSaved += OnSceneSaved;
            Level.SceneSaveError += OnSceneSaveError;
            Level.SceneLoaded += OnSceneLoaded;
            Level.SceneUnloading += OnSceneUnloading;
            Level.ActorSpawned += OnActorSpawned;
            Level.ActorDeleted += OnActorDeleted;
            Level.ActorParentChanged += OnActorParentChanged;
            Level.ActorOrderInParentChanged += OnActorOrderInParentChanged;
            Level.ActorNameChanged += OnActorNameChanged;
            Level.ActorActiveChanged += OnActorActiveChanged;
            Level.ActorDestroyChildren += OnActorDestroyChildren;
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            // Unbind events
            Level.SceneSaving -= OnSceneSaving;
            Level.SceneSaved -= OnSceneSaved;
            Level.SceneSaveError -= OnSceneSaveError;
            Level.SceneLoaded -= OnSceneLoaded;
            Level.SceneUnloading -= OnSceneUnloading;
            Level.ActorSpawned -= OnActorSpawned;
            Level.ActorDeleted -= OnActorDeleted;
            Level.ActorParentChanged -= OnActorParentChanged;
            Level.ActorOrderInParentChanged -= OnActorOrderInParentChanged;
            Level.ActorNameChanged -= OnActorNameChanged;
            Level.ActorActiveChanged -= OnActorActiveChanged;
            Level.ActorDestroyChildren -= OnActorDestroyChildren;

            DisposeSceneActorsWatcher();

            // Cleanup graph
            Root.Dispose();

            if (SceneGraphFactory.Nodes.Count > 0)
            {
                Editor.LogWarning("Not all scene graph nodes has been disposed!");
            }
        }
    }
}
