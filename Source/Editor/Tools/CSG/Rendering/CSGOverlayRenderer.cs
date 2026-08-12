// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Gizmo.Snapping;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Rendering
{
    /// <summary>
    /// Draws allocation-free CSG working-plane and snap feedback using editor debug primitives.
    /// </summary>
    public sealed class CSGOverlayRenderer
    {
        private const int GridHalfCells = 20;

        /// <summary>
        /// Draws the arbitrary-orientation working grid and optional snap marker.
        /// </summary>
        public void Draw(ref CSGWorkingPlane plane, Vector3 viewPosition, bool isHoverPreview, bool hasSnap, ref ViewportSnapResult snap, float snapMarkerSize)
        {
            if (!plane.IsValid)
                return;

            float spacing = Mathf.Max(plane.Spacing, 0.0001f);
            float halfExtent = spacing * GridHalfCells;
            float viewDistance = (float)Vector3.Distance(viewPosition, plane.Origin);
            float distanceFade = Mathf.Saturate(1.0f - viewDistance / Mathf.Max(halfExtent * 8.0f, 1000.0f));
            float baseAlpha = (isHoverPreview ? 0.48f : 0.72f) * Mathf.Lerp(0.3f, 1.0f, distanceFade);
            float bias = Mathf.Max(0.08f, spacing * 0.008f);
            var origin = plane.Origin + plane.Normal * bias;
            var minor = new Color(0.32f, 0.32f, 0.32f, baseAlpha * 0.68f);
            var major = new Color(0.16f, 0.16f, 0.16f, baseAlpha * 0.9f);
            var tangentAxis = new Color(0.08f, 0.08f, 0.08f, baseAlpha);
            var bitangentAxis = tangentAxis;

            for (int i = -GridHalfCells; i <= GridHalfCells; i++)
            {
                float offset = i * spacing;
                bool axis = i == 0;
                bool majorLine = !axis && i % plane.MajorLineInterval == 0;
                var rowColor = axis ? tangentAxis : majorLine ? major : minor;
                var columnColor = axis ? bitangentAxis : majorLine ? major : minor;
                var rowCenter = origin + plane.Bitangent * offset;
                var columnCenter = origin + plane.Tangent * offset;
                DebugDraw.DrawLine(rowCenter - plane.Tangent * halfExtent, rowCenter + plane.Tangent * halfExtent, rowColor, 0.0f, true);
                DebugDraw.DrawLine(columnCenter - plane.Bitangent * halfExtent, columnCenter + plane.Bitangent * halfExtent, columnColor, 0.0f, true);
            }

            DebugDraw.DrawLine(origin, origin + plane.Normal * spacing * 2.0f, new Color(0.38f, 1.0f, 0.42f, baseAlpha), 0.0f, false);
            if (hasSnap)
                DrawSnapMarker(ref plane, ref snap, spacing, snapMarkerSize);
        }

        private static void DrawSnapMarker(ref CSGWorkingPlane plane, ref ViewportSnapResult snap, float spacing, float snapMarkerSize)
        {
            float size = snapMarkerSize > Mathf.Epsilon ? snapMarkerSize : Mathf.Clamp(spacing * 0.15f, 0.75f, 7.5f);
            var point = snap.Point + plane.Normal * Mathf.Max(0.12f, spacing * 0.012f);
            var color = new Color(1.0f, 0.82f, 0.12f, 1.0f);
            var cube = new OrientedBoundingBox(new Vector3(-size * 0.5f), new Vector3(size * 0.5f))
            {
                Transformation = new Transform(point, Quaternion.LookRotation(-plane.Bitangent, plane.Normal)),
            };
            DebugDraw.DrawBox(cube, color, 0.0f, false);
            DebugDraw.DrawWireBox(cube, new Color(0.55f, 0.38f, 0.02f, 1.0f), 0.0f, false);
        }
    }
}
