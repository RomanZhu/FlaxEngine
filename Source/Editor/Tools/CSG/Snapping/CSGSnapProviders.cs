// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEditor.Gizmo.Snapping;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEditor.Viewport;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Snapping
{
    /// <summary>
    /// Collects screen-thresholded CSG vertex, edge, and face snap candidates.
    /// </summary>
    public static class CSGSnapProviders
    {
        private static readonly int[] BoxEdges =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };

        private static readonly int[] BoxFaces =
        {
            0, 1, 4, 5,
            2, 3, 6, 7,
            0, 1, 2, 3,
            4, 5, 6, 7,
            0, 3, 4, 7,
            1, 2, 5, 6,
        };

        /// <summary>
        /// Appends candidates into a caller-owned buffer without retaining scene objects.
        /// </summary>
        public static void Gather(List<ActorNode> actors, IReadOnlyList<SceneGraphNode> excludedSelection, EditorViewport viewport, Float2 pointer, float threshold, List<ViewportSnapCandidate> candidates, Vector3[] corners)
        {
            candidates.Clear();
            float thresholdSquared = threshold * threshold;
            for (int actorIndex = 0; actorIndex < actors.Count; actorIndex++)
            {
                if (actors[actorIndex] is not BoxBrushNode node || !node.IsActiveInHierarchy || IsExcluded(node, excludedSelection))
                    continue;
                var brush = (BoxBrush)node.Actor;
                brush.OrientedBox.GetCorners(corners);

                for (int i = 0; i < corners.Length; i++)
                {
                    viewport.ProjectPoint(corners[i], out var screen);
                    float distanceSquared = (pointer - screen).LengthSquared;
                    if (distanceSquared <= thresholdSquared)
                        Add(candidates, corners[i], ViewportSnapTargetKind.CSGVertex, node.ID, i, Mathf.Sqrt(distanceSquared));
                }

                for (int i = 0; i < BoxEdges.Length; i += 2)
                {
                    int startIndex = BoxEdges[i];
                    int endIndex = BoxEdges[i + 1];
                    viewport.ProjectPoint(corners[startIndex], out var startScreen);
                    viewport.ProjectPoint(corners[endIndex], out var endScreen);
                    float distanceSquared = DistanceSquaredToSegment(pointer, startScreen, endScreen, out float amount);
                    if (distanceSquared <= thresholdSquared)
                    {
                        var point = corners[startIndex] + (corners[endIndex] - corners[startIndex]) * amount;
                        Add(candidates, point, ViewportSnapTargetKind.CSGEdge, node.ID, i / 2, Mathf.Sqrt(distanceSquared));
                    }
                }

                for (int face = 0; face < 6; face++)
                {
                    int offset = face * 4;
                    var center = (corners[BoxFaces[offset]] + corners[BoxFaces[offset + 1]] + corners[BoxFaces[offset + 2]] + corners[BoxFaces[offset + 3]]) * 0.25f;
                    viewport.ProjectPoint(center, out var screen);
                    float distanceSquared = (pointer - screen).LengthSquared;
                    if (distanceSquared <= thresholdSquared)
                        Add(candidates, center, ViewportSnapTargetKind.CSGFace, node.ID, face, Mathf.Sqrt(distanceSquared));
                }
            }
        }

        /// <summary>
        /// Gets whether a brush belongs to the active editing selection and must be excluded from snap targets.
        /// </summary>
        public static bool IsExcluded(BoxBrushNode brush, IReadOnlyList<SceneGraphNode> excludedSelection)
        {
            if (excludedSelection == null)
                return false;
            for (int i = 0; i < excludedSelection.Count; i++)
            {
                if (excludedSelection[i] == brush || excludedSelection[i]?.ParentNode == brush)
                    return true;
            }
            return false;
        }

        private static void Add(List<ViewportSnapCandidate> candidates, Vector3 point, ViewportSnapTargetKind kind, System.Guid actorId, int componentIndex, float screenDistance)
        {
            candidates.Add(new ViewportSnapCandidate
            {
                Point = point,
                Kind = kind,
                ActorId = actorId,
                ComponentIndex = componentIndex,
                ScreenDistance = screenDistance,
            });
        }

        private static float DistanceSquaredToSegment(Float2 point, Float2 start, Float2 end, out float amount)
        {
            var edge = end - start;
            float lengthSquared = edge.LengthSquared;
            if (lengthSquared <= Mathf.Epsilon)
            {
                amount = 0.0f;
                return (point - start).LengthSquared;
            }
            amount = Mathf.Saturate(Float2.Dot(point - start, edge) / lengthSquared);
            return (point - (start + edge * amount)).LengthSquared;
        }
    }
}
