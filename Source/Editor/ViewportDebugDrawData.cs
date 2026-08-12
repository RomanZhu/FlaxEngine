// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using FlaxEngine;

namespace FlaxEditor
{
    /// <summary>
    /// The custom data container used during collecting draw data for drawing debug visuals of selected objects.
    /// </summary>
    [HideInEditor]
    public class ViewportDebugDrawData
    {
        private readonly List<IntPtr> _actors;
        private readonly List<HighlightData> _highlights;
        private MaterialBase _highlightMaterial;

        internal Span<IntPtr> ActorsPtrs => CollectionsMarshal.AsSpan(_actors);

        internal int ActorsCount => _actors.Count;

        /// <summary>
        /// Initializes a new instance of the <see cref="ViewportDebugDrawData" /> class.
        /// </summary>
        /// <param name="actorsCapacity">The actors capacity.</param>
        public ViewportDebugDrawData(int actorsCapacity = 0)
        {
            _actors = new List<IntPtr>(actorsCapacity);
            _highlights = new List<HighlightData>(actorsCapacity);
            _highlightMaterial = EditorAssets.Cache.HighlightMaterialInstance;
        }

        /// <summary>
        /// Adds the specified actor to draw it's debug visuals.
        /// </summary>
        /// <param name="actor">The actor.</param>
        public void Add(Actor actor)
        {
            _actors.Add(FlaxEngine.Object.GetUnmanagedPtr(actor));
        }

        /// <summary>
        /// Highlights the model.
        /// </summary>
        /// <param name="model">The model.</param>
        public void HighlightModel(StaticModel model)
        {
            if (model.Model == null)
                return;

            var entries = model.Entries;
            for (var i = 0; i < entries.Length; i++)
                HighlightModel(model, i);
        }

        /// <summary>
        /// Highlights the model entry.
        /// </summary>
        /// <param name="model">The model.</param>
        /// <param name="entryIndex">Index of the entry to highlight.</param>
        public void HighlightModel(StaticModel model, int entryIndex)
        {
            _highlights.Add(new HighlightData
            {
                Target = model,
                EntryIndex = entryIndex
            });
        }

        /// <summary>
        /// Highlights the brush surface.
        /// </summary>
        /// <param name="surface">The surface.</param>
        public void HighlightBrushSurface(BrushSurface surface)
        {
            surface.Brush.GetVertices(surface.Index, out var vertices);
            if (vertices.Length > 0)
            {
                var color = new Color(1.0f, 0.68f, 0.12f, 0.32f);
                for (int i = 0; i + 2 < vertices.Length; i += 3)
                    DebugDraw.DrawTriangle(vertices[i], vertices[i + 1], vertices[i + 2], color, 0.0f, true);
            }
        }

        /// <summary>
        /// Draws the collected actors via <see cref="DebugDraw"/>.
        /// </summary>
        /// <param name="drawScenes">True if draw all loaded scenes too, otherwise will draw only provided actors.</param>
        public unsafe void DrawActors(bool drawScenes = false)
        {
            fixed (IntPtr* actors = ActorsPtrs)
            {
                DebugDraw.DrawActors(new IntPtr(actors), _actors.Count, drawScenes);
            }
        }

        /// <summary>
        /// Called when task calls <see cref="SceneRenderTask.CollectDrawCalls" /> event.
        /// </summary>
        /// <param name="renderContext">The rendering context.</param>
        public virtual void OnDraw(ref RenderContext renderContext)
        {
            if (_highlightMaterial == null
                || _highlights.Count == 0
                || renderContext.View.Pass == DrawPass.Depth
               )
                return;
            Profiler.BeginEvent("ViewportDebugDrawData.OnDraw");

            Matrix world;
            for (var i = 0; i < _highlights.Count; i++)
            {
                HighlightData highlight = _highlights[i];
                if (highlight.Target is StaticModel staticModel)
                {
                    var model = staticModel.Model;
                    if (model == null)
                        continue;
                    staticModel.Transform.GetWorld(out world);
                    var bounds = BoundingSphere.FromBox(staticModel.Box);

                    // Pick a proper LOD
                    Float3 center = bounds.Center - renderContext.View.Origin;
                    int lodIndex = RenderTools.ComputeModelLOD(model, ref center, (float)bounds.Radius, ref renderContext);
                    var lods = model.LODs;
                    if (lods == null || lods.Length < lodIndex || lodIndex < 0)
                        continue;
                    var lod = lods[lodIndex];

                    // Draw meshes
                    for (int meshIndex = 0; meshIndex < lod.Meshes.Length; meshIndex++)
                    {
                        if (lod.Meshes[meshIndex].MaterialSlotIndex == highlight.EntryIndex)
                        {
                            lod.Meshes[meshIndex].Draw(ref renderContext, _highlightMaterial, ref world);
                        }
                    }
                }
            }

            Profiler.EndEvent();
        }

        /// <summary>
        /// Clears this data collector.
        /// </summary>
        public virtual void Clear()
        {
            _actors.Clear();
            _highlights.Clear();
        }

        /// <summary>
        /// Releases unmanaged and - optionally - managed resources.
        /// </summary>
        public virtual void Dispose()
        {
            _highlightMaterial = null;
        }

        private struct HighlightData
        {
            public object Target;
            public int EntryIndex;
        }
    }
}
