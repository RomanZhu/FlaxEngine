// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using FlaxEngine;
using FlaxEngine.GUI;
using EditorUtils = FlaxEditor.Utilities.Utils;

namespace FlaxEditor.Gizmo
{
    /// <summary>
    /// Describes the visual state of a transform handle.
    /// </summary>
    public enum FeedbackHandleState
    {
        /// <summary>
        /// No handle is under the pointer.
        /// </summary>
        Idle,

        /// <summary>
        /// A handle is under the pointer.
        /// </summary>
        Hover,

        /// <summary>
        /// A handle was pressed and is being latched.
        /// </summary>
        Pressed,

        /// <summary>
        /// A handle owns an active pointer drag.
        /// </summary>
        Dragging,

        /// <summary>
        /// The active result is constrained by snapping.
        /// </summary>
        Snapped,

        /// <summary>
        /// The active result is being edited with precision gain.
        /// </summary>
        Precision,

        /// <summary>
        /// The last transaction was cancelled.
        /// </summary>
        Cancelled,
    }

    /// <summary>
    /// Identifies the basis used for a feedback measurement.
    /// </summary>
    public enum FeedbackMeasurementBasis
    {
        /// <summary>
        /// No measurement is available.
        /// </summary>
        None,

        /// <summary>
        /// Net translation from transaction origin.
        /// </summary>
        NetTranslation,

        /// <summary>
        /// Translation projected onto one axis.
        /// </summary>
        AxisTranslation,

        /// <summary>
        /// Translation projected onto two active axes.
        /// </summary>
        PlaneTranslation,

        /// <summary>
        /// Translation on the camera-facing plane.
        /// </summary>
        ViewPlaneTranslation,

        /// <summary>
        /// Signed, unwrapped rotation angle.
        /// </summary>
        SignedRotation,

        /// <summary>
        /// A scale factor without a dimension basis.
        /// </summary>
        ScaleFactor,

        /// <summary>
        /// A local object dimension.
        /// </summary>
        ActiveLocalSize,

        /// <summary>
        /// An aggregate world-space bounds dimension.
        /// </summary>
        AggregateWorldBounds,

        /// <summary>
        /// A group scale factor.
        /// </summary>
        GroupFactor,
    }

    /// <summary>
    /// Identifies a world-space annotation consumed by the feedback renderer.
    /// </summary>
    public enum FeedbackGuideKind
    {
        /// <summary>
        /// An extended translation axis.
        /// </summary>
        TranslationAxis,

        /// <summary>
        /// A finite translation plane patch.
        /// </summary>
        TranslationPlane,

        /// <summary>
        /// A camera-facing free-move plane.
        /// </summary>
        ViewPlane,

        /// <summary>
        /// A scale bounds guide.
        /// </summary>
        ScaleBounds,

        /// <summary>
        /// A source-to-target snap tether.
        /// </summary>
        SnapTether,
    }

    /// <summary>
    /// A single label/value row in the cursor HUD.
    /// </summary>
    public readonly struct FeedbackHudRow
    {
        /// <summary>
        /// Initializes a row.
        /// </summary>
        public FeedbackHudRow(string label, string value)
        {
            Label = label ?? string.Empty;
            Value = value ?? string.Empty;
        }

        /// <summary>
        /// Gets the row label.
        /// </summary>
        public string Label { get; }

        /// <summary>
        /// Gets the row value.
        /// </summary>
        public string Value { get; }
    }

    /// <summary>
    /// A projected world marker used by active-operation feedback.
    /// </summary>
    public readonly struct FeedbackWorldMarker
    {
        /// <summary>
        /// Initializes a marker.
        /// </summary>
        public FeedbackWorldMarker(Vector3 position, Color color, float radius, bool isTarget, string label)
        {
            Position = position;
            Color = color;
            Radius = radius;
            IsTarget = isTarget;
            Label = label ?? string.Empty;
        }

        /// <summary>
        /// Gets the world-space position.
        /// </summary>
        public Vector3 Position { get; }

        /// <summary>
        /// Gets the marker color.
        /// </summary>
        public Color Color { get; }

        /// <summary>
        /// Gets the screen-space marker radius.
        /// </summary>
        public float Radius { get; }

        /// <summary>
        /// Gets a value indicating whether this marker is a snap target.
        /// </summary>
        public bool IsTarget { get; }

        /// <summary>
        /// Gets the optional marker label.
        /// </summary>
        public string Label { get; }
    }

    /// <summary>
    /// A world-space guide primitive produced by the interaction layer.
    /// </summary>
    public readonly struct FeedbackWorldGuide
    {
        /// <summary>
        /// Initializes a guide.
        /// </summary>
        public FeedbackWorldGuide(FeedbackGuideKind kind, Vector3 start, Vector3 end, Vector3 secondary, Vector3 normal, float radius, float angle, Color color, bool isOriginal)
        {
            Kind = kind;
            Start = start;
            End = end;
            Secondary = secondary;
            Normal = normal;
            Radius = radius;
            Angle = angle;
            Color = color;
            IsOriginal = isOriginal;
        }

        /// <summary>
        /// Gets the guide kind.
        /// </summary>
        public FeedbackGuideKind Kind { get; }

        /// <summary>
        /// Gets the primary world-space point.
        /// </summary>
        public Vector3 Start { get; }

        /// <summary>
        /// Gets the secondary world-space point or direction endpoint.
        /// </summary>
        public Vector3 End { get; }

        /// <summary>
        /// Gets the second direction endpoint for plane guides.
        /// </summary>
        public Vector3 Secondary { get; }

        /// <summary>
        /// Gets the guide normal or rotation axis.
        /// </summary>
        public Vector3 Normal { get; }

        /// <summary>
        /// Gets the world-space radius for rotation guides.
        /// </summary>
        public float Radius { get; }

        /// <summary>
        /// Gets the signed angle in radians for rotation guides.
        /// </summary>
        public float Angle { get; }

        /// <summary>
        /// Gets the guide color.
        /// </summary>
        public Color Color { get; }

        /// <summary>
        /// Gets a value indicating whether this is the original/ghost guide.
        /// </summary>
        public bool IsOriginal { get; }
    }

    /// <summary>
    /// Describes a semantic snap annotation.
    /// </summary>
    public sealed class FeedbackSnapInfo
    {
        /// <summary>
        /// Initializes snap information.
        /// </summary>
        public FeedbackSnapInfo(object identity, Vector3 sourcePosition, Vector3 targetPosition, string targetName, float value)
        {
            Identity = identity;
            SourcePosition = sourcePosition;
            TargetPosition = targetPosition;
            TargetName = targetName ?? string.Empty;
            Value = value;
        }

