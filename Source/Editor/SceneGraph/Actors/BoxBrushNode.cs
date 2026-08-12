// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using FlaxEngine;

namespace FlaxEditor.SceneGraph.Actors
{
    /// <summary>
    /// Actor node for <see cref="BoxBrush"/>.
    /// </summary>
    /// <seealso cref="ActorNode" />
    [HideInEditor]
    public sealed class BoxBrushNode : ActorNode
    {
        private const float MinimumComponentExtent = 0.001f;

        internal static readonly int[] BoxEdgeCorners =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };

        internal static Vector3 GetCornerSigns(int cornerIndex)
        {
            switch (cornerIndex)
            {
            case 0: return new Vector3(1, 1, 1);
            case 1: return new Vector3(1, 1, -1);
            case 2: return new Vector3(-1, 1, -1);
            case 3: return new Vector3(-1, 1, 1);
            case 4: return new Vector3(1, -1, 1);
            case 5: return new Vector3(1, -1, -1);
            case 6: return new Vector3(-1, -1, -1);
            default: return new Vector3(-1, -1, 1);
            }
        }

        internal static Vector3 GetComponentPoint(BoxBrush brush, Vector3 signs)
        {
            return brush.Transform.LocalToWorld(brush.Center + signs * brush.Size * 0.5f);
        }

        internal static void SetComponentPoint(BoxBrush brush, Vector3 signs, Vector3 worldPoint)
        {
            var point = brush.Transform.WorldToLocal(worldPoint);
            var center = brush.Center;
            var size = brush.Size;
            for (int axis = 0; axis < 3; axis++)
            {
                float sign = GetComponent(signs, axis);
                if (Mathf.Abs(sign) < 0.5f)
                    continue;
                float halfSize = GetComponent(size, axis) * 0.5f;
                float opposite = GetComponent(center, axis) - sign * halfSize;
                float target = GetComponent(point, axis);
                if (sign > 0.0f)
                    target = Mathf.Max(target, opposite + MinimumComponentExtent);
                else
                    target = Mathf.Min(target, opposite - MinimumComponentExtent);
                SetComponent(ref center, axis, (target + opposite) * 0.5f);
                SetComponent(ref size, axis, Mathf.Abs(target - opposite));
            }
            brush.Center = center;
            brush.Size = size;
        }

        private static float GetComponent(Vector3 value, int axis)
        {
            return axis == 0 ? (float)value.X : axis == 1 ? (float)value.Y : (float)value.Z;
        }

        private static void SetComponent(ref Vector3 value, int axis, float component)
        {
            if (axis == 0)
                value.X = component;
            else if (axis == 1)
                value.Y = component;
            else
                value.Z = component;
        }

        /// <summary>
        /// Sub actor node used to edit volume.
        /// </summary>
        /// <seealso cref="FlaxEditor.SceneGraph.ActorChildNode{T}" />
        public sealed class SideLinkNode : ActorChildNode<BoxBrushNode>
        {
            private static readonly int[] BoxFaces =
            {
                0, 1, 4, 5,
                2, 3, 6, 7,
                0, 1, 3, 2,
                4, 5, 7, 6,
                0, 3, 4, 7,
                1, 2, 5, 6,
            };

            private readonly Vector3[] _boxCorners = new Vector3[8];

            private sealed class BrushSurfaceProxy
            {
                [HideInEditor]
                public BoxBrush Brush;

                [HideInEditor]
                public int Index;

                [EditorOrder(10), EditorDisplay("Brush")]
                [Tooltip("The material used to render the brush surface.")]
                public MaterialBase Material
                {
                    get => Brush.Surfaces[Index].Material;
                    set
                    {
                        var surfaces = Brush.Surfaces;
                        surfaces[Index].Material = value;
                        Brush.Surfaces = surfaces;
                    }
                }

                [EditorOrder(30), EditorDisplay("Brush", "UV Scale"), Limit(-1000, 1000, 0.01f)]
                [Tooltip("The surface texture coordinates scale.")]
                public Float2 TexCoordScale
                {
                    get => Brush.Surfaces[Index].TexCoordScale;
                    set
                    {
                        var surfaces = Brush.Surfaces;
                        surfaces[Index].TexCoordScale = value;
                        Brush.Surfaces = surfaces;
                    }
                }

                [EditorOrder(40), EditorDisplay("Brush", "UV Offset"), Limit(-1000, 1000, 0.01f)]
                [Tooltip("The surface texture coordinates offset.")]
                public Float2 TexCoordOffset
                {
                    get => Brush.Surfaces[Index].TexCoordOffset;
                    set
                    {
                        var surfaces = Brush.Surfaces;
                        surfaces[Index].TexCoordOffset = value;
                        Brush.Surfaces = surfaces;
                    }
                }

