// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        internal const float ScalePixelsPerDoubling = 120.0f;
        internal const float PrecisionScaleGain = 0.1f;
        internal const float PositiveScaleGain = 0.1f;

        internal static bool TrySolveAxisTranslation(Ray anchorRay, Ray currentRay, Vector3 pivot, Vector3 axis, out Vector3 delta)
        {
            delta = Vector3.Zero;
            if (axis.LengthSquared < 0.00000001f ||
                anchorRay.Direction.LengthSquared < 0.00000001f ||
                currentRay.Direction.LengthSquared < 0.00000001f)
                return false;

            axis.Normalize();
            anchorRay.Direction.Normalize();
            currentRay.Direction.Normalize();
            if (!TryGetClosestAxisParameter(anchorRay, pivot, axis, out Real anchorParameter) ||
                !TryGetClosestAxisParameter(currentRay, pivot, axis, out Real currentParameter))
            {
                // Freeze a plane which contains the axis and faces the anchor ray.
                Vector3 planeNormal = anchorRay.Direction - axis * Vector3.Dot(anchorRay.Direction, axis);
                if (planeNormal.LengthSquared < 0.00000001f)
                    return false;
                planeNormal.Normalize();
                var plane = new Plane(pivot, planeNormal);
                if (!anchorRay.Intersects(ref plane, out Real anchorDistance) || anchorDistance < 0.0f ||
                    !currentRay.Intersects(ref plane, out Real currentDistance) || currentDistance < 0.0f)
                    return false;
                Vector3 anchorPoint = anchorRay.GetPoint(anchorDistance);
                Vector3 currentPoint = currentRay.GetPoint(currentDistance);
                delta = axis * Vector3.Dot(currentPoint - anchorPoint, axis);
                return IsFiniteMath(delta);
            }

            delta = axis * (currentParameter - anchorParameter);
            return IsFiniteMath(delta);
        }

        private static bool TryGetClosestAxisParameter(Ray ray, Vector3 pivot, Vector3 axis, out Real parameter)
        {
            Vector3 w = pivot - ray.Position;
            Real b = Vector3.Dot(axis, ray.Direction);
            Real denominator = 1.0 - b * b;
            if (Math.Abs(denominator) < 0.0001)
            {
                parameter = 0.0f;
                return false;
            }
            Real d = Vector3.Dot(axis, w);
            Real e = Vector3.Dot(ray.Direction, w);
            parameter = (b * e - d) / denominator;
            return !Real.IsNaN(parameter) && !Real.IsInfinity(parameter);
        }

        internal static bool TrySolvePlaneTranslation(Ray anchorRay, Ray currentRay, Plane plane, out Vector3 delta)
        {
            delta = Vector3.Zero;
            if (!anchorRay.Intersects(ref plane, out Real anchorDistance) || anchorDistance < 0.0f ||
                !currentRay.Intersects(ref plane, out Real currentDistance) || currentDistance < 0.0f)
                return false;
            delta = currentRay.GetPoint(currentDistance) - anchorRay.GetPoint(anchorDistance);
            return IsFiniteMath(delta);
        }

        internal static float SolveExponentialScaleFactor(float projectedDeltaPixels, float pixelsPerDoubling, float gain = 1.0f)
        {
            pixelsPerDoubling = Mathf.Max(pixelsPerDoubling, 1.0f);
            gain = Mathf.Max(gain, 0.0001f);
            double exponent = projectedDeltaPixels * gain * Math.Log(2.0) / pixelsPerDoubling;
            exponent = Math.Max(Math.Log(0.0001), Math.Min(exponent, Math.Log(99_999_999.0)));
            return (float)Math.Exp(exponent);
        }

        internal static float SolvePointerScaleFactor(float projectedDeltaPixels, float gain = 1.0f)
        {
            // Growing scale needs substantially more pointer travel than
            // shrinking scale to avoid enormous factors near a screen edge.
            // This remains anchor-based: returning to zero pointer displacement
            // still returns exactly to factor one.
            if (projectedDeltaPixels > 0.0f)
                gain *= PositiveScaleGain;
            return SolveExponentialScaleFactor(projectedDeltaPixels, ScalePixelsPerDoubling, gain);
        }

        internal static Vector3 MultiplyScaleFactors(Vector3 anchorFactors, Vector3 relativeFactors)
        {
            return new Vector3(
                anchorFactors.X * relativeFactors.X,
                anchorFactors.Y * relativeFactors.Y,
                anchorFactors.Z * relativeFactors.Z);
        }

        internal static Real GetSignedAngleFromAnchor(Vector3 anchor, Vector3 current, Vector3 axis)
        {
            if (anchor.LengthSquared < 0.00000001f || current.LengthSquared < 0.00000001f || axis.LengthSquared < 0.00000001f)
                return 0.0f;
            anchor.Normalize();
            current.Normalize();
            axis.Normalize();
            Real sin = Vector3.Dot(axis, Vector3.Cross(anchor, current));
            Real cos = Math.Max(-1.0, Math.Min(1.0, Vector3.Dot(anchor, current)));
            return Math.Atan2(sin, cos);
        }

        internal static float UnwrapAngle(float previousUnwrapped, float previousWrapped, float currentWrapped)
        {
            float step = currentWrapped - previousWrapped;
            if (step > Mathf.Pi)
                step -= Mathf.TwoPi;
            else if (step < -Mathf.Pi)
                step += Mathf.TwoPi;
            return previousUnwrapped + step;
        }

        internal static Quaternion SolveArcballRotation(Vector3 anchorPoint, Vector3 currentPoint, Quaternion basis, float angleSnapRadians = 0.0f, float gain = 1.0f)
        {
            if (anchorPoint.LengthSquared < 0.00000001f || currentPoint.LengthSquared < 0.00000001f)
                return Quaternion.Identity;
            anchorPoint.Normalize();
            currentPoint.Normalize();
            Vector3 axisLocal = Vector3.Cross(anchorPoint, currentPoint);
            Real axisLength = axisLocal.Length;
            Real dot = Math.Max(-1.0, Math.Min(1.0, Vector3.Dot(anchorPoint, currentPoint)));
            Real angle;
            if (axisLength < 0.00000001f)
            {
                if (dot > 0.0f)
                    return Quaternion.Identity;
                axisLocal = Vector3.Cross(anchorPoint, Vector3.Up);
                if (axisLocal.LengthSquared < 0.00000001f)
                    axisLocal = Vector3.Cross(anchorPoint, Vector3.Right);
                axisLength = axisLocal.Length;
                angle = Math.PI;
            }
            else
            {
                angle = Math.Atan2(axisLength, dot);
            }
            angle *= Mathf.Max(gain, 0.0001f);
            if (angleSnapRadians > Mathf.Epsilon)
                angle = Math.Round(angle / angleSnapRadians) * angleSnapRadians;
            axisLocal /= axisLength;
            Vector3 axisWorld = axisLocal * basis;
            Float3 rotationAxis = axisWorld;
            rotationAxis.Normalize();
            Quaternion.RotationAxis(ref rotationAxis, (float)angle, out var result);
            Quaternion.Normalize(ref result, out var normalized);
            return normalized;
        }

        internal static Vector3 ScalePositionAroundPivot(Vector3 position, Vector3 pivot, Quaternion basis, Vector3 factors)
        {
            Quaternion inverseBasis = Quaternion.Invert(basis);
            Vector3 localOffset = (position - pivot) * inverseBasis;
            localOffset = new Vector3(localOffset.X * factors.X, localOffset.Y * factors.Y, localOffset.Z * factors.Z);
            return pivot + localOffset * basis;
        }

        private static bool IsFiniteMath(Vector3 value)
        {
            return !Real.IsNaN(value.X) && !Real.IsInfinity(value.X) &&
                   !Real.IsNaN(value.Y) && !Real.IsInfinity(value.Y) &&
                   !Real.IsNaN(value.Z) && !Real.IsInfinity(value.Z);
        }
    }
}