        /// <summary>
        /// Gets the stable snap identity.
        /// </summary>
        public object Identity { get; }

        /// <summary>
        /// Gets the source marker position.
        /// </summary>
        public Vector3 SourcePosition { get; }

        /// <summary>
        /// Gets the target marker position.
        /// </summary>
        public Vector3 TargetPosition { get; }

        /// <summary>
        /// Gets the target display name.
        /// </summary>
        public string TargetName { get; }

        /// <summary>
        /// Gets the snap distance or value.
        /// </summary>
        public float Value { get; }
    }

    /// <summary>
    /// Pure data produced by a transform interaction and consumed by gizmo rendering/UI.
    /// </summary>
    public sealed class FeedbackModel
    {
        /// <summary>
        /// Empty feedback for an inactive gizmo.
        /// </summary>
        public static readonly FeedbackModel Empty = new FeedbackModel(
            FeedbackHandleState.Idle,
            SemanticHandle.None,
            InteractionResult.Empty,
            FeedbackMeasurementBasis.None,
            Array.Empty<FeedbackHudRow>(),
            Array.Empty<FeedbackWorldMarker>(),
            Array.Empty<FeedbackWorldGuide>(),
            BoundingBox.Empty,
            BoundingBox.Empty,
            Vector3.Zero,
            Float2.Zero,
            0.0f,
            0.0f,
            false,
            false,
            null,
            string.Empty,
            null);

        /// <summary>
        /// Initializes a feedback model.
        /// </summary>
        public FeedbackModel(FeedbackHandleState state, SemanticHandle activeHandle, InteractionResult result, FeedbackMeasurementBasis measurementBasis, IReadOnlyList<FeedbackHudRow> hudRows, IReadOnlyList<FeedbackWorldMarker> markers, IReadOnlyList<FeedbackWorldGuide> guides, BoundingBox originalBounds, BoundingBox currentBounds, Vector3 pivotPosition, Float2 cursorPosition, float rotationDegrees, float snapStep, bool precisionEnabled, bool snapped, FeedbackSnapInfo snap, string warning, object snapIdentity)
        {
            State = state;
            ActiveHandle = activeHandle;
            Result = result ?? InteractionResult.Empty;
            MeasurementBasis = measurementBasis;
            HudRows = Copy(hudRows);
            Markers = Copy(markers);
            Guides = Copy(guides);
            OriginalBounds = originalBounds;
            CurrentBounds = currentBounds;
            PivotPosition = pivotPosition;
            CursorPosition = cursorPosition;
            RotationDegrees = rotationDegrees;
            SnapStep = snapStep;
            PrecisionEnabled = precisionEnabled;
            IsSnapped = snapped;
            Snap = snap;
            Warning = warning ?? string.Empty;
            SnapIdentity = snapIdentity;
        }

        /// <summary>
        /// Gets the visual handle state.
        /// </summary>
        public FeedbackHandleState State { get; }

        /// <summary>
        /// Gets the active semantic handle.
        /// </summary>
        public SemanticHandle ActiveHandle { get; }

        /// <summary>
        /// Gets the origin-derived interaction result.
        /// </summary>
        public InteractionResult Result { get; }

        /// <summary>
        /// Gets the declared measurement basis.
        /// </summary>
        public FeedbackMeasurementBasis MeasurementBasis { get; }

        /// <summary>
        /// Gets the cursor HUD rows.
        /// </summary>
        public IReadOnlyList<FeedbackHudRow> HudRows { get; }

        /// <summary>
        /// Gets the world markers.
        /// </summary>
        public IReadOnlyList<FeedbackWorldMarker> Markers { get; }

        /// <summary>
        /// Gets the world guides.
        /// </summary>
        public IReadOnlyList<FeedbackWorldGuide> Guides { get; }

        /// <summary>
        /// Gets the original bounds used by scale feedback.
        /// </summary>
        public BoundingBox OriginalBounds { get; }

        /// <summary>
        /// Gets the current bounds used by scale feedback.
        /// </summary>
        public BoundingBox CurrentBounds { get; }

        /// <summary>
        /// Gets the current world-space interaction pivot.
        /// </summary>
        public Vector3 PivotPosition { get; }

        /// <summary>
        /// Gets the pointer position used by HUD placement.
        /// </summary>
        public Float2 CursorPosition { get; }

        /// <summary>
        /// Gets the signed unwrapped rotation in degrees.
        /// </summary>
        public float RotationDegrees { get; }

        /// <summary>
        /// Gets the active snap step in scene units or degrees.
        /// </summary>
        public float SnapStep { get; }

        /// <summary>
        /// Gets a value indicating whether precision gain is active.
        /// </summary>
        public bool PrecisionEnabled { get; }

        /// <summary>
        /// Gets a value indicating whether a snap constraint is active.
        /// </summary>
        public bool IsSnapped { get; }

        /// <summary>
        /// Gets the semantic snap annotation, if any.
        /// </summary>
        public FeedbackSnapInfo Snap { get; }

        /// <summary>
        /// Gets a warning or policy badge.
        /// </summary>
        public string Warning { get; }

        /// <summary>
        /// Gets the raw snap identity exposed by the solver.
        /// </summary>
        public object SnapIdentity { get; }

        /// <summary>
        /// Gets a value indicating whether this model contains active-operation feedback.
        /// </summary>
        public bool IsVisible => State != FeedbackHandleState.Idle;

        private static IReadOnlyList<T> Copy<T>(IReadOnlyList<T> source)
        {
            if (source == null || source.Count == 0)
                return Array.Empty<T>();
            var copy = new T[source.Count];
            for (int i = 0; i < source.Count; i++)
                copy[i] = source[i];
            return Array.AsReadOnly(copy);
        }
    }

    public partial class TransformGizmoBase
    {
        private const float FeedbackPressedDuration = 0.08f;
        private const float FeedbackCancelledDuration = 0.08f;
        private const float FeedbackHudOffset = 20.0f;
        private const float FeedbackHudMargin = 12.0f;
        private const float FeedbackHudRowHeight = 17.0f;

        private FeedbackModel _feedback = FeedbackModel.Empty;
        private float _pressedFeedbackTime;
        private float _cancelledFeedbackTime;
        private SemanticHandle _cancelledFeedbackHandle = SemanticHandle.None;
        private FeedbackHudQuadrant _feedbackHudQuadrant = FeedbackHudQuadrant.UpperRight;
        private bool _feedbackHudQuadrantValid;

        /// <summary>
        /// Gets the pure active-operation feedback model.
        /// </summary>
        public FeedbackModel Feedback
        {
            get
            {
                UpdateFeedbackModel();
                return _feedback;
            }
        }

