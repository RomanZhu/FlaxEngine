// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
using Mathr = FlaxEngine.Mathd;
#else
using Real = System.Single;
using Mathr = FlaxEngine.Mathf;
#endif

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
            if (IsScaleSnappingActive)
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

                // Point the stem away from the bounds. Keeping the stem outside
                // prevents opposite face handles from joining across the object.
                Float3 from = Float3.Backward;
                Float3 to = -direction;
                Float3 fallback = Mathf.Abs(Float3.Dot(to, Float3.Up)) > 0.99f ? Float3.Right : Float3.Up;
                Quaternion orientation = Quaternion.GetRotationFromTo(from, to, fallback);
                Vector3 faceCenter = GetBoundsFaceCenter(bounds, axis);
                if (!TryGetGizmoWorldRadius(faceCenter, out float gizmoWorldRadius))
                    continue;
                float screenScale = gizmoWorldRadius / GizmoGeometryRadiusRaw;
                float stemWorldLength = gizmoWorldRadius * (BoundsHandleStemPixels / GizmoRadiusPixels);

                // The connector has independent axial scaling so its projected
                // length is constant while its thickness follows the gizmo size.
                Vector3 connectorOrigin = faceCenter + direction * stemWorldLength;
                var connectorTransform = new Transform(connectorOrigin, orientation, new Float3(screenScale, screenScale, stemWorldLength / AxisLength));
                renderContext.View.GetWorldMatrix(ref connectorTransform, out var connectorWorld);

                // The cube mesh is authored at AxisLength. Give it a separate
                // full-size transform that keeps its center exactly on the face.
                Vector3 cubeOrigin = faceCenter + direction * (screenScale * AxisLength);
                var cubeTransform = new Transform(cubeOrigin, orientation, new Float3(screenScale));
                renderContext.View.GetWorldMatrix(ref cubeTransform, out var cubeWorld);

                bool focused = !_isDisabled && (_activeAxis == axis || _hoveredHandle.Axis == axis);
                MaterialInstance material = focused ? _materialAxisFocus : GetBoundsAxisMaterial(axis);
                DrawGizmoMesh(ref renderContext, connectorMesh, GetBoundsConnectorMaterial(axis), ref connectorWorld, sortOrder);
                DrawGizmoMesh(ref renderContext, cubeMesh, material, ref cubeWorld, sortOrder);
            }
        }
    }
}
