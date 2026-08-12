// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEngine;

namespace FlaxEditor.Gizmo.Snapping
{
    /// <summary>
    /// Identifies the feature that produced a viewport snap result.
    /// </summary>
    public enum ViewportSnapTargetKind
    {
        /// <summary>No snap target.</summary>
        None,
        /// <summary>Working-plane grid.</summary>
        Grid,
        /// <summary>CSG vertex.</summary>
        CSGVertex,
        /// <summary>CSG edge.</summary>
        CSGEdge,
        /// <summary>CSG face center.</summary>
        CSGFace,
    }

    /// <summary>
    /// Screen-filtered geometry snap candidate.
    /// </summary>
    public struct ViewportSnapCandidate
    {
        /// <summary>World-space candidate point.</summary>
        public Vector3 Point;
        /// <summary>Candidate feature kind.</summary>
        public ViewportSnapTargetKind Kind;
        /// <summary>Owning actor identity.</summary>
        public Guid ActorId;
        /// <summary>Component index, or -1.</summary>
        public int ComponentIndex;
        /// <summary>Distance from the pointer in physical screen pixels.</summary>
        public float ScreenDistance;
    }

    /// <summary>
    /// Result returned by the shared viewport snap solver.
    /// </summary>
    public struct ViewportSnapResult
    {
        /// <summary>Whether snapping affected the point.</summary>
        public bool IsSnapped;
        /// <summary>Resolved world-space point.</summary>
        public Vector3 Point;
        /// <summary>Resolved plane-local coordinates.</summary>
        public Float2 PlaneCoordinates;
        /// <summary>Chosen target kind.</summary>
        public ViewportSnapTargetKind Kind;
        /// <summary>Chosen actor identity.</summary>
        public Guid ActorId;
        /// <summary>Chosen component index.</summary>
        public int ComponentIndex;
        /// <summary>Chosen screen distance.</summary>
        public float ScreenDistance;
    }

    /// <summary>
    /// Reusable plane-grid and geometry-candidate viewport snap solver.
    /// </summary>
    public sealed class ViewportSnapService
    {
        /// <summary>
        /// Resolves geometry candidates first, then the working-plane grid.
        /// </summary>
        public void Solve(ref CSGWorkingPlane plane, Vector3 point, bool gridEnabled, float geometryThreshold, List<ViewportSnapCandidate> candidates, out ViewportSnapResult result)
        {
            result = new ViewportSnapResult
            {
                Point = point,
                PlaneCoordinates = plane.ToPlane(point),
                Kind = ViewportSnapTargetKind.None,
                ComponentIndex = -1,
                ScreenDistance = float.MaxValue,
            };

            int best = -1;
            if (candidates != null)
            {
                for (int i = 0; i < candidates.Count; i++)
                {
                    var candidate = candidates[i];
                    if (candidate.ScreenDistance > geometryThreshold)
                        continue;
                    if (best < 0 || IsBetter(candidate, candidates[best]))
                        best = i;
                }
            }
            if (best >= 0)
            {
                var candidate = candidates[best];
                var pointOnPlane = plane.ToWorld(plane.ToPlane(candidate.Point));
                result.IsSnapped = true;
                result.Point = pointOnPlane;
                result.PlaneCoordinates = plane.ToPlane(pointOnPlane);
                result.Kind = candidate.Kind;
                result.ActorId = candidate.ActorId;
                result.ComponentIndex = candidate.ComponentIndex;
                result.ScreenDistance = candidate.ScreenDistance;
                return;
            }

            if (gridEnabled)
            {
                result.IsSnapped = true;
                result.Point = SnapToGrid(ref plane, point, out result.PlaneCoordinates);
                result.Kind = ViewportSnapTargetKind.Grid;
                result.ScreenDistance = 0.0f;
            }
        }

        /// <summary>
        /// Snaps a point exactly in plane-local coordinates, including negative coordinates.
        /// </summary>
        public static Vector3 SnapToGrid(ref CSGWorkingPlane plane, Vector3 point, out Float2 planeCoordinates)
        {
            planeCoordinates = plane.ToPlane(point);
            float spacing = Mathf.Max(plane.Spacing, 0.0001f);
            planeCoordinates.X = Mathf.Round(planeCoordinates.X / spacing) * spacing;
            planeCoordinates.Y = Mathf.Round(planeCoordinates.Y / spacing) * spacing;
            return plane.ToWorld(planeCoordinates);
        }

        private static bool IsBetter(ViewportSnapCandidate candidate, ViewportSnapCandidate current)
        {
            if (!Mathf.NearEqual(candidate.ScreenDistance, current.ScreenDistance))
                return candidate.ScreenDistance < current.ScreenDistance;
            if (candidate.Kind != current.Kind)
                return candidate.Kind < current.Kind;
            int actorOrder = candidate.ActorId.CompareTo(current.ActorId);
            return actorOrder < 0 || actorOrder == 0 && candidate.ComponentIndex < current.ComponentIndex;
        }
    }
}