        /// <summary>
        /// Gets the current visual state of the active or hovered handle.
        /// </summary>
        public FeedbackHandleState HandleState => Feedback.State;

        private enum FeedbackHudQuadrant
        {
            UpperRight,
            UpperLeft,
            LowerRight,
            LowerLeft,
        }

        private void UpdateFeedbackModel(FeedbackHandleState? forcedState = null)
        {
            if (forcedState == null && _cancelledFeedbackTime > 0.0f && _feedback.State == FeedbackHandleState.Cancelled)
                return;
            _feedback = BuildFeedbackModel(forcedState);
        }

        private FeedbackModel BuildFeedbackModel(FeedbackHandleState? forcedState)
        {
            var owner = Owner;
            bool activeTransaction = HasActiveTransaction || _isTransforming;
            bool cancelled = forcedState == FeedbackHandleState.Cancelled;
            var transaction = _transactionOrigin;
            var result = _interactionResult ?? InteractionResult.Empty;
            var mode = transaction != null ? transaction.InitialMode : _activeMode;
            var axis = cancelled ? _cancelledFeedbackHandle.Axis : activeTransaction ? _latchedHandle.Axis : _activeAxis;
            var pivot = GetFeedbackPivot(result, transaction);
            var currentBounds = GetFeedbackCurrentBounds(activeTransaction || cancelled);
            var originalBounds = transaction != null ? transaction.OriginalBounds : currentBounds;
            bool precision = activeTransaction && owner != null && owner.IsAltKeyDown;
            bool snapped = activeTransaction && IsFeedbackSnapActive(mode);
            float snapStep = snapped && !_geometrySnapTargetValid ? GetFeedbackSnapStep(mode) : 0.0f;
            FeedbackHandleState state = forcedState ?? GetFeedbackHandleState(activeTransaction, axis, precision, snapped);

            var handle = SemanticHandle.None;
            if (axis != Axis.None)
            {
                handle = new SemanticHandle(mode, axis);
                var direction = GetFeedbackHandleDirection(axis, mode);
                Float2 screenPosition = Float2.Zero;
                float depth = 0.0f;
                if (owner?.Viewport != null)
                {
                    owner.Viewport.ProjectPoint(pivot, out screenPosition);
                    depth = (float)Vector3.Dot(pivot - owner.ViewPosition, (Vector3)owner.ViewDirection);
                }
                handle = handle.WithDisplayGeometry(pivot, direction, screenPosition, depth, !activeTransaction || axis == _latchedHandle.Axis);
            }

            var rows = new List<FeedbackHudRow>(8);
            var markers = new List<FeedbackWorldMarker>(8);
            var guides = new List<FeedbackWorldGuide>(8);
            FeedbackMeasurementBasis basis = FeedbackMeasurementBasis.None;
            BuildFeedbackGeometry(mode, axis, result, pivot, originalBounds, currentBounds, snapStep, activeTransaction || cancelled, rows, markers, guides, ref basis);

            if (_geometrySnapTargetValid)
                markers.Add(new FeedbackWorldMarker(_geometrySnapTarget, new Color(1.0f, 0.9f, 0.04f, 1.0f), 4.0f, false, "Surface"));

            FeedbackSnapInfo snap = BuildFeedbackSnapInfo();
            if (snap != null)
            {
                markers.Add(new FeedbackWorldMarker(snap.SourcePosition, new Color(0.0f, 0.95f, 1.0f, 1.0f), 4.0f, false, "Source"));
                markers.Add(new FeedbackWorldMarker(snap.TargetPosition, new Color(1.0f, 0.1f, 0.82f, 1.0f), 4.0f, true, snap.TargetName));
                guides.Add(new FeedbackWorldGuide(FeedbackGuideKind.SnapTether, snap.SourcePosition, snap.TargetPosition, Vector3.Zero, Vector3.Zero, 0.0f, 0.0f, new Color(1.0f, 0.35f, 0.9f, 1.0f), false));
                rows.Add(new FeedbackHudRow("Snap", string.IsNullOrEmpty(snap.TargetName) ? FormatDistance(snap.Value, snapStep) : snap.TargetName));
                if (!string.IsNullOrEmpty(snap.TargetName))
                    rows.Add(new FeedbackHudRow("Value", FormatDistance(snap.Value, snapStep)));
            }

            if (precision)
                rows.Add(new FeedbackHudRow("Precision", "0.5x"));
            if (snapStep > Mathf.Epsilon && snapped)
                rows.Add(new FeedbackHudRow("Step", FormatFeedbackStep(mode, snapStep)));

            string warning = GetFeedbackWarning(mode, result);
            if (!string.IsNullOrEmpty(warning))
                rows.Add(new FeedbackHudRow("Warning", warning));
            if (cancelled)
                rows.Insert(0, new FeedbackHudRow("Status", "Canceled"));

            Float2 cursorPosition = owner?.Viewport != null ? owner.Viewport.ViewMousePosition : Float2.Zero;
            object snapIdentity = snap?.Identity ?? _geometrySnapTargetNode ?? result.SnapIdentity;
            return new FeedbackModel(state, handle, result, basis, rows, markers, guides, originalBounds, currentBounds, pivot, cursorPosition, _rotationAccumulatedAngle * Mathf.RadiansToDegrees, snapStep, precision, snapped, snap, warning, snapIdentity);
        }

        private FeedbackHandleState GetFeedbackHandleState(bool activeTransaction, Axis axis, bool precision, bool snapped)
        {
            if (!activeTransaction)
                return axis == Axis.None ? FeedbackHandleState.Idle : FeedbackHandleState.Hover;
            if (_interactionState == InteractionState.Armed || _pressedFeedbackTime > 0.0f)
                return FeedbackHandleState.Pressed;
            if (precision)
                return FeedbackHandleState.Precision;
            if (snapped)
                return FeedbackHandleState.Snapped;
            return FeedbackHandleState.Dragging;
        }

        private Vector3 GetFeedbackPivot(InteractionResult result, TransactionOrigin transaction)
        {
            var origin = transaction != null ? transaction.PivotPosition : Position - result.Translation;
            return origin + result.Translation;
        }

        private BoundingBox GetFeedbackCurrentBounds(bool active)
        {
            if (!active || SelectionCount == 0)
                return BoundingBox.Empty;
            try
            {
                GetSelectedObjectsBounds(out var bounds, out _);
                return bounds;
            }
            catch
            {
                return BoundingBox.Empty;
            }
        }

