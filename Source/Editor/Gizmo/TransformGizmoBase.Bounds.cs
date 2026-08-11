// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
using Mathr = FlaxEngine.Mathd;
#else
using Real = System.Single;
using Mathr = FlaxEngine.Mathf;
#endif

using System.Collections.Generic;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        private static readonly Axis[] BoundsFaceAxes =
        {
            Axis.XNegative,
            Axis.XPositive,
            Axis.YNegative,
            Axis.YPositive,
            Axis.ZNegative,
            Axis.ZPositive,
        };
        private static readonly int[] BoundsEdges =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };
        private readonly Float2[] _boundsHandleCenters = new Float2[BoundsFaceAxes.Length];
        private readonly List<Float2> _boundsEdgeGaps = new List<Float2>(BoundsFaceAxes.Length);

        internal static bool IsBoundsFaceAxis(Axis axis)
        {
            return axis == Axis.XNegative || axis == Axis.XPositive ||
                   axis == Axis.YNegative || axis == Axis.YPositive ||
                   axis == Axis.ZNegative || axis == Axis.ZPositive;
        }

        private static bool TryGetBoundsFace(Axis axis, out int component, out Real sign, out Vector3 direction)
        {
            switch (axis)
            {
            case Axis.XNegative:
                component = 0;
                sign = -1.0f;
                direction = Vector3.Left;
                return true;
            case Axis.XPositive:
                component = 0;
                sign = 1.0f;
                direction = Vector3.Right;
                return true;
            case Axis.YNegative:
                component = 1;
                sign = -1.0f;
                direction = Vector3.Down;
                return true;
            case Axis.YPositive:
                component = 1;
                sign = 1.0f;
                direction = Vector3.Up;
                return true;
            case Axis.ZNegative:
                component = 2;
                sign = -1.0f;
                direction = Vector3.Backward;
                return true;
            case Axis.ZPositive:
                component = 2;
                sign = 1.0f;
                direction = Vector3.Forward;
                return true;
            default:
                component = -1;
                sign = 0.0f;
                direction = Vector3.Zero;
                return false;
            }
        }

        private static Real GetComponent(Vector3 value, int component)
        {
            return component == 0 ? value.X : component == 1 ? value.Y : value.Z;
        }

        private static void SetComponent(ref Vector3 value, int component, Real componentValue)
        {
            if (component == 0)
                value.X = componentValue;
            else if (component == 1)
                value.Y = componentValue;
            else
                value.Z = componentValue;
        }

        private static Vector3 GetBoundsFaceCenter(BoundingBox bounds, Axis axis)
        {
            Vector3 result = bounds.Center;
            if (!TryGetBoundsFace(axis, out var component, out var sign, out _))
                return result;
            Vector3 extent = bounds.Size * 0.5f;
            SetComponent(ref result, component, GetComponent(result, component) + GetComponent(extent, component) * sign);
            return result;
        }

        internal static Vector3 GetBoundsResizePivot(BoundingBox bounds, Axis draggedFace)
        {
            if (!TryGetBoundsFace(draggedFace, out var component, out var sign, out _))
                return bounds.Center;
            Vector3 result = bounds.Center;
            Vector3 extent = bounds.Size * 0.5f;
            SetComponent(ref result, component, GetComponent(result, component) - GetComponent(extent, component) * sign);
            return result;
        }

        internal static Real SolveBoundsResizeFactor(Real anchorFactor, Real displacement, Real originalExtent, Real faceSign)
        {
            if (Mathr.Abs(originalExtent) <= Mathf.Epsilon)
                return anchorFactor;
            return Mathr.Max(anchorFactor + faceSign * displacement / originalExtent, 0.0001f);
        }

        private void BuildBoundsSemanticTargets()
        {
            GetSelectedObjectsBounds(out var bounds, out _);
            if (!IsValidBounds(ref bounds))
                return;

            for (int i = 0; i < BoundsFaceAxes.Length; i++)
                AddBoundsFaceSemanticTarget(bounds, BoundsFaceAxes[i]);
        }

        private void AddBoundsFaceSemanticTarget(BoundingBox bounds, Axis axis)
        {
            if (!TryGetBoundsFace(axis, out var component, out _, out var direction))
                return;
            Vector3 worldEnd = GetBoundsFaceCenter(bounds, axis);
            if (!TryProjectSemanticWorldPoint(worldEnd, out var screenEnd))
                return;

            bool hasExtent = Mathr.Abs(GetComponent(bounds.Size, component)) > Mathf.Epsilon;
            bool isAvailable = hasExtent && IsTargetAvailable(new SemanticHandle(_activeMode, axis));
            float depth = GetSemanticDepth(worldEnd);
            var handle = CreateSemanticHandle(axis, worldEnd, direction, screenEnd, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.CenterSquare, Float2.Zero, Float2.Zero, screenEnd, Float2.Zero, Float2.Zero, Float2.Zero, Float2.Zero, null, CapMotorTargetWidthPixels * 0.5f, depth, depth, depth, null, isAvailable));
        }

        private void UpdateBoundsResize()
        {
            var anchor = InteractionAnchor;
            var origin = TransactionOrigin;
            if (anchor == null || origin == null || Owner?.Viewport == null ||
                !TryGetBoundsFace(_activeAxis, out var component, out var sign, out var faceDirection))
                return;

            Real originalExtent = GetComponent(origin.OriginalBounds.Size, component);
            if (Mathr.Abs(originalExtent) <= Mathf.Epsilon)
                return;

            Vector3 positiveAxis = faceDirection * sign;
            Vector3 faceCenter = GetBoundsFaceCenter(origin.OriginalBounds, _activeAxis);
            Real displacement;
            Ray currentRay = Owner.MouseRay;
            if (TrySolveAxisTranslation(anchor.PointerRay, currentRay, faceCenter, positiveAxis, out var worldDelta))
            {
                displacement = Vector3.Dot(worldDelta, positiveAxis);
            }
            else
            {
                // A view ray parallel to the resize axis has no stable closest-point
                // solution. Retain the projected fallback used by the regular gizmo.
                if (!TryGetProjectedAxis(faceCenter, positiveAxis, out var screenDirection, out Real worldUnitsPerPixel))
                    return;
                float pixels = Float2.Dot(Owner.Viewport.ContinuousViewMousePosition - anchor.PointerPosition, screenDirection);
                displacement = pixels * worldUnitsPerPixel;
            }
            if (Owner.IsAltKeyDown)
                displacement *= PrecisionScaleGain;

            Vector3 desired = anchor.Result.Scale;
            Real factor = SolveBoundsResizeFactor(GetComponent(desired, component), displacement, originalExtent, sign);
            if (ScaleSnapEnabled || Owner.UseSnapping)
            {
                Vector3 step = GetLinearSnapStep(origin);
                factor = SnapScaleFactorToGrid(factor, originalExtent, GetComponent(step, component));
            }
            SetComponent(ref desired, component, factor);

            _scaleDelta = desired - InteractionResult.Scale;
        }

        private MaterialInstance GetBoundsAxisMaterial(Axis axis)
        {
            if (axis == Axis.XNegative || axis == Axis.XPositive)
                return _materialAxisX;
            if (axis == Axis.YNegative || axis == Axis.YPositive)
                return _materialAxisY;
            return _materialAxisZ;
        }

        private MaterialInstance GetBoundsConnectorMaterial(Axis axis)
        {
            if (!_isDisabled && (_activeAxis == axis || _hoveredHandle.Axis == axis))
                return _materialBoundsConnectorFocus;
            if (axis == Axis.XNegative || axis == Axis.XPositive)
                return _materialBoundsConnectorX;
            if (axis == Axis.YNegative || axis == Axis.YPositive)
                return _materialBoundsConnectorY;
            return _materialBoundsConnectorZ;
        }

        private void DrawBoundsResizeHandles(ref RenderContext renderContext, sbyte sortOrder)
        {
            if (!_modelScaleAxis || !_modelScaleAxis.IsLoaded)
                return;
            GetSelectedObjectsBounds(out var bounds, out _);
            if (!IsValidBounds(ref bounds))
                return;

            var connectorMesh = _modelScaleAxis.LODs[0].Meshes[0];
            var cubeMesh = _modelScaleAxis.LODs[0].Meshes[1];
            for (int i = 0; i < BoundsFaceAxes.Length; i++)
            {
                Axis axis = BoundsFaceAxes[i];
                if (!TryGetBoundsFace(axis, out _, out _, out var direction))
                    continue;

                Float3 from = Float3.Backward;
                Float3 to = direction;
                Float3 fallback = Mathf.Abs(Float3.Dot(to, Float3.Up)) > 0.99f ? Float3.Right : Float3.Up;
                Quaternion orientation = Quaternion.GetRotationFromTo(from, to, fallback);
                Vector3 faceCenter = GetBoundsFaceCenter(bounds, axis);
                if (!TryGetGizmoWorldRadius(faceCenter, out float gizmoWorldRadius))
                    continue;
                float screenScale = gizmoWorldRadius / GizmoGeometryRadiusRaw;
                Vector3 origin = faceCenter - direction * (screenScale * AxisLength);
                var transform = new Transform(origin, orientation, new Float3(screenScale));
                renderContext.View.GetWorldMatrix(ref transform, out var world);

                bool focused = !_isDisabled && (_activeAxis == axis || _hoveredHandle.Axis == axis);
                MaterialInstance material = focused ? _materialAxisFocus : GetBoundsAxisMaterial(axis);
                DrawGizmoMesh(ref renderContext, connectorMesh, GetBoundsConnectorMaterial(axis), ref world, sortOrder);
                DrawGizmoMesh(ref renderContext, cubeMesh, material, ref world, sortOrder);
            }
        }

        private void DrawBoundsResizeOverlay()
        {
            if (!_isActive || !IsActive || _activeMode != Mode.Bounds || SelectionCount == 0)
                return;

            GetSelectedObjectsBounds(out var bounds, out _);
            if (!IsValidBounds(ref bounds))
                return;

            var corners = bounds.GetCorners();
            var screenCorners = new Float2[corners.Length];
            for (int i = 0; i < corners.Length; i++)
            {
                if (!TryProjectGizmoPoint(corners[i], out screenCorners[i]))
                    return;
            }

            for (int i = 0; i < BoundsFaceAxes.Length; i++)
            {
                if (!TryProjectGizmoPoint(GetBoundsFaceCenter(bounds, BoundsFaceAxes[i]), out _boundsHandleCenters[i]))
                    _boundsHandleCenters[i] = new Float2(float.MaxValue);
            }
            Color boundsColor = new Color(1.0f, 0.82f, 0.08f, 0.92f);
            for (int i = 0; i < BoundsEdges.Length; i += 2)
            {
                Float2 start = screenCorners[BoundsEdges[i]];
                Float2 end = screenCorners[BoundsEdges[i + 1]];
                DrawBoundsEdgeWithHandleGaps(start, end, boundsColor);
            }

        }

        private void DrawBoundsEdgeWithHandleGaps(Float2 start, Float2 end, Color color)
        {
            const float gapRadius = 11.0f;
            Float2 segment = end - start;
            float lengthSquared = segment.LengthSquared;
            if (lengthSquared < 0.0001f)
                return;

            _boundsEdgeGaps.Clear();
            for (int i = 0; i < _boundsHandleCenters.Length; i++)
            {
                Float2 toCenter = _boundsHandleCenters[i] - start;
                float centerAmount = Float2.Dot(toCenter, segment) / lengthSquared;
                if (centerAmount < 0.0f || centerAmount > 1.0f)
                    continue;
                Float2 closest = start + segment * centerAmount;
                float distanceSquared = (_boundsHandleCenters[i] - closest).LengthSquared;
                if (distanceSquared >= gapRadius * gapRadius)
                    continue;
                float halfAmount = Mathf.Sqrt((gapRadius * gapRadius - distanceSquared) / lengthSquared);
                _boundsEdgeGaps.Add(new Float2(Mathf.Max(0.0f, centerAmount - halfAmount), Mathf.Min(1.0f, centerAmount + halfAmount)));
            }

            _boundsEdgeGaps.Sort((a, b) => a.X.CompareTo(b.X));
            float visibleStart = 0.0f;
            for (int i = 0; i < _boundsEdgeGaps.Count; i++)
            {
                Float2 gap = _boundsEdgeGaps[i];
                if (gap.X > visibleStart)
                    DrawBoundsEdgeSegment(start + segment * visibleStart, start + segment * gap.X, color);
                visibleStart = Mathf.Max(visibleStart, gap.Y);
            }
            if (visibleStart < 1.0f)
                DrawBoundsEdgeSegment(start + segment * visibleStart, end, color);
        }

        private static void DrawBoundsEdgeSegment(Float2 start, Float2 end, Color color)
        {
            Render2D.DrawLine(start, end, Color.Black.AlphaMultiplied(0.7f), 3.5f);
            Render2D.DrawLine(start, end, color, 1.5f);
        }
    }
}
