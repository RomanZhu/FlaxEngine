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
            Ray ray = Owner.MouseRay;
            Ray localRay;
            _gizmoWorld.WorldToLocalVector(ref ray.Direction, out localRay.Direction);
            _gizmoWorld.WorldToLocal(ref ray.Position, out localRay.Position);
            localRay.Direction.Normalize();
            return localRay;
        }

        private bool GetRotateTrackballPointLocal(out Vector3 point)
        {
            Ray localRay = GetLocalMouseRay();
            var sphere = new BoundingSphere(Vector3.Zero, _rotationTrackballRadiusRaw);
            if (sphere.Intersects(ref localRay, out Real distance))
            {
                point = localRay.GetPoint(distance);
                if (point.LengthSquared > 0.0001f)
                    point = Vector3.Normalize(point) * _rotationTrackballRadiusRaw;
                return true;
            }

            Vector3 viewNormal = GetRotateToViewLocal();
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

            Ray localRay = GetLocalMouseRay();
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref localRay, out Real distance))
                return GetRotateRingPointFromScreenLocal(normal, out point);

            point = localRay.GetPoint(distance);
            point = Vector3.ProjectOnPlane(point, normal);
            if (point.LengthSquared < 0.0001f)
                return GetRotateRingPointFromScreenLocal(normal, out point);

            point = Vector3.Normalize(point) * RotateRadiusRaw;
            return true;
        }

        private bool GetRotateRingPointFromScreenLocal(Vector3 normal, out Vector3 point)
        {
            Vector3 tangentU = GetRotateFrontDirectionLocal(normal);
            Vector3 tangentV = Vector3.Cross(normal, tangentU);
            if (tangentV.LengthSquared < 0.0001f)
            {
                point = tangentU * RotateRadiusRaw;
                return true;
            }
            tangentV.Normalize();

            Owner.Viewport.ProjectPoint(Position, out var centerScreen);
            Owner.Viewport.ProjectPoint(_gizmoWorld.LocalToWorld(tangentU * RotateRadiusRaw), out var tangentUScreenPoint);
            Owner.Viewport.ProjectPoint(_gizmoWorld.LocalToWorld(tangentV * RotateRadiusRaw), out var tangentVScreenPoint);

            Float2 tangentUScreen = tangentUScreenPoint - centerScreen;
            Float2 tangentVScreen = tangentVScreenPoint - centerScreen;
            Float2 mouseScreen = Owner.Viewport.ViewMousePosition - centerScreen;
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
            Ray localRay = GetLocalMouseRay();
            Vector3 normal = GetRotateToViewLocal();
            var plane = new Plane(Vector3.Zero, normal);
            if (!plane.Intersects(ref localRay, out Real distance))
            {
                point = Vector3.ProjectOnPlane(GetRotateFrontDirectionLocal(Vector3.UnitY), normal);
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

        private void SelectAxis()
        {
            // Get mouse ray
            Ray localRay = GetLocalMouseRay();

            // Find gizmo collisions with mouse
            Real closestIntersection = Real.MaxValue;
            Real intersection;
            _activeAxis = Axis.None;
            switch (_activeMode)
            {
            case Mode.Translate:
            {
                // Axis boxes collision
                if (XAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.X;
                    closestIntersection = intersection;
                }

                if (YAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Y;
                    closestIntersection = intersection;
                }

                if (ZAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Z;
                    closestIntersection = intersection;
                }

                // Quad planes collision
                if (closestIntersection >= float.MaxValue)
                    closestIntersection = float.MinValue;
                if (XYBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.XY;
                    closestIntersection = intersection;
                }

                if (XZBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.ZX;
                    closestIntersection = intersection;
                }
                if (YZBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.YZ;
                    closestIntersection = intersection;
                }

                // Center
                if (CenterBoxRaw.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.Center;
                    closestIntersection = intersection;
                }

                break;
            }
            case Mode.Rotate:
            {
                // Circles
                if (IntersectsRotateCircle(Vector3.UnitX, ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.X;
                    closestIntersection = intersection;
                }
                if (IntersectsRotateCircle(Vector3.UnitY, ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Y;
                    closestIntersection = intersection;
                }
                if (IntersectsRotateCircle(Vector3.UnitZ, ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Z;
                    closestIntersection = intersection;
                }
                if (IntersectsRotateScreenRing(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Screen;
                    closestIntersection = intersection;
                }
                if (_activeAxis == Axis.None && IntersectsRotateTrackball(ref localRay, out intersection))
                {
                    _activeAxis = Axis.Center;
                    closestIntersection = intersection;
                }

                break;
            }
            case Mode.Scale:
            {
                // Boxes collision
                if (XAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.X;
                    closestIntersection = intersection;
                }
                if (YAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Y;
                    closestIntersection = intersection;
                }
                if (ZAxisBox.Intersects(ref localRay, out intersection) && intersection < closestIntersection)
                {
                    _activeAxis = Axis.Z;
                    closestIntersection = intersection;
                }

                // Quad planes collision
                if (closestIntersection >= float.MaxValue)
                    closestIntersection = float.MinValue;

                if (XYBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.XY;
                    closestIntersection = intersection;
                }
                if (XZBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.ZX;
                    closestIntersection = intersection;
                }
                if (YZBox.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.YZ;
                    closestIntersection = intersection;
                }

                // Center
                if (CenterBoxRaw.Intersects(ref localRay, out intersection) && intersection > closestIntersection)
                {
                    _activeAxis = Axis.Center;
                    closestIntersection = intersection;
                }

                break;
            }
            }
        }
    }
}