        private Vector3 GetFeedbackHandleDirection(Axis axis, Mode mode)
        {
            if (axis == Axis.Center)
                return Owner != null ? (Vector3)Owner.ViewDirection : Vector3.Forward;
            if (axis == Axis.Screen)
                return Owner != null ? (Vector3)Owner.ViewDirection : Vector3.Forward;
            if (!TryGetFeedbackAxisLocal(axis, out var local))
                return Vector3.Zero;
            var basis = _transactionOrigin != null ? _transactionOrigin.InitialBasis : _gizmoWorld.Orientation;
            var world = local * basis;
            if (world.LengthSquared > 0.0001f)
                world.Normalize();
            return world;
        }

        private static bool TryGetFeedbackAxisLocal(Axis axis, out Vector3 direction)
        {
            if (TryGetBoundsFace(axis, out _, out _, out direction))
                return true;
            switch (axis)
            {
            case Axis.X:
                direction = Vector3.UnitX;
                return true;
            case Axis.Y:
                direction = Vector3.UnitY;
                return true;
            case Axis.Z:
                direction = Vector3.UnitZ;
                return true;
            default:
                direction = Vector3.Zero;
                return false;
            }
        }

        private bool IsFeedbackSnapActive(Mode mode)
        {
            if (_vertexSnapObjectTo != null || _geometrySnapTargetValid)
                return true;
            if (Owner == null)
                return false;
            switch (mode)
            {
            case Mode.Translate:
                return TranslationSnapEnable || Owner.UseSnapping;
            case Mode.Rotate:
                return RotationSnapEnabled || Owner.UseSnapping;
            case Mode.Scale:
            case Mode.Bounds:
                return ScaleSnapEnabled || Owner.UseSnapping;
            default:
                return false;
            }
        }

        private float GetFeedbackSnapStep(Mode mode)
        {
            float value;
            switch (mode)
            {
            case Mode.Translate:
                value = TranslationSnapValue;
                break;
            case Mode.Rotate:
                value = RotationSnapValue;
                break;
            case Mode.Scale:
            case Mode.Bounds:
                value = TranslationSnapValue;
                break;
            default:
                return 0.0f;
            }
            return Mathf.Abs(value);
        }

        private bool IsFeedbackAbsoluteSnap(Mode mode)
        {
            if (ActiveTransformSpace != TransformSpace.World || Owner == null)
                return false;
            switch (mode)
            {
            case Mode.Translate:
                return AbsoluteSnapEnabled;
            case Mode.Rotate:
                return AbsoluteSnapEnabled || RotationSnapEnabled;
            case Mode.Scale:
            case Mode.Bounds:
                return false;
            default:
                return false;
            }
        }

        private FeedbackSnapInfo BuildFeedbackSnapInfo()
        {
            if (_vertexSnapObject == null || _vertexSnapObjectTo == null)
                return null;
            try
            {
                var source = _vertexSnapObject.Transform.LocalToWorld(_vertexSnapPoint);
                var target = _vertexSnapObjectTo.Transform.LocalToWorld(_vertexSnapPointTo);
                return new FeedbackSnapInfo(_vertexSnapObjectTo, source, target, _vertexSnapObjectTo.Name, (float)Vector3.Distance(source, target));
            }
            catch
            {
                return null;
            }
        }

