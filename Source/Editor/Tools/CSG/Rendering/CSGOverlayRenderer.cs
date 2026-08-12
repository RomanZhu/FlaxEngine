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
        private const int MaximumVisibleHalfLines = 160;

        /// <summary>
        /// Draws the arbitrary-orientation working grid and optional snap marker.
        /// </summary>
        public void Draw(ref CSGWorkingPlane plane, Vector3 viewPosition, Vector3 focusPoint, float requestedHalfExtent, bool isHoverPreview, bool hasSnap, ref ViewportSnapResult snap, float snapMarkerSize)
        {
            if (!plane.IsValid)
                return;

            float spacing = Mathf.Max(plane.Spacing, 0.0001f);
            float halfExtent = Mathf.Max(requestedHalfExtent, 0.0001f);
            int halfCells = Mathf.Max(Mathf.FloorToInt(halfExtent / spacing), 0);
            int lineStride = CalculateLineStride(halfCells);
            var focusCoordinates = plane.ToPlane(focusPoint);
            int focusTangentCell = Mathf.RoundToInt(focusCoordinates.X / spacing);
            int focusBitangentCell = Mathf.RoundToInt(focusCoordinates.Y / spacing);
            var snappedFocus = plane.ToWorld(new Float2(focusTangentCell * spacing, focusBitangentCell * spacing));
            float viewDistance = (float)Vector3.Distance(viewPosition, snappedFocus);
            float distanceFade = Mathf.Saturate(1.0f - viewDistance / Mathf.Max(halfExtent * 8.0f, 1000.0f));
            float baseAlpha = (isHoverPreview ? 0.72f : 0.9f) * Mathf.Lerp(0.65f, 1.0f, distanceFade);
            float bias = Mathf.Max(0.18f, spacing * 0.012f);
            var origin = snappedFocus + plane.Normal * bias;
            var minor = new Color(0.32f, 0.72f, 1.0f, baseAlpha * 0.82f);
            var major = new Color(0.035f, 0.18f, 0.42f, baseAlpha);
            var tangentAxis = new Color(0.015f, 0.08f, 0.24f, baseAlpha);
            var bitangentAxis = tangentAxis;

            int firstTangentCell = Mathf.CeilToInt((float)(focusTangentCell - halfCells) / lineStride) * lineStride;
            int lastTangentCell = focusTangentCell + halfCells;
            for (int tangentCell = firstTangentCell; tangentCell <= lastTangentCell; tangentCell += lineStride)
            {
                float offset = (tangentCell - focusTangentCell) * spacing;
                bool columnAxis = tangentCell == 0;
                bool columnMajor = !columnAxis && tangentCell % plane.MajorLineInterval == 0;
                var columnColor = columnAxis ? bitangentAxis : columnMajor ? major : minor;
                var columnCenter = origin + plane.Tangent * offset;
                DebugDraw.DrawLine(columnCenter - plane.Bitangent * halfExtent, columnCenter + plane.Bitangent * halfExtent, columnColor, 0.0f, false);
            }

            int firstBitangentCell = Mathf.CeilToInt((float)(focusBitangentCell - halfCells) / lineStride) * lineStride;
            int lastBitangentCell = focusBitangentCell + halfCells;
            for (int bitangentCell = firstBitangentCell; bitangentCell <= lastBitangentCell; bitangentCell += lineStride)
            {
                float offset = (bitangentCell - focusBitangentCell) * spacing;
                bool rowAxis = bitangentCell == 0;
                bool rowMajor = !rowAxis && bitangentCell % plane.MajorLineInterval == 0;
                var rowColor = rowAxis ? tangentAxis : rowMajor ? major : minor;
                var rowCenter = origin + plane.Bitangent * offset;
                DebugDraw.DrawLine(rowCenter - plane.Tangent * halfExtent, rowCenter + plane.Tangent * halfExtent, rowColor, 0.0f, false);
            }

            var planeOrigin = plane.Origin + plane.Normal * bias;
            DebugDraw.DrawLine(planeOrigin, planeOrigin + plane.Normal * spacing * 2.0f, new Color(0.38f, 1.0f, 0.42f, baseAlpha), 0.0f, false);
            if (hasSnap)
                DrawSnapMarker(ref plane, ref snap, spacing, snapMarkerSize);
        }

        /// <summary>
        /// Draws a stationary neutral grid for a face-axis drag and emphasizes the current snapped step.
        /// </summary>
        public void DrawFaceDragGuide(ref CSGWorkingPlane plane, Vector3 currentFaceCenter, float requestedHalfExtent)
        {
            if (!plane.IsValid)
                return;

            float spacing = Mathf.Max(plane.Spacing, 0.0001f);
            float halfExtent = Mathf.Max(requestedHalfExtent, spacing);
            int halfCells = Mathf.Max(Mathf.FloorToInt(halfExtent / spacing), 1);
            int lineStride = CalculateLineStride(halfCells);
            float bias = Mathf.Max(0.18f, spacing * 0.012f);
            var origin = plane.Origin + plane.Normal * bias;
            var minor = new Color(0.62f, 0.64f, 0.68f, 0.28f);
            var major = new Color(0.72f, 0.74f, 0.78f, 0.42f);
            var axis = new Color(0.84f, 0.85f, 0.88f, 0.5f);

            int firstCell = Mathf.CeilToInt((float)-halfCells / lineStride) * lineStride;
            for (int cell = firstCell; cell <= halfCells; cell += lineStride)
            {
                float offset = cell * spacing;
                bool isAxis = cell == 0;
                bool isMajor = !isAxis && cell % plane.MajorLineInterval == 0;
                var color = isAxis ? axis : isMajor ? major : minor;
                var columnCenter = origin + plane.Tangent * offset;
                DebugDraw.DrawLine(columnCenter - plane.Bitangent * halfExtent, columnCenter + plane.Bitangent * halfExtent, color, 0.0f, false);
                var rowCenter = origin + plane.Bitangent * offset;
                DebugDraw.DrawLine(rowCenter - plane.Tangent * halfExtent, rowCenter + plane.Tangent * halfExtent, color, 0.0f, false);
            }

            var coordinates = plane.ToPlane(currentFaceCenter);
            int activeStep = Mathf.RoundToInt(coordinates.Y / spacing);
            float activeOffset = activeStep * spacing;
            if (Mathf.Abs(activeOffset) <= halfExtent)
            {
                var activeCenter = origin + plane.Bitangent * activeOffset;
                var activeColor = new Color(0.92f, 0.93f, 0.96f, 0.72f);
                DebugDraw.DrawLine(activeCenter - plane.Tangent * halfExtent, activeCenter + plane.Tangent * halfExtent, activeColor, 0.0f, false);
            }
        }

        private static int CalculateLineStride(int halfCells)
        {
            int stride = 1;
            while (halfCells / stride > MaximumVisibleHalfLines)
            {
                if (stride == 1)
                    stride = 2;
                else if (stride == 2)
                    stride = 5;
                else
                    stride *= 2;
            }
            return stride;
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
