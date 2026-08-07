// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        /// <summary>
        /// Gets the selection bounds center point (in world space).
        /// </summary>
        /// <returns>Center point or <see cref="Vector3.Zero"/> if no object selected.</returns>
        public Vector3 GetSelectionCenter()
        {
            int count = SelectionCount;

            // Check if there is no objects selected at all
            if (count == 0)
                return Vector3.Zero;

            GetSelectedObjectsBounds(out var bounds, out _);
            if (IsValidBounds(ref bounds))
                return bounds.Center;

            // Fallback for gizmos that cannot provide bounds.
            Vector3 center = Vector3.Zero;
            for (int i = 0; i < count; i++)
                center += GetSelectedTransform(i).Translation;

            return center / count;
        }

        private static bool IsValidBounds(ref BoundingBox bounds)
        {
            return bounds.Minimum.X <= bounds.Maximum.X && bounds.Minimum.Y <= bounds.Maximum.Y && bounds.Minimum.Z <= bounds.Maximum.Z;
        }

        private bool IntersectsRotateCircle(Vector3 normal, ref Ray ray, out Real distance)
        {
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref ray, out distance))
                return false;
            Vector3 hitPoint = ray.Position + ray.Direction * distance;
            Real distanceNormalized = hitPoint.Length / RotateRadiusRaw;
            if (!Mathf.IsInRange(distanceNormalized, 0.9f, 1.1f))
                return false;
            return Vector3.Dot(hitPoint, GetRotateFrontDirectionLocal(normal)) >= 0.0f;
        }

        private bool IntersectsRotateTrackball(ref Ray ray, out Real distance)
        {
            var sphere = new BoundingSphere(Vector3.Zero, _rotationTrackballRadiusRaw);
            return sphere.Intersects(ref ray, out distance);
        }

        private bool IntersectsRotateScreenRing(ref Ray ray, out Real distance)
        {
            Vector3 normal = GetRotateToViewLocal();
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref ray, out distance))
                return false;

            Vector3 hitPoint = ray.Position + ray.Direction * distance;
            Real ringDistance = hitPoint.Length;
            return Mathf.IsInRange((float)ringDistance, _rotationScreenRingRadiusRaw - _rotationScreenRingThicknessRaw * 3.0f, _rotationScreenRingRadiusRaw + _rotationScreenRingThicknessRaw * 3.0f);
        }

        private Ray GetLocalMouseRay()
        {
            var transform = _gizmoWorld;
            return GetLocalMouseRay(ref transform);
        }

        private Ray GetLocalMouseRay(ref Transform transform)
        {
            Ray ray = Owner.MouseRay;
            Ray localRay;
            transform.WorldToLocalVector(ref ray.Direction, out localRay.Direction);
            transform.WorldToLocal(ref ray.Position, out localRay.Position);
            localRay.Direction.Normalize();
            return localRay;
        }

        private bool GetRotateTrackballPointLocal(out Vector3 point)
        {
            var trackballTransform = GetRotationTrackballTransform();
            Ray localRay = GetLocalMouseRay(ref trackballTransform);
            var sphere = new BoundingSphere(Vector3.Zero, _rotationTrackballRadiusRaw);
            if (sphere.Intersects(ref localRay, out Real distance))
            {
                point = localRay.GetPoint(distance);
                if (point.LengthSquared > 0.0001f)
                    point = Vector3.Normalize(point) * _rotationTrackballRadiusRaw;
                return true;
            }

            Vector3 viewNormal = GetRotateToViewLocal(ref trackballTransform);
            var plane = new Plane(Vector3.Zero, viewNormal);
            if (!plane.Intersects(ref localRay, out distance))
            {
                point = Vector3.Zero;
                return false;
            }

            Vector3 planePoint = localRay.GetPoint(distance);
            Vector3 planeOffset = planePoint - viewNormal * Vector3.Dot(planePoint, viewNormal);
            Real radiusSquared = _rotationTrackballRadiusRaw * _rotationTrackballRadiusRaw;
            Real planeLengthSquared = planeOffset.LengthSquared;
            if (planeLengthSquared >= radiusSquared)
            {
                point = Vector3.Normalize(planeOffset) * _rotationTrackballRadiusRaw;
                return true;
            }

            point = planeOffset + viewNormal * (Real)System.Math.Sqrt(radiusSquared - planeLengthSquared);
            return true;
        }

        private bool GetRotateRingPointLocal(Axis axis, out Vector3 point)
        {
            var transform = GetRotationTrackballTransform();
            Vector3 normal;
            switch (axis)
            {
            case Axis.X:
                normal = Vector3.UnitX;
                break;
            case Axis.Y:
                normal = Vector3.UnitY;
                break;
            case Axis.Z:
                normal = Vector3.UnitZ;
                break;
            default:
                point = Vector3.Zero;
                return false;
            }

            Ray localRay = GetLocalMouseRay(ref transform);
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref localRay, out Real distance))
                return GetRotateRingPointFromScreenLocal(ref transform, normal, out point);

            point = localRay.GetPoint(distance);
            point = Vector3.ProjectOnPlane(point, normal);
            if (point.LengthSquared < 0.0001f)
                return GetRotateRingPointFromScreenLocal(ref transform, normal, out point);

            point = Vector3.Normalize(point) * RotateRadiusRaw;
            return true;
        }

        private bool GetRotateRingPointFromScreenLocal(ref Transform transform, Vector3 normal, out Vector3 point)
        {
            Vector3 tangentU = GetRotateFrontDirectionLocal(ref transform, normal);
            Vector3 tangentV = Vector3.Cross(normal, tangentU);
            if (tangentV.LengthSquared < 0.0001f)
            {
                point = tangentU * RotateRadiusRaw;
                return true;
            }
            tangentV.Normalize();

            Owner.Viewport.ProjectPoint(transform.Translation, out var centerScreen);
            Owner.Viewport.ProjectPoint(transform.LocalToWorld(tangentU * RotateRadiusRaw), out var tangentUScreenPoint);
            Owner.Viewport.ProjectPoint(transform.LocalToWorld(tangentV * RotateRadiusRaw), out var tangentVScreenPoint);

            Float2 tangentUScreen = tangentUScreenPoint - centerScreen;
            Float2 tangentVScreen = tangentVScreenPoint - centerScreen;
            Float2 mouseScreen = Owner.Viewport.ContinuousViewMousePosition - centerScreen;
            float determinant = tangentUScreen.X * tangentVScreen.Y - tangentUScreen.Y * tangentVScreen.X;
            if (Mathf.Abs(determinant) < 0.0001f || mouseScreen.LengthSquared < 0.0001f)
            {
                point = tangentU * RotateRadiusRaw;
                return true;
            }

            float u = (mouseScreen.X * tangentVScreen.Y - mouseScreen.Y * tangentVScreen.X) / determinant;
            float v = (tangentUScreen.X * mouseScreen.Y - tangentUScreen.Y * mouseScreen.X) / determinant;
            point = tangentU * u + tangentV * v;
            if (point.LengthSquared < 0.0001f)
                point = tangentU;
            else
                point.Normalize();
            point *= RotateRadiusRaw;
            return true;
        }

        private bool GetRotateScreenRingPointLocal(out Vector3 point)
        {
            var transform = GetRotationTrackballTransform();
            Ray localRay = GetLocalMouseRay(ref transform);
            Vector3 normal = GetRotateToViewLocal(ref transform);
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref localRay, out Real distance))
            {
                point = Vector3.ProjectOnPlane(GetRotateFrontDirectionLocal(ref transform, Vector3.UnitY), normal);
                if (point.LengthSquared < 0.0001f)
                    point = Vector3.ProjectOnPlane(Vector3.UnitX, normal);
                point.Normalize();
                point *= _rotationScreenRingRadiusRaw;
                return true;
            }

            point = localRay.GetPoint(distance);
            point = Vector3.ProjectOnPlane(point, normal);
            if (point.LengthSquared < 0.0001f)
            {
                point = Vector3.ProjectOnPlane(Vector3.UnitX, normal);
                if (point.LengthSquared < 0.0001f)
                    point = Vector3.ProjectOnPlane(Vector3.UnitY, normal);
            }
            point.Normalize();
            point *= _rotationScreenRingRadiusRaw;
            return true;
        }

        private static float DistancePointToSegmentSquared(Float2 point, Float2 start, Float2 end, out float segmentAmount)
        {
            Float2 segment = end - start;
            float lengthSquared = segment.LengthSquared;
            if (lengthSquared < 0.0001f)
            {
                segmentAmount = 0.0f;
                return (point - start).LengthSquared;
            }

            segmentAmount = Mathf.Clamp(Float2.Dot(point - start, segment) / lengthSquared, 0.0f, 1.0f);
            Float2 closest = start + segment * segmentAmount;
            return (point - closest).LengthSquared;
        }

        private bool TryGetAxisHandleScreenDepth(Axis axis, out Real depth)
        {
            depth = Real.MaxValue;
            if (!TryGetRotationAxisLocal(axis, out var localDirection))
                return false;

            float headLength = _activeMode == Mode.Scale ? AxisScaleCubeSize * 0.5f : AxisArrowHeadLength;
            Vector3 worldStart = _gizmoWorld.LocalToWorld(localDirection * AxisVisualStart);
            Vector3 worldHeadBase = _gizmoWorld.LocalToWorld(localDirection * (AxisLength - headLength));
            Vector3 worldEnd = _gizmoWorld.LocalToWorld(localDirection * AxisLength);
            if (!TryProjectGizmoPoint(worldStart, out var screenStart) ||
                !TryProjectGizmoPoint(worldHeadBase, out var screenHeadBase) ||
                !TryProjectGizmoPoint(worldEnd, out var screenEnd))
                return false;

            const float shaftAcquisitionRadius = 8.0f;
            const float headAcquisitionRadius = 10.0f;
            Float2 cursor = Owner.Viewport.ContinuousViewMousePosition;
            float shaftDistanceSquared = DistancePointToSegmentSquared(cursor, screenStart, screenHeadBase, out var shaftAmount);
            float headDistanceSquared = DistancePointToSegmentSquared(cursor, screenHeadBase, screenEnd, out var headAmount);
            bool hitsShaft = shaftDistanceSquared <= shaftAcquisitionRadius * shaftAcquisitionRadius;
            bool hitsHead = headDistanceSquared <= headAcquisitionRadius * headAcquisitionRadius;
            if (!hitsShaft && !hitsHead)
                return false;

            Vector3 worldPoint = hitsHead && (!hitsShaft || headDistanceSquared < shaftDistanceSquared)
                ? worldHeadBase + (worldEnd - worldHeadBase) * headAmount
                : worldStart + (worldHeadBase - worldStart) * shaftAmount;
            depth = Vector3.Dot(worldPoint - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
            return depth >= 0.0f;
        }

        private static float ScreenTriangleSign(Float2 point, Float2 a, Float2 b)
        {
            return (point.X - b.X) * (a.Y - b.Y) - (a.X - b.X) * (point.Y - b.Y);
        }

        private static bool IsPointInScreenTriangle(Float2 point, Float2 a, Float2 b, Float2 c)
        {
            float d1 = ScreenTriangleSign(point, a, b);
            float d2 = ScreenTriangleSign(point, b, c);
            float d3 = ScreenTriangleSign(point, c, a);
            bool hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
            bool hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
            return !(hasNegative && hasPositive);
        }

        private bool TryGetPlaneHandleScreenDepth(Axis axis, ref Ray localRay, out Real depth)
        {
            depth = Real.MaxValue;
            if (!TryGetPlaneHandleWorldCorners(axis, out var world0, out var world1, out var world2, out var world3) ||
                !TryProjectGizmoPoint(world0, out var screen0) ||
                !TryProjectGizmoPoint(world1, out var screen1) ||
                !TryProjectGizmoPoint(world2, out var screen2) ||
                !TryProjectGizmoPoint(world3, out var screen3))
                return false;

            Float2 cursor = Owner.Viewport.ContinuousViewMousePosition;
            bool contains = IsPointInScreenTriangle(cursor, screen0, screen1, screen3) ||
                            IsPointInScreenTriangle(cursor, screen0, screen3, screen2);
            if (!contains)
            {
                const float edgeRadius = 3.0f;
                float edgeRadiusSquared = edgeRadius * edgeRadius;
                contains = DistancePointToSegmentSquared(cursor, screen0, screen1, out _) <= edgeRadiusSquared ||
                           DistancePointToSegmentSquared(cursor, screen1, screen3, out _) <= edgeRadiusSquared ||
                           DistancePointToSegmentSquared(cursor, screen3, screen2, out _) <= edgeRadiusSquared ||
                           DistancePointToSegmentSquared(cursor, screen2, screen0, out _) <= edgeRadiusSquared;
            }
            if (!contains)
                return false;

            Vector3 normal;
            switch (axis)
            {
            case Axis.XY:
                normal = Vector3.Backward;
                break;
            case Axis.YZ:
                normal = Vector3.Left;
                break;
            case Axis.ZX:
                normal = Vector3.Down;
                break;
            default:
                return false;
            }

            var plane = new Plane(Vector3.Zero, normal);
            if (!localRay.Intersects(ref plane, out Real intersection))
                return false;
            Vector3 worldPoint = _gizmoWorld.LocalToWorld(localRay.GetPoint(intersection));
            depth = Vector3.Dot(worldPoint - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
            return depth >= 0.0f;
        }

        private bool TryGetCenterHandleScreenDepth(out Real depth)
        {
            depth = Real.MaxValue;
            if (!TryProjectGizmoPoint(Position, out var center))
                return false;
            const float acquisitionRadius = 12.0f;
            if ((Owner.Viewport.ContinuousViewMousePosition - center).LengthSquared > acquisitionRadius * acquisitionRadius)
                return false;
            depth = Vector3.Dot(Position - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
            return depth >= 0.0f;
        }

        private void SelectTranslateScaleHandle(ref Ray localRay)
        {
            Real closestDepth = Real.MaxValue;
            _activeAxis = Axis.None;

            // The center marker is a distinct uniform/free-transform control.
            // Its motor target intentionally owns the inner circle so nearby
            // axis capsules cannot make it inaccessible in a projected local
            // basis.
            if (TryGetCenterHandleScreenDepth(out var depth))
            {
                _activeAxis = Axis.Center;
                return;
            }

            if (TryGetAxisHandleScreenDepth(Axis.X, out depth) && depth < closestDepth)
            {
                _activeAxis = Axis.X;
                closestDepth = depth;
            }
            if (TryGetAxisHandleScreenDepth(Axis.Y, out depth) && depth < closestDepth)
            {
                _activeAxis = Axis.Y;
                closestDepth = depth;
            }
            if (TryGetAxisHandleScreenDepth(Axis.Z, out depth) && depth < closestDepth)
            {
                _activeAxis = Axis.Z;
                closestDepth = depth;
            }

            // Planes are tested against their rendered quadrilaterals. On an
            // exact depth tie the plane wins because it is the filled surface
            // directly beneath the pointer.
            if (TryGetPlaneHandleScreenDepth(Axis.XY, ref localRay, out depth) && depth <= closestDepth)
            {
                _activeAxis = Axis.XY;
                closestDepth = depth;
            }
            if (TryGetPlaneHandleScreenDepth(Axis.ZX, ref localRay, out depth) && depth <= closestDepth)
            {
                _activeAxis = Axis.ZX;
                closestDepth = depth;
            }
            if (TryGetPlaneHandleScreenDepth(Axis.YZ, ref localRay, out depth) && depth <= closestDepth)
            {
                _activeAxis = Axis.YZ;
                closestDepth = depth;
            }
        }

        private void SelectAxis()
        {
            // Once pressed, the semantic handle owns the transaction until commit or cancel.
            // Never reacquire a different handle while the pointer is dragging.
            if (HasActiveTransaction)
            {
                _activeAxis = _latchedHandle.Axis;
                return;
            }

            // Acquisition is intentionally based on cached projected semantic
            // targets. The selected handle is then consumed by the existing
            // world-space solvers during the active transaction.
            SelectSemanticAxis();
        }
    }
}