        private void BuildFeedbackGeometry(Mode mode, Axis axis, InteractionResult result, Vector3 pivot, BoundingBox originalBounds, BoundingBox currentBounds, float snapStep, bool active, List<FeedbackHudRow> rows, List<FeedbackWorldMarker> markers, List<FeedbackWorldGuide> guides, ref FeedbackMeasurementBasis basis)
        {
            if (!active)
                return;

            var origin = _transactionOrigin != null ? _transactionOrigin.PivotPosition : pivot - result.Translation;
            bool hasMoved = mode == Mode.Translate
                ? result.Translation.LengthSquared > 0.000001f
                : (mode == Mode.Scale || mode == Mode.Bounds) && (result.Scale - Vector3.One).LengthSquared > 0.000001f;
            if (hasMoved && !IsGeometrySnapActive)
                markers.Add(new FeedbackWorldMarker(origin, Color.White, 4.0f, false, "Origin"));
            if (axis == Axis.None)
            {
                switch (mode)
                {
                case Mode.Translate:
                    rows.Add(new FeedbackHudRow("Move", FormatDistance((float)result.Translation.Length, snapStep)));
                    basis = FeedbackMeasurementBasis.NetTranslation;
                    break;
                case Mode.Rotate:
                    rows.Add(new FeedbackHudRow("Rotation", FormatSignedAngle(_rotationAccumulatedAngle * Mathf.RadiansToDegrees, snapStep)));
                    basis = FeedbackMeasurementBasis.SignedRotation;
                    break;
                case Mode.Scale:
                case Mode.Bounds:
                    rows.Add(new FeedbackHudRow("Scale", FormatFactor((float)result.Scale.X, snapStep)));
                    basis = FeedbackMeasurementBasis.ScaleFactor;
                    break;
                }
                return;
            }

            var activeColor = GetFeedbackAxisColor(axis);
            switch (mode)
            {
            case Mode.Translate:
                if (IsTranslateAxis(axis))
                {
                    var direction = GetFeedbackHandleDirection(axis, mode);
                    var current = pivot;
                    float value = (float)Vector3.Dot(result.Translation, direction);
                    guides.Add(new FeedbackWorldGuide(FeedbackGuideKind.TranslationAxis, origin, current, Vector3.Zero, direction, 0.0f, 0.0f, activeColor, false));
                    rows.Add(new FeedbackHudRow(axis.ToString(), FormatSignedDistance(value, snapStep)));
                    basis = FeedbackMeasurementBasis.AxisTranslation;
                }
                else if (axis == Axis.Center)
                {
                    rows.Add(new FeedbackHudRow("Move", FormatDistance((float)result.Translation.Length, snapStep)));
                    basis = FeedbackMeasurementBasis.ViewPlaneTranslation;
                }
                else if (TryGetFeedbackPlaneAxes(axis, out var axisA, out var axisB))
                {
                    var first = GetFeedbackHandleDirection(axisA, mode);
                    var second = GetFeedbackHandleDirection(axisB, mode);
                    guides.Add(new FeedbackWorldGuide(FeedbackGuideKind.TranslationAxis, origin, pivot, Vector3.Zero, Vector3.Zero, 0.0f, 0.0f, new Color(1.0f, 0.9f, 0.04f, 0.78f), false));
                    if (TryGetPlaneHandleWorldCorners(axis, out var plane0, out var plane1, out var plane2, out _))
                        guides.Add(new FeedbackWorldGuide(FeedbackGuideKind.TranslationPlane, plane0, plane1, plane2 - plane0, Vector3.Cross(first, second), 0.0f, 0.0f, new Color(1.0f, 0.9f, 0.04f, 1.0f), false));
                    rows.Add(new FeedbackHudRow(axisA.ToString(), FormatSignedDistance((float)Vector3.Dot(result.Translation, first), snapStep)));
                    rows.Add(new FeedbackHudRow(axisB.ToString(), FormatSignedDistance((float)Vector3.Dot(result.Translation, second), snapStep)));
                    basis = FeedbackMeasurementBasis.PlaneTranslation;
                }
                break;

            case Mode.Rotate:
                if (axis == Axis.Center)
                    break;
                if (IsTranslateAxis(axis) && snapStep > Mathf.Epsilon && IsFeedbackAbsoluteSnap(mode))
                {
                    var orientation = GetSelectedTransform(0).Orientation.EulerAngles;
                    rows.Add(new FeedbackHudRow(axis + " rotation", FormatAngle(GetFeedbackWorldComponent((Vector3)orientation, axis), snapStep)));
                }
                else
                {
                    rows.Add(new FeedbackHudRow(axis == Axis.Screen ? "View" : axis.ToString(), FormatSignedAngle(_rotationAccumulatedAngle * Mathf.RadiansToDegrees, snapStep)));
                }
                basis = FeedbackMeasurementBasis.SignedRotation;
                break;

            case Mode.Scale:
            case Mode.Bounds:
                var factor = result.Scale;
                string factorText;
                if (axis == Axis.Center)
                    factorText = FormatFactor((float)factor.X);
                else if (TryGetFeedbackPlaneAxes(axis, out var scaleAxisA, out var scaleAxisB))
                    factorText = scaleAxisA + " " + FormatFactor(GetFeedbackScaleComponent(factor, scaleAxisA)) + "  " + scaleAxisB + " " + FormatFactor(GetFeedbackScaleComponent(factor, scaleAxisB));
                else if (TryGetFeedbackAxisLocal(axis, out var scaleLocal))
                {
                    Vector3 displayedScale = snapStep > Mathf.Epsilon && IsFeedbackAbsoluteSnap(mode) ? (Vector3)GetSelectedTransform(0).Scale : factor;
                    float component = (float)(scaleLocal.X != 0.0f ? displayedScale.X : scaleLocal.Y != 0.0f ? displayedScale.Y : displayedScale.Z);
                    factorText = FormatFactor(component);
                }
                else
                    factorText = FormatFactor((float)factor.X);
                string scaleLabel = IsTranslateAxis(axis) && snapStep > Mathf.Epsilon && IsFeedbackAbsoluteSnap(mode) ? axis + " scale" : axis == Axis.Center ? "Scale" : axis.ToString();
                rows.Add(new FeedbackHudRow(scaleLabel, factorText));
                if (!IsPlaneAxis(axis) && IsValidFeedbackBounds(originalBounds) && IsValidFeedbackBounds(currentBounds))
                {
                    Vector3 size = currentBounds.Size;
                    rows.Add(new FeedbackHudRow("Size", FormatDistance((float)size.X, 0.0f) + " × " + FormatDistance((float)size.Y, 0.0f) + " × " + FormatDistance((float)size.Z, 0.0f)));
                    guides.Add(new FeedbackWorldGuide(FeedbackGuideKind.ScaleBounds, currentBounds.Center, currentBounds.Center, Vector3.Zero, Vector3.Zero, 0.0f, 0.0f, activeColor.AlphaMultiplied(0.55f), false));
                    basis = FeedbackMeasurementBasis.ScaleFactor;
                }
                else
                {
                    basis = FeedbackMeasurementBasis.ScaleFactor;
                }
                break;
            }
        }

        private static float GetFeedbackScaleComponent(Vector3 factor, Axis axis)
        {
            switch (axis)
            {
            case Axis.X:
                return (float)factor.X;
            case Axis.Y:
                return (float)factor.Y;
            case Axis.Z:
                return (float)factor.Z;
            default:
                return 1.0f;
            }
        }

        private static float GetFeedbackWorldComponent(Vector3 value, Axis axis)
        {
            switch (axis)
            {
            case Axis.X:
                return (float)value.X;
            case Axis.Y:
                return (float)value.Y;
            case Axis.Z:
                return (float)value.Z;
            default:
                return 0.0f;
            }
        }

        private static bool TryGetFeedbackPlaneAxes(Axis axis, out Axis first, out Axis second)
        {
            switch (axis)
            {
            case Axis.XY:
                first = Axis.X;
                second = Axis.Y;
                return true;
            case Axis.ZX:
                first = Axis.Z;
                second = Axis.X;
                return true;
            case Axis.YZ:
                first = Axis.Y;
                second = Axis.Z;
                return true;
            default:
                first = Axis.None;
                second = Axis.None;
                return false;
            }
        }

        private static bool IsValidFeedbackBounds(BoundingBox bounds)
        {
            return bounds.Minimum.X <= bounds.Maximum.X && bounds.Minimum.Y <= bounds.Maximum.Y && bounds.Minimum.Z <= bounds.Maximum.Z;
        }

        private static Color GetFeedbackAxisColor(Axis axis)
        {
            if (axis == Axis.XY)
                return new Color(1.0f, 0.05f, 0.05f, 1.0f);
            if (axis == Axis.ZX)
                return new Color(0.1f, 0.4f, 1.0f, 1.0f);
            if (axis == Axis.YZ)
                return new Color(0.2f, 1.0f, 0.1f, 1.0f);
            if (axis == Axis.XNegative || axis == Axis.XPositive)
                return new Color(1.0f, 0.05f, 0.05f, 1.0f);
            if (axis == Axis.YNegative || axis == Axis.YPositive)
                return new Color(0.2f, 1.0f, 0.1f, 1.0f);
            if (axis == Axis.ZNegative || axis == Axis.ZPositive)
                return new Color(0.1f, 0.4f, 1.0f, 1.0f);
            if ((axis & Axis.X) == Axis.X)
                return new Color(1.0f, 0.05f, 0.05f, 1.0f);
            if ((axis & Axis.Y) == Axis.Y)
                return new Color(0.2f, 1.0f, 0.1f, 1.0f);
            if ((axis & Axis.Z) == Axis.Z)
                return new Color(0.1f, 0.4f, 1.0f, 1.0f);
            return Color.White;
        }