                [EditorOrder(50), EditorDisplay("Brush", "UV Rotation")]
                [Tooltip("The surface texture coordinates rotation angle (in degrees).")]
                public float TexCoordRotation
                {
                    get => Brush.Surfaces[Index].TexCoordRotation;
                    set
                    {
                        var surfaces = Brush.Surfaces;
                        surfaces[Index].TexCoordRotation = value;
                        Brush.Surfaces = surfaces;
                    }
                }

                [EditorOrder(20), EditorDisplay("Brush", "Scale In Lightmap"), Limit(0, 10000, 0.1f)]
                [Tooltip("The scale in lightmap (per surface).")]
                public float ScaleInLightmap
                {
                    get => Brush.Surfaces[Index].ScaleInLightmap;
                    set
                    {
                        var surfaces = Brush.Surfaces;
                        surfaces[Index].ScaleInLightmap = value;
                        Brush.Surfaces = surfaces;
                    }
                }
            }

            private Vector3 _offset;

            /// <summary>
            /// Gets the brush actor.
            /// </summary>
            public BoxBrush Brush => (BoxBrush)((BoxBrushNode)ParentNode).Actor;

            /// <summary>
            /// Gets the brush surface.
            /// </summary>
            public BrushSurface Surface
            {
                get => Brush.Surfaces[Index];
                set
                {
                    var surfaces = Brush.Surfaces;
                    surfaces[Index] = value;
                    Brush.Surfaces = surfaces;
                }
            }

            /// <inheritdoc />
            public override CSGViewportSelectionKind CSGViewportSelection => CSGViewportSelectionKind.Face;

            /// <inheritdoc />
            public override bool CanBeSelectedDirectly => true;

            /// <summary>
            /// Initializes a new instance of the <see cref="SideLinkNode"/> class.
            /// </summary>
            /// <param name="actor">The parent node.</param>
            /// <param name="id">The identifier.</param>
            /// <param name="index">The index.</param>
            public SideLinkNode(BoxBrushNode actor, Guid id, int index)
            : base(actor, id, index)
            {
                switch (index)
                {
                case 0:
                    _offset = new Vector3(0.5f, 0, 0);
                    break;
                case 1:
                    _offset = new Vector3(-0.5f, 0, 0);
                    break;
                case 2:
                    _offset = new Vector3(0, 0.5f, 0);
                    break;
                case 3:
                    _offset = new Vector3(0, -0.5f, 0);
                    break;
                case 4:
                    _offset = new Vector3(0, 0, 0.5f);
                    break;
                case 5:
                    _offset = new Vector3(0, 0, -0.5f);
                    break;
                }
            }

            /// <inheritdoc />
            public override Transform Transform
            {
                get
                {
                    var actor = Brush;
                    var localOffset = _offset * actor.Size + actor.Center;
                    Transform localTrans = new Transform(localOffset);
                    return actor.Transform.LocalToWorld(localTrans);
                }
                set
                {
                    var actor = Brush;
                    SetComponentPoint(actor, _offset * 2.0f, value.Translation);
                }
            }

            /// <inheritdoc />
            public override object EditableObject => new BrushSurfaceProxy
            {
                Brush = Brush,
                Index = Index,
            };

            /// <inheritdoc />
            public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
            {
                return Brush.Intersects(Index, ref ray.Ray, out distance, out normal);
            }

            /// <inheritdoc />
            public override void OnDebugDraw(ViewportDebugDrawData data)
            {
                DrawSelectedBrushBounds(Brush);
                Brush.OrientedBox.GetCorners(_boxCorners);
                int offset = Index * 4;
                var v0 = _boxCorners[BoxFaces[offset]];
                var v1 = _boxCorners[BoxFaces[offset + 1]];
                var v2 = _boxCorners[BoxFaces[offset + 2]];
                var v3 = _boxCorners[BoxFaces[offset + 3]];
                var fill = new Color(1.0f, 0.68f, 0.12f, 0.2f);
                var outline = new Color(1.0f, 0.82f, 0.18f, 1.0f);
                DebugDraw.DrawTriangle(v0, v1, v2, fill, 0.0f, true);
                DebugDraw.DrawTriangle(v1, v3, v2, fill, 0.0f, true);
                var xray = new Color(outline.R, outline.G, outline.B, 0.22f);
                DrawDashedLine(v0, v1, xray);
                DrawDashedLine(v1, v3, xray);
                DrawDashedLine(v3, v2, xray);
                DrawDashedLine(v2, v0, xray);
                DebugDraw.DrawLine(v0, v1, outline, 0.0f, true);
                DebugDraw.DrawLine(v1, v3, outline, 0.0f, true);
                DebugDraw.DrawLine(v3, v2, outline, 0.0f, true);
                DebugDraw.DrawLine(v2, v0, outline, 0.0f, true);
            }

