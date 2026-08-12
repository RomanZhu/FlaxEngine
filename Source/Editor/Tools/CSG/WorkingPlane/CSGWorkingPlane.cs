// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.WorkingPlane
{
    /// <summary>
    /// Immutable-value description of the CSG construction plane and its plane-local grid.
    /// </summary>
    public struct CSGWorkingPlane
    {
        /// <summary>
        /// Stable world-space grid origin on the plane.
        /// </summary>
        public Vector3 Origin;

        /// <summary>
        /// Unit plane normal.
        /// </summary>
        public Vector3 Normal;

        /// <summary>
        /// Unit plane-local horizontal axis.
        /// </summary>
        public Vector3 Tangent;

        /// <summary>
        /// Unit plane-local vertical axis.
        /// </summary>
        public Vector3 Bitangent;

        /// <summary>
        /// Minor grid spacing in world units.
        /// </summary>
        public float Spacing;

        /// <summary>
        /// Number of minor cells between major grid lines.
        /// </summary>
        public int MajorLineInterval;

        /// <summary>
        /// Optional source actor identity.
        /// </summary>
        public Guid SourceActorId;

        /// <summary>
        /// Optional source component index, or -1.
        /// </summary>
        public int SourceComponentIndex;

        /// <summary>
        /// Whether this plane was derived from visible scene surface geometry.
        /// </summary>
        public bool IsSurfaceDerived;

        /// <summary>
        /// Whether the plane is explicitly locked.
        /// </summary>
        public bool IsLocked;

        /// <summary>
        /// Whether the plane contains a usable orthonormal basis.
        /// </summary>
        public bool IsValid;

        /// <summary>
        /// Gets a default XZ world plane.
        /// </summary>
        public static CSGWorkingPlane World(float spacing, int majorLineInterval = 10)
        {
            return new CSGWorkingPlane
            {
                Origin = Vector3.Zero,
                Normal = Vector3.Up,
                Tangent = Vector3.Right,
                Bitangent = Vector3.Backward,
                Spacing = Mathf.Max(spacing, 0.0001f),
                MajorLineInterval = Mathf.Max(majorLineInterval, 1),
                SourceActorId = Guid.Empty,
                SourceComponentIndex = -1,
                IsSurfaceDerived = false,
                IsValid = true,
            };
        }

        /// <summary>
        /// Converts a world-space point into plane-local coordinates.
        /// </summary>
        public Float2 ToPlane(Vector3 point)
        {
            var offset = point - Origin;
            return new Float2((float)Vector3.Dot(offset, Tangent), (float)Vector3.Dot(offset, Bitangent));
        }

        /// <summary>
        /// Converts plane-local coordinates into a world-space point.
        /// </summary>
        public Vector3 ToWorld(Float2 point)
        {
            return Origin + Tangent * point.X + Bitangent * point.Y;
        }

        /// <summary>
        /// Returns whether this plane describes the same stable grid as another plane.
        /// </summary>
        public bool NearlyEquals(ref CSGWorkingPlane other)
        {
            return IsValid == other.IsValid &&
                   Vector3.NearEqual(Origin, other.Origin) &&
                   Vector3.NearEqual(Normal, other.Normal) &&
                   Vector3.NearEqual(Tangent, other.Tangent) &&
                   Vector3.NearEqual(Bitangent, other.Bitangent) &&
                   Mathf.NearEqual(Spacing, other.Spacing) &&
                   MajorLineInterval == other.MajorLineInterval &&
                   SourceActorId == other.SourceActorId &&
                   SourceComponentIndex == other.SourceComponentIndex &&
                   IsSurfaceDerived == other.IsSurfaceDerived &&
                   IsLocked == other.IsLocked;
        }
    }
}