        private string GetFeedbackWarning(Mode mode, InteractionResult result)
        {
            if (mode != Mode.Scale && mode != Mode.Bounds)
                return string.Empty;
            var scale = result.Scale;
            if (Mathf.Abs(scale.X) <= 0.00011f || Mathf.Abs(scale.Y) <= 0.00011f || Mathf.Abs(scale.Z) <= 0.00011f)
                return "Scale clamped";
            return string.Empty;
        }

        private static string FormatDistance(float value, float snapStep)
        {
            return EditorUtils.FormatFloat(RoundFeedbackValue(value, snapStep), FlaxEngine.Utils.ValueCategory.Distance);
        }

        private static string FormatSignedDistance(float value, float snapStep)
        {
            var text = FormatDistance(value, snapStep);
            return value >= 0.0f && !text.StartsWith("+", StringComparison.Ordinal) ? "+" + text : text;
        }

        private static string FormatSignedAngle(float value, float snapStep)
        {
            var text = FormatAngle(value, snapStep);
            return value >= 0.0f && !text.StartsWith("+", StringComparison.Ordinal) ? "+" + text : text;
        }

        private static string FormatAngle(float value, float snapStep)
        {
            return EditorUtils.FormatFloat(RoundFeedbackValue(value, snapStep), FlaxEngine.Utils.ValueCategory.Angle);
        }

        private static string FormatFactor(float value, float snapStep = 0.0f)
        {
            value = RoundFeedbackValue(value, snapStep);
            return "×" + value.ToString("0.######", CultureInfo.InvariantCulture);
        }

        private static float RoundFeedbackValue(float value, float snapStep)
        {
            snapStep = Mathf.Abs(snapStep);
            if (snapStep <= Mathf.Epsilon)
            {
                float magnitude = Mathf.Abs(value);
                if (magnitude <= Mathf.Epsilon)
                    return 0.0f;
                int displayDecimals = Math.Max(0, Math.Min(6, 4 - (int)Math.Floor(Math.Log10(magnitude))));
                return (float)Math.Round(value, displayDecimals, MidpointRounding.AwayFromZero);
            }
            int stepDecimals = 0;
            double scaledStep = snapStep;
            while (stepDecimals < 6 && Math.Abs(scaledStep - Math.Round(scaledStep)) > 0.000001)
            {
                scaledStep *= 10.0;
                stepDecimals++;
            }
            return (float)Math.Round(value, stepDecimals, MidpointRounding.AwayFromZero);
        }

        private static string FormatFeedbackStep(Mode mode, float step)
        {
            return mode == Mode.Rotate ? EditorUtils.FormatFloat(step, FlaxEngine.Utils.ValueCategory.Angle) : (mode == Mode.Scale || mode == Mode.Bounds) ? FormatFactor(step, step) : FormatDistance(step, step);
        }

        private void DrawFeedbackOverlay()
        {
            if (!_isActive || !IsInteractionActive)
                return;
            var owner = Owner;
            var viewport = owner?.Viewport;
            var feedback = Feedback;
            if (viewport == null || !feedback.IsVisible || feedback.State == FeedbackHandleState.Hover || feedback.State == FeedbackHandleState.Idle)
                return;

            var features = Render2D.Features;
            Render2D.Features = features & ~Render2D.RenderingFeatures.VertexSnapping;
            try
            {
                DrawFeedbackGuides(feedback);
                DrawFeedbackMarkers(feedback);
                DrawFeedbackHud(feedback);
            }
            finally
            {
                Render2D.Features = features;
            }
        }

        private bool ShouldDrawFeedbackHandle(Axis handle)
        {
            if (handle == Axis.Center && IsConstrainedSupplementalTranslation)
                return false;
            if (!HasActiveTransaction)
                return true;

            // Rotation is easier to read when the complete orientation frame remains
            // visible. The active ring is already distinguished by its focus material.
            if ((_latchedHandle.IsValid ? _latchedHandle.Mode : _activeMode) == Mode.Rotate)
                return true;

            Axis active = _latchedHandle.IsValid ? _latchedHandle.Axis : _activeAxis;
            if (handle == active)
                return true;
            if ((_activeMode == Mode.Translate || _activeMode == Mode.Scale) && TryGetFeedbackPlaneAxes(active, out var first, out var second))
                return handle == first || handle == second;
            return false;
        }

        private void DrawFeedbackGuides(FeedbackModel feedback)
        {
            for (int i = 0; i < feedback.Guides.Count; i++)
            {
                var guide = feedback.Guides[i];
                switch (guide.Kind)
                {
                case FeedbackGuideKind.TranslationAxis:
                    // The mirrored 3D handle is the arrowhead; this line is its shaft.
                    DrawFeedbackLine(guide.Start, guide.End, guide.Color, false, false);
                    break;
                case FeedbackGuideKind.SnapTether:
                    DrawFeedbackLine(guide.Start, guide.End, guide.Color, true, false);
                    break;
                case FeedbackGuideKind.TranslationPlane:
                case FeedbackGuideKind.ViewPlane:
                    DrawFeedbackPlane(guide);
                    break;
                case FeedbackGuideKind.ScaleBounds:
                    DrawFeedbackBounds(feedback, guide.Color);
                    break;
                }
            }
        }

        private void DrawFeedbackLine(Vector3 start, Vector3 end, Color color, bool dashed, bool arrow)
        {
            if (!TryProjectGizmoPoint(start, out var startScreen) || !TryProjectGizmoPoint(end, out var endScreen))
                return;
            var line = endScreen - startScreen;
            if (line.LengthSquared < 1.0f)
                return;
            Render2D.DrawLine(startScreen, endScreen, Color.Black.AlphaMultiplied(color.A * 0.65f), 4.0f);
            if (dashed)
                DrawTranslationDistanceDashLine(startScreen, endScreen, color, 4.0f, 4.0f);
            else
                Render2D.DrawLine(startScreen, endScreen, color, 2.0f);
            if (arrow)
            {
                var direction = line / line.Length;
                var perpendicular = new Float2(-direction.Y, direction.X);
                var arrowBase = endScreen - direction * 11.0f;
                var arrowLeft = arrowBase + perpendicular * 5.0f;
                var arrowRight = arrowBase - perpendicular * 5.0f;
                Render2D.DrawLine(arrowLeft, endScreen, Color.Black.AlphaMultiplied(0.7f), 4.0f);
                Render2D.DrawLine(arrowRight, endScreen, Color.Black.AlphaMultiplied(0.7f), 4.0f);
                Render2D.DrawLine(arrowLeft, endScreen, color, 2.0f);
                Render2D.DrawLine(arrowRight, endScreen, color, 2.0f);
            }
        }