            private static void DrawDashedLine(Vector3 start, Vector3 end, Color color)
            {
                const int dashCount = 8;
                const float dashFraction = 0.55f;
                var edge = end - start;
                for (int dash = 0; dash < dashCount; dash++)
                {
                    float from = (float)dash / dashCount;
                    float to = (dash + dashFraction) / dashCount;
                    DebugDraw.DrawLine(start + edge * from, start + edge * to, color, 0.0f, false);
                }
            }
        }

        /// <summary>
        /// Stable selectable midpoint of one box edge. Translating it offsets the two incident planes.
        /// </summary>
        public sealed class EdgeLinkNode : ActorChildNode<BoxBrushNode>
        {
            /// <summary>The zero-based box edge index.</summary>
            public readonly int EdgeIndex;

            /// <summary>Gets the brush actor.</summary>
            public BoxBrush Brush => (BoxBrush)_node.Actor;

            /// <inheritdoc />
            public override CSGViewportSelectionKind CSGViewportSelection => CSGViewportSelectionKind.Edge;

            /// <inheritdoc />
            public override bool CanBeSelectedDirectly => true;

            /// <summary>Creates a stable edge component node.</summary>
            public EdgeLinkNode(BoxBrushNode actor, Guid id, int edgeIndex)
            : base(actor, id, 6 + edgeIndex)
            {
                EdgeIndex = edgeIndex;
            }

            /// <inheritdoc />
            public override Transform Transform
            {
                get
                {
                    int offset = EdgeIndex * 2;
                    var first = GetCornerSigns(BoxEdgeCorners[offset]);
                    var second = GetCornerSigns(BoxEdgeCorners[offset + 1]);
                    return new Transform((GetComponentPoint(Brush, first) + GetComponentPoint(Brush, second)) * 0.5f, Brush.Transform.Orientation);
                }
                set
                {
                    int offset = EdgeIndex * 2;
                    var first = GetCornerSigns(BoxEdgeCorners[offset]);
                    var second = GetCornerSigns(BoxEdgeCorners[offset + 1]);
                    var signs = (first + second) * 0.5f;
                    SetComponentPoint(Brush, signs, value.Translation);
                }
            }

            /// <inheritdoc />
            public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
            {
                distance = 0;
                normal = Vector3.Up;
                return false;
            }

            /// <inheritdoc />
            public override void OnDebugDraw(ViewportDebugDrawData data)
            {
                DrawSelectedBrushBounds(Brush);
            }
        }

        /// <summary>
        /// Stable selectable box vertex. Translating it offsets its three incident planes.
        /// </summary>
        public sealed class VertexLinkNode : ActorChildNode<BoxBrushNode>
        {
            /// <summary>The zero-based box vertex index.</summary>
            public readonly int VertexIndex;

            /// <summary>Gets the brush actor.</summary>
            public BoxBrush Brush => (BoxBrush)_node.Actor;

            /// <inheritdoc />
            public override CSGViewportSelectionKind CSGViewportSelection => CSGViewportSelectionKind.Vertex;

            /// <inheritdoc />
            public override bool CanBeSelectedDirectly => true;

            /// <summary>Creates a stable vertex component node.</summary>
            public VertexLinkNode(BoxBrushNode actor, Guid id, int vertexIndex)
            : base(actor, id, 18 + vertexIndex)
            {
                VertexIndex = vertexIndex;
            }

            /// <inheritdoc />
            public override Transform Transform
            {
                get => new Transform(GetComponentPoint(Brush, GetCornerSigns(VertexIndex)), Brush.Transform.Orientation);
                set => SetComponentPoint(Brush, GetCornerSigns(VertexIndex), value.Translation);
            }

            /// <inheritdoc />
            public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
            {
                distance = 0;
                normal = Vector3.Up;
                return false;
            }

            /// <inheritdoc />
            public override void OnDebugDraw(ViewportDebugDrawData data)
            {
                DrawSelectedBrushBounds(Brush);
            }
        }

