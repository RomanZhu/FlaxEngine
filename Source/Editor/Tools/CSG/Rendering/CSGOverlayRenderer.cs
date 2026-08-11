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
        public void Draw(ref CSGWorkingPlane plane, Vector3 viewPosition, bool isHoverPreview, bool hasSnap, ref ViewportSnapResult snap)
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
            var minor = new Color(0.28f, 0.68f, 0.9f, baseAlpha * 0.36f);
            var major = new Color(0.35f, 0.8f, 1.0f, baseAlpha * 0.78f);
            var tangentAxis = new Color(0.95f, 0.28f, 0.22f, baseAlpha);
            var bitangentAxis = new Color(0.2f, 0.55f, 1.0f, baseAlpha);

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
                DrawSnapMarker(ref plane, ref snap, spacing);
        }

        private static void DrawSnapMarker(ref CSGWorkingPlane plane, ref ViewportSnapResult snap, float spacing)
        {
            float radius = Mathf.Clamp(spacing * 0.22f, 1.5f, 15.0f);
            var point = snap.Point + plane.Normal * Mathf.Max(0.12f, spacing * 0.012f);
            var color = snap.Kind == ViewportSnapTargetKind.Grid
                ? new Color(1.0f, 0.82f, 0.18f, 1.0f)
                : new Color(0.35f, 1.0f, 0.52f, 1.0f);
            DebugDraw.DrawLine(point - plane.Tangent * radius, point + plane.Tangent * radius, color, 0.0f, false);
            DebugDraw.DrawLine(point - plane.Bitangent * radius, point + plane.Bitangent * radius, color, 0.0f, false);
            DebugDraw.DrawWireSphere(new BoundingSphere(point, radius * 0.42f), color, 0.0f, false);
        }
    }
}