        private static void DrawTranslationDistanceDashLine(Float2 start, Float2 end, Color color, float dashLength, float gapLength)
        {
            Float2 line = end - start;
            float length = line.Length;
            if (length < 1.0f)
                return;
            Float2 direction = line / length;
            for (float distance = 0.0f; distance < length; distance += dashLength + gapLength)
            {
                Float2 dashStart = start + direction * distance;
                Float2 dashEnd = start + direction * Mathf.Min(distance + dashLength, length);
                Render2D.DrawLine(dashStart, dashEnd, color, 1.5f);
            }
        }

        private void DrawFeedbackPlane(FeedbackWorldGuide guide)
        {
            Vector3 p0 = guide.Start;
            Vector3 p1 = guide.End;
            Vector3 p2 = guide.Start + guide.Secondary;
            Vector3 p3 = guide.End + guide.Secondary;
            if (!TryProjectGizmoPoint(p0, out var screen0) ||
                !TryProjectGizmoPoint(p1, out var screen1) ||
                !TryProjectGizmoPoint(p2, out var screen2) ||
                !TryProjectGizmoPoint(p3, out var screen3))
                return;

            if (guide.Kind == FeedbackGuideKind.TranslationPlane)
            {
                DrawScreenRectangleOutline(screen0, screen1, screen3, screen2, guide.Color, 2.0f);
                return;
            }

            var fill = new[] { screen0, screen1, screen3, screen0, screen3, screen2 };
            Render2D.FillTriangles(fill, guide.Color.AlphaMultiplied(0.16f));
            Render2D.DrawLine(screen0, screen1, guide.Color, 1.5f);
            Render2D.DrawLine(screen1, screen3, guide.Color, 1.5f);
            Render2D.DrawLine(screen3, screen2, guide.Color, 1.5f);
            Render2D.DrawLine(screen2, screen0, guide.Color, 1.5f);

            // Keep the active plane legible without competing with the solid gizmo.
            for (int i = 1; i < 4; i++)
            {
                float alpha = i / 4.0f;
                var rowStart = screen0 + (screen2 - screen0) * alpha;
                var rowEnd = screen1 + (screen3 - screen1) * alpha;
                var columnStart = screen0 + (screen1 - screen0) * alpha;
                var columnEnd = screen2 + (screen3 - screen2) * alpha;
                var gridColor = guide.Color.AlphaMultiplied(0.38f);
                Render2D.DrawLine(rowStart, rowEnd, gridColor, 1.0f);
                Render2D.DrawLine(columnStart, columnEnd, gridColor, 1.0f);
            }
            if (guide.Kind == FeedbackGuideKind.ViewPlane)
            {
                Render2D.DrawLine(screen0, screen3, guide.Color, 1.0f);
                Render2D.DrawLine(screen1, screen2, guide.Color.AlphaMultiplied(0.65f), 1.0f);
            }
        }

        private void DrawFeedbackBounds(FeedbackModel feedback, Color color)
        {
            DrawProjectedBounds(feedback.CurrentBounds, color, false);
        }

        private void DrawProjectedBounds(BoundingBox bounds, Color color, bool dashed)
        {
            if (!IsValidFeedbackBounds(bounds))
                return;
            var corners = bounds.GetCorners();
            var edges = new[]
            {
                0, 1, 1, 2, 2, 3, 3, 0,
                4, 5, 5, 6, 6, 7, 7, 4,
                0, 4, 1, 5, 2, 6, 3, 7,
            };
            for (int i = 0; i < edges.Length; i += 2)
            {
                if (!TryProjectGizmoPoint(corners[edges[i]], out var start) || !TryProjectGizmoPoint(corners[edges[i + 1]], out var end))
                    continue;
                if (dashed)
                    DrawTranslationDistanceDashLine(start, end, color, 3.0f, 3.0f);
                else
                    Render2D.DrawLine(start, end, color, 1.5f);
            }
        }

        private void DrawFeedbackMarkers(FeedbackModel feedback)
        {
            for (int i = 0; i < feedback.Markers.Count; i++)
            {
                var marker = feedback.Markers[i];
                if (!TryProjectGizmoPoint(marker.Position, out var screen))
                    continue;
                float radius = Mathf.Max(marker.Radius, 2.0f);
                var color = marker.Color;
                if (string.Equals(marker.Label, "Origin", StringComparison.Ordinal))
                {
                    float extent = radius + 2.0f;
                    Render2D.DrawLine(screen - new Float2(extent, 0.0f), screen + new Float2(extent, 0.0f), Color.Black.AlphaMultiplied(0.72f), 3.5f);
                    Render2D.DrawLine(screen - new Float2(0.0f, extent), screen + new Float2(0.0f, extent), Color.Black.AlphaMultiplied(0.72f), 3.5f);
                    Render2D.DrawLine(screen - new Float2(extent, 0.0f), screen + new Float2(extent, 0.0f), color, 1.5f);
                    Render2D.DrawLine(screen - new Float2(0.0f, extent), screen + new Float2(0.0f, extent), color, 1.5f);
                    continue;
                }
                Render2D.DrawLine(screen - new Float2(radius + 2.0f, 0.0f), screen + new Float2(radius + 2.0f, 0.0f), Color.Black.AlphaMultiplied(0.7f), 3.0f);
                Render2D.DrawLine(screen - new Float2(0.0f, radius + 2.0f), screen + new Float2(0.0f, radius + 2.0f), Color.Black.AlphaMultiplied(0.7f), 3.0f);
                if (marker.IsTarget)
                {
                    Render2D.DrawLine(screen - new Float2(radius, radius), screen + new Float2(radius, radius), color, 2.0f);
                    Render2D.DrawLine(screen - new Float2(radius, -radius), screen + new Float2(radius, -radius), color, 2.0f);
                }
                else
                {
                    var rect = new Rectangle(screen - new Float2(radius), new Float2(radius * 2.0f));
                    StyleRendering.FillRoundedRectangle(rect, color, radius);
                }
            }
        }