        private static void DrawSelectedBrushBounds(BoxBrush brush)
        {
            var box = brush.OrientedBox;
            var corners = box.GetCorners();
            var xray = new Color(1.0f, 1.0f, 0.0f, 0.22f);
            const int dashCount = 8;
            const float dashFraction = 0.55f;
            for (int edgeIndex = 0; edgeIndex < BoxEdgeCorners.Length; edgeIndex += 2)
            {
                var start = corners[BoxEdgeCorners[edgeIndex]];
                var edge = corners[BoxEdgeCorners[edgeIndex + 1]] - start;
                for (int dash = 0; dash < dashCount; dash++)
                {
                    float from = (float)dash / dashCount;
                    float to = (dash + dashFraction) / dashCount;
                    DebugDraw.DrawLine(start + edge * from, start + edge * to, xray, 0.0f, false);
                }
            }
            DebugDraw.DrawWireBox(box, Color.Yellow, 0.0f, true);
        }

        /// <inheritdoc />
        public BoxBrushNode(Actor actor)
        : base(actor)
        {
            var id = ID;
            for (int i = 0; i < 6; i++)
                AddChildNode(new SideLinkNode(this, GetSubID(id, i), i));
            for (int i = 0; i < 12; i++)
                AddChildNode(new EdgeLinkNode(this, GetSubID(id, 6 + i), i));
            for (int i = 0; i < 8; i++)
                AddChildNode(new VertexLinkNode(this, GetSubID(id, 18 + i), i));
        }

        /// <inheritdoc />
        public override CSGViewportSelectionKind CSGViewportSelection => CSGViewportSelectionKind.Brush;

        /// <inheritdoc />
        public override void OnDebugDraw(ViewportDebugDrawData data)
        {
            DrawSelectedBrushBounds((BoxBrush)_actor);
        }

        /// <inheritdoc />
        public override bool RayCastSelf(ref RayCastData ray, out Real distance, out Vector3 normal)
        {
            if (((BoxBrush)_actor).OrientedBox.Intersects(ref ray.Ray))
            {
                Real closestDistance = Real.MaxValue;
                Vector3 closestNormal = Vector3.Up;
                bool hit = false;
                for (int i = 0; i < ChildNodes.Count; i++)
                {
                    if (ChildNodes[i] is SideLinkNode node &&
                        node.RayCastSelf(ref ray, out var faceDistance, out var faceNormal) &&
                        faceDistance >= 0.0f && faceDistance < closestDistance)
                    {
                        hit = true;
                        closestDistance = faceDistance;
                        closestNormal = faceNormal;
                    }
                }
                if (hit)
                {
                    distance = closestDistance;
                    normal = closestNormal;
                    return true;
                }
            }

            distance = 0;
            normal = Vector3.Up;
            return false;
        }

        /// <inheritdoc />
        public override bool OnVertexSnap(ref Ray ray, Real hitDistance, out Vector3 result)
        {
            result = Vector3.Zero;
            var brush = (BoxBrush)_actor;
            var minDistance = Real.MaxValue;
            var minRayDistance = Real.MaxValue;
            var hit = false;
            for (int surfaceIndex = 0; surfaceIndex < 6; surfaceIndex++)
            {
                brush.GetVertices(surfaceIndex, out var vertices);
                if (vertices == null)
                    continue;
                for (int i = 0; i < vertices.Length; i++)
                {
                    if (UpdateClosestVertexToRay(ref ray, vertices[i], ref minDistance, ref minRayDistance, ref result))
                        hit = true;
                }
            }
            return hit;
        }

        /// <inheritdoc />
        public override bool OnVertexSnap(ref Ray ray, Real hitDistance, FlaxEditor.Viewport.EditorViewport viewport, Float2 mousePosition, out Vector3 result, out Real screenDistance)
        {
            result = Vector3.Zero;
            screenDistance = Real.MaxValue;
            var brush = (BoxBrush)_actor;
            var minRayDistance = Real.MaxValue;
            var hit = false;
            for (int surfaceIndex = 0; surfaceIndex < 6; surfaceIndex++)
            {
                brush.GetVertices(surfaceIndex, out var vertices);
                if (vertices == null)
                    continue;
                for (int i = 0; i < vertices.Length; i++)
                {
                    if (UpdateClosestVertexToScreen(ref ray, viewport, mousePosition, vertices[i], hitDistance, ref screenDistance, ref minRayDistance, ref result))
                        hit = true;
                }
            }
            return hit;
        }

        /// <inheritdoc />
        public override void OnVertexSnapEdges(Vector3 vertex, List<Vector3> connectedVertices)
        {
            var brush = (BoxBrush)_actor;
            GetBoxVertexSnapEdges(brush.OrientedBox.GetCorners(), vertex, connectedVertices);
        }
    }
}
