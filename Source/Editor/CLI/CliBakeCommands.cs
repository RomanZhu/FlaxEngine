// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Progress.Handlers;
using FlaxEditor.States;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// Exposes the editor's scene-data builders through explicit, non-interactive CLI actions.
    /// </summary>
    internal static class CliBakeCommands
    {
        [CliCommand("bake.status", Description = "Return status for Flax scene-data builders.", Access = CliCommandAccess.ReadOnly)]
        public static object Status()
        {
            var editor = Editor.Instance;
            return new
            {
                scenes = new
                {
                    active = editor.StateMachine.BuildingScenesState.IsActive,
                    status = editor.StateMachine.BuildingScenesState.IsActive ? editor.StateMachine.BuildingScenesState.Status : "idle",
                },
                lighting = new
                {
                    active = editor.ProgressReporting.BakeLightmaps.IsActive,
                    progress = editor.ProgressReporting.BakeLightmaps.Progress,
                    info = editor.ProgressReporting.BakeLightmaps.InfoText,
                    supported = BakeLightmapsProgress.CanBake,
                },
                navmesh = new
                {
                    active = Navigation.IsBuildingNavMesh,
                    progress = Navigation.IsBuildingNavMesh ? Navigation.NavMeshBuildingProgress : 0.0f,
                },
                environmentProbes = new
                {
                    active = editor.ProgressReporting.BakeEnvProbes.IsActive,
                    progress = editor.ProgressReporting.BakeEnvProbes.Progress,
                    info = editor.ProgressReporting.BakeEnvProbes.InfoText,
                },
                csg = new
                {
                    active = Editor.Internal_GetIsCSGActive(),
                },
                sdf = new
                {
                    active = false,
                    supported = true,
                },
            };
        }

        [CliCommand("bake.lighting.start", Description = "Start static lightmap baking for loaded scenes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartLighting()
        {
            EnsureIdle("lighting");
            if (!BakeLightmapsProgress.CanBake)
                throw new InvalidOperationException("Static lighting bake is not supported by the current GPU/device.");
            if (!Editor.Instance.ProgressReporting.BakeLightmaps.IsActive)
                Editor.Instance.BakeLightmapsOrCancel();
            return new { requested = true, operation = "lighting", status = Status() };
        }

        [CliCommand("bake.lighting.cancel", Description = "Cancel an active static lightmap bake.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CancelLighting()
        {
            if (Editor.Instance.ProgressReporting.BakeLightmaps.IsActive)
                Editor.Internal_BakeLightmaps(true);
            return new { requested = true, operation = "lighting", status = Status() };
        }

        [CliCommand("bake.lighting.clear", Description = "Clear baked lightmap linkage from loaded scenes and save them.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ClearLighting()
        {
            Editor.Instance.ClearLightmaps();
            var saved = SaveScenes();
            return new { requested = true, operation = "lighting.clear", savedSceneIds = saved, status = Status() };
        }

        [CliCommand("bake.navmesh.start", Description = "Build navigation meshes for loaded scenes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartNavMesh()
        {
            EnsureIdle("navmesh");
            if (!Navigation.IsBuildingNavMesh)
                Editor.Instance.BuildNavMesh();
            return new { requested = true, operation = "navmesh", status = Status() };
        }

        [CliCommand("bake.navmesh.clear", Description = "Clear navigation mesh tile data from loaded NavMesh actors and save scenes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object ClearNavMesh()
        {
            var changed = new List<Guid>();
            foreach (var scene in Level.Scenes)
            {
                var sceneChanged = false;
                foreach (var actor in Enumerate(scene))
                {
                    if (actor is NavMesh navMesh)
                    {
                        navMesh.ClearData();
                        sceneChanged = true;
                    }
                }
                if (sceneChanged)
                {
                    Editor.Instance.Scene.MarkSceneEdited(scene);
                    changed.Add(scene.ID);
                }
            }
            var saved = SaveScenes();
            return new { changed = changed.ToArray(), savedSceneIds = saved, status = Status() };
        }

        [CliCommand("bake.probes.start", Description = "Bake active Environment Probes and CaptureScene Sky Lights.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartProbes()
        {
            EnsureIdle("probes");
            if (!Editor.Instance.ProgressReporting.BakeEnvProbes.IsActive)
                Editor.Instance.BakeAllEnvProbes();
            return new { requested = true, operation = "probes", status = Status() };
        }

        [CliCommand("bake.csg.start", Description = "Build CSG meshes for loaded scenes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartCsg()
        {
            EnsureIdle("csg");
            Editor.Instance.BuildCSG();
            return new { requested = true, operation = "csg", status = Status() };
        }

        [CliCommand("bake.scenes.start", Description = "Run the editor-configured Build Scenes action sequence.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartScenes()
        {
            EnsureIdle("scenes");
            if (!Editor.Instance.StateMachine.BuildingScenesState.IsActive)
                Editor.Instance.StateMachine.GoToState<BuildingScenesState>();
            return new { requested = true, operation = "scenes", status = Status() };
        }

        [CliCommand("bake.scenes.cancel", Description = "Cancel the configured Build Scenes action sequence.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object CancelScenes()
        {
            if (Editor.Instance.StateMachine.BuildingScenesState.IsActive)
                Editor.Instance.StateMachine.BuildingScenesState.Cancel();
            return new { requested = true, operation = "scenes", status = Status() };
        }

        [CliCommand("bake.sdf.start", Description = "Generate SDF data for static models in loaded scenes.", Access = CliCommandAccess.MutatesProject, RequiresScene = true)]
        public static object StartSdf()
        {
            EnsureIdle("sdf");
            Editor.Instance.BuildAllMeshesSDF();
            return new { requested = true, operation = "sdf", status = Status() };
        }

        private static IEnumerable<Actor> Enumerate(Actor root)
        {
            yield return root;
            for (var i = 0; i < root.ChildrenCount; i++)
            foreach (var child in Enumerate(root.GetChild(i)))
                yield return child;
        }

        private static Guid[] SaveScenes()
        {
            if (Editor.IsPlayMode)
                return Array.Empty<Guid>();
            var ids = Level.Scenes.Select(x => x.ID).ToArray();
            if (Level.SaveAllScenes())
                throw new InvalidOperationException("Failed to save one or more scenes after the bake operation.");
            Editor.Instance.Undo.MarkScenesSaved();
            return ids;
        }

        private static void EnsureIdle(string operation)
        {
            var editor = Editor.Instance;
            if (editor.StateMachine.BuildingScenesState.IsActive || editor.ProgressReporting.BakeLightmaps.IsActive || Navigation.IsBuildingNavMesh || editor.ProgressReporting.BakeEnvProbes.IsActive || Editor.Internal_GetIsCSGActive())
                throw new InvalidOperationException($"Cannot start {operation} bake while another scene-data operation is active. Query bake.status and wait for it to become idle.");
        }
    }
}
