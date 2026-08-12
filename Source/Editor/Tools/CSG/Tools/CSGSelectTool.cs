// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Tools.CSG.WorkingPlane;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Tools
{
    /// <summary>Lifecycle of a direct Select/Place brush drag.</summary>
    public enum CSGSelectDragStage
    {
        /// <summary>No pointer interaction is pending.</summary>
        Idle,
        /// <summary>A brush was pressed but the drag threshold has not been crossed.</summary>
        Armed,
        /// <summary>The selected brush set is being moved.</summary>
        Dragging,
    }

    /// <summary>
    /// Solves thresholded, plane-local, rigid movement for a set of CSG box brushes.
    /// Scene selection, snapping, transactions, and duplication remain owned by the authoring gizmo.
    /// </summary>
    public sealed class CSGSelectTool
    {
        /// <summary>Logical pixels required before a click becomes a direct drag.</summary>
        public const float DragThreshold = 4.0f;

        private readonly List<BoxBrush> _brushes = new List<BoxBrush>(8);
        private readonly List<Transform> _initialTransforms = new List<Transform>(8);
        private readonly List<Vector3> _gridReferencePoints = new List<Vector3>(64);
        private readonly List<Vector3> _surfaceGridReferencePoints = new List<Vector3>(64);
        private readonly Vector3[] _brushCorners = new Vector3[8];
        private CSGWorkingPlane _plane;
        private Float2 _pointerDown;
        private bool _duplicateConsumed;
        private bool _wasAligned;
        private Quaternion _surfaceRotationDelta = Quaternion.Identity;

        /// <summary>Gets the current interaction stage.</summary>
        public CSGSelectDragStage Stage { get; private set; }

        /// <summary>Gets whether a click or drag currently owns the pointer.</summary>
        public bool IsInteracting => Stage != CSGSelectDragStage.Idle;

        /// <summary>Gets the frozen working plane.</summary>
        public CSGWorkingPlane Plane => _plane;

        /// <summary>Gets the world-space point initially grabbed by the pointer.</summary>
        public Vector3 Anchor { get; private set; }

        /// <summary>Gets the current snapped or unsnapped target point.</summary>
        public Vector3 Target { get; private set; }

        /// <summary>Gets the current rigid world-space translation.</summary>
        public Vector3 Delta { get; private set; }

        /// <summary>Gets the current plane-local translation.</summary>
        public Float2 PlaneDelta { get; private set; }

        /// <summary>Gets the brushes participating in the drag.</summary>
        public IReadOnlyList<BoxBrush> Brushes => _brushes;

        /// <summary>Creates the deterministic world-XZ plane used by direct object movement.</summary>
        public static CSGWorkingPlane CreateHorizontalDragPlane(float spacing, Vector3 anchor)
        {
            var plane = CSGWorkingPlane.World(spacing);
            plane.Origin = new Vector3(0.0f, anchor.Y, 0.0f);
            return plane;
        }

        /// <summary>Arms a direct drag without mutating scene objects.</summary>
        public bool Arm(ref CSGWorkingPlane plane, Float2 pointer, Vector3 anchor, IEnumerable<BoxBrush> brushes)
        {
            Reset();
            if (!plane.IsValid || brushes == null)
                return false;

            foreach (var brush in brushes)
            {
                if (brush == null || _brushes.Contains(brush))
                    continue;
                _brushes.Add(brush);
                _initialTransforms.Add(brush.Transform);
            }
            if (_brushes.Count == 0)
            {
                Reset();
                return false;
            }

            _plane = plane;
            CaptureGridReferencePoints();
            _pointerDown = pointer;
            Anchor = Target = ProjectToPlane(ref plane, anchor);
            Stage = CSGSelectDragStage.Armed;
            return true;
        }

        /// <summary>Crosses the DPI-scaled drag threshold at most once.</summary>
        public bool TryBeginDrag(Float2 pointer, float dpiScale)
        {
            if (Stage != CSGSelectDragStage.Armed || !HasExceededDragThreshold(_pointerDown, pointer, dpiScale))
                return false;
            Stage = CSGSelectDragStage.Dragging;
            return true;
        }

        /// <summary>
        /// Marks the one-shot duplicate-at-drag-start request as consumed.
        /// </summary>
        public bool TryConsumeDuplicate(bool requested)
        {
            if (Stage != CSGSelectDragStage.Dragging || !requested || _duplicateConsumed)
                return false;
            _duplicateConsumed = true;
            return true;
        }

        /// <summary>
        /// Replaces the moving set after duplication while retaining the original pointer anchor.
        /// </summary>
        public bool Rebind(IEnumerable<BoxBrush> brushes)
        {
            if (Stage != CSGSelectDragStage.Dragging || brushes == null)
                return false;
            _brushes.Clear();
            _initialTransforms.Clear();
            foreach (var brush in brushes)
            {
                if (brush == null || _brushes.Contains(brush))
                    continue;
                _brushes.Add(brush);
                _initialTransforms.Add(brush.Transform);
            }
            CaptureGridReferencePoints();
            return _brushes.Count != 0;
        }

        /// <summary>
        /// Starts a new continuous movement segment from the brushes' current transforms.
        /// Used when switching between planar movement and surface ray placement without a jump.
        /// </summary>
        public bool Rebase(ref CSGWorkingPlane plane, Vector3 anchor)
        {
            if (Stage != CSGSelectDragStage.Dragging || !plane.IsValid)
                return false;
            _plane = plane;
            _initialTransforms.Clear();
            for (int i = 0; i < _brushes.Count; i++)
                _initialTransforms.Add(_brushes[i].Transform);
            CaptureGridReferencePoints();
            Anchor = Target = ProjectToPlane(ref plane, anchor);
            Delta = Vector3.Zero;
            PlaneDelta = Float2.Zero;
            _wasAligned = false;
            _surfaceRotationDelta = Quaternion.Identity;
            return true;
        }

        /// <summary>Applies a rigid plane-local translation from the initial transforms.</summary>
        public bool ApplyTarget(Vector3 target, bool alignToNormal = false, bool snapToGrid = false, float snapIncrement = 0.0f)
        {
            if (Stage != CSGSelectDragStage.Dragging)
                return false;

            var projectedTarget = ProjectToPlane(ref _plane, target);
            var planeDelta = _plane.ToPlane(projectedTarget) - _plane.ToPlane(Anchor);
            var delta = _plane.Tangent * planeDelta.X + _plane.Bitangent * planeDelta.Y;
            if (snapToGrid)
                delta = SnapDeltaToGrid(delta, _gridReferencePoints, snapIncrement, ref _plane);
            if (Vector3.NearEqual(delta, Delta) && alignToNormal == _wasAligned)
                return false;
            Target = Anchor + delta;
            PlaneDelta = _plane.ToPlane(Target) - _plane.ToPlane(Anchor);
            Delta = delta;
            _wasAligned = alignToNormal;
            var rotationDelta = Quaternion.Identity;
            if (alignToNormal)
            {
                var currentUp = Vector3.Transform(Vector3.Up, _initialTransforms[0].Orientation);
                currentUp.Normalize();
                rotationDelta = Quaternion.FindBetween(currentUp, _plane.Normal);
            }
            for (int i = 0; i < _brushes.Count; i++)
            {
                _brushes[i].Transform = ApplyRigidTransform(_initialTransforms[i], Anchor, Delta, rotationDelta);
            }
            return true;
        }

        /// <summary>Places the selection on a raycast surface using the legacy align-local-up behavior.</summary>
        public bool ApplySurfaceTarget(Vector3 target, Vector3 surfaceNormal, bool alignToNormal = true)
        {
            return ApplySurfaceTarget(target, surfaceNormal,
                                      alignToNormal ? CSGRayPlacementAlignment.AlignToSurface : CSGRayPlacementAlignment.KeepRotation,
                                      CSGRayPlacementFront.Top, false, 0.0f, Vector3.Up, Vector3.Forward);
        }

        /// <summary>
        /// Places the selection flush on a raycast surface. Rotation and tangent-grid snapping are
        /// solved from transformed bounds, never from the arbitrary point grabbed by the pointer.
        /// </summary>
        public bool ApplySurfaceTarget(Vector3 target, Vector3 surfaceNormal, CSGRayPlacementAlignment alignment,
                                       CSGRayPlacementFront front, bool snapToGrid, float snapIncrement,
                                       Vector3 cameraUp, Vector3 cameraForward)
        {
            if (Stage != CSGSelectDragStage.Dragging)
                return false;
            if (surfaceNormal.LengthSquared < 0.000001f)
                return false;

            surfaceNormal.Normalize();

            var placementPivot = _initialTransforms[0].Translation;
            var rotationDelta = Quaternion.Identity;
            if (alignment != CSGRayPlacementAlignment.KeepRotation)
            {
                var orientation = CalculateSurfaceOrientation(surfaceNormal, alignment, front, cameraUp, cameraForward);
                rotationDelta = orientation * Quaternion.Invert(_initialTransforms[0].Orientation);
                rotationDelta.Normalize();
            }

            // Rotate the complete selection first, then offset its nearest support point onto the
            // hit plane. This prevents center pivots from leaving brushes half a grid cell embedded.
            _surfaceGridReferencePoints.Clear();
            float minimumDistance = float.MaxValue;
            for (int i = 0; i < _gridReferencePoints.Count; i++)
            {
                var point = placementPivot + Vector3.Transform(_gridReferencePoints[i] - placementPivot, rotationDelta);
                _surfaceGridReferencePoints.Add(point);
                minimumDistance = Mathf.Min(minimumDistance, (float)Vector3.Dot(point - placementPivot, surfaceNormal));
            }
            if (minimumDistance == float.MaxValue)
                minimumDistance = 0.0f;

            var delta = target - placementPivot - surfaceNormal * minimumDistance;
            if (snapToGrid && snapIncrement > Mathf.Epsilon &&
                CSGWorkingPlaneService.TryDerive(target, surfaceNormal, Vector3.Right, snapIncrement, 10, Guid.Empty, -1, out var surfacePlane))
            {
                float normalDistance = (float)Vector3.Dot(delta, surfaceNormal);
                var planarDelta = delta - surfaceNormal * normalDistance;
                planarDelta = SnapSurfaceDeltaToGrid(planarDelta, _surfaceGridReferencePoints, snapIncrement, ref surfacePlane);
                delta = surfaceNormal * normalDistance + planarDelta;
            }

            if (Vector3.NearEqual(delta, Delta) && Quaternion.NearEqual(rotationDelta, _surfaceRotationDelta))
                return false;
            Target = placementPivot + delta;
            PlaneDelta = _plane.ToPlane(Target) - _plane.ToPlane(Anchor);
            Delta = delta;
            _wasAligned = alignment != CSGRayPlacementAlignment.KeepRotation;
            _surfaceRotationDelta = rotationDelta;
            for (int i = 0; i < _brushes.Count; i++)
                _brushes[i].Transform = ApplyRigidTransform(_initialTransforms[i], placementPivot, Delta, rotationDelta);
            return true;
        }

        /// <summary>Calculates the absolute orientation used by RealtimeCSG-style ray placement.</summary>
        public static Quaternion CalculateSurfaceOrientation(Vector3 surfaceNormal, CSGRayPlacementAlignment alignment,
                                                             CSGRayPlacementFront front, Vector3 cameraUp, Vector3 cameraForward)
        {
            if (alignment == CSGRayPlacementAlignment.KeepRotation || surfaceNormal.LengthSquared < 0.000001f)
                return Quaternion.Identity;

            surfaceNormal.Normalize();
            if (alignment == CSGRayPlacementAlignment.AlignSurfaceUp)
            {
                surfaceNormal.Y = 0.0f;
                if (surfaceNormal.LengthSquared < 0.000001f)
                {
                    surfaceNormal = new Vector3(cameraForward.X, 0.0f, cameraForward.Z);
                    if (surfaceNormal.LengthSquared < 0.000001f)
                        surfaceNormal = Vector3.Forward;
                }
                surfaceNormal.Normalize();
            }

            var tangent = Vector3.Up;
            if (Mathf.Abs((float)Vector3.Dot(tangent, surfaceNormal)) > 0.999f)
            {
                tangent = SnapToClosestAxis(cameraUp);
                if (Mathf.Abs((float)Vector3.Dot(tangent, surfaceNormal)) > 0.999f)
                    tangent = Mathf.Abs((float)Vector3.Dot(Vector3.Forward, surfaceNormal)) < 0.999f ? Vector3.Forward : Vector3.Right;
            }

            Quaternion sourceRotation;
            switch (front)
            {
            case CSGRayPlacementFront.Back:
                sourceRotation = Quaternion.LookRotation(Vector3.Backward);
                break;
            case CSGRayPlacementFront.Left:
                sourceRotation = Quaternion.LookRotation(Vector3.Right);
                break;
            case CSGRayPlacementFront.Right:
                sourceRotation = Quaternion.LookRotation(Vector3.Left);
                break;
            case CSGRayPlacementFront.Top:
                sourceRotation = Quaternion.LookRotation(Vector3.Up, Vector3.Forward);
                break;
            case CSGRayPlacementFront.Bottom:
                sourceRotation = Quaternion.LookRotation(Vector3.Down, Vector3.Backward);
                break;
            default:
                sourceRotation = Quaternion.LookRotation(Vector3.Forward);
                break;
            }
            // Map the chosen source frame onto the destination frame. Using the inverse
            // guarantees, for example, that Aligned Top sends local Up to the hit normal.
            var result = Quaternion.LookRotation(surfaceNormal, tangent) * Quaternion.Invert(sourceRotation);
            result.Normalize();
            return result;
        }

        /// <summary>
        /// Snaps a candidate rigid translation by testing the selection's original geometry against the grid.
        /// The pointer position never participates in this calculation.
        /// </summary>
        public static Vector3 SnapDeltaToGrid(Vector3 delta, IReadOnlyList<Vector3> referencePoints, float increment, ref CSGWorkingPlane plane)
        {
            if (referencePoints == null || referencePoints.Count == 0 || increment <= Mathf.Epsilon)
                return delta;

            var localDelta = new Float2((float)Vector3.Dot(delta, plane.Tangent), (float)Vector3.Dot(delta, plane.Bitangent));
            float snappedX = localDelta.X;
            float snappedY = localDelta.Y;
            bool hasX = false;
            bool hasY = false;
            for (int i = 0; i < referencePoints.Count; i++)
            {
                var point = plane.ToPlane(referencePoints[i]);
                float candidateX = Mathf.Round((point.X + localDelta.X) / increment) * increment - point.X;
                float candidateY = Mathf.Round((point.Y + localDelta.Y) / increment) * increment - point.Y;
                if (!hasX || Mathf.Abs(candidateX) < Mathf.Abs(snappedX))
                {
                    snappedX = candidateX;
                    hasX = true;
                }
                if (!hasY || Mathf.Abs(candidateY) < Mathf.Abs(snappedY))
                {
                    snappedY = candidateY;
                    hasY = true;
                }
            }

            // Match RCSG's snap-to-self guard: a small pointer motion cannot pull geometry to a
            // farther grid line in the opposite direction.
            if (Mathf.Abs(snappedX - localDelta.X) > Mathf.Abs(localDelta.X))
                snappedX = 0.0f;
            if (Mathf.Abs(snappedY - localDelta.Y) > Mathf.Abs(localDelta.Y))
                snappedY = 0.0f;
            return plane.Tangent * snappedX + plane.Bitangent * snappedY;
        }

        /// <summary>
        /// Snaps transformed surface-placement geometry even when the required correction is
        /// larger than the pointer delta. Ray placement has an absolute hit target, so the
        /// planar drag snap-to-self guard does not apply.
        /// </summary>
        public static Vector3 SnapSurfaceDeltaToGrid(Vector3 delta, IReadOnlyList<Vector3> referencePoints, float increment, ref CSGWorkingPlane plane)
        {
            if (referencePoints == null || referencePoints.Count == 0 || increment <= Mathf.Epsilon)
                return delta;

            var localDelta = new Float2((float)Vector3.Dot(delta, plane.Tangent), (float)Vector3.Dot(delta, plane.Bitangent));
            float snappedX = localDelta.X;
            float snappedY = localDelta.Y;
            float bestCorrectionX = float.MaxValue;
            float bestCorrectionY = float.MaxValue;
            for (int i = 0; i < referencePoints.Count; i++)
            {
                var point = plane.ToPlane(referencePoints[i]);
                float candidateX = Mathf.Round((point.X + localDelta.X) / increment) * increment - point.X;
                float candidateY = Mathf.Round((point.Y + localDelta.Y) / increment) * increment - point.Y;
                float correctionX = Mathf.Abs(candidateX - localDelta.X);
                float correctionY = Mathf.Abs(candidateY - localDelta.Y);
                if (correctionX < bestCorrectionX)
                {
                    bestCorrectionX = correctionX;
                    snappedX = candidateX;
                }
                if (correctionY < bestCorrectionY)
                {
                    bestCorrectionY = correctionY;
                    snappedY = candidateY;
                }
            }
            return plane.Tangent * snappedX + plane.Bitangent * snappedY;
        }

        /// <summary>Applies one group rotation around a pivot followed by one world translation.</summary>
        public static Transform ApplyRigidTransform(Transform initial, Vector3 pivot, Vector3 delta, Quaternion rotationDelta)
        {
            initial.Translation = pivot + Vector3.Transform(initial.Translation - pivot, rotationDelta) + delta;
            if (rotationDelta != Quaternion.Identity)
            {
                initial.Orientation = rotationDelta * initial.Orientation;
                initial.Orientation.Normalize();
            }
            return initial;
        }

        /// <summary>Applies a world translation without changing orientation or scale.</summary>
        public static Transform ApplyTranslation(Transform initial, Vector3 delta)
        {
            initial.Translation += delta;
            return initial;
        }

        /// <summary>
        /// Explicitly aligns a transform's local up axis to a target normal while preserving scale.
        /// The caller decides when this opt-in operation is requested.
        /// </summary>
        public static Transform AlignToNormal(Transform initial, Vector3 targetNormal)
        {
            if (targetNormal.LengthSquared < 0.000001f)
                return initial;
            targetNormal.Normalize();
            var currentUp = Vector3.Transform(Vector3.Up, initial.Orientation);
            currentUp.Normalize();
            initial.Orientation = Quaternion.FindBetween(currentUp, targetNormal) * initial.Orientation;
            initial.Orientation.Normalize();
            return initial;
        }

        /// <summary>Clears all pointer and actor state.</summary>
        public void Reset()
        {
            _brushes.Clear();
            _initialTransforms.Clear();
            _gridReferencePoints.Clear();
            _surfaceGridReferencePoints.Clear();
            _plane = default;
            _pointerDown = Float2.Zero;
            _duplicateConsumed = false;
            _wasAligned = false;
            _surfaceRotationDelta = Quaternion.Identity;
            Anchor = Vector3.Zero;
            Target = Vector3.Zero;
            Delta = Vector3.Zero;
            PlaneDelta = Float2.Zero;
            Stage = CSGSelectDragStage.Idle;
        }

        private void CaptureGridReferencePoints()
        {
            _gridReferencePoints.Clear();
            for (int i = 0; i < _brushes.Count; i++)
            {
                _brushes[i].OrientedBox.GetCorners(_brushCorners);
                for (int corner = 0; corner < _brushCorners.Length; corner++)
                    _gridReferencePoints.Add(_brushCorners[corner]);
            }
        }

        /// <summary>Tests a pointer displacement against a DPI-scaled logical-pixel threshold.</summary>
        public static bool HasExceededDragThreshold(Float2 start, Float2 current, float dpiScale)
        {
            float threshold = DragThreshold * Mathf.Max(dpiScale, 0.01f);
            return (current - start).LengthSquared >= threshold * threshold;
        }

        private static Vector3 ProjectToPlane(ref CSGWorkingPlane plane, Vector3 point)
        {
            return plane.ToWorld(plane.ToPlane(point));
        }

        private static Vector3 SnapToClosestAxis(Vector3 direction)
        {
            if (direction.LengthSquared < 0.000001f)
                return Vector3.Up;
            var absolute = Vector3.Abs(direction);
            if (absolute.X >= absolute.Y && absolute.X >= absolute.Z)
                return direction.X < 0.0f ? Vector3.Left : Vector3.Right;
            if (absolute.Y >= absolute.Z)
                return direction.Y < 0.0f ? Vector3.Down : Vector3.Up;
            return direction.Z < 0.0f ? Vector3.Backward : Vector3.Forward;
        }
    }
}
