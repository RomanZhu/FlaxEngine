// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Viewport.Cameras;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        private readonly struct SemanticCandidate
        {
            public SemanticCandidate(int index, SemanticTarget target, float score, float depth, int priority, bool cap)
            {
                Index = index;
                Target = target;
                Score = score;
                Depth = depth;
                Priority = priority;
                IsCap = cap;
            }

            public int Index { get; }
            public SemanticTarget Target { get; }
            public float Score { get; }
            public float Depth { get; }
            public int Priority { get; }
            public bool IsCap { get; }
        }

        private readonly List<SemanticTarget> _semanticTargets = new List<SemanticTarget>(16);
        private SemanticHandle _hoveredHandle = SemanticHandle.None;
        private SemanticTarget _hoveredTarget;
        private bool _hasHoveredTarget;
        private float _hoveredTargetScore = float.MaxValue;
        private bool _gizmoProjectionValid;

        /// <summary>
        /// Gets the current projected semantic targets.
        /// </summary>
        public IReadOnlyList<SemanticTarget> SemanticTargets => _semanticTargets;

        /// <summary>
        /// Gets the semantic handle currently retained under the pointer.
        /// </summary>
        public SemanticHandle HoveredHandle => _hoveredHandle;

        /// <summary>
        /// Computes projection-derived gizmo sizing in physical viewport pixels.
        /// Logical dimensions and the desired logical radius are converted to physical
        /// pixels together, so DPI is applied exactly once.
        /// </summary>
        /// <param name="orthographic">True for an orthographic projection.</param>
        /// <param name="forwardDepth">Pivot depth along the camera forward direction.</param>
        /// <param name="verticalFovDegrees">Perspective vertical field of view in degrees.</param>
        /// <param name="orthographicScale">Orthographic world height per logical viewport pixel.</param>
        /// <param name="viewportHeightLogical">Viewport height in logical UI pixels.</param>
        /// <param name="dpiScale">Logical-to-physical viewport scale.</param>
        /// <param name="desiredRadiusLogicalPixels">Desired gizmo radius in logical UI pixels.</param>
        /// <param name="nearPlane">Camera near clipping plane.</param>
        /// <param name="worldUnitsPerPhysicalPixel">The resulting world size of one physical pixel.</param>
        /// <param name="gizmoWorldRadius">The resulting world-space gizmo radius.</param>
        /// <returns>True when the projection is valid and the pivot is in front of the camera.</returns>
        public static bool TryCalculateProjectionSizing(bool orthographic, float forwardDepth, float verticalFovDegrees, float orthographicScale, float viewportHeightLogical, float dpiScale, float desiredRadiusLogicalPixels, float nearPlane, out float worldUnitsPerPhysicalPixel, out float gizmoWorldRadius)
        {
            worldUnitsPerPhysicalPixel = 0.0f;
            gizmoWorldRadius = 0.0f;
            if (!IsFinite(viewportHeightLogical) || !IsFinite(dpiScale) || !IsFinite(desiredRadiusLogicalPixels) ||
                !IsFinite(forwardDepth) || viewportHeightLogical <= Mathf.Epsilon || dpiScale <= Mathf.Epsilon ||
                desiredRadiusLogicalPixels <= Mathf.Epsilon || forwardDepth <= 0.0f)
                return false;

            float physicalViewportHeight = viewportHeightLogical * dpiScale;
            if (!IsFinite(physicalViewportHeight) || physicalViewportHeight <= Mathf.Epsilon)
                return false;

            if (orthographic)
            {
                if (!IsFinite(orthographicScale) || orthographicScale <= Mathf.Epsilon)
                    return false;
                float orthographicViewHeight = viewportHeightLogical * orthographicScale;
                worldUnitsPerPhysicalPixel = orthographicViewHeight / physicalViewportHeight;
            }
            else
            {
                if (!IsFinite(verticalFovDegrees))
                    return false;

                float clampedDepth = Mathf.Max(forwardDepth, Mathf.Max(nearPlane, MinimumProjectionDepth));
                float fov = Mathf.Clamp(verticalFovDegrees, 0.01f, 179.0f) * Mathf.DegreesToRadians;
                float verticalViewHeight = 2.0f * clampedDepth * (float)Math.Tan(fov * 0.5f);
                worldUnitsPerPhysicalPixel = verticalViewHeight / physicalViewportHeight;
            }

            float desiredRadiusPhysicalPixels = desiredRadiusLogicalPixels * dpiScale;
            gizmoWorldRadius = desiredRadiusPhysicalPixels * worldUnitsPerPhysicalPixel;
            return IsFinite(worldUnitsPerPhysicalPixel) && worldUnitsPerPhysicalPixel > 0.0f && IsFinite(gizmoWorldRadius) && gizmoWorldRadius > 0.0f;
        }

        private bool TryGetGizmoWorldRadius(Vector3 position, out float gizmoWorldRadius)
        {
            gizmoWorldRadius = 0.0f;
            var viewport = Owner?.Viewport;
            if (viewport == null)
                return false;

            float forwardDepth = (float)Vector3.Dot(position - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
            float verticalFov = viewport.FieldOfView;
            if (viewport.ViewportCamera is FPSCamera fpsCamera)
                verticalFov += fpsCamera.AdditionalZoomFOV;
            float desiredRadius = GizmoRadiusPixels * Mathf.Max(Editor.Instance.Options.Options.Visual.GizmoSize, 0.01f);
            return TryCalculateProjectionSizing(
                viewport.UseOrthographicProjection,
                forwardDepth,
                verticalFov,
                viewport.OrthographicScale,
                viewport.Height,
                viewport.DpiScale,
                desiredRadius,
                viewport.NearPlane,
                out _,
                out gizmoWorldRadius);
        }

        private static bool IsFinite(float value)
        {
            return !float.IsNaN(value) && !float.IsInfinity(value);
        }

        private bool IsTargetAvailable(SemanticHandle handle)
        {
            return IsSupplementalTranslationHandleAllowed(handle.Axis) && (!HasActiveTransaction || handle == _latchedHandle);
        }

        private bool TryProjectSemanticWorldPoint(Vector3 worldPosition, out Float2 screenPosition)
        {
            return TryProjectGizmoPoint(worldPosition, out screenPosition);
        }

        private float GetSemanticDepth(Vector3 worldPosition)
        {
            return (float)Vector3.Dot(worldPosition - Owner.ViewPosition, (Vector3)Owner.ViewDirection);
        }

        private SemanticHandle CreateSemanticHandle(Axis axis, Vector3 worldPosition, Vector3 worldDirection, Float2 screenPosition, float depth, bool isAvailable)
        {
            return new SemanticHandle(_activeMode, axis).WithDisplayGeometry(worldPosition, worldDirection, screenPosition, depth, isAvailable);
        }

        private void RebuildSemanticTargets()
        {
            _semanticTargets.Clear();

            if (!_gizmoProjectionValid || !_isActive || !IsInteractionActive || SelectionCount == 0 || _activeMode == Mode.Select)
            {
                _hasHoveredTarget = false;
                _hoveredHandle = SemanticHandle.None;
                return;
            }

            if (_activeMode == Mode.Rotate)
                BuildRotationSemanticTargets();
            else if (_activeMode == Mode.Bounds)
                BuildBoundsSemanticTargets();
            else
                BuildTranslateScaleSemanticTargets();

            if (!_hoveredHandle.IsValid || _hoveredHandle.Mode != _activeMode)
            {
                _hoveredHandle = SemanticHandle.None;
                _hasHoveredTarget = false;
            }
        }

        private void BuildTranslateScaleSemanticTargets()
        {
            AddTranslateScaleAxisTarget(Axis.X);
            AddTranslateScaleAxisTarget(Axis.Y);
            AddTranslateScaleAxisTarget(Axis.Z);
            AddPlaneSemanticTarget(Axis.XY);
            AddPlaneSemanticTarget(Axis.ZX);
            AddPlaneSemanticTarget(Axis.YZ);
            // A camera-facing free-move center is unsuitable for grid-authored CSG.
            // CSG keeps the axis and plane motors and provides its own XZ direct drag.
            if (!IsConstrainedSupplementalTranslation)
                AddCenterSemanticTarget();
        }

        private void AddTranslateScaleAxisTarget(Axis axis)
        {
            if (!TryGetRotationAxisLocal(axis, out var localDirection))
                return;

            float headLength = _activeMode == Mode.Scale ? AxisScaleCubeSize * 0.5f : AxisArrowHeadLength;
            Vector3 localStart = localDirection * AxisVisualStart;
            Vector3 localHeadBase = localDirection * (AxisLength - headLength);
            Vector3 localEnd = localDirection * AxisLength;
            Vector3 worldStart = _gizmoWorld.LocalToWorld(localStart);
            Vector3 worldHeadBase = _gizmoWorld.LocalToWorld(localHeadBase);
            Vector3 worldEnd = _gizmoWorld.LocalToWorld(localEnd);
            if (!TryProjectSemanticWorldPoint(worldStart, out var screenStart) ||
                !TryProjectSemanticWorldPoint(worldHeadBase, out var screenHeadBase) ||
                !TryProjectSemanticWorldPoint(worldEnd, out var screenEnd))
                return;

            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, axis));
            Vector3 worldDirection = _gizmoWorld.LocalToWorldVector(localDirection);
            if (worldDirection.LengthSquared > 0.0001f)
                worldDirection.Normalize();

            Float2 capDirection = screenEnd - screenHeadBase;
            Float2[] cap;
            if (capDirection.LengthSquared < 0.0001f)
            {
                float halfSize = CapMotorTargetWidthPixels * 0.5f;
                cap = new[]
                {
                    screenEnd + new Float2(-halfSize, -halfSize),
                    screenEnd + new Float2(halfSize, -halfSize),
                    screenEnd + new Float2(halfSize, halfSize),
                    screenEnd + new Float2(-halfSize, halfSize),
                };
            }
            else
            {
                capDirection.Normalize();
                Float2 perpendicular = new Float2(-capDirection.Y, capDirection.X) * (CapMotorTargetWidthPixels * 0.5f);
                cap = new[]
                {
                    screenHeadBase + perpendicular,
                    screenEnd + perpendicular,
                    screenEnd - perpendicular,
                    screenHeadBase - perpendicular,
                };
            }
            Vector3 worldCenter = _gizmoWorld.LocalToWorld(localDirection * ((AxisVisualStart + AxisLength) * 0.5f));
            Float2 screenCenter = (screenStart + screenEnd) * 0.5f;
            float depth = GetSemanticDepth(worldCenter);
            float startDepth = GetSemanticDepth(worldStart);
            float headBaseDepth = GetSemanticDepth(worldHeadBase);
            float endDepth = GetSemanticDepth(worldEnd);
            var capDepths = new[] { headBaseDepth, endDepth, endDepth, headBaseDepth };
            var handle = CreateSemanticHandle(axis, worldCenter, worldDirection, screenCenter, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.AxisSegment, screenStart, screenEnd, screenCenter, cap[0], cap[1], cap[2], cap[3], cap, AxisMotorTargetWidthPixels * 0.5f, depth, startDepth, endDepth, capDepths, isAvailable));
        }

        private void AddPlaneSemanticTarget(Axis axis)
        {
            if (!TryGetPlaneHandleWorldCorners(axis, out var world0, out var world1, out var world2, out var world3) ||
                !TryProjectSemanticWorldPoint(world0, out var screen0) ||
                !TryProjectSemanticWorldPoint(world1, out var screen1) ||
                !TryProjectSemanticWorldPoint(world2, out var screen2) ||
                !TryProjectSemanticWorldPoint(world3, out var screen3))
                return;

            var points = new[] { screen0, screen1, screen3, screen2 };
            Float2 center = (screen0 + screen1 + screen2 + screen3) * 0.25f;
            Vector3 worldCenter = (world0 + world1 + world2 + world3) * 0.25f;
            float depth = GetSemanticDepth(worldCenter);
            var pointDepths = new[]
            {
                GetSemanticDepth(world0),
                GetSemanticDepth(world1),
                GetSemanticDepth(world3),
                GetSemanticDepth(world2),
            };
            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, axis));
            var handle = CreateSemanticHandle(axis, worldCenter, Vector3.Zero, center, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.PlaneQuadrilateral, Float2.Zero, Float2.Zero, center, points[0], points[1], points[2], points[3], points, PlaneMotorExpansionPixels, depth, depth, depth, pointDepths, isAvailable));
        }

        private void AddCenterSemanticTarget()
        {
            if (!TryProjectSemanticWorldPoint(Position, out var center))
                return;

            float depth = GetSemanticDepth(Position);
            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, Axis.Center));
            var handle = CreateSemanticHandle(Axis.Center, Position, Vector3.Zero, center, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.CenterCircle, Float2.Zero, Float2.Zero, center, Float2.Zero, Float2.Zero, Float2.Zero, Float2.Zero, null, CenterMotorTargetSizePixels * 0.5f, depth, depth, depth, null, isAvailable));
        }

        private void BuildRotationSemanticTargets()
        {
            AddRotationRingSemanticTarget(Axis.X, Vector3.UnitX);
            AddRotationRingSemanticTarget(Axis.Y, Vector3.UnitY);
            AddRotationRingSemanticTarget(Axis.Z, Vector3.UnitZ);
            AddScreenRingSemanticTarget();
            AddRotationCenterSemanticTarget();
        }

        private void AddRotationRingSemanticTarget(Axis axis, Vector3 normal)
        {
            if (!TryBuildProjectedRing(normal, RotateRadiusRaw, true, out var points, out var pointDepths, out var depth))
                return;

            Float2 center;
            if (!TryProjectSemanticWorldPoint(Position, out center))
                return;
            Vector3 worldDirection = _gizmoWorld.LocalToWorldVector(normal);
            if (worldDirection.LengthSquared > 0.0001f)
                worldDirection.Normalize();
            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, axis));
            var handle = CreateSemanticHandle(axis, Position, worldDirection, center, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.AxisRing, Float2.Zero, Float2.Zero, center, Float2.Zero, Float2.Zero, Float2.Zero, Float2.Zero, points, RingMotorTargetWidthPixels * 0.5f, depth, depth, depth, pointDepths, isAvailable));
        }

        private void AddScreenRingSemanticTarget()
        {
            var transform = _gizmoWorld;
            Vector3 normal = GetRotateToViewLocal(ref transform);
            if (!TryBuildProjectedRing(normal, _rotationScreenRingRadiusRaw, false, out var points, out var pointDepths, out var depth))
                return;
            if (!TryProjectSemanticWorldPoint(Position, out var center))
                return;

            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, Axis.Screen));
            Vector3 worldDirection = _gizmoWorld.LocalToWorldVector(normal);
            if (worldDirection.LengthSquared > 0.0001f)
                worldDirection.Normalize();
            var handle = CreateSemanticHandle(Axis.Screen, Position, worldDirection, center, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.ScreenRing, Float2.Zero, Float2.Zero, center, Float2.Zero, Float2.Zero, Float2.Zero, Float2.Zero, points, RingMotorTargetWidthPixels * 0.5f, depth, depth, depth, pointDepths, isAvailable));
        }

        private void AddRotationCenterSemanticTarget()
        {
            if (!TryProjectSemanticWorldPoint(Position, out var center))
                return;

            var transform = GetRotationTrackballTransform();
            Vector3 viewNormal = GetRotateToViewLocal(ref transform);
            Vector3 tangent = Vector3.Cross(viewNormal, Vector3.Up);
            if (tangent.LengthSquared < 0.0001f)
                tangent = Vector3.Cross(viewNormal, Vector3.Right);
            if (tangent.LengthSquared < 0.0001f)
                return;
            tangent.Normalize();
            Vector3 rim = transform.LocalToWorld(tangent * _rotationTrackballRadiusRaw);
            if (!TryProjectSemanticWorldPoint(rim, out var rimScreen))
                return;

            float radius = (rimScreen - center).Length;
            if (radius < 1.0f)
                return;
            float depth = GetSemanticDepth(Position);
            bool isAvailable = IsTargetAvailable(new SemanticHandle(_activeMode, Axis.Center));
            var handle = CreateSemanticHandle(Axis.Center, Position, Vector3.Zero, center, depth, isAvailable);
            _semanticTargets.Add(new SemanticTarget(handle, SemanticTargetKind.TrackballCircle, Float2.Zero, Float2.Zero, center, Float2.Zero, Float2.Zero, Float2.Zero, Float2.Zero, null, radius, depth, depth, depth, null, isAvailable));
        }

        private bool TryBuildProjectedRing(Vector3 normal, float radius, bool frontOnly, out List<Float2> points, out List<float> pointDepths, out float depth)
        {
            points = new List<Float2>(frontOnly ? 25 : 49);
            pointDepths = new List<float>(frontOnly ? 25 : 49);
            depth = float.MaxValue;
            Vector3 tangentU = GetRotateFrontDirectionLocal(normal);
            Vector3 tangentV = Vector3.Cross(normal, tangentU);
            if (tangentV.LengthSquared < 0.0001f)
                return false;
            tangentV.Normalize();

            int segments = frontOnly ? 24 : 48;
            float start = frontOnly ? -Mathf.PiOverTwo : 0.0f;
            float span = frontOnly ? Mathf.Pi : Mathf.TwoPi;
            for (int i = 0; i <= segments; i++)
            {
                float angle = start + span * i / segments;
                Vector3 localPoint = tangentU * (float)Math.Cos(angle) + tangentV * (float)Math.Sin(angle);
                Vector3 worldPoint = _gizmoWorld.LocalToWorld(localPoint * radius);
                if (!TryProjectSemanticWorldPoint(worldPoint, out var screenPoint))
                    continue;
                points.Add(screenPoint);
                float pointDepth = GetSemanticDepth(worldPoint);
                pointDepths.Add(pointDepth);
                depth = Mathf.Min(depth, pointDepth);
            }

            if (points.Count < 2)
                return false;
            return IsFinite(depth);
        }

        private static bool IsPointInConvexPolygon(Float2 point, IReadOnlyList<Float2> polygon)
        {
            if (polygon == null || polygon.Count < 3)
                return false;

            bool hasNegative = false;
            bool hasPositive = false;
            float signedArea = 0.0f;
            for (int i = 0; i < polygon.Count; i++)
            {
                Float2 a = polygon[i];
                Float2 b = polygon[(i + 1) % polygon.Count];
                float cross = (b.X - a.X) * (point.Y - a.Y) - (b.Y - a.Y) * (point.X - a.X);
                hasNegative |= cross < 0.0f;
                hasPositive |= cross > 0.0f;
                signedArea += a.X * b.Y - b.X * a.Y;
            }
            return Mathf.Abs(signedArea) > 0.001f && !(hasNegative && hasPositive);
        }

        private static float DistanceToPolyline(Float2 point, IReadOnlyList<Float2> points, IReadOnlyList<float> depths, bool closed, float fallbackDepth, out float depth)
        {
            depth = fallbackDepth;
            if (points == null || points.Count < 2)
                return float.MaxValue;

            float distanceSquared = float.MaxValue;
            int segmentCount = closed ? points.Count : points.Count - 1;
            bool hasDepths = depths != null && depths.Count == points.Count;
            for (int i = 0; i < segmentCount; i++)
            {
                int next = (i + 1) % points.Count;
                float candidate = DistancePointToSegmentSquared(point, points[i], points[next], out var amount);
                if (candidate < distanceSquared)
                {
                    distanceSquared = candidate;
                    if (hasDepths)
                        depth = depths[i] + (depths[next] - depths[i]) * amount;
                }
            }
            return (float)Math.Sqrt(distanceSquared);
        }

        private static bool TryInterpolateTriangleDepth(Float2 point, Float2 a, Float2 b, Float2 c, float depthA, float depthB, float depthC, out float depth)
        {
            depth = 0.0f;
            float denominator = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
            if (Mathf.Abs(denominator) < 0.0001f)
                return false;
            float weightA = ((b.Y - c.Y) * (point.X - c.X) + (c.X - b.X) * (point.Y - c.Y)) / denominator;
            float weightB = ((c.Y - a.Y) * (point.X - c.X) + (a.X - c.X) * (point.Y - c.Y)) / denominator;
            float weightC = 1.0f - weightA - weightB;
            if (weightA < -0.001f || weightB < -0.001f || weightC < -0.001f)
                return false;
            depth = depthA * weightA + depthB * weightB + depthC * weightC;
            return true;
        }

        private static float GetQuadDistanceAndDepth(Float2 point, SemanticTarget target, bool inside, out float depth)
        {
            var quad = new[] { target.Point0, target.Point1, target.Point2, target.Point3 };
            var depths = target.PointDepths;
            if (inside && depths.Count == 4 &&
                (TryInterpolateTriangleDepth(point, quad[0], quad[1], quad[2], depths[0], depths[1], depths[2], out depth) ||
                 TryInterpolateTriangleDepth(point, quad[0], quad[2], quad[3], depths[0], depths[2], depths[3], out depth)))
            {
                return 0.0f;
            }
            return DistanceToPolyline(point, quad, depths, true, target.Depth, out depth);
        }

        private static bool IsPointInQuad(Float2 point, SemanticTarget target)
        {
            Float2[] quad = { target.Point0, target.Point1, target.Point2, target.Point3 };
            return IsPointInConvexPolygon(point, quad);
        }

        private static bool TryEvaluateSemanticTarget(SemanticTarget target, Float2 cursor, float expansion, out float score, out float depth, out int priority, out bool isCap)
        {
            score = float.MaxValue;
            depth = target.Depth;
            priority = 0;
            isCap = false;
            if (!target.IsAvailable)
                return false;

            float scoreRadius = Mathf.Max(target.MotorRadius, 1.0f);
            float hitRadius = Mathf.Max(target.MotorRadius + expansion, 1.0f);
            float distance;
            switch (target.Kind)
            {
            case SemanticTargetKind.AxisSegment:
            {
                var cap = new[] { target.Point0, target.Point1, target.Point2, target.Point3 };
                if (IsPointInConvexPolygon(cursor, cap))
                {
                    score = 0.0f;
                    GetQuadDistanceAndDepth(cursor, target, true, out depth);
                    priority = 3;
                    isCap = true;
                    return true;
                }
                distance = (float)Math.Sqrt(DistancePointToSegmentSquared(cursor, target.Start, target.End, out var amount));
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                depth = target.StartDepth + (target.EndDepth - target.StartDepth) * amount;
                priority = 1;
                return true;
            }
            case SemanticTargetKind.PlaneQuadrilateral:
            {
                bool inside = IsPointInQuad(cursor, target);
                distance = GetQuadDistanceAndDepth(cursor, target, inside, out depth);
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                float edgeDistance = DistanceToPolyline(cursor, target.Points, target.PointDepths, true, target.Depth, out _);
                priority = inside && edgeDistance >= 4.0f ? 2 : 1;
                return true;
            }
            case SemanticTargetKind.CenterCircle:
            {
                distance = (cursor - target.Center).Length;
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                priority = 4;
                return true;
            }
            case SemanticTargetKind.CenterSquare:
            {
                Float2 offset = cursor - target.Center;
                distance = Mathf.Max(Mathf.Abs(offset.X), Mathf.Abs(offset.Y));
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                priority = 4;
                return true;
            }
            case SemanticTargetKind.TrackballCircle:
            {
                distance = (cursor - target.Center).Length;
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                priority = 0;
                return true;
            }
            case SemanticTargetKind.AxisRing:
            case SemanticTargetKind.ScreenRing:
            {
                distance = DistanceToPolyline(cursor, target.Points, target.PointDepths, false, target.Depth, out depth);
                if (distance > hitRadius)
                    return false;
                score = distance / scoreRadius;
                priority = 2;
                return true;
            }
            default:
                return false;
            }
        }

        private static int CompareSemanticCandidates(SemanticCandidate left, SemanticCandidate right)
        {
            if (left.Priority != right.Priority)
                return right.Priority.CompareTo(left.Priority);
            if (Mathf.Abs(left.Depth - right.Depth) > 0.001f)
                return left.Depth < right.Depth ? -1 : 1;
            float scoreDifference = left.Score - right.Score;
            if (Mathf.Abs(scoreDifference) > 0.05f)
                return scoreDifference < 0.0f ? -1 : 1;
            return left.Index.CompareTo(right.Index);
        }

        private bool TryGetBestSemanticCandidate(Float2 cursor, float expansion, out SemanticCandidate candidate)
        {
            candidate = default;
            bool found = false;
            for (int i = 0; i < _semanticTargets.Count; i++)
            {
                var target = _semanticTargets[i];
                if (!TryEvaluateSemanticTarget(target, cursor, expansion, out var score, out var depth, out var priority, out var isCap))
                    continue;
                var current = new SemanticCandidate(i, target, score, depth, priority, isCap);
                if (!found || CompareSemanticCandidates(current, candidate) < 0)
                {
                    candidate = current;
                    found = true;
                }
            }
            return found;
        }

        private bool TryGetRetainedHoverCandidate(Float2 cursor, out SemanticCandidate candidate)
        {
            candidate = default;
            bool found = false;
            for (int i = 0; i < _semanticTargets.Count; i++)
            {
                var target = _semanticTargets[i];
                if (target.Handle != _hoveredHandle || !TryEvaluateSemanticTarget(target, cursor, HoverRetentionExpansionPixels, out var score, out var depth, out var priority, out var isCap))
                    continue;
                var current = new SemanticCandidate(i, target, score, depth, priority, isCap);
                if (!found || CompareSemanticCandidates(current, candidate) < 0)
                {
                    candidate = current;
                    found = true;
                }
            }
            return found;
        }

        private void ApplySemanticCandidate(SemanticCandidate candidate)
        {
            _activeAxis = candidate.Target.Handle.Axis;
            _hoveredHandle = candidate.Target.Handle;
            _hoveredTarget = candidate.Target;
            _hoveredTargetScore = candidate.Score;
            _hasHoveredTarget = true;
        }

        private void SelectSemanticAxis()
        {
            Float2 cursor = Owner.Viewport.ViewMousePosition;
            bool hasBest = TryGetBestSemanticCandidate(cursor, 0.0f, out var best);
            if (_hasHoveredTarget && TryGetRetainedHoverCandidate(cursor, out var retained))
            {
                bool bestIsDifferent = !hasBest || best.Target.Handle != retained.Target.Handle;
                bool retain = !hasBest || !bestIsDifferent ||
                              (retained.Priority >= best.Priority && (retained.Score <= 0.001f || best.Score > retained.Score * 0.8f));
                if (retain)
                    best = retained;
                hasBest = true;
            }

            if (hasBest)
                ApplySemanticCandidate(best);
            else
            {
                _activeAxis = Axis.None;
                _hoveredHandle = SemanticHandle.None;
                _hoveredTarget = default;
                _hoveredTargetScore = float.MaxValue;
                _hasHoveredTarget = false;
            }
        }

    }
}
