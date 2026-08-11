// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Transactions
{
    /// <summary>
    /// Serializable material and UV state for one box-brush surface.
    /// </summary>
    [Serializable]
    public struct CSGBrushSurfaceState
    {
        /// <summary>The surface material.</summary>
        public MaterialBase Material;
        /// <summary>The UV scale.</summary>
        public Float2 TexCoordScale;
        /// <summary>The UV offset.</summary>
        public Float2 TexCoordOffset;
        /// <summary>The UV rotation in degrees.</summary>
        public float TexCoordRotation;
        /// <summary>The lightmap scale.</summary>
        public float ScaleInLightmap;

        /// <summary>
        /// Captures a native brush surface without retaining its transient owner pointer.
        /// </summary>
        public static CSGBrushSurfaceState Capture(ref BrushSurface surface)
        {
            return new CSGBrushSurfaceState
            {
                Material = surface.Material,
                TexCoordScale = surface.TexCoordScale,
                TexCoordOffset = surface.TexCoordOffset,
                TexCoordRotation = surface.TexCoordRotation,
                ScaleInLightmap = surface.ScaleInLightmap,
            };
        }

        /// <summary>
        /// Applies the serializable values while preserving the live owner and surface index.
        /// </summary>
        public void Apply(ref BrushSurface surface)
        {
            surface.Material = Material;
            surface.TexCoordScale = TexCoordScale;
            surface.TexCoordOffset = TexCoordOffset;
            surface.TexCoordRotation = TexCoordRotation;
            surface.ScaleInLightmap = ScaleInLightmap;
        }

        /// <summary>
        /// Compares serialized surface values.
        /// </summary>
        public bool Matches(ref CSGBrushSurfaceState other)
        {
            return Material == other.Material &&
                   TexCoordScale == other.TexCoordScale &&
                   TexCoordOffset == other.TexCoordOffset &&
                   TexCoordRotation.Equals(other.TexCoordRotation) &&
                   ScaleInLightmap.Equals(other.ScaleInLightmap);
        }
    }

    /// <summary>
    /// Serializable snapshot of all box-brush values modified by CSG authoring tools.
    /// </summary>
    [Serializable]
    public struct CSGBoxBrushState
    {
        /// <summary>The stable brush identifier.</summary>
        public Guid BrushId;
        /// <summary>The actor transform.</summary>
        public Transform Transform;
        /// <summary>The local brush center.</summary>
        public Vector3 Center;
        /// <summary>The local brush size.</summary>
        public Vector3 Size;
        /// <summary>The boolean operation.</summary>
        public BrushMode Mode;
        /// <summary>The six surface states.</summary>
        public CSGBrushSurfaceState[] Surfaces;

        /// <summary>
        /// Captures a box brush.
        /// </summary>
        public static CSGBoxBrushState Capture(BoxBrush brush)
        {
            if (brush == null)
                return default;
            var brushSurfaces = brush.Surfaces;
            var surfaces = new CSGBrushSurfaceState[brushSurfaces.Length];
            for (int i = 0; i < surfaces.Length; i++)
                surfaces[i] = CSGBrushSurfaceState.Capture(ref brushSurfaces[i]);
            return new CSGBoxBrushState
            {
                BrushId = brush.ID,
                Transform = brush.Transform,
                Center = brush.Center,
                Size = brush.Size,
                Mode = brush.Mode,
                Surfaces = surfaces,
            };
        }

        /// <summary>
        /// Resolves the live brush by stable identifier.
        /// </summary>
        public BoxBrush Resolve()
        {
            var id = BrushId;
            return id != Guid.Empty ? FlaxEngine.Object.Find<BoxBrush>(ref id) : null;
        }

        /// <summary>
        /// Applies this snapshot to the live brush if it still exists.
        /// </summary>
        public bool Apply()
        {
            var brush = Resolve();
            if (brush == null)
                return false;
            brush.Transform = Transform;
            brush.Center = Center;
            brush.Size = Size;
            brush.Mode = Mode;
            if (Surfaces != null)
            {
                var brushSurfaces = brush.Surfaces;
                int count = Math.Min(brushSurfaces.Length, Surfaces.Length);
                for (int i = 0; i < count; i++)
                    Surfaces[i].Apply(ref brushSurfaces[i]);
                brush.Surfaces = brushSurfaces;
            }
            return true;
        }

        /// <summary>
        /// Compares two captured brush states.
        /// </summary>
        public bool Matches(ref CSGBoxBrushState other)
        {
            if (BrushId != other.BrushId || Transform != other.Transform || Center != other.Center || Size != other.Size || Mode != other.Mode)
                return false;
            int count = Surfaces?.Length ?? 0;
            if (count != (other.Surfaces?.Length ?? 0))
                return false;
            for (int i = 0; i < count; i++)
            {
                var surface = Surfaces[i];
                var otherSurface = other.Surfaces[i];
                if (!surface.Matches(ref otherSurface))
                    return false;
            }
            return true;
        }
    }
}
