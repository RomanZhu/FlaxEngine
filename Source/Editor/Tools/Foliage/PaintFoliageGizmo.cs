// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Tools.Foliage.Undo;
using FlaxEditor.Tools.Terrain.Brushes;
using FlaxEngine;

namespace FlaxEditor.Tools.Foliage
{
    /// <summary>
    /// Gizmo for painting with foliage. Managed by the <see cref="PaintFoliageGizmoMode"/>.
    /// </summary>
    /// <seealso cref="FlaxEditor.Gizmo.GizmoBase" />
    public sealed class PaintFoliageGizmo : GizmoBase
    {
        private FlaxEngine.Foliage _paintFoliage;
        private Model _brushModel;
        private List<int> _foliageTypesIndices;
        private EditFoliageAction _undoAction;
        private int _paintUpdateCount;
        private Actor _placementSurface;
        private readonly CircleBrush _terrainBrush = new CircleBrush();
        private readonly List<TerrainChunkLocation> _terrainChunks = new List<TerrainChunkLocation>();

        private struct TerrainChunkLocation
        {
            public Int2 PatchCoord;
            public Int2 ChunkCoord;
        }

        /// <summary>
        /// The parent mode.
        /// </summary>
        public readonly PaintFoliageGizmoMode Mode;

        /// <summary>
        /// Gets a value indicating whether gizmo tool is painting the foliage.
        /// </summary>
        public bool IsPainting => _paintFoliage != null;

        /// <summary>
        /// Occurs when foliage paint has been started.
        /// </summary>
        public event Action PaintStarted;

        /// <summary>
        /// Occurs when foliage paint has been ended.
        /// </summary>
        public event Action PaintEnded;

        /// <summary>
        /// Initializes a new instance of the <see cref="PaintFoliageGizmo"/> class.
        /// </summary>
        /// <param name="owner">The owner.</param>
        /// <param name="mode">The mode.</param>
        public PaintFoliageGizmo(IGizmoOwner owner, PaintFoliageGizmoMode mode)
        : base(owner)
        {
            Mode = mode;
        }

        private FlaxEngine.Foliage SelectedFoliage
        {
            get
            {
                var sceneEditing = Editor.Instance.SceneEditing;
                var foliageNode = sceneEditing.SelectionCount == 1 ? sceneEditing.Selection[0] as FoliageNode : null;
                return (FlaxEngine.Foliage)foliageNode?.Actor;
            }
        }

        /// <inheritdoc />
        public override void Draw(ref RenderContext renderContext)
        {
            if (!IsActive)
                return;

            var foliage = SelectedFoliage;
            if (!foliage)
                return;

            if (Mode.HasValidHit)
            {
                var brushPosition = Mode.CursorPosition;
                var brushColor = new Color(1.0f, 0.85f, 0.0f); // TODO: expose to editor options
                if (_placementSurface is FlaxEngine.Terrain terrain)
                {
                    _terrainBrush.Size = Mode.CurrentBrush.Size;
                    var terrainBrushMaterial = _terrainBrush.GetBrushMaterial(ref renderContext, ref brushPosition, ref brushColor);
                    if (!terrainBrushMaterial)
                        return;

                    for (int i = 0; i < _terrainChunks.Count; i++)
                    {
                        var chunk = _terrainChunks[i];
                        terrain.DrawChunk(ref renderContext, ref chunk.PatchCoord, ref chunk.ChunkCoord, terrainBrushMaterial, 0);
                    }
                    return;
                }

                var brushNormal = Mode.CursorNormal;
                var sceneDepth = Owner.RenderTask.Buffers.DepthBuffer;
                var brushMaterial = Mode.CurrentBrush.GetBrushMaterial(ref brushPosition, ref brushColor, sceneDepth);
                if (!_brushModel)
                {
                    _brushModel = FlaxEngine.Content.LoadAsyncInternal<Model>("Editor/Primitives/Sphere");
                }

                // Draw paint brush
                if (_brushModel && brushMaterial)
                {
                    Quaternion rotation;
                    if (brushNormal == Vector3.Down)
                        rotation = Quaternion.RotationZ(Mathf.Pi);
                    else
                        rotation = Quaternion.LookRotation(Vector3.Cross(Vector3.Cross(brushNormal, Vector3.Forward), brushNormal), brushNormal);
                    Matrix transform = Matrix.Scaling(Mode.CurrentBrush.Size * 0.01f) * Matrix.RotationQuaternion(rotation) * Matrix.Translation(brushPosition - renderContext.View.Origin);
                    _brushModel.Draw(ref renderContext, brushMaterial, ref transform);
                }
            }
        }

        /// <summary>
        /// Called to start foliage painting
        /// </summary>
        /// <param name="foliage">The foliage.</param>
        private void PaintStart(FlaxEngine.Foliage foliage)
        {
            // Skip if already is painting
            if (IsPainting)
                return;

            if (Editor.Instance.Undo.Enabled)
                _undoAction = new EditFoliageAction(foliage);
            _paintFoliage = foliage;
            _paintUpdateCount = 0;
            PaintStarted?.Invoke();
        }

