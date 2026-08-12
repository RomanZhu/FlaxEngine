// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.WorkingPlane
{
    /// <summary>
    /// Owns default, hovered, locked, and transaction-frozen CSG working planes.
    /// </summary>
    public sealed class CSGWorkingPlaneService
    {
        private CSGWorkingPlane _defaultPlane = CSGWorkingPlane.World(10.0f);
        private CSGWorkingPlane _hoverPlane;
        private CSGWorkingPlane _lockedPlane;
        private CSGWorkingPlane _frozenPlane;
        private bool _hasHover;
        private bool _isLocked;
        private bool _isFrozen;

        /// <summary>
        /// Gets the plane used by the active authoring operation.
        /// </summary>
        public CSGWorkingPlane ActivePlane => _isFrozen ? _frozenPlane : _isLocked ? _lockedPlane : _hasHover ? _hoverPlane : _defaultPlane;

        /// <summary>
        /// Gets whether a stable surface hover plane is available.
        /// </summary>
        public bool HasHover => _hasHover;

        /// <summary>
        /// Gets whether the current plane is locked.
        /// </summary>
        public bool IsLocked => _isLocked;

        /// <summary>
        /// Gets whether a transaction has frozen its working plane.
        /// </summary>
        public bool IsFrozen => _isFrozen;

        /// <summary>
        /// Resets to the world plane and clears hover, lock, and freeze state.
        /// </summary>
        public void Reset(float spacing)
        {
            _defaultPlane = CSGWorkingPlane.World(spacing);
            _hoverPlane = default;
            _lockedPlane = default;
            _frozenPlane = default;
            _hasHover = false;
            _isLocked = false;
            _isFrozen = false;
        }

        /// <summary>
        /// Updates grid spacing without changing plane identity or orientation.
        /// </summary>
        public void SetSpacing(float spacing)
        {
            spacing = Mathf.Max(spacing, 0.0001f);
            _defaultPlane.Spacing = spacing;
            _hoverPlane.Spacing = spacing;
            _lockedPlane.Spacing = spacing;
            _frozenPlane.Spacing = spacing;
        }

        /// <summary>
        /// Derives and stores a hover plane unless a lock or transaction freeze owns the plane.
        /// </summary>
        public bool TrySetHover(Vector3 hitPoint, Vector3 hitNormal, Vector3 preferredTangent, Ray pointerRay, float spacing, Guid sourceActorId, int sourceComponentIndex, bool isSurfaceDerived = false)
        {
            if (_isLocked || _isFrozen)
                return false;
            float grazing = Mathf.Abs((float)Vector3.Dot(pointerRay.Direction, hitNormal));
            if (grazing < 0.075f || !TryDerive(hitPoint, hitNormal, preferredTangent, spacing, 10, sourceActorId, sourceComponentIndex, out var plane))
            {
                _hasHover = false;
                return false;
            }

            plane.IsSurfaceDerived |= isSurfaceDerived;
            _hoverPlane = plane;
            _hasHover = true;
            return true;
        }

        /// <summary>
        /// Clears the transient hover plane.
        /// </summary>
        public void ClearHover()
        {
            if (!_isLocked && !_isFrozen)
                _hasHover = false;
        }

        /// <summary>
        /// Locks the active hover/default plane or releases the current lock.
        /// </summary>
        public void SetLocked(bool value)
        {
            if (_isLocked == value)
                return;
            if (value)
            {
                _lockedPlane = _hasHover ? _hoverPlane : _defaultPlane;
                _lockedPlane.IsLocked = true;
            }
            _isLocked = value;
        }

        /// <summary>
        /// Explicitly picks and locks the current hover plane.
        /// </summary>
        public bool PickHovered()
        {
            if (!_hasHover || _isFrozen)
                return false;
            _lockedPlane = _hoverPlane;
            _lockedPlane.IsLocked = true;
            _isLocked = true;
            return true;
        }

        /// <summary>
        /// Offsets and locks the active plane along its normal.
        /// </summary>
        public void Offset(float distance)
        {
            if (!_isLocked)
                SetLocked(true);
            _lockedPlane.Origin += _lockedPlane.Normal * distance;
        }

        /// <summary>
        /// Rotates and locks the plane-local grid basis around its normal.
        /// </summary>
        public void Rotate(float angleDegrees)
        {
            if (!_isLocked)
                SetLocked(true);
            float angle = angleDegrees * Mathf.DegreesToRadians;
            float cos = Mathf.Cos(angle);
            float sin = Mathf.Sin(angle);
            var tangent = _lockedPlane.Tangent * cos + _lockedPlane.Bitangent * sin;
            tangent.Normalize();
            _lockedPlane.Tangent = tangent;
            _lockedPlane.Bitangent = Vector3.Cross(_lockedPlane.Normal, tangent);
            _lockedPlane.Bitangent.Normalize();
        }

        /// <summary>
        /// Freezes the current plane for a transaction.
        /// </summary>
        public void Freeze()
        {
            if (_isFrozen)
                return;
            _frozenPlane = ActivePlane;
            _isFrozen = true;
        }

        /// <summary>
        /// Releases a transaction-frozen plane.
        /// </summary>
        public void Unfreeze()
        {
            _isFrozen = false;
        }

        /// <summary>
        /// Intersects a ray with a working plane.
        /// </summary>
        public static bool TryIntersect(ref CSGWorkingPlane plane, ref Ray ray, out Vector3 point)
        {
            float denominator = (float)Vector3.Dot(ray.Direction, plane.Normal);
            if (!plane.IsValid || Mathf.Abs(denominator) < 0.0001f)
            {
                point = Vector3.Zero;
                return false;
            }
            float distance = (float)Vector3.Dot(plane.Origin - ray.Position, plane.Normal) / denominator;
            if (distance < 0.0f)
            {
                point = Vector3.Zero;
                return false;
            }
            point = ray.Position + ray.Direction * distance;
            return true;
        }

        /// <summary>
        /// Aligns the normal coordinate of an axis-aligned plane to the world grid.
        /// Arbitrarily oriented surface planes retain their surface-local origin.
        /// </summary>
        public static void AlignAxisAlignedOriginToGrid(ref CSGWorkingPlane plane, float spacing)
        {
            if (!plane.IsValid || spacing <= 0.0001f)
                return;
            var normal = Vector3.Abs(plane.Normal);
            if (Mathf.Max((float)normal.X, Mathf.Max((float)normal.Y, (float)normal.Z)) < 0.999999f)
                return;
            float distance = (float)Vector3.Dot(plane.Origin, plane.Normal);
            float snappedDistance = Mathf.Round(distance / spacing) * spacing;
            plane.Origin += plane.Normal * (snappedDistance - distance);
        }

        /// <summary>
        /// Derives a stable orthonormal grid basis from a plane hit.
        /// </summary>
        public static bool TryDerive(Vector3 hitPoint, Vector3 hitNormal, Vector3 preferredTangent, float spacing, int majorLineInterval, Guid sourceActorId, int sourceComponentIndex, out CSGWorkingPlane plane)
        {
            plane = default;
            if (hitNormal.LengthSquared < 0.000001f)
                return false;

            var normal = hitNormal;
            normal.Normalize();
            var tangent = ProjectAxis(preferredTangent, normal);
            if (tangent.LengthSquared < 0.000001f)
            {
                tangent = ProjectAxis(Vector3.Right, normal);
                float tangentLength = (float)tangent.LengthSquared;
                var candidate = ProjectAxis(Vector3.Up, normal);
                if ((float)candidate.LengthSquared > tangentLength + 0.000001f)
                {
                    tangent = candidate;
                    tangentLength = (float)tangent.LengthSquared;
                }
                candidate = ProjectAxis(Vector3.Forward, normal);
                if ((float)candidate.LengthSquared > tangentLength + 0.000001f)
                    tangent = candidate;
            }
            if (tangent.LengthSquared < 0.000001f)
                return false;

            tangent.Normalize();
            var bitangent = Vector3.Cross(normal, tangent);
            bitangent.Normalize();
            float planeDistance = (float)Vector3.Dot(hitPoint, normal);
            plane = new CSGWorkingPlane
            {
                Origin = normal * planeDistance,
                Normal = normal,
                Tangent = tangent,
                Bitangent = bitangent,
                Spacing = Mathf.Max(spacing, 0.0001f),
                MajorLineInterval = Mathf.Max(majorLineInterval, 1),
                SourceActorId = sourceActorId,
                SourceComponentIndex = sourceComponentIndex,
                IsSurfaceDerived = sourceActorId != Guid.Empty && sourceComponentIndex >= 0,
                IsValid = true,
            };
            return true;
        }

        private static Vector3 ProjectAxis(Vector3 axis, Vector3 normal)
        {
            if (axis.LengthSquared < 0.000001f)
                return Vector3.Zero;
            return axis - normal * Vector3.Dot(axis, normal);
        }
    }
}
