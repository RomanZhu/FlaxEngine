// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Gizmo;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG.Tools
{
    /// <summary>Direct box face, edge, and vertex offset interaction that keeps opposite extents fixed.</summary>
    public sealed class CSGBoxFaceEditTool
    {
        private static readonly int[] EdgeCorners =
        {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4,
            0, 4, 1, 5, 2, 6, 3, 7,
        };

        private BoxBrush _brush;
        private Ray _anchorRay;
        private Vector3 _faceCenter;
        private Vector3 _axisWorld;
        private Plane _dragPlane;
        private Vector3 _initialCenter;
        private Vector3 _initialSize;
        private int _axis;
        private float _sign;
        private int _cornerIndex = -1;
        private int _edgeIndex = -1;

        /// <summary>Gets whether a face resize transaction is active.</summary>
        public bool IsInteracting => _brush != null;

        /// <summary>Gets the selected face index, or -1.</summary>
        public int FaceIndex { get; private set; } = -1;

        /// <summary>Gets the selected corner index, or -1.</summary>
        public int CornerIndex => _cornerIndex;

        /// <summary>Gets the selected edge index, or -1.</summary>
        public int EdgeIndex => _edgeIndex;

        /// <summary>Gets the current signed face movement in brush-local units.</summary>
        public float Delta { get; private set; }

        /// <summary>Gets the current local component movement.</summary>
        public Vector3 DeltaVector { get; private set; }

        /// <summary>Begins resizing one stable box face.</summary>
        public bool Begin(BoxBrush brush, int faceIndex, Ray pointerRay)
        {
            Reset();
            if (brush == null || faceIndex < 0 || faceIndex > 5)
                return false;
            GetFaceAxis(faceIndex, out _axis, out _sign);
            _brush = brush;
            FaceIndex = faceIndex;
            _anchorRay = pointerRay;
            _initialCenter = brush.Center;
            _initialSize = brush.Size;
            var localAxis = GetAxis(_axis) * _sign;
            _axisWorld = brush.Transform.LocalToWorldVector(localAxis);
            _axisWorld.Normalize();
            _faceCenter = brush.Transform.LocalToWorld(_initialCenter + GetAxis(_axis) * GetComponent(_initialSize, _axis) * 0.5f * _sign);
            return true;
        }

        /// <summary>Begins a free vertex offset on the camera-facing drag plane.</summary>
        public bool BeginCorner(BoxBrush brush, int cornerIndex, Ray pointerRay, Vector3 viewDirection)
        {
            Reset();
            if (brush == null || cornerIndex < 0 || cornerIndex > 7)
                return false;
            _brush = brush;
            _cornerIndex = cornerIndex;
            _anchorRay = pointerRay;
            _initialCenter = brush.Center;
            _initialSize = brush.Size;
            Vector3 signs = GetCornerSigns(cornerIndex);
            Vector3 localCorner = _initialCenter + signs * _initialSize * 0.5f;
            _faceCenter = brush.Transform.LocalToWorld(localCorner);
            _dragPlane = CreateViewPlane(_faceCenter, viewDirection);
            return true;
        }

        /// <summary>Begins a two-plane edge offset on the camera-facing drag plane.</summary>
        public bool BeginEdge(BoxBrush brush, int edgeIndex, Ray pointerRay, Vector3 viewDirection)
        {
            Reset();
            if (brush == null || edgeIndex < 0 || edgeIndex > 11)
                return false;
            _brush = brush;
            _edgeIndex = edgeIndex;
            _anchorRay = pointerRay;
            _initialCenter = brush.Center;
            _initialSize = brush.Size;
            GetEdgeSigns(edgeIndex, out var signs, out _);
            _faceCenter = brush.Transform.LocalToWorld(_initialCenter + signs * _initialSize * 0.5f);
            _dragPlane = CreateViewPlane(_faceCenter, viewDirection);
            return true;
        }

        /// <summary>Updates the face from the initial brush dimensions.</summary>
        public bool Update(Ray pointerRay, bool snap, float increment)
        {
            if (_brush == null)
                return false;
            Vector3 worldDelta;
            if (FaceIndex >= 0)
            {
                if (!TransformGizmoBase.TrySolveAxisTranslation(_anchorRay, pointerRay, _faceCenter, _axisWorld, out worldDelta))
                    return false;
            }
            else if (!TransformGizmoBase.TrySolvePlaneTranslation(_anchorRay, pointerRay, _dragPlane, out worldDelta))
            {
                return false;
            }
            var localWorldDelta = _brush.Transform.WorldToLocalVector(worldDelta);
            if (_cornerIndex >= 0)
                return UpdateCorner(localWorldDelta, snap, increment);
            if (_edgeIndex >= 0)
                return UpdateEdge(localWorldDelta, snap, increment);
            float localDelta = (float)Vector3.Dot(localWorldDelta, GetAxis(_axis) * _sign);
            if (snap)
            {
                float localStep = GetLocalSnapStep(_brush, _axis, increment);
                localDelta = Mathf.Round(localDelta / localStep) * localStep;
            }

            float initialSize = GetComponent(_initialSize, _axis);
            float size = Mathf.Max(initialSize + localDelta, 0.001f);
            localDelta = size - initialSize;
            TrySolve(_initialCenter, _initialSize, FaceIndex, localDelta, 0.001f, out var newCenter, out var newSize);
            if (Vector3.NearEqual(_brush.Size, newSize) && Vector3.NearEqual(_brush.Center, newCenter))
                return false;
            _brush.Size = newSize;
            _brush.Center = newCenter;
            Delta = localDelta;
            DeltaVector = GetAxis(_axis) * (_sign * localDelta);
            return true;
        }

        private bool UpdateCorner(Vector3 localWorldDelta, bool snap, float increment)
        {
            Vector3 signs = GetCornerSigns(_cornerIndex);
            Vector3 delta = localWorldDelta * signs;
            if (snap)
            {
                delta.X = SnapLocalComponent((float)delta.X, GetLocalSnapStep(_brush, 0, increment));
                delta.Y = SnapLocalComponent((float)delta.Y, GetLocalSnapStep(_brush, 1, increment));
                delta.Z = SnapLocalComponent((float)delta.Z, GetLocalSnapStep(_brush, 2, increment));
            }
            TrySolveCorner(_initialCenter, _initialSize, _cornerIndex, delta, 0.001f, out var newCenter, out var newSize);
            if (Vector3.NearEqual(_brush.Size, newSize) && Vector3.NearEqual(_brush.Center, newCenter))
                return false;
            _brush.Size = newSize;
            _brush.Center = newCenter;
            Delta = (float)delta.Length;
            DeltaVector = delta * signs;
            return true;
        }

        private bool UpdateEdge(Vector3 localWorldDelta, bool snap, float increment)
        {
            GetEdgeSigns(_edgeIndex, out var signs, out _);
            Vector3 delta = localWorldDelta * signs;
            if (snap)
            {
                delta.X = SnapLocalComponent((float)delta.X, GetLocalSnapStep(_brush, 0, increment));
                delta.Y = SnapLocalComponent((float)delta.Y, GetLocalSnapStep(_brush, 1, increment));
                delta.Z = SnapLocalComponent((float)delta.Z, GetLocalSnapStep(_brush, 2, increment));
            }
            TrySolveEdge(_initialCenter, _initialSize, _edgeIndex, delta, 0.001f, out var newCenter, out var newSize);
            if (Vector3.NearEqual(_brush.Size, newSize) && Vector3.NearEqual(_brush.Center, newCenter))
                return false;
            _brush.Size = newSize;
            _brush.Center = newCenter;
            DeltaVector = delta * signs;
            Delta = (float)DeltaVector.Length;
            return true;
        }

        /// <summary>Clears the interaction.</summary>
        public void Reset()
        {
            _brush = null;
            FaceIndex = -1;
            _cornerIndex = -1;
            _edgeIndex = -1;
            Delta = 0.0f;
            DeltaVector = Vector3.Zero;
        }

        /// <summary>Maps a stable box face to its local axis and sign.</summary>
        public static void GetFaceAxis(int faceIndex, out int axis, out float sign)
        {
            axis = faceIndex / 2;
            sign = faceIndex % 2 == 0 ? 1.0f : -1.0f;
        }

        /// <summary>Pure face-resize solver used by interaction and tests.</summary>
        public static bool TrySolve(Vector3 initialCenter, Vector3 initialSize, int faceIndex, float delta, float minimumSize, out Vector3 center, out Vector3 size)
        {
            center = initialCenter;
            size = initialSize;
            if (faceIndex < 0 || faceIndex > 5 || float.IsNaN(delta) || float.IsInfinity(delta))
                return false;
            GetFaceAxis(faceIndex, out int axis, out float sign);
            float initialAxisSize = GetComponent(initialSize, axis);
            float newAxisSize = Math.Max(initialAxisSize + delta, Math.Max(minimumSize, 0.0001f));
            delta = newAxisSize - initialAxisSize;
            SetComponent(ref size, axis, newAxisSize);
            SetComponent(ref center, axis, GetComponent(initialCenter, axis) + delta * 0.5f * sign);
            return true;
        }

        /// <summary>Pure corner-resize solver used by interaction and tests.</summary>
        public static bool TrySolveCorner(Vector3 initialCenter, Vector3 initialSize, int cornerIndex, Vector3 outwardDelta, float minimumSize, out Vector3 center, out Vector3 size)
        {
            center = initialCenter;
            size = initialSize;
            if (cornerIndex < 0 || cornerIndex > 7)
                return false;
            Vector3 signs = GetCornerSigns(cornerIndex);
            float minimum = Math.Max(minimumSize, 0.0001f);
            for (int axis = 0; axis < 3; axis++)
            {
                float initialAxisSize = GetComponent(initialSize, axis);
                float newAxisSize = Math.Max(initialAxisSize + GetComponent(outwardDelta, axis), minimum);
                float appliedDelta = newAxisSize - initialAxisSize;
                SetComponent(ref size, axis, newAxisSize);
                SetComponent(ref center, axis, GetComponent(initialCenter, axis) + appliedDelta * GetComponent(signs, axis) * 0.5f);
            }
            return true;
        }

        /// <summary>Pure edge-offset solver used by interaction and tests.</summary>
        public static bool TrySolveEdge(Vector3 initialCenter, Vector3 initialSize, int edgeIndex, Vector3 outwardDelta, float minimumSize, out Vector3 center, out Vector3 size)
        {
            center = initialCenter;
            size = initialSize;
            if (edgeIndex < 0 || edgeIndex > 11)
                return false;
            GetEdgeSigns(edgeIndex, out var signs, out _);
            float minimum = Math.Max(minimumSize, 0.0001f);
            for (int axis = 0; axis < 3; axis++)
            {
                float sign = GetComponent(signs, axis);
                if (Mathf.Abs(sign) < 0.5f)
                    continue;
                float initialAxisSize = GetComponent(initialSize, axis);
                float newAxisSize = Math.Max(initialAxisSize + GetComponent(outwardDelta, axis), minimum);
                float appliedDelta = newAxisSize - initialAxisSize;
                SetComponent(ref size, axis, newAxisSize);
                SetComponent(ref center, axis, GetComponent(initialCenter, axis) + appliedDelta * sign * 0.5f);
            }
            return true;
        }

        /// <summary>Gets the two fixed side signs and varying axis for a box edge.</summary>
        public static void GetEdgeSigns(int edgeIndex, out Vector3 signs, out int edgeAxis)
        {
            int offset = edgeIndex * 2;
            var first = GetCornerSigns(EdgeCorners[offset]);
            var second = GetCornerSigns(EdgeCorners[offset + 1]);
            signs = (first + second) * 0.5f;
            edgeAxis = Mathf.Abs((float)signs.X) < 0.5f ? 0 : Mathf.Abs((float)signs.Y) < 0.5f ? 1 : 2;
        }

        private static Plane CreateViewPlane(Vector3 point, Vector3 viewDirection)
        {
            if (viewDirection.LengthSquared < 0.000001f)
                viewDirection = Vector3.Forward;
            viewDirection.Normalize();
            return new Plane(point, -viewDirection);
        }

        private static float GetLocalSnapStep(BoxBrush brush, int axis, float worldIncrement)
        {
            float axisScale = (float)brush.Transform.LocalToWorldVector(GetAxis(axis)).Length;
            return Mathf.Max(worldIncrement, 0.0001f) / Mathf.Max(axisScale, 0.0001f);
        }

        private static float SnapLocalComponent(float value, float step)
        {
            return Mathf.Round(value / step) * step;
        }

        private static Vector3 GetCornerSigns(int cornerIndex)
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

        private static Vector3 GetAxis(int axis)
        {
            return axis == 0 ? Vector3.Right : axis == 1 ? Vector3.Up : Vector3.Forward;
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
    }
}
