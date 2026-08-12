// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Placement
{
    /// <summary>
    /// Operation inferred from the signed extrusion of a CSG face.
    /// </summary>
    public enum CSGBoxOperationInference
    {
        /// <summary>No operation can be inferred safely.</summary>
        None,
        /// <summary>The extrusion grows out of the source face.</summary>
        Additive,
        /// <summary>The extrusion cuts into the source face.</summary>
        Subtractive,
    }

    /// <summary>
    /// Solved oriented-box placement in world space.
    /// </summary>
    public struct CSGBoxPlacement
    {
        /// <summary>World-space box center.</summary>
        public Vector3 Center;
        /// <summary>World-space box orientation.</summary>
        public Quaternion Orientation;
        /// <summary>Positive local X/Y/Z size.</summary>
        public Vector3 Size;
        /// <summary>Signed extrusion along the working-plane normal.</summary>
        public float SignedHeight;
        /// <summary>Operation inferred from the source face and signed extrusion.</summary>
        public CSGBoxOperationInference InferredOperation;

        /// <summary>Creates the preview bounds.</summary>
        public OrientedBoundingBox ToOrientedBox()
        {
            var box = new OrientedBoundingBox(-Size * 0.5f, Size * 0.5f);
            box.Transformation = new Transform(Center, Orientation);
            return box;
        }
    }

    /// <summary>
    /// Pure plane-local math used by staged CSG box placement.
    /// </summary>
    public static class CSGBoxPlacementSolver
    {
        /// <summary>Smallest accepted brush dimension.</summary>
        public const float MinimumDimension = 0.001f;

        /// <summary>
        /// Applies square-footprint and symmetric-footprint constraints and returns ordered bounds.
        /// </summary>
        public static void SolveFootprint(Float2 anchor, Float2 pointer, bool square, bool symmetric, out Float2 minimum, out Float2 maximum)
        {
            var delta = pointer - anchor;
            if (square)
            {
                float extent = Mathf.Max(Mathf.Abs(delta.X), Mathf.Abs(delta.Y));
                delta.X = delta.X < 0.0f ? -extent : extent;
                delta.Y = delta.Y < 0.0f ? -extent : extent;
            }

            if (symmetric)
            {
                var extent = new Float2(Mathf.Abs(delta.X), Mathf.Abs(delta.Y));
                minimum = anchor - extent;
                maximum = anchor + extent;
            }
            else
            {
                var end = anchor + delta;
                minimum = Float2.Min(anchor, end);
                maximum = Float2.Max(anchor, end);
            }
        }

        /// <summary>
        /// Solves a positive-size oriented box from a footprint and signed extrusion.
        /// </summary>
        public static bool TrySolve(ref CSGWorkingPlane plane, Float2 anchor, Float2 pointer, float signedHeight, bool squareFootprint, bool symmetricFootprint, bool symmetricExtrusion, out CSGBoxPlacement placement)
        {
            placement = default;
            if (!plane.IsValid || !IsFinite(signedHeight))
                return false;

            SolveFootprint(anchor, pointer, squareFootprint, symmetricFootprint, out var minimum, out var maximum);
            float width = maximum.X - minimum.X;
            float depth = maximum.Y - minimum.Y;
            float height = Mathf.Abs(signedHeight) * (symmetricExtrusion ? 2.0f : 1.0f);
            if (!IsFinite(width) || !IsFinite(depth) || width <= MinimumDimension || depth <= MinimumDimension || height <= MinimumDimension)
                return false;

            var planeCenter = plane.ToWorld((minimum + maximum) * 0.5f);
            var center = symmetricExtrusion ? planeCenter : planeCenter + plane.Normal * (signedHeight * 0.5f);
            placement = new CSGBoxPlacement
            {
                Center = center,
                Orientation = Quaternion.LookRotation(-plane.Bitangent, plane.Normal),
                Size = new Vector3(width, height, depth),
                SignedHeight = signedHeight,
                InferredOperation = InferOperation(ref plane, signedHeight),
            };
            return true;
        }

        /// <summary>
        /// Projects a pointer ray onto a camera-facing plane that contains the extrusion axis.
        /// </summary>
        public static bool TrySolveHeight(ref CSGWorkingPlane plane, Vector3 footprintCenter, ref Ray pointerRay, Vector3 viewDirection, out float signedHeight)
        {
            signedHeight = 0.0f;
            if (!plane.IsValid)
                return false;

            var dragPlaneNormal = viewDirection - plane.Normal * Vector3.Dot(viewDirection, plane.Normal);
            if (dragPlaneNormal.LengthSquared < 0.000001f)
                dragPlaneNormal = plane.Tangent;
            dragPlaneNormal.Normalize();
            var denominator = Vector3.Dot(pointerRay.Direction, dragPlaneNormal);
            if (Mathf.Abs((float)denominator) < 0.00001f)
                return false;
            var distance = Vector3.Dot(footprintCenter - pointerRay.Position, dragPlaneNormal) / denominator;
            if (distance < 0.0)
                return false;
            var hit = pointerRay.Position + pointerRay.Direction * distance;
            signedHeight = (float)Vector3.Dot(hit - footprintCenter, plane.Normal);
            return IsFinite(signedHeight);
        }

        /// <summary>Rounds a signed dimension to an exact positive increment.</summary>
        public static float SnapDimension(float value, float increment)
        {
            if (!IsFinite(value) || !IsFinite(increment) || increment <= MinimumDimension)
                return value;
            return Mathf.Round(value / increment) * increment;
        }

        /// <summary>Infers an operation only when drawing from visible surface geometry.</summary>
        public static CSGBoxOperationInference InferOperation(ref CSGWorkingPlane plane, float signedHeight)
        {
            if (!plane.IsSurfaceDerived || Mathf.Abs(signedHeight) <= MinimumDimension)
                return CSGBoxOperationInference.None;
            return signedHeight > 0.0f ? CSGBoxOperationInference.Additive : CSGBoxOperationInference.Subtractive;
        }

        private static bool IsFinite(float value)
        {
            return !float.IsNaN(value) && !float.IsInfinity(value);
        }
    }
}