        /// <summary>
        /// Called to update foliage painting logic.
        /// </summary>
        /// <param name="dt">The delta time (in seconds).</param>
        private void PaintUpdate(float dt)
        {
            // Skip if is not painting
            if (!IsPainting)
                return;
            if (Mode.CurrentBrush.SingleClick && _paintUpdateCount > 0)
                return;

            // Edit the foliage
            var foliage = SelectedFoliage;
            int foliageTypesCount = foliage.FoliageTypesCount;
            var foliageTypeModelIdsToPaint = Editor.Instance.Windows.ToolboxWin.Foliage.FoliageTypeModelIdsToPaint;
            if (_foliageTypesIndices == null)
                _foliageTypesIndices = new List<int>(foliageTypesCount);
            else
                _foliageTypesIndices.Clear();
            for (int index = 0; index < foliageTypesCount; index++)
            {
                var model = foliage.GetFoliageType(index).Model;
                if (model && (!foliageTypeModelIdsToPaint.TryGetValue(model.ID, out var selected) || selected))
                {
                    _foliageTypesIndices.Add(index);
                }
            }
            bool additive = !Owner.IsControlDown;
            if (additive && !_placementSurface)
                return;
            // TODO: don't call _foliageTypesIndices.ToArray() but reuse allocation
            FoliageTools.Paint(foliage, _foliageTypesIndices.ToArray(), Mode.CursorPosition, Mode.CurrentBrush.Size * 0.5f, additive, Mode.CurrentBrush.DensityScale, _placementSurface);
            _paintUpdateCount++;
        }

        /// <summary>
        /// Called to end foliage painting.
        /// </summary>
        private void PaintEnd()
        {
            // Skip if nothing was painted
            if (!IsPainting)
                return;

            if (_undoAction != null)
            {
                _undoAction.RecordEnd();
                Editor.Instance.Undo.AddAction(_undoAction);
                _undoAction = null;
            }
            _paintFoliage = null;
            _paintUpdateCount = 0;
            PaintEnded?.Invoke();
        }

        /// <inheritdoc />
        public override bool IsControllingMouse => IsPainting;

        /// <inheritdoc />
        public override void Update(float dt)
        {
            base.Update(dt);

            // Check if gizmo is not active
            if (!IsActive)
            {
                PaintEnd();
                return;
            }

            // Check if no foliage is selected
            var foliage = SelectedFoliage;
            if (!foliage)
            {
                PaintEnd();
                return;
            }

            // Check if selected foliage was changed during painting
            if (foliage != _paintFoliage && IsPainting)
            {
                PaintEnd();
            }
            
            // Perform detailed tracing to find cursor location for the foliage placement
            var mouseRay = Owner.MouseRay;
            var ray = Owner.MouseRay;
            var view = new Ray(Owner.ViewPosition, Owner.ViewDirection);
            var rayCastFlags = SceneGraphNode.RayCastData.FlagTypes.SkipEditorPrimitives | SceneGraphNode.RayCastData.FlagTypes.SkipColliders;
            var hit = Editor.Instance.Scene.Root.RayCast(ref ray, ref view, out var closest, out var hitNormal, rayCastFlags);
            if (hit != null)
            {
                var hitLocation = mouseRay.GetPoint(closest);
                var actorNode = hit as ActorNode ?? hit.ParentNode as ActorNode;
                var hitActor = actorNode?.Actor;
                _placementSurface = hitActor is FlaxEngine.Terrain || hitActor is StaticModel || hitActor is TextRender ? hitActor : null;
                Mode.SetCursor(ref hitLocation, ref hitNormal);
                UpdateTerrainChunks();
            }
            // No hit
            else
            {
                _placementSurface = null;
                _terrainChunks.Clear();
                Mode.ClearCursor();
            }

            // Handle painting
            if (Owner.IsLeftMouseButtonDown)
                PaintStart(foliage);
            else
                PaintEnd();
            PaintUpdate(dt);
        }

        private void UpdateTerrainChunks()
        {
            _terrainChunks.Clear();
            if (_placementSurface is not FlaxEngine.Terrain terrain)
                return;

            float radius = Mode.CurrentBrush.Size * 0.5f;
            var center = Mode.CursorPosition;
            var brushBounds = new BoundingBox(
                new Vector3(center.X - radius, center.Y - radius - 10000.0f, center.Z - radius),
                new Vector3(center.X + radius, center.Y + radius + 10000.0f, center.Z + radius));
            for (int patchIndex = 0; patchIndex < terrain.PatchesCount; patchIndex++)
            {
                terrain.GetPatchBounds(patchIndex, out var patchBounds);
                if (!patchBounds.Intersects(ref brushBounds))
                    continue;

                terrain.GetPatchCoord(patchIndex, out var patchCoord);
                for (int chunkIndex = 0; chunkIndex < FlaxEngine.Terrain.PatchChunksCount; chunkIndex++)
                {
                    terrain.GetChunkBounds(patchIndex, chunkIndex, out var chunkBounds);
                    if (!chunkBounds.Intersects(ref brushBounds))
                        continue;
                    _terrainChunks.Add(new TerrainChunkLocation
                    {
                        PatchCoord = patchCoord,
                        ChunkCoord = new Int2(chunkIndex % FlaxEngine.Terrain.PatchEdgeChunksCount, chunkIndex / FlaxEngine.Terrain.PatchEdgeChunksCount),
                    });
                }
            }
        }
    }
}
