// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using FlaxEditor.SceneGraph;
using FlaxEditor.SceneGraph.Actors;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.HitTesting
{
    /// <summary>
    /// Identifies how a ray hit participates in CSG authoring.
    /// </summary>
    public enum CSGHitKind
    {
        /// <summary>
        /// A CSG brush body.
        /// </summary>
        Brush,

        /// <summary>
        /// A stable CSG brush face.
        /// </summary>
        Face,

        /// <summary>
        /// A stable CSG brush edge.
        /// </summary>
        Edge,

        /// <summary>
        /// A stable CSG brush vertex.
        /// </summary>
        Vertex,

        /// <summary>
        /// Non-CSG geometry that can be used for placement or snapping only.
        /// </summary>
        Placement,
    }

    /// <summary>
    /// A classified CSG authoring ray hit.
    /// </summary>
    public struct CSGHit
    {
        /// <summary>
        /// The intersected scene graph node.
        /// </summary>
        public SceneGraphNode Node;

        /// <summary>
        /// The owning brush node, or null for placement-only geometry.
        /// </summary>
        public BoxBrushNode Brush;

        /// <summary>
        /// The hit classification.
        /// </summary>
        public CSGHitKind Kind;

        /// <summary>
        /// Stable component index. Faces use their surface index; bodies and placement hits use -1.
        /// </summary>
        public int ComponentIndex;

        /// <summary>
        /// Distance from the ray origin.
        /// </summary>
        public Real Distance;

        /// <summary>
        /// Surface normal at the intersection.
        /// </summary>
        public Vector3 Normal;

        /// <summary>
        /// World-space intersection point.
        /// </summary>
        public Vector3 Point;

        /// <summary>
        /// Gets the node that should be synchronized with the editor selection for the given tool.
        /// </summary>
        public SceneGraphNode SelectionNode => Kind == CSGHitKind.Face ? Node : Brush;
    }

    /// <summary>
    /// Collects, classifies, and deterministically orders viewport hits for CSG tools.
    /// </summary>
    public sealed class CSGHitTestService
    {
        private sealed class HitComparer : IComparer<CSGHit>
        {
            public static readonly HitComparer Instance = new HitComparer();

            public int Compare(CSGHit x, CSGHit y)
            {
                int result = x.Distance.CompareTo(y.Distance);
                if (result != 0)
                    return result;

                var xOrderNode = (SceneGraphNode)x.Brush ?? x.Node;
                var yOrderNode = (SceneGraphNode)y.Brush ?? y.Node;
                result = xOrderNode.OrderInParent.CompareTo(yOrderNode.OrderInParent);
                if (result != 0)
                    return result;
                result = xOrderNode.ID.CompareTo(yOrderNode.ID);
                if (result != 0)
                    return result;
                result = x.Kind.CompareTo(y.Kind);
                if (result != 0)
                    return result;
                return x.ComponentIndex.CompareTo(y.ComponentIndex);
            }
        }

        private readonly List<SceneGraphNode.RayCastHit> _rawHits = new List<SceneGraphNode.RayCastHit>(128);

        /// <summary>
        /// Collects all scene hits into a caller-owned buffer.
        /// </summary>
        /// <param name="root">The scene graph root.</param>
        /// <param name="ray">The viewport ray.</param>
        /// <param name="view">The camera view ray.</param>
        /// <param name="results">The classified result buffer. Existing entries are cleared.</param>
        /// <param name="flags">The scene graph raycast flags.</param>
        public void Gather(RootNode root, ref Ray ray, ref Ray view, List<CSGHit> results, SceneGraphNode.RayCastData.FlagTypes flags)
        {
            if (results == null)
                throw new ArgumentNullException(nameof(results));

            results.Clear();
            if (root == null)
                return;

            root.RayCastAll(ref ray, ref view, _rawHits, flags);
            for (int i = 0; i < _rawHits.Count; i++)
            {
                var raw = _rawHits[i];
                var hit = new CSGHit
                {
                    Node = raw.Node,
                    Brush = null,
                    Kind = CSGHitKind.Placement,
                    ComponentIndex = -1,
                    Distance = raw.Distance,
                    Normal = raw.Normal,
                    Point = ray.Position + ray.Direction * raw.Distance,
                };

                if (raw.Node.CSGViewportSelection == CSGViewportSelectionKind.Brush)
                {
                    hit.Brush = raw.Node as BoxBrushNode;
                    hit.Kind = CSGHitKind.Brush;
                }
                else if (raw.Node.CSGViewportSelection == CSGViewportSelectionKind.Face)
                {
                    hit.Brush = raw.Node.ParentNode as BoxBrushNode;
                    hit.Kind = CSGHitKind.Face;
                    hit.ComponentIndex = raw.Node.OrderInParent;
                }
                else if (raw.Node.CSGViewportSelection == CSGViewportSelectionKind.Edge)
                {
                    hit.Brush = raw.Node.ParentNode as BoxBrushNode;
                    hit.Kind = CSGHitKind.Edge;
                    hit.ComponentIndex = raw.Node.OrderInParent - 6;
                }
                else if (raw.Node.CSGViewportSelection == CSGViewportSelectionKind.Vertex)
                {
                    hit.Brush = raw.Node.ParentNode as BoxBrushNode;
                    hit.Kind = CSGHitKind.Vertex;
                    hit.ComponentIndex = raw.Node.OrderInParent - 18;
                }
                results.Add(hit);
            }
            results.Sort(HitComparer.Instance);
        }

        /// <summary>
        /// Sorts classified hits from front to back using the stable CSG selection order.
        /// </summary>
        /// <param name="results">The classified hits.</param>
        public void Sort(List<CSGHit> results)
        {
            if (results == null)
                throw new ArgumentNullException(nameof(results));
            results.Sort(HitComparer.Instance);
        }

        /// <summary>
        /// Gets whether a classified hit is directly selectable by the active CSG tool.
        /// </summary>
        public static bool IsSelectable(CSGTool tool, ref CSGHit hit)
        {
            if (tool == CSGTool.Edit)
                return hit.Kind == CSGHitKind.Brush || hit.Kind == CSGHitKind.Face || hit.Kind == CSGHitKind.Edge || hit.Kind == CSGHitKind.Vertex;
            if (tool == CSGTool.Surface || tool == CSGTool.Brush)
                return hit.Kind == CSGHitKind.Face;
            return hit.Kind == CSGHitKind.Brush;
        }
    }
}