        private void DrawFeedbackHud(FeedbackModel feedback)
        {
            var viewport = Owner?.Viewport;
            if (viewport == null || feedback.HudRows.Count == 0)
                return;

            var font = Style.Current.FontSmall;
            float labelWidth = 0.0f;
            float valueWidth = 0.0f;
            for (int i = 0; i < feedback.HudRows.Count; i++)
            {
                var row = feedback.HudRows[i];
                labelWidth = Mathf.Max(labelWidth, font.MeasureText(row.Label).X);
                valueWidth = Mathf.Max(valueWidth, font.MeasureText(row.Value).X);
            }
            var size = new Float2(labelWidth + valueWidth + 24.0f, feedback.HudRows.Count * FeedbackHudRowHeight + 12.0f);
            var bounds = GetFeedbackHudBounds(feedback, size);
            StyleRendering.DrawRoundedRectangle(bounds, Color.Black.AlphaMultiplied(0.76f), Color.White.AlphaMultiplied(0.18f), 1.0f, 5.0f);

            float y = bounds.Y + 6.0f;
            for (int i = 0; i < feedback.HudRows.Count; i++)
            {
                var row = feedback.HudRows[i];
                var labelRect = new Rectangle(bounds.X + 8.0f, y, labelWidth + 4.0f, FeedbackHudRowHeight);
                var valueRect = new Rectangle(labelRect.Right + 4.0f, y, valueWidth, FeedbackHudRowHeight);
                Render2D.DrawText(font, row.Label, labelRect, Color.White.AlphaMultiplied(0.72f), TextAlignment.Near, TextAlignment.Center, TextWrapping.NoWrap);
                Render2D.DrawText(font, row.Value, valueRect, Color.White, TextAlignment.Far, TextAlignment.Center, TextWrapping.NoWrap);
                y += FeedbackHudRowHeight;
            }
        }

        private Rectangle GetFeedbackHudBounds(FeedbackModel feedback, Float2 size)
        {
            var viewport = Owner.Viewport;
            var obstacles = new List<Rectangle>(feedback.Markers.Count + 2);
            if (feedback.ActiveHandle.IsValid)
                obstacles.Add(new Rectangle(feedback.ActiveHandle.ScreenPosition - new Float2(18.0f), new Float2(36.0f)));
            for (int i = 0; i < feedback.Markers.Count; i++)
            {
                if (TryProjectGizmoPoint(feedback.Markers[i].Position, out var position))
                    obstacles.Add(new Rectangle(position - new Float2(12.0f), new Float2(24.0f)));
            }
            if (TryGetProjectedBoundsRect(feedback.CurrentBounds, out var boundsRect))
                obstacles.Add(boundsRect.MakeExpanded(4.0f));

            var candidates = new FeedbackHudQuadrant[4]
            {
                FeedbackHudQuadrant.UpperRight,
                FeedbackHudQuadrant.UpperLeft,
                FeedbackHudQuadrant.LowerRight,
                FeedbackHudQuadrant.LowerLeft,
            };
            if (_feedbackHudQuadrantValid)
            {
                var stable = ClampFeedbackHudBounds(CreateFeedbackHudBounds(feedback.CursorPosition, size, _feedbackHudQuadrant), viewport.Width, viewport.Height);
                if (HasActiveTransaction && feedback.ActiveHandle.Mode == Mode.Rotate)
                    return stable;
                if (IsFeedbackHudUsable(stable, obstacles, viewport.Width, viewport.Height))
                    return stable;
            }

            for (int i = 0; i < candidates.Length; i++)
            {
                var candidate = ClampFeedbackHudBounds(CreateFeedbackHudBounds(feedback.CursorPosition, size, candidates[i]), viewport.Width, viewport.Height);
                if (IsFeedbackHudUsable(candidate, obstacles, viewport.Width, viewport.Height))
                {
                    _feedbackHudQuadrant = candidates[i];
                    _feedbackHudQuadrantValid = true;
                    return candidate;
                }
            }

            _feedbackHudQuadrant = FeedbackHudQuadrant.UpperRight;
            _feedbackHudQuadrantValid = true;
            return ClampFeedbackHudBounds(CreateFeedbackHudBounds(feedback.CursorPosition, size, _feedbackHudQuadrant), viewport.Width, viewport.Height);
        }

        private static Rectangle CreateFeedbackHudBounds(Float2 cursor, Float2 size, FeedbackHudQuadrant quadrant)
        {
            switch (quadrant)
            {
            case FeedbackHudQuadrant.UpperLeft:
                return new Rectangle(cursor.X - size.X - FeedbackHudOffset, cursor.Y - size.Y - FeedbackHudOffset, size.X, size.Y);
            case FeedbackHudQuadrant.LowerRight:
                return new Rectangle(cursor.X + FeedbackHudOffset, cursor.Y + FeedbackHudOffset, size.X, size.Y);
            case FeedbackHudQuadrant.LowerLeft:
                return new Rectangle(cursor.X - size.X - FeedbackHudOffset, cursor.Y + FeedbackHudOffset, size.X, size.Y);
            default:
                return new Rectangle(cursor.X + FeedbackHudOffset, cursor.Y - size.Y - FeedbackHudOffset, size.X, size.Y);
            }
        }

        private static Rectangle ClampFeedbackHudBounds(Rectangle bounds, float viewportWidth, float viewportHeight)
        {
            float maxX = Mathf.Max(FeedbackHudMargin, viewportWidth - FeedbackHudMargin - bounds.Width);
            float maxY = Mathf.Max(FeedbackHudMargin, viewportHeight - FeedbackHudMargin - bounds.Height);
            bounds.X = Mathf.Clamp(bounds.X, FeedbackHudMargin, maxX);
            bounds.Y = Mathf.Clamp(bounds.Y, FeedbackHudMargin, maxY);
            return bounds;
        }

        private static bool IsFeedbackHudUsable(Rectangle bounds, List<Rectangle> obstacles, float viewportWidth, float viewportHeight)
        {
            if (bounds.X < FeedbackHudMargin || bounds.Y < FeedbackHudMargin || bounds.Right > viewportWidth - FeedbackHudMargin || bounds.Bottom > viewportHeight - FeedbackHudMargin)
                return false;
            for (int i = 0; i < obstacles.Count; i++)
            {
                if (bounds.Intersects(obstacles[i]))
                    return false;
            }
            return true;
        }

        private bool TryGetProjectedBoundsRect(BoundingBox bounds, out Rectangle result)
        {
            result = default;
            if (!IsValidFeedbackBounds(bounds))
                return false;
            var corners = bounds.GetCorners();
            bool hasPoint = false;
            Float2 minimum = new Float2(float.MaxValue);
            Float2 maximum = new Float2(float.MinValue);
            for (int i = 0; i < corners.Length; i++)
            {
                if (!TryProjectGizmoPoint(corners[i], out var point))
                    continue;
                hasPoint = true;
                minimum.X = Mathf.Min(minimum.X, point.X);
                minimum.Y = Mathf.Min(minimum.Y, point.Y);
                maximum.X = Mathf.Max(maximum.X, point.X);
                maximum.Y = Mathf.Max(maximum.Y, point.Y);
            }
            if (!hasPoint)
                return false;
            result = new Rectangle(minimum, maximum - minimum);
            return true;
        }
    }
}
