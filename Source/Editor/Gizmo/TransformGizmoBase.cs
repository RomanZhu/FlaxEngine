// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
using Mathr = FlaxEngine.Mathd;
#else
using Real = System.Single;
using Mathr = FlaxEngine.Mathf;
#endif

using System;
using System.Collections.Generic;
using FlaxEditor.SceneGraph;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    /// <summary>
    /// Base class for transformation gizmos that can be used to select objects and transform them.
    /// </summary>
    /// <seealso cref="FlaxEditor.Gizmo.GizmoBase" />
    [HideInEditor]
    public abstract partial class TransformGizmoBase : GizmoBase
    {
        /// <summary>
        /// The start transforms list cached for selected objects before transformation apply. Can be used to create undo operations.
        /// </summary>
        protected readonly List<Transform> _startTransforms = new List<Transform>();

        /// <summary>
        /// Flag used to indicate that navigation data was modified.
        /// </summary>
        protected bool _navigationDirty;

        /// <summary>
        /// The initial world bounds of the selected objects before performing any transformations. Used to find the dirty volume of the world during editing.
        /// </summary>
        protected BoundingBox _startBounds = BoundingBox.Empty;

        private Vector3 _accMoveDelta;
        private Transform _gizmoWorld = Transform.Identity;
        private Vector3 _intersectPosition;
        private bool _isActive;
        private bool _isDuplicating;

        private bool _isTransforming;
        private Vector3 _lastIntersectionPosition;

        private Quaternion _rotationDelta = Quaternion.Identity;
        private Quaternion _rotationGizmoDelta = Quaternion.Identity;
        private float _rotationSnapDelta;
        private float _rotationAccumulatedAngle;
        private float _rotationAnchorAccumulatedAngle;
        private float _rotationPreviousWrappedAngle;
        private float _rotationUnwrappedAngle;
        private bool _rotationSolverInitialized;
        private Vector3 _rotationAnchorPointLocal;
        private Quaternion _rotationSolverBasis = Quaternion.Identity;
        private Float3 _rotationSolverAxisWorld;
        private float _rotationSolverScreenScale;
        private Float2 _rotationScreenDragDirection;
        private float _rotationRadiansPerPixel;
        private bool _rotationScreenDragValid;
        private bool _isDrawingRotationDrag;
        private Vector3 _rotationDragStartPointWorld;
        private Vector3 _rotationDragCurrentPointWorld;
        private Vector3 _rotationDragMousePointWorld;
        private bool _isDrawingTranslationDistance;
        private Vector3 _translationDragStartPosition;
        private Vector3 _scaleDelta;
        private float _screenScale;

        /// <summary>
        /// Allows the transform gizmo to remain interactive as a supplemental manipulator
        /// while another authoring gizmo owns viewport selection and overlays.
        /// </summary>
        internal bool SupplementalActive
        {
            get => _supplementalActive;
            set
            {
                if (_supplementalActive == value)
                    return;
                _supplementalActive = value;
                if (value)
                    EndVertexSnapping();
            }
        }

        private bool _supplementalActive;

        /// <summary>
        /// Enables authoring-mode translation snapping without changing the user's regular transform-gizmo settings.
        /// </summary>
        internal bool SupplementalTranslationSnapEnabled { get; set; }

        /// <summary>
        /// Gets or sets the authoring-mode translation grid size in world units.
        /// </summary>
        internal float SupplementalTranslationSnapValue { get; set; }

        private bool IsInteractionActive => IsActive || SupplementalActive;

        private bool IsConstrainedSupplementalTranslation => SupplementalActive && _activeMode == Mode.Translate;

        private Vector3 _tDelta;
        private Vector3 _translationDelta;
        private Vector3 _translationScaleSnapDelta;
        private Vector3 _translationSnapAppliedTotal;
        private Vector3 _translationSnapAnchorPosition;
        private bool _translationSnapAnchorInitialized;
        private Plane _axisDragPlane;
        private bool _axisDragPlaneValid;
        private bool _axisDragPreviousScalarValid;
        private bool _axisDragReanchorOnNextValid;
        private Real _axisDragPreviousScalar;
        private bool _axisDragScreenFallbackValid;
        private Float2 _axisDragScreenDirection;
        private Real _axisDragWorldUnitsPerPixel;

        private SceneGraphNode _vertexSnapObject, _vertexSnapObjectTo;
        private Vector3 _vertexSnapPoint, _vertexSnapPointTo;
        private Float2 _vertexSnapDragStartMousePosition;
        private bool _isVertexSnapPivotLocked;
        private bool _isVertexSnapTemporaryPivot;
        private bool _isVertexSnapDragPending;
        private bool _wasLeftMouseButtonDown;
        private bool _suppressSelectionRelease;
        private bool _wasSnapToVertex;
        private bool _geometrySnapTargetValid;
        private Vector3 _geometrySnapTarget;
        private SceneGraphNode _geometrySnapTargetNode;
        private const float VertexSnapDragStartDistanceSquared = 16.0f;
        private static readonly SceneGraphNode.RayCastData.FlagTypes VertexSnapRayCastFlags = SceneGraphNode.RayCastData.FlagTypes.None;

        /// <summary>
        /// Gets the gizmo position.
        /// </summary>
        public Vector3 Position { get; private set; }

        /// <summary>
        /// Gets the selected temporary vertex snapping pivot point.
        /// </summary>
        /// <param name="worldPosition">The selected pivot position in world space.</param>
        /// <returns>True if the temporary pivot is selected, otherwise false.</returns>
        public bool TryGetTemporaryVertexSnapPivot(out Vector3 worldPosition)
        {
            if (_isVertexSnapTemporaryPivot && _vertexSnapObject != null)
            {
                worldPosition = _vertexSnapObject.Transform.LocalToWorld(_vertexSnapPoint);
                return true;
            }

            worldPosition = Vector3.Zero;
            return false;
        }

        /// <summary>
        /// Gets the last transformation delta.
        /// </summary>
        public Transform LastDelta { get; private set; }

        /// <summary>
        /// Occurs when transforming selection started.
        /// </summary>
        public event Action TransformingStarted;

        /// <summary>
        /// Occurs when transforming selection ended.
        /// </summary>
        public event Action TransformingEnded;

        /// <summary>
        /// Initializes a new instance of the <see cref="TransformGizmoBase" /> class.
        /// </summary>
        /// <param name="owner">The gizmos owner.</param>
        public TransformGizmoBase(IGizmoOwner owner)
        : base(owner)
        {
            InitDrawing();
            ModeChanged += ResetTranslationScale;
            owner.Undo.UndoDone += OnUndoRedoDone;
            owner.Undo.RedoDone += OnUndoRedoDone;
        }

        /// <inheritdoc />
        public override void Destroy()
        {
            CancelTransforming();
            if (Owner != null)
            {
                Owner.Undo.UndoDone -= OnUndoRedoDone;
                Owner.Undo.RedoDone -= OnUndoRedoDone;
            }
            base.Destroy();
        }

        private void UpdateGizmoPosition()
        {
            var position = Vector3.Zero;

            // Get gizmo pivot
            if (_activeMode == Mode.Bounds)
            {
                position = GetSelectionCenter();
            }
            else switch (_activePivotType)
            {
            case PivotType.ObjectCenter:
                if (SelectionCount > 0)
                    position = GetSelectedTransform(0).Translation;
                break;
            case PivotType.SelectionCenter:
                position = GetSelectionCenter();
                break;
            }

            // Apply vertex snapping
            if (_vertexSnapObject != null)
            {
                Vector3 vertexSnapPoint = _vertexSnapObject.Transform.LocalToWorld(_vertexSnapPoint);
                position += vertexSnapPoint - position;
            }

            // Apply current movement
            position += _translationDelta;

            Position = position;
        }

        private void UpdateMatrices()
        {
            // Check there is no need to perform update
            if (SelectionCount == 0)
            {
                _gizmoProjectionValid = false;
                _semanticTargets.Clear();
                _hoveredHandle = SemanticHandle.None;
                _hoveredTarget = default;
                _hoveredTargetScore = float.MaxValue;
                _hasHoveredTarget = false;
                return;
            }

            // Set positions of the gizmo
            UpdateGizmoPosition();

            // Scale the authored gizmo basis from the active projection. This uses
            // forward depth for perspective views and keeps the logical-to-physical
            // DPI conversion in the projection helper.
            Vector3 position = Position;
            _gizmoProjectionValid = TryGetGizmoWorldRadius(position, out var gizmoWorldRadius);
            _screenScale = _gizmoProjectionValid ? gizmoWorldRadius / GizmoGeometryRadiusRaw : 0.0f;

            // Setup world
            Quaternion orientation = GetSelectedTransform(0).Orientation;
            _gizmoWorld = new Transform(position, orientation, new Float3(_screenScale));
            if (_activeTransformSpace == TransformSpace.World || _activeMode == Mode.Bounds)
            {
                _gizmoWorld.Orientation = Quaternion.Identity;
            }

            RebuildSemanticTargets();
        }

        private bool TrySolveAxisDrag(ref Ray worldRay, ref Matrix rotationMatrix, Vector3 axisLocal, out Vector3 deltaLocal)
        {
            const float minimumRayPlaneDot = 0.10f;
            deltaLocal = Vector3.Zero;
            Vector3 axisWorld = Vector3.TransformNormal(axisLocal, rotationMatrix);
            if (axisWorld.LengthSquared < 0.0001f)
                return false;
            axisWorld.Normalize();

            Vector3 pivot = _transactionOrigin != null ? _transactionOrigin.PivotPosition : Position;
            var interactionAnchor = InteractionAnchor;
            Ray anchorRay = interactionAnchor != null ? interactionAnchor.PointerRay : worldRay;
            if (anchorRay.Direction.LengthSquared < 0.0001f)
                return false;
            anchorRay.Direction.Normalize();

            if (!_axisDragPlaneValid)
            {
                // Freeze one well-conditioned plane for the whole anchor. The
                // previous solver selected between two planes every frame,
                // allowing the result to alternate as the translated pivot
                // crossed their selection threshold.
                Vector3 planeNormal = anchorRay.Direction - axisWorld * Vector3.Dot(anchorRay.Direction, axisWorld);
                if (planeNormal.LengthSquared >= 0.0225f)
                {
                    planeNormal.Normalize();
                }
                else
                {
                    // An axis aimed almost at the camera has no stable projected
                    // line solve. Use the frozen view plane and project its motion
                    // onto the axis; this favors no movement over an unbounded jump.
                    planeNormal = interactionAnchor != null ? interactionAnchor.FallbackPlane.Normal : -(Vector3)Owner.ViewDirection;
                    if (planeNormal.LengthSquared < 0.0001f)
                        planeNormal = Vector3.Forward;
                    else
                        planeNormal.Normalize();
                }
                _axisDragPlane = new Plane(pivot, planeNormal);
                _axisDragPlaneValid = true;

                // Cache a matching screen-space continuation for the small
                // interval where this plane becomes parallel to the mouse ray.
                // This keeps the control moving through the singularity without
                // permitting an unbounded ray/plane intersection.
                _axisDragScreenFallbackValid = false;
                Owner.Viewport.ProjectPoint(pivot, out var pivotScreen);
                Owner.Viewport.ProjectPoint(pivot + axisWorld * _screenScale, out var axisScreenPoint);
                Float2 screenAxis = axisScreenPoint - pivotScreen;
                float screenAxisLength = screenAxis.Length;
                if (screenAxisLength >= 1.0f && _screenScale > Mathf.Epsilon)
                {
                    _axisDragScreenDirection = screenAxis / screenAxisLength;
                    _axisDragWorldUnitsPerPixel = _screenScale / screenAxisLength;
                    _axisDragScreenFallbackValid = true;
                }
            }

            Ray currentRay = worldRay;
            if (currentRay.Direction.LengthSquared < 0.0001f)
                return false;
            currentRay.Direction.Normalize();
            if (Mathf.Abs((float)Vector3.Dot(currentRay.Direction, _axisDragPlane.Normal)) < minimumRayPlaneDot ||
                !currentRay.Intersects(ref _axisDragPlane, out Real currentDistance) || currentDistance < 0.0f)
            {
                _axisDragPreviousScalarValid = false;
                _axisDragReanchorOnNextValid = true;
                if (_axisDragScreenFallbackValid)
                {
                    Real fallbackDelta = Float2.Dot(Owner.MouseDelta, _axisDragScreenDirection) * _axisDragWorldUnitsPerPixel;
                    deltaLocal = axisLocal * fallbackDelta;
                    return IsFinite(deltaLocal);
                }
                return false;
            }

            Vector3 currentPoint = currentRay.GetPoint(currentDistance);
            Real currentScalar = Vector3.Dot(currentPoint - pivot, axisWorld);
            if (!_axisDragPreviousScalarValid)
            {
                Real previousScalar = currentScalar;
                if (!_axisDragReanchorOnNextValid &&
                    Mathf.Abs((float)Vector3.Dot(anchorRay.Direction, _axisDragPlane.Normal)) >= minimumRayPlaneDot &&
                    anchorRay.Intersects(ref _axisDragPlane, out Real anchorDistance) && anchorDistance >= 0.0f)
                {
                    Vector3 anchorPoint = anchorRay.GetPoint(anchorDistance);
                    previousScalar = Vector3.Dot(anchorPoint - pivot, axisWorld);
                }
                _axisDragPreviousScalar = previousScalar;
                _axisDragPreviousScalarValid = true;
                _axisDragReanchorOnNextValid = false;
            }

            Real scalarDelta = currentScalar - _axisDragPreviousScalar;
            _axisDragPreviousScalar = currentScalar;
            _intersectPosition = currentPoint;
            deltaLocal = axisLocal * scalarDelta;
            return IsFinite(deltaLocal);
        }

        private void UpdateTranslateScale()
        {
            bool isScaling = _activeMode == Mode.Scale;
            if (isScaling)
            {
                UpdateScaleFromAnchor();
                return;
            }
            if (UpdateTranslationFromAnchor())
                return;
            if (!IsGeometrySnapActive)
            {
                _translationDelta = Vector3.Zero;
                return;
            }

            bool geometrySnapSolved = false;
            Vector3 delta = Vector3.Zero;
            Ray ray = Owner.MouseRay;
            Ray worldRay = ray;

            Matrix.RotationQuaternion(ref _gizmoWorld.Orientation, out var rotationMatrix);
            Matrix.Invert(ref rotationMatrix, out var invRotationMatrix);
            ray.Position = Vector3.Transform(ray.Position, invRotationMatrix);
            Vector3.TransformNormal(ref ray.Direction, ref invRotationMatrix, out ray.Direction);

            var position = Position;
            var interactionAnchor = InteractionAnchor;
            var planeXY = new Plane(Vector3.Backward, Vector3.Transform(position, invRotationMatrix).Z);
            var planeYZ = new Plane(Vector3.Left, Vector3.Transform(position, invRotationMatrix).X);
            var planeZX = new Plane(Vector3.Down, Vector3.Transform(position, invRotationMatrix).Y);

            Real intersection;
            switch (_activeAxis)
            {
            case Axis.X:
            {
                TrySolveAxisDrag(ref worldRay, ref rotationMatrix, Vector3.UnitX, out delta);
                break;
            }
            case Axis.Y:
            {
                TrySolveAxisDrag(ref worldRay, ref rotationMatrix, Vector3.UnitY, out delta);
                break;
            }
            case Axis.Z:
            {
                TrySolveAxisDrag(ref worldRay, ref rotationMatrix, Vector3.UnitZ, out delta);
                break;
            }
            case Axis.YZ:
            {
                if (ray.Intersects(ref planeYZ, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref planeYZ, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(0, _tDelta.Y, _tDelta.Z);
                }
                break;
            }
            case Axis.XY:
            {
                if (ray.Intersects(ref planeXY, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref planeXY, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(_tDelta.X, _tDelta.Y, 0);
                }
                break;
            }
            case Axis.ZX:
            {
                if (ray.Intersects(ref planeZX, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref planeZX, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(_tDelta.X, 0, _tDelta.Z);
                }
                break;
            }
            case Axis.Center:
            {
                if (isScaling)
                {
                    float amount = Owner.MouseDelta.X - Owner.MouseDelta.Y;
                    delta = new Vector3(amount);
                }
                else
                {
                    if (TrySolveGeometrySnap(ref invRotationMatrix, out delta))
                    {
                        geometrySnapSolved = true;
                        break;
                    }

                    var viewDirection = (Vector3)Owner.ViewDirection;
                    var plane = new Plane(position, -viewDirection);
                    if (worldRay.Intersects(ref plane, out intersection))
                    {
                        _intersectPosition = worldRay.GetPoint(intersection);
                        if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                        {
                            var anchorRay = interactionAnchor.PointerRay;
                            var anchorPlane = interactionAnchor.FallbackPlane;
                            if (anchorRay.Intersects(ref anchorPlane, out Real anchorIntersection))
                                _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                        }
                        if (!_lastIntersectionPosition.IsZero)
                            _tDelta = _intersectPosition - _lastIntersectionPosition;
                        Vector3.TransformNormal(ref _tDelta, ref invRotationMatrix, out delta);
                    }
                }
                break;
            }
            }

            // Modifiers
            if (isScaling)
                ApplyScaleScreenDirection(ref delta);
            if (isScaling)
                delta *= 0.01f;
            if (Owner.IsAltKeyDown && !geometrySnapSolved)
                delta *= 0.5f;
            if (!geometrySnapSolved && ((isScaling ? ScaleSnapEnabled : TranslationSnapEnable) || Owner.UseSnapping))
            {
                var snapValue = new Vector3(isScaling ? ScaleSnapValue : TranslationSnapValue);

                if (!isScaling && snapValue.X < 0.0f)
                {
                    // Snap to object bounding box
                    GetSelectedObjectsBounds(out var b, out _);
                    if (b.Minimum.X < 0.0f)
                        snapValue.X = (Real)Math.Abs(b.Minimum.X) + b.Maximum.X;
                    else
                        snapValue.X = (Real)b.Minimum.X - b.Maximum.X;
                    if (b.Minimum.Y < 0.0f)
                        snapValue.Y = (Real)Math.Abs(b.Minimum.Y) + b.Maximum.Y;
                    else
                        snapValue.Y = (Real)b.Minimum.Y - b.Maximum.Y;
                    if (b.Minimum.Z < 0.0f)
                        snapValue.Z = (Real)Math.Abs(b.Minimum.Z) + b.Maximum.Z;
                    else
                        snapValue.Z = (Real)b.Minimum.Z - b.Maximum.Z;
                }

                if (!isScaling)
                {
                    // Snap the solved transaction total, not each individual mouse update.
                    // In world space the grid is anchored at the world origin; in local
                    // space the drag distance is anchored at the transaction origin.
                    _translationScaleSnapDelta += delta;
                    Vector3 snappedTotal;
                    if (ActiveTransformSpace == TransformSpace.World)
                    {
                        if (!_translationSnapAnchorInitialized)
                        {
                            _translationSnapAnchorPosition = GetSelectedTransform(0).Translation;
                            _translationSnapAnchorInitialized = true;
                        }
                        Vector3 target = _translationSnapAnchorPosition + _translationScaleSnapDelta;
                        snappedTotal = new Vector3(
                            Mathr.Round(target.X / snapValue.X) * snapValue.X,
                            Mathr.Round(target.Y / snapValue.Y) * snapValue.Y,
                            Mathr.Round(target.Z / snapValue.Z) * snapValue.Z) - _translationSnapAnchorPosition;
                    }
                    else
                    {
                        Vector3 anchorTotal = Vector3.Zero;
                        if (interactionAnchor != null)
                        {
                            Vector3 anchorTranslation = interactionAnchor.Result.Translation;
                            Vector3.TransformNormal(ref anchorTranslation, ref invRotationMatrix, out anchorTotal);
                        }
                        Vector3 targetTotal = anchorTotal + _translationScaleSnapDelta;
                        snappedTotal = new Vector3(
                            Mathr.Round(targetTotal.X / snapValue.X) * snapValue.X,
                            Mathr.Round(targetTotal.Y / snapValue.Y) * snapValue.Y,
                            Mathr.Round(targetTotal.Z / snapValue.Z) * snapValue.Z) - anchorTotal;
                    }
                    ConstrainSnapDeltaToActiveHandle(ref snappedTotal);
                    delta = snappedTotal - _translationSnapAppliedTotal;
                    _translationSnapAppliedTotal = snappedTotal;
                }
                else
                {
                    _translationScaleSnapDelta += delta;
                    Vector3 absoluteDelta = Vector3.Zero;
                    if (ActiveTransformSpace == TransformSpace.World && (AbsoluteSnapEnabled || ScaleSnapEnabled))
                    {
                        Vector3 currentScale = (Vector3)GetSelectedTransform(0).Scale;
                        absoluteDelta = currentScale - new Vector3(
                            Mathr.Round(currentScale.X / snapValue.X) * snapValue.X,
                            Mathr.Round(currentScale.Y / snapValue.Y) * snapValue.Y,
                            Mathr.Round(currentScale.Z / snapValue.Z) * snapValue.Z);
                        ConstrainSnapDeltaToActiveHandle(ref absoluteDelta);
                    }

                    delta = new Vector3(
                        (int)(_translationScaleSnapDelta.X / snapValue.X) * snapValue.X,
                        (int)(_translationScaleSnapDelta.Y / snapValue.Y) * snapValue.Y,
                        (int)(_translationScaleSnapDelta.Z / snapValue.Z) * snapValue.Z);
                    _translationScaleSnapDelta -= delta;
                    delta -= absoluteDelta;
                }
            }

            if (_activeMode == Mode.Translate)
            {
                // Transform (local or world)
                delta = Vector3.Transform(delta, rotationMatrix);
                _translationDelta = delta;
            }
            else if (_activeMode == Mode.Scale)
            {
                // Scale
                _scaleDelta = delta;
            }
        }

        private bool UpdateTranslationFromAnchor()
        {
            var anchor = InteractionAnchor;
            var origin = TransactionOrigin;
            if (anchor == null || origin == null || Owner?.Viewport == null || IsGeometrySnapActive)
                return false;

            Ray currentRay = Owner.MouseRay;
            Vector3 rawDelta;
            Quaternion basis = origin.InitialBasis;
            Vector3 anchorPivot = origin.PivotPosition + anchor.Result.Translation;
            switch (_activeAxis)
            {
            case Axis.X:
            case Axis.Y:
            case Axis.Z:
            {
                Vector3 axisLocal = _activeAxis == Axis.X ? Vector3.UnitX : (_activeAxis == Axis.Y ? Vector3.UnitY : Vector3.UnitZ);
                Vector3 axisWorld = axisLocal * basis;
                // Solve ordinary axis motion in projected screen space. The
                // pointer coordinate is continuous across desktop wraps, so
                // crossing an edge cannot swap the closest-ray branch or flip
                // the translation direction. Retain closest-line math only for
                // axes whose projection is too small to measure reliably.
                if (TryGetProjectedAxis(anchorPivot, axisWorld, out var screenDirection, out Real worldUnitsPerPixel))
                {
                    float pixels = Float2.Dot(Owner.Viewport.ContinuousViewMousePosition - anchor.PointerPosition, screenDirection);
                    rawDelta = axisWorld * (pixels * worldUnitsPerPixel);
                }
                else if (!TrySolveAxisTranslation(anchor.PointerRay, currentRay, anchorPivot, axisWorld, out rawDelta))
                {
                    return false;
                }
                break;
            }
            case Axis.XY:
            case Axis.YZ:
            case Axis.ZX:
            {
                Vector3 normalLocal = _activeAxis == Axis.XY ? Vector3.UnitZ : (_activeAxis == Axis.YZ ? Vector3.UnitX : Vector3.UnitY);
                Vector3 normalWorld = normalLocal * basis;
                var plane = new Plane(anchorPivot, normalWorld);
                if (!TrySolvePlaneTranslation(anchor.PointerRay, currentRay, plane, out rawDelta))
                    return false;
                Vector3 localDelta = rawDelta * Quaternion.Invert(basis);
                if (_activeAxis == Axis.XY)
                    localDelta.Z = 0.0f;
                else if (_activeAxis == Axis.YZ)
                    localDelta.X = 0.0f;
                else
                    localDelta.Y = 0.0f;
                rawDelta = localDelta * basis;
                break;
            }
            case Axis.Center:
            {
                if (!TrySolvePlaneTranslation(anchor.PointerRay, currentRay, anchor.FallbackPlane, out rawDelta))
                    return false;
                break;
            }
            default:
                return false;
            }

            if (Owner.IsAltKeyDown)
                rawDelta *= PrecisionScaleGain;
            Vector3 desired = anchor.Result.Translation + rawDelta;
            if (rawDelta.LengthSquared > 0.00000001f)
                desired = SnapTranslationTotal(desired, origin);
            _translationDelta = desired - InteractionResult.Translation;
            _intersectPosition = origin.PivotPosition + desired;
            return IsFiniteMath(_translationDelta);
        }

        private bool TryGetProjectedAxis(Vector3 pivot, Vector3 axisWorld, out Float2 screenDirection, out Real worldUnitsPerPixel)
        {
            Owner.Viewport.ProjectPoint(pivot, out var start);
            Owner.Viewport.ProjectPoint(pivot + axisWorld * _screenScale, out var end);
            screenDirection = end - start;
            float length = screenDirection.Length;
            if (length < 0.0001f || _screenScale <= Mathf.Epsilon)
            {
                worldUnitsPerPixel = 0.0f;
                return false;
            }
            screenDirection /= length;
            worldUnitsPerPixel = _screenScale / length;
            return true;
        }

        private Vector3 SnapTranslationTotal(Vector3 desired, TransactionOrigin origin)
        {
            bool useSupplementalGrid = IsConstrainedSupplementalTranslation && SupplementalTranslationSnapEnabled;
            if (!useSupplementalGrid && !TranslationSnapEnable && !Owner.UseSnapping)
                return desired;
            Vector3 step = useSupplementalGrid
                ? new Vector3(Mathf.Abs(SupplementalTranslationSnapValue))
                : GetLinearSnapStep(origin);
            // CSG brushes are grid-authored geometry. Snap their movement delta rather than
            // snapping the selection-center pivot, which may legitimately sit on a half-cell.
            bool absolute = useSupplementalGrid ? false : AbsoluteSnapEnabled;
            return SnapTranslationToGrid(desired, origin.PivotPosition, origin.InitialBasis, origin.InitialTransformSpace, _activeAxis, step, absolute);
        }

        private void UpdateScaleFromAnchor()
        {
            var anchor = InteractionAnchor;
            var origin = TransactionOrigin;
            if (anchor == null || origin == null || Owner?.Viewport == null)
                return;

            Float2 pointerDelta = Owner.Viewport.ContinuousViewMousePosition - anchor.PointerPosition;
            float gain = Owner.IsAltKeyDown ? PrecisionScaleGain : 1.0f;
            Vector3 relativeFactors = Vector3.One;
            Vector3 pivot = origin.PivotPosition;
            Quaternion basis = origin.InitialBasis;

            if (_activeAxis == Axis.Center)
            {
                Float2 uniformDirection = new Float2(1.0f, -1.0f);
                uniformDirection.Normalize();
                float factor = SolvePointerScaleFactor(Float2.Dot(pointerDelta, uniformDirection), gain);
                relativeFactors = new Vector3(factor);
            }
            else if (_activeAxis == Axis.X || _activeAxis == Axis.Y || _activeAxis == Axis.Z)
            {
                Vector3 axisLocal = _activeAxis == Axis.X ? Vector3.UnitX : (_activeAxis == Axis.Y ? Vector3.UnitY : Vector3.UnitZ);
                Vector3 axisWorld = axisLocal * basis;
                if (!TryGetProjectedAxis(pivot, axisWorld, out var direction, out _))
                    return;
                float factor = SolvePointerScaleFactor(Float2.Dot(pointerDelta, direction), gain);
                if (_activeAxis == Axis.X)
                    relativeFactors.X = factor;
                else if (_activeAxis == Axis.Y)
                    relativeFactors.Y = factor;
                else
                    relativeFactors.Z = factor;
            }
            else if (_activeAxis == Axis.XY || _activeAxis == Axis.YZ || _activeAxis == Axis.ZX)
            {
                Vector3 firstLocal = _activeAxis == Axis.YZ ? Vector3.UnitY : Vector3.UnitX;
                Vector3 secondLocal = _activeAxis == Axis.XY ? Vector3.UnitY : Vector3.UnitZ;
                if (!TryGetProjectedAxis(pivot, firstLocal * basis, out var firstDirection, out _) ||
                    !TryGetProjectedAxis(pivot, secondLocal * basis, out var secondDirection, out _))
                    return;
                float determinant = firstDirection.X * secondDirection.Y - firstDirection.Y * secondDirection.X;
                if (Mathf.Abs(determinant) < 0.0001f)
                    return;
                float firstPixels = (pointerDelta.X * secondDirection.Y - pointerDelta.Y * secondDirection.X) / determinant;
                float secondPixels = (firstDirection.X * pointerDelta.Y - firstDirection.Y * pointerDelta.X) / determinant;
                float firstFactor = SolvePointerScaleFactor(firstPixels, gain);
                float secondFactor = SolvePointerScaleFactor(secondPixels, gain);
                if (_activeAxis == Axis.XY)
                {
                    relativeFactors.X = firstFactor;
                    relativeFactors.Y = secondFactor;
                }
                else if (_activeAxis == Axis.YZ)
                {
                    relativeFactors.Y = firstFactor;
                    relativeFactors.Z = secondFactor;
                }
                else
                {
                    relativeFactors.X = firstFactor;
                    relativeFactors.Z = secondFactor;
                }
            }
            else
            {
                return;
            }

            Vector3 desired = MultiplyScaleFactors(anchor.Result.Scale, relativeFactors);
            if (pointerDelta.LengthSquared > 0.00000001f && (ScaleSnapEnabled || Owner.UseSnapping))
                desired = SnapScaleFactorsToGrid(desired, origin.OriginalBounds, origin.PivotPosition, basis, _activeAxis, GetLinearSnapStep(origin));
            _scaleDelta = desired - InteractionResult.Scale;
        }

        private Vector3 GetLinearSnapStep(TransactionOrigin origin)
        {
            if (TranslationSnapValue >= 0.0f)
                return new Vector3(Mathf.Abs(TranslationSnapValue));

            Vector3 size = GetBoundsSizeInBasis(origin.OriginalBounds, origin.PivotPosition, origin.InitialBasis);
            return new Vector3(Mathr.Abs(size.X), Mathr.Abs(size.Y), Mathr.Abs(size.Z));
        }

        private bool IsGeometrySnapActive => _activeMode == Mode.Translate && _activeAxis == Axis.Center && Owner != null && Owner.IsShiftDown;

        private bool TrySolveGeometrySnap(ref Matrix invRotationMatrix, out Vector3 delta)
        {
            delta = Vector3.Zero;
            if (!IsGeometrySnapActive)
            {
                ClearGeometrySnapTarget();
                return false;
            }

            ClearGeometrySnapTarget();
            if (!TryFindGeometrySnapTarget(out var targetNode, out var target))
                return true;

            _geometrySnapTargetValid = true;
            _geometrySnapTarget = target;
            _geometrySnapTargetNode = targetNode;
            Vector3 worldDelta = target - Position;
            Vector3.TransformNormal(ref worldDelta, ref invRotationMatrix, out delta);
            return true;
        }

        private bool TryFindGeometrySnapTarget(out SceneGraphNode targetNode, out Vector3 target)
        {
            targetNode = null;
            target = Vector3.Zero;
            if (Owner.SceneGraphRoot == null)
                return false;

            var rayCast = new SceneGraphNode.RayCastData
            {
                Ray = Owner.MouseRay,
                View = Owner.Viewport.ViewRay,
                Flags = VertexSnapRayCastFlags,
            };
            var excludedRoots = new List<SceneGraphNode>(SelectionCount);
            for (int i = 0; i < SelectionCount; i++)
                excludedRoots.Add(GetSelectedObject(i));

            // Use the same screen-space candidate filtering as V-snap. It only
            // admits nodes that expose real snap geometry, avoiding cameras,
            // icons, generic actor bounds, and enclosing helper volumes.
            Real closestScreenDistance = Real.MaxValue;
            Real closestRayDistance = Real.MaxValue;
            TryFindClosestGeometrySnapPoint(Owner.SceneGraphRoot, ref rayCast, excludedRoots, Owner.Viewport, Owner.Viewport.ContinuousViewMousePosition, ref closestScreenDistance, ref closestRayDistance, ref targetNode, ref target);
            return targetNode != null;
        }

        private static void TryFindClosestGeometrySnapPoint(SceneGraphNode node, ref SceneGraphNode.RayCastData ray, List<SceneGraphNode> excludedRoots, FlaxEditor.Viewport.EditorViewport viewport, Float2 mousePosition, ref Real closestScreenDistance, ref Real closestRayDistance, ref SceneGraphNode closestObject, ref Vector3 closestPoint)
        {
            if (node == null || !node.IsActive || IsVertexSnapExcluded(node, excludedRoots))
                return;

            if (node.RayCastSelf(ref ray, out var distance, out _) && distance >= 0.0f &&
                node.OnVertexSnap(ref ray.Ray, distance, viewport, mousePosition, out _, out var screenDistance) &&
                (screenDistance < closestScreenDistance || (screenDistance == closestScreenDistance && distance < closestRayDistance)))
            {
                closestScreenDistance = screenDistance;
                closestRayDistance = distance;
                closestObject = node;
                closestPoint = ray.Ray.GetPoint(distance);
            }

            for (int i = 0; i < node.ChildNodes.Count; i++)
                TryFindClosestGeometrySnapPoint(node.ChildNodes[i], ref ray, excludedRoots, viewport, mousePosition, ref closestScreenDistance, ref closestRayDistance, ref closestObject, ref closestPoint);
        }

        private void ClearGeometrySnapTarget()
        {
            _geometrySnapTargetValid = false;
            _geometrySnapTarget = Vector3.Zero;
            _geometrySnapTargetNode = null;
        }

        private void ResetTranslationScale()
        {
            ClearTransformInteraction(!_isVertexSnapTemporaryPivot);
        }

        private static bool IsTranslateAxis(Axis axis)
        {
            return axis == Axis.X || axis == Axis.Y || axis == Axis.Z;
        }

        private static bool IsPlaneAxis(Axis axis)
        {
            return axis == Axis.XY || axis == Axis.YZ || axis == Axis.ZX;
        }

        private void ConstrainSnapDeltaToActiveHandle(ref Vector3 delta)
        {
            switch (_activeAxis)
            {
            case Axis.X:
                delta.Y = delta.Z = 0.0f;
                break;
            case Axis.Y:
                delta.X = delta.Z = 0.0f;
                break;
            case Axis.Z:
                delta.X = delta.Y = 0.0f;
                break;
            case Axis.XY:
                delta.Z = 0.0f;
                break;
            case Axis.YZ:
                delta.X = 0.0f;
                break;
            case Axis.ZX:
                delta.Y = 0.0f;
                break;
            }
        }

        private void ClearTransformInteraction(bool clearVertexSnapping = true)
        {
            _accMoveDelta = Vector3.Zero;
            _lastIntersectionPosition = _intersectPosition = Vector3.Zero;
            _tDelta = Vector3.Zero;
            _translationDelta = Vector3.Zero;
            _scaleDelta = Vector3.Zero;
            _translationScaleSnapDelta = Vector3.Zero;
            _translationSnapAppliedTotal = Vector3.Zero;
            _translationSnapAnchorPosition = Vector3.Zero;
            _translationSnapAnchorInitialized = false;
            _axisDragPlaneValid = false;
            _axisDragPreviousScalarValid = false;
            _axisDragReanchorOnNextValid = false;
            _axisDragPreviousScalar = 0.0f;
            _axisDragScreenFallbackValid = false;
            _axisDragScreenDirection = Float2.Zero;
            _axisDragWorldUnitsPerPixel = 0.0f;
            _rotationDelta = Quaternion.Identity;
            _rotationGizmoDelta = Quaternion.Identity;
            _rotationSnapDelta = 0.0f;
            _rotationAccumulatedAngle = 0.0f;
            _isDrawingRotationDrag = false;
            _isDrawingTranslationDistance = false;
            ClearGeometrySnapTarget();
            if (clearVertexSnapping)
                EndVertexSnapping();
            else
                EndVertexSnappingTarget();
        }

        private void OnUndoRedoDone(IUndoAction action)
        {
            LogGizmoFocusDebug("Undo/redo callback before reset", action);
            bool suppressSelectionRelease = Owner.IsLeftMouseButtonDown || _wasLeftMouseButtonDown;
            if (HasActiveTransaction || _isTransforming || _transactionOrigin != null)
                CancelTransforming();
            else
                ResetTransactionState();
            // Consume the release from the cancelled drag so the viewport does not pick through the gizmo.
            _suppressSelectionRelease |= suppressSelectionRelease;
            LogGizmoFocusDebug("Undo/redo callback after reset", action);
        }

        internal bool ConsumeSelectionRelease()
        {
            if (!_suppressSelectionRelease)
                return false;
            _suppressSelectionRelease = false;
            return true;
        }

        internal bool TryCancelPointerInteractionForUndo()
        {
            bool hasPointerGesture = Owner.IsLeftMouseButtonDown || _wasLeftMouseButtonDown;
            bool hasTransformGesture = HasActiveTransaction || _isTransforming || _transactionOrigin != null || _activeAxis != Axis.None;
            if (!hasPointerGesture || !hasTransformGesture)
                return false;

            if (HasActiveTransaction || _isTransforming || _transactionOrigin != null)
                CancelTransforming();
            else
                ResetTransactionState();
            _suppressSelectionRelease = true;
            return true;
        }

        internal void ResetSelectionReleaseSuppression()
        {
            _suppressSelectionRelease = false;
        }

        private void LogGizmoFocusDebug(string point, IUndoAction action)
        {
            /* Temporarily disabled to keep gizmo focus diagnostics out of the log.
            var viewport = Owner?.Viewport;
            var root = viewport?.Root;
            var focusedControl = root?.FocusedControl;
            Editor.Log(string.Format(
                "[GizmoFocusDebug] {0} {1}; Action={2}; Focused={3}; ViewportFocus={4}; State={5}; ActiveTransaction={6}; Transforming={7}; FocusLost={8}; Ctrl={9}; Shift={10}; Alt={11}",
                GetType().Name,
                point,
                action != null ? action.GetType().Name + ": " + action.ActionString : "<none>",
                focusedControl != null ? focusedControl.GetType().Name : "<none>",
                viewport?.ContainsFocus ?? false,
                State,
                HasActiveTransaction,
                _isTransforming,
                _focusLost,
                root?.GetKey(KeyboardKeys.Control) ?? false,
                root?.GetKey(KeyboardKeys.Shift) ?? false,
                root?.GetKey(KeyboardKeys.Alt) ?? false));
            */
        }

        private void StartRotationDrag(Vector3 startPoint)
        {
            var transform = _gizmoWorld;
            StartRotationDrag(startPoint, ref transform);
        }

        private void StartRotationDrag(Vector3 startPoint, ref Transform transform)
        {
            _isDrawingRotationDrag = true;
            _rotationDragStartPointWorld = transform.LocalToWorld(startPoint);
            _rotationDragCurrentPointWorld = _rotationDragStartPointWorld;
            _rotationDragMousePointWorld = _rotationDragStartPointWorld;
        }

        private Transform GetRotationTrackballTransform()
        {
            var transform = _gizmoWorld;
            if (_transactionOrigin != null)
            {
                transform.Translation = _transactionOrigin.PivotPosition;
                transform.Orientation = _transactionOrigin.InitialBasis;
                if (_rotationSolverInitialized && _rotationSolverScreenScale > Mathf.Epsilon)
                    transform.Scale = new Float3(_rotationSolverScreenScale);
            }
            return transform;
        }

        private void UpdateRotationDragPoint()
        {
            if (!_isDrawingRotationDrag || _rotationDelta.IsIdentity)
                return;

            Vector3 offset = _rotationDragCurrentPointWorld - Position;
            Vector3.Transform(ref offset, ref _rotationDelta, out offset);
            _rotationDragCurrentPointWorld = Position + offset;
        }

        private void UpdateRotateTrackball()
        {
            var anchor = InteractionAnchor;
            if (anchor == null)
                return;
            var trackballTransform = GetRotationTrackballTransform();
            if (!_rotationSolverInitialized)
            {
                if (!GetRotateTrackballPointLocal(out var startPoint))
                    startPoint = GetRotateToViewLocal(ref trackballTransform) * _rotationTrackballRadiusRaw;
                _rotationAnchorPointLocal = Vector3.Normalize(startPoint);
                _rotationSolverBasis = trackballTransform.Orientation;
                _rotationSolverScreenScale = _screenScale;
                _rotationAnchorAccumulatedAngle = _rotationAccumulatedAngle;
                _rotationSolverInitialized = true;
                StartRotationDrag(startPoint, ref trackballTransform);
                _rotationDelta = Quaternion.Identity;
                return;
            }

            if (!GetRotateTrackballPointLocal(out var currentPointLocal))
            {
                _rotationDelta = Quaternion.Identity;
                return;
            }

            _rotationDragMousePointWorld = trackballTransform.LocalToWorld(currentPointLocal);
            _rotationDragCurrentPointWorld = _rotationDragMousePointWorld;
            float snap = RotationSnapEnabled || Owner.UseSnapping
                ? Mathf.Abs(RotationSnapValue) * Mathf.DegreesToRadians
                : 0.0f;
            float gain = Owner.IsAltKeyDown ? PrecisionScaleGain : 1.0f;
            Quaternion relative = SolveArcballRotation(_rotationAnchorPointLocal, currentPointLocal, _rotationSolverBasis, snap, gain);
            Vector3 anchorNormalized = Vector3.Normalize(_rotationAnchorPointLocal);
            Vector3 currentNormalized = Vector3.Normalize(currentPointLocal);
            float solvedAngle = (float)Math.Acos(Mathf.Clamp((float)Vector3.Dot(anchorNormalized, currentNormalized), -1.0f, 1.0f));
            solvedAngle *= gain;
            if (snap > Mathf.Epsilon)
                solvedAngle = Mathf.Round(solvedAngle / snap) * snap;
            _rotationAccumulatedAngle = _rotationAnchorAccumulatedAngle + solvedAngle;
            SetDesiredRotation(relative * anchor.Result.Rotation);
        }

        private static bool TryGetRotationAxisLocal(Axis axis, out Vector3 axisLocal)
        {
            switch (axis)
            {
            case Axis.X:
                axisLocal = Vector3.UnitX;
                return true;
            case Axis.Y:
                axisLocal = Vector3.UnitY;
                return true;
            case Axis.Z:
                axisLocal = Vector3.UnitZ;
                return true;
            default:
                axisLocal = Vector3.Zero;
                return false;
            }
        }

        private static bool TryGetScaleDirectionLocal(Axis axis, out Vector3 directionLocal)
        {
            switch (axis)
            {
            case Axis.X:
                directionLocal = Vector3.UnitX;
                return true;
            case Axis.Y:
                directionLocal = Vector3.UnitY;
                return true;
            case Axis.Z:
                directionLocal = Vector3.UnitZ;
                return true;
            default:
                directionLocal = Vector3.Zero;
                return false;
            }
        }

        private bool TryGetScreenDirectionSign(Vector3 directionLocal, out float sign)
        {
            sign = 0.0f;

            Vector3 directionWorld = directionLocal * _gizmoWorld.Orientation;
            if (directionWorld.LengthSquared < 0.0001f)
                return false;
            directionWorld.Normalize();

            Owner.Viewport.ProjectPoint(Position, out var startScreen);
            Owner.Viewport.ProjectPoint(Position + directionWorld * _screenScale, out var endScreen);
            Float2 screenDirection = endScreen - startScreen;
            if (screenDirection.LengthSquared < 0.0001f)
                return false;

            Float2 mouseDelta = Owner.MouseDelta;
            if (mouseDelta.LengthSquared < 0.0001f)
                return false;

            float dot = Float2.Dot(mouseDelta, screenDirection);
            if (Mathf.IsZero(dot))
                return false;

            sign = Mathf.Sign(dot);
            return true;
        }

        private void ApplyScaleScreenDirection(ref Vector3 delta)
        {
            if (!TryGetScaleDirectionLocal(_activeAxis, out var directionLocal) ||
                !TryGetScreenDirectionSign(directionLocal, out var sign))
            {
                return;
            }

            switch (_activeAxis)
            {
            case Axis.X:
                delta.X = Mathf.Abs(delta.X) * sign;
                break;
            case Axis.Y:
                delta.Y = Mathf.Abs(delta.Y) * sign;
                break;
            case Axis.Z:
                delta.Z = Mathf.Abs(delta.Z) * sign;
                break;
            }
        }

        private bool UpdateRotateRing(out Float3 axisWorld, out float delta)
        {
            axisWorld = Float3.Zero;
            delta = 0.0f;

            if (!TryGetRotationAxisLocal(_activeAxis, out var axisLocal))
                return false;

            if (!_isDrawingRotationDrag)
            {
                if (!GetRotateRingPointLocal(_activeAxis, out var startPoint))
                    startPoint = Vector3.Zero;
                StartRotationDrag(startPoint);
                return false;
            }

            Vector3 previousPointWorld = _rotationDragMousePointWorld;
            if (!GetRotateRingPointLocal(_activeAxis, out var currentPointLocal))
                return false;

            _rotationDragMousePointWorld = _gizmoWorld.LocalToWorld(currentPointLocal);
            _gizmoWorld.WorldToLocal(ref previousPointWorld, out var previousPointLocal);
            delta = (float)GetSignedAngle(previousPointLocal, currentPointLocal, axisLocal);
            if (Mathf.IsZero(delta))
                return false;

            axisWorld = axisLocal * _gizmoWorld.Orientation;
            return true;
        }

        private static Real GetSignedAngle(Vector3 from, Vector3 to, Vector3 axis)
        {
            if (from.LengthSquared < 0.0001f || to.LengthSquared < 0.0001f)
                return 0.0f;
            from.Normalize();
            to.Normalize();
            axis.Normalize();
            var cross = Vector3.Cross(from, to);
            Real sin = Vector3.Dot(axis, cross);
            Real cos = Mathf.Clamp((float)Vector3.Dot(from, to), -1.0f, 1.0f);
            return (Real)System.Math.Atan2(sin, cos);
        }

        private void UpdateRotateScreen()
        {
            if (!_isDrawingRotationDrag)
            {
                if (!GetRotateScreenRingPointLocal(out var startPoint))
                    startPoint = GetRotateToViewLocal() * _rotationScreenRingRadiusRaw;
                StartRotationDrag(startPoint);
                _rotationDelta = Quaternion.Identity;
                return;
            }

            Vector3 previousPointWorld = _rotationDragMousePointWorld;
            if (!GetRotateScreenRingPointLocal(out var currentPointLocal))
            {
                _rotationDelta = Quaternion.Identity;
                return;
            }

            _rotationDragMousePointWorld = _gizmoWorld.LocalToWorld(currentPointLocal);
            Vector3 previous = previousPointWorld - Position;
            Vector3 current = _rotationDragMousePointWorld - Position;
            Vector3 axisVector = Owner.ViewDirection;
            Real delta = GetSignedAngle(previous, current, axisVector);
            if (Mathf.IsZero((float)delta))
            {
                _rotationDelta = Quaternion.Identity;
                return;
            }

            if (RotationSnapEnabled || Owner.UseSnapping)
            {
                float snapValue = RotationSnapValue * Mathf.DegreesToRadians;
                _rotationSnapDelta += (float)delta;
                float snapped = Mathf.Round(_rotationSnapDelta / snapValue) * snapValue;
                _rotationSnapDelta -= snapped;
                delta = snapped;
                if (Mathf.IsZero((float)delta))
                {
                    _rotationDelta = Quaternion.Identity;
                    return;
                }
            }

            Float3 axis = Owner.ViewDirection;
            _rotationAccumulatedAngle += (float)delta;
            Quaternion.RotationAxis(ref axis, (float)delta, out _rotationDelta);
            UpdateRotationDragPoint();
            AccumulateRotationGizmoDelta();
        }

        private void AccumulateRotationGizmoDelta()
        {
            if (_activeTransformSpace == TransformSpace.World && _activeMode == Mode.Rotate && !_rotationDelta.IsIdentity)
                _rotationGizmoDelta *= _rotationDelta;
        }

        private void UpdateRotate()
        {
            if (_activeAxis == Axis.Center)
            {
                UpdateRotateTrackball();
                return;
            }
            var anchor = InteractionAnchor;
            if (anchor == null)
                return;

            var solverTransform = GetRotationTrackballTransform();
            Vector3 axisLocal;
            Vector3 currentPointLocal;
            if (_activeAxis == Axis.Screen)
            {
                axisLocal = GetRotateToViewLocal(ref solverTransform);
                if (!GetRotateScreenRingPointLocal(out currentPointLocal))
                    return;
            }
            else
            {
                if (!TryGetRotationAxisLocal(_activeAxis, out axisLocal) || !GetRotateRingPointLocal(_activeAxis, out currentPointLocal))
                    return;
            }

            currentPointLocal.Normalize();
            if (!_rotationSolverInitialized)
            {
                _rotationAnchorPointLocal = currentPointLocal;
                _rotationPreviousWrappedAngle = 0.0f;
                _rotationUnwrappedAngle = 0.0f;
                _rotationAnchorAccumulatedAngle = _rotationAccumulatedAngle;
                _rotationSolverBasis = solverTransform.Orientation;
                _rotationSolverScreenScale = _screenScale;
                Vector3 axisWorld = axisLocal * _rotationSolverBasis;
                _rotationSolverAxisWorld = axisWorld;
                _rotationSolverAxisWorld.Normalize();
                float visualRadius = _activeAxis == Axis.Screen ? _rotationScreenRingRadiusRaw : RotateRadiusRaw;
                _rotationScreenDragValid = TryInitializeRotationScreenDrag(ref solverTransform, currentPointLocal, axisLocal, visualRadius);
                _rotationSolverInitialized = true;
                StartRotationDrag(currentPointLocal * visualRadius, ref solverTransform);
                _rotationDelta = Quaternion.Identity;
                return;
            }

            float gain = Owner.IsAltKeyDown ? PrecisionScaleGain : 1.0f;
            float solvedAngle;
            if (_rotationScreenDragValid)
            {
                Float2 pointerDelta = Owner.Viewport.ContinuousViewMousePosition - anchor.PointerPosition;
                _rotationUnwrappedAngle = Float2.Dot(pointerDelta, _rotationScreenDragDirection) * _rotationRadiansPerPixel;
                solvedAngle = _rotationUnwrappedAngle * gain;
            }
            else
            {
                float wrapped = (float)GetSignedAngleFromAnchor(_rotationAnchorPointLocal, currentPointLocal, axisLocal);
                _rotationUnwrappedAngle = UnwrapAngle(_rotationUnwrappedAngle, _rotationPreviousWrappedAngle, wrapped);
                _rotationPreviousWrappedAngle = wrapped;
                solvedAngle = _rotationUnwrappedAngle * gain;
            }
            if (RotationSnapEnabled || Owner.UseSnapping)
            {
                float snap = Mathf.Abs(RotationSnapValue) * Mathf.DegreesToRadians;
                if (snap > Mathf.Epsilon)
                    solvedAngle = Mathf.Round(solvedAngle / snap) * snap;
            }

            _rotationAccumulatedAngle = _rotationAnchorAccumulatedAngle + solvedAngle;
            Quaternion.RotationAxis(ref _rotationSolverAxisWorld, solvedAngle, out var relative);
            SetDesiredRotation(relative * anchor.Result.Rotation);
            float currentVisualRadius = _activeAxis == Axis.Screen ? _rotationScreenRingRadiusRaw : RotateRadiusRaw;
            Vector3 visualPointLocal = currentPointLocal;
            if (_rotationScreenDragValid)
            {
                Float3 visualAxis = axisLocal;
                visualAxis.Normalize();
                Quaternion.RotationAxis(ref visualAxis, solvedAngle, out var visualRotation);
                Vector3 anchorPoint = _rotationAnchorPointLocal;
                Vector3.Transform(ref anchorPoint, ref visualRotation, out visualPointLocal);
            }
            _rotationDragMousePointWorld = solverTransform.LocalToWorld(visualPointLocal * currentVisualRadius);
            _rotationDragCurrentPointWorld = _rotationDragMousePointWorld;
        }

        private bool TryInitializeRotationScreenDrag(ref Transform transform, Vector3 pointLocal, Vector3 axisLocal, float radius)
        {
            _rotationScreenDragDirection = Float2.Zero;
            _rotationRadiansPerPixel = 0.0f;
            if (pointLocal.LengthSquared < 0.0001f || axisLocal.LengthSquared < 0.0001f)
                return false;

            pointLocal.Normalize();
            axisLocal.Normalize();
            Vector3 tangentLocal = Vector3.Cross(axisLocal, pointLocal);
            if (tangentLocal.LengthSquared < 0.0001f)
                return false;
            tangentLocal.Normalize();

            const float sampleAngle = 0.05f;
            Vector3 nextPointLocal = pointLocal * (float)Math.Cos(sampleAngle) + tangentLocal * (float)Math.Sin(sampleAngle);
            if (!TryProjectGizmoPoint(transform.LocalToWorld(pointLocal * radius), out var startScreen) ||
                !TryProjectGizmoPoint(transform.LocalToWorld(nextPointLocal * radius), out var nextScreen))
                return false;

            Float2 screenStep = nextScreen - startScreen;
            float length = screenStep.Length;
            if (length < 0.01f)
                return false;
            _rotationScreenDragDirection = screenStep / length;
            _rotationRadiansPerPixel = sampleAngle / length;
            return true;
        }

        private void SetDesiredRotation(Quaternion desired)
        {
            Quaternion.Normalize(ref desired, out var normalizedDesired);
            Quaternion inverseCurrent = Quaternion.Invert(InteractionResult.Rotation);
            _rotationDelta = normalizedDesired * inverseCurrent;
            Quaternion.Normalize(ref _rotationDelta, out var normalizedDelta);
            _rotationDelta = normalizedDelta;
            if (_activeTransformSpace == TransformSpace.World)
                _rotationGizmoDelta = normalizedDesired;
        }

        /// <inheritdoc />
        public override bool IsControllingMouse => _interactionState != InteractionState.Clutched && Owner.IsLeftMouseButtonDown && (HasActiveTransaction || _activeAxis != Axis.None);

        /// <inheritdoc />
        public override void Update(float dt)
        {
            if (_pressedFeedbackTime > 0.0f)
                _pressedFeedbackTime = Mathf.Max(0.0f, _pressedFeedbackTime - Mathf.Max(dt, 0.0f));
            if (_cancelledFeedbackTime > 0.0f)
            {
                _cancelledFeedbackTime = Mathf.Max(0.0f, _cancelledFeedbackTime - Mathf.Max(dt, 0.0f));
                if (_cancelledFeedbackTime <= 0.0f)
                    UpdateFeedbackModel();
            }
            LastDelta = Transform.Identity;
            bool wasLeftBtnDown = _wasLeftMouseButtonDown;
            bool isLeftBtnDown = Owner.IsLeftMouseButtonDown;
            bool isLeftMouseButtonPressed = isLeftBtnDown && !wasLeftBtnDown;
            bool isLeftMouseButtonReleased = !isLeftBtnDown && wasLeftBtnDown;
            _wasLeftMouseButtonDown = isLeftBtnDown;
            if (!IsInteractionActive)
            {
                if (HasActiveTransaction || _isTransforming)
                    CancelTransforming();
                SetHoveringState(false);
                return;
            }

            var root = Owner.Viewport.Root;
            if (HasActiveTransaction && root != null)
            {
                if (root.GetKeyDown(KeyboardKeys.Escape))
                {
                    CancelTransforming();
                    return;
                }
                if (root.GetKeyDown(KeyboardKeys.Return))
                {
                    CommitTransforming();
                    return;
                }
            }

            if (HasActiveTransaction && !ValidateTransactionObjects())
            {
                CancelTransforming();
                return;
            }

            if (HasActiveTransaction && HandleFocusAndClutch())
            {
                UpdateGizmoPosition();
                UpdateMatrices();
                return;
            }

            if (!HasActiveTransaction && isLeftMouseButtonPressed)
            {
                // Rebuild projected motor targets on the press frame. Cached
                // targets from the previous update can noticeably diverge from
                // the visible handle after a large bounds change or at a large
                // world-space distance.
                UpdateMatrices();
                SelectAxis();
                if (_activeAxis != Axis.None)
                    ArmInteraction();
            }

            if (_interactionState == InteractionState.Armed && _interactionAnchor == null)
            {
                _interactionAnchor = CreateInteractionAnchor();
                ResetSolverAnchorState();
            }

            // CSG exposes explicit face, edge and vertex components. The generic temporary
            // vertex pivot turns those edits back into a camera-plane free move and can pull
            // an otherwise grid-aligned brush off-grid.
            bool snapToVertex = Owner.SnapToVertex && !IsConstrainedSupplementalTranslation;
            bool snapToVertexPressed = snapToVertex && !_wasSnapToVertex;
            _wasSnapToVertex = snapToVertex;
            bool cancelVertexSnapPivot = (snapToVertexPressed && _isVertexSnapTemporaryPivot) ||
                                         (Owner.Viewport.Root != null && Owner.Viewport.Root.GetKeyDown(KeyboardKeys.Escape));
            if (cancelVertexSnapPivot)
                EndVertexSnapping();
            bool skipVertexSnapSelection = cancelVertexSnapPivot;
            if (_isVertexSnapDragPending)
            {
                var mouseDelta = Owner.Viewport.ContinuousViewMousePosition - _vertexSnapDragStartMousePosition;
                if (!isLeftBtnDown || mouseDelta.LengthSquared >= VertexSnapDragStartDistanceSquared)
                    _isVertexSnapDragPending = false;
            }

            // Snap to ground
            if (_activeAxis == Axis.None && SelectionCount != 0 && Owner.SnapToGround)
            {
                try
                {
                    SnapToGround();
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Ground snapping failed; cancelling the transaction.", ex);
                    CancelTransforming();
                    return;
                }
            }
            // Only when is active
            else if (_isActive)
            {
                // Backup position
                _lastIntersectionPosition = _intersectPosition;
                _intersectPosition = Vector3.Zero;

                if (snapToVertex && !skipVertexSnapSelection && isLeftMouseButtonPressed)
                {
                    bool selectedSourceOnPress = false;
                    if (!_isVertexSnapTemporaryPivot && !_isVertexSnapPivotLocked && TrySelectVertexSnappingSource())
                    {
                        selectedSourceOnPress = true;
                        UpdateGizmoPosition();
                        UpdateMatrices();
                        SelectAxis();
                    }
                    if (_vertexSnapObject != null && (_activeAxis != Axis.None || selectedSourceOnPress))
                    {
                        _isVertexSnapPivotLocked = true;
                        if (selectedSourceOnPress)
                        {
                            _isVertexSnapDragPending = true;
                            _vertexSnapDragStartMousePosition = Owner.Viewport.ContinuousViewMousePosition;
                        }
                        if (_activeAxis == Axis.None && selectedSourceOnPress)
                            _activeAxis = Axis.Center;
                    }
                }

                // Check if user is holding left mouse button and any axis is selected
                try
                {
                    if (_interactionState != InteractionState.Clutched && isLeftBtnDown && _activeAxis != Axis.None && !_isVertexSnapDragPending)
                {
                    switch (_activeMode)
                    {
                    case Mode.Translate:
                        UpdateTranslateScale();
                        break;
                    case Mode.Scale:
                        UpdateTranslateScale();
                        break;
                    case Mode.Bounds:
                        UpdateBoundsResize();
                        break;
                    case Mode.Rotate:
                        UpdateRotate();
                        break;
                    }
                    if (snapToVertex && _activeMode == Mode.Translate && _activeAxis == Axis.Center)
                        UpdateVertexSnapping();
                    else
                        EndVertexSnappingTarget();
                }
                else if (_interactionState != InteractionState.Clutched)
                {
                    // If nothing selected, try to select any axis
                    if (!isLeftBtnDown && !Owner.IsRightMouseButtonDown)
                    {
                        if (snapToVertex)
                        {
                            if (isLeftMouseButtonReleased)
                            {
                                if (_isVertexSnapPivotLocked && _vertexSnapObject != null)
                                    _isVertexSnapTemporaryPivot = true;
                                _isVertexSnapPivotLocked = false;
                                EndVertexSnappingTarget();
                            }
                            if (!skipVertexSnapSelection && !_isVertexSnapTemporaryPivot && !_isVertexSnapPivotLocked && !TrySelectVertexSnappingSource())
                                EndVertexSnapping();
                            UpdateGizmoPosition();
                            UpdateMatrices();
                            SelectAxis();
                        }
                        else
                        {
                            if (isLeftMouseButtonReleased && _isVertexSnapPivotLocked && _vertexSnapObject != null)
                            {
                                _isVertexSnapTemporaryPivot = true;
                                _isVertexSnapPivotLocked = false;
                                EndVertexSnappingTarget();
                            }
                            if (_isVertexSnapTemporaryPivot)
                                EndVertexSnappingTarget();
                            else
                                EndVertexSnapping();
                            SelectAxis();
                        }
                    }
                }
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Transform preview failed; cancelling the transaction.", ex);
                    CancelTransforming();
                    return;
                }

                // Set positions of the gizmo
                UpdateGizmoPosition();

                // Trigger Translation, Rotation & Scale events
                if (isLeftBtnDown)
                {
                    var anyValid = false;

                    // Translation
                    Vector3 translationDelta = Vector3.Zero;
                    if (_translationDelta.LengthSquared > 0.000001f)
                    {
                        anyValid = true;
                        translationDelta = _translationDelta;
                        _translationDelta = Vector3.Zero;

                        // Prevent from moving objects too far away, like to a different galaxy or sth
                        Vector3 prevMoveDelta = _accMoveDelta;
                        _accMoveDelta += translationDelta;
                        if (_accMoveDelta.Length > Owner.ViewFarPlane * 0.7f)
                            _accMoveDelta = prevMoveDelta;
                    }

                    // Rotation
                    Quaternion rotationDelta = Quaternion.Identity;
                    if (!_rotationDelta.IsIdentity)
                    {
                        anyValid = true;
                        rotationDelta = _rotationDelta;
                        _rotationDelta = Quaternion.Identity;
                    }

                    // Scale
                    Vector3 scaleDelta = Vector3.Zero;
                    if (_scaleDelta.LengthSquared > 0.000001f)
                    {
                        anyValid = true;
                        scaleDelta = _scaleDelta;
                        _scaleDelta = Vector3.Zero;
                    }

                    // Apply transformation (but to the parents, not whole selection pool)
                    if (anyValid || (_isTransforming && Owner.UseDuplicate))
                    {
                        try
                        {
                            StartTransforming();
                            if (_isTransforming)
                            {
                                LastDelta = new Transform(translationDelta, rotationDelta, scaleDelta);
                                ApplyInteractionDelta(ref translationDelta, ref rotationDelta, ref scaleDelta);
                            }
                        }
                        catch (Exception ex)
                        {
                            ReportInteractionFailure("Transform application failed; cancelling the transaction.", ex);
                            CancelTransforming();
                            return;
                        }
                    }
                }
                else if (_interactionState != InteractionState.Clutched)
                {
                    try
                    {
                        // Clear cache
                        ClearTransformInteraction(!((snapToVertex || _isVertexSnapTemporaryPivot) && _vertexSnapObject != null));
                        EndTransforming();
                    }
                    catch (Exception ex)
                    {
                        ReportInteractionFailure("Transform commit failed; cancelling the transaction.", ex);
                        CancelTransforming();
                        return;
                    }
                }
            }

            // Check if has no objects selected
            if (SelectionCount == 0)
            {
                // Deactivate
                if (HasActiveTransaction || _isTransforming)
                    CancelTransforming();
                SetHoveringState(false);
                _isActive = false;
                _activeAxis = Axis.None;
                ClearTransformInteraction();
                return;
            }

            // Helps solve visual lag (1-frame-lag) after selecting a new entity
            if (!_isActive)
                UpdateGizmoPosition();

            // Activate
            _isActive = true;

            if (!HasActiveTransaction)
            {
                if (_activeAxis == Axis.None)
                    SetHoveringState(false);
                else
                    SetHoveringState(true);
            }

            // Update
            UpdateMatrices();
        }

        private bool TrySelectVertexSnappingSource()
        {
            // Find the closest object in selection that is hit by the mouse ray
            var ray = new SceneGraphNode.RayCastData
            {
                Ray = Owner.MouseRay,
                View = Owner.Viewport.ViewRay,
                Flags = VertexSnapRayCastFlags,
            };
            var closestScreenDistance = Real.MaxValue;
            var closestRayDistance = Real.MaxValue;
            SceneGraphNode closestObject = null;
            Vector3 closestPoint = Vector3.Zero;
            var mousePosition = Owner.Viewport.ContinuousViewMousePosition;
            for (int i = 0; i < SelectionCount; i++)
            {
                TryFindClosestVertexSnapPoint(GetSelectedObject(i), ref ray, null, Owner.Viewport, mousePosition, ref closestScreenDistance, ref closestRayDistance, ref closestObject, ref closestPoint);
            }
            if (closestObject == null)
            {
                return false; // Ignore it if no selected object under the mouse supports vertex snapping.
            }

            // Transform back to the local space of the object to work when moving it
            _vertexSnapObject = closestObject;
            _vertexSnapPoint = closestObject.Transform.WorldToLocal(closestPoint);
            _vertexSnapObjectTo = null;
            _vertexSnapPointTo = Vector3.Zero;
            _isVertexSnapTemporaryPivot = false;
            return true;
        }

        private static bool IsVertexSnapExcluded(SceneGraphNode node, List<SceneGraphNode> excludedRoots)
        {
            if (excludedRoots == null)
                return false;
            for (int i = 0; i < excludedRoots.Count; i++)
            {
                var excludedRoot = excludedRoots[i];
                if (excludedRoot == node || excludedRoot.ContainsInHierarchy(node))
                    return true;
            }
            return false;
        }

        private static void TryFindClosestVertexSnapPoint(SceneGraphNode node, ref SceneGraphNode.RayCastData ray, List<SceneGraphNode> excludedRoots, FlaxEditor.Viewport.EditorViewport viewport, Float2 mousePosition, ref Real closestScreenDistance, ref Real closestRayDistance, ref SceneGraphNode closestObject, ref Vector3 closestPoint)
        {
            if (node == null || !node.IsActive || IsVertexSnapExcluded(node, excludedRoots))
                return;

            if (!node.RayCastSelf(ref ray, out var distance, out _))
                distance = Real.MaxValue;
            if (node.OnVertexSnap(ref ray.Ray, distance, viewport, mousePosition, out var vertexSnapPoint, out var screenDistance) &&
                (screenDistance < closestScreenDistance || (screenDistance == closestScreenDistance && distance < closestRayDistance)))
            {
                closestScreenDistance = screenDistance;
                closestRayDistance = distance;
                closestObject = node;
                closestPoint = vertexSnapPoint;
            }

            for (int i = 0; i < node.ChildNodes.Count; i++)
                TryFindClosestVertexSnapPoint(node.ChildNodes[i], ref ray, excludedRoots, viewport, mousePosition, ref closestScreenDistance, ref closestRayDistance, ref closestObject, ref closestPoint);
        }

        private void EndVertexSnapping()
        {
            // Clear current vertex snapping data
            _vertexSnapObject = null;
            _vertexSnapPoint = Vector3.Zero;
            _vertexSnapDragStartMousePosition = Float2.Zero;
            _isVertexSnapPivotLocked = false;
            _isVertexSnapTemporaryPivot = false;
            _isVertexSnapDragPending = false;
            EndVertexSnappingTarget();
        }

        private void EndVertexSnappingTarget()
        {
            _vertexSnapObjectTo = null;
            _vertexSnapPointTo = Vector3.Zero;
        }

        private void UpdateVertexSnapping()
        {
            _vertexSnapObjectTo = null;
            if (_vertexSnapObject == null || Owner.SceneGraphRoot == null)
                return;
            Profiler.BeginEvent("VertexSnap");

            // Raycast nearby objects to snap to (excluding selection)
            var rayCast = new SceneGraphNode.RayCastData
            {
                Ray = Owner.MouseRay,
                View = Owner.Viewport.ViewRay,
                Flags = VertexSnapRayCastFlags,
            };
            var excludeObjects = new List<SceneGraphNode>();
            for (int i = 0; i < SelectionCount; i++)
                excludeObjects.Add(GetSelectedObject(i));
            var closestScreenDistance = Real.MaxValue;
            var closestRayDistance = Real.MaxValue;
            SceneGraphNode hit = null;
            Vector3 pointSnapped = Vector3.Zero;
            TryFindClosestVertexSnapPoint(Owner.SceneGraphRoot, ref rayCast, excludeObjects, Owner.Viewport, Owner.Viewport.ContinuousViewMousePosition, ref closestScreenDistance, ref closestRayDistance, ref hit, ref pointSnapped);
            if (hit != null)
            {
                _vertexSnapObjectTo = hit;
                _vertexSnapPointTo = hit.Transform.WorldToLocal(pointSnapped);

                // Snap current vertex to the target vertex
                _translationDelta = pointSnapped - Position;
            }

            Profiler.EndEvent();
        }

        /// <summary>
        /// Gets a value indicating whether this tool can transform objects.
        /// </summary>
        protected virtual bool CanTransform => true;

        /// <summary>
        /// Gets a value indicating whether this tool can duplicate objects.
        /// </summary>
        protected virtual bool CanDuplicate => true;

        /// <summary>
        /// Gets the selected objects count.
        /// </summary>
        protected abstract int SelectionCount { get; }

        /// <summary>
        /// Gets the selected object.
        /// </summary>
        /// <param name="index">The selected object index.</param>
        /// <returns>The selected object (eg. actor node).</returns>
        protected abstract SceneGraphNode GetSelectedObject(int index);

        /// <summary>
        /// Gets the selected object transformation.
        /// </summary>
        /// <param name="index">The selected object index.</param>
        /// <returns>The transformation of the selected object.</returns>
        protected abstract Transform GetSelectedTransform(int index);

        /// <summary>
        /// Gets the selected objects bounding box (contains the whole selection).
        /// </summary>
        /// <param name="bounds">The bounds of the selected objects (merged bounds).</param>
        /// <param name="navigationDirty">True if editing the selected objects transformations marks the navigation system area dirty (for auto-rebuild), otherwise skip update.</param>
        protected abstract void GetSelectedObjectsBounds(out BoundingBox bounds, out bool navigationDirty);

        /// <summary>
        /// Checks if the specified object is selected.
        /// </summary>
        /// <param name="obj">The object to check.</param>
        /// <returns>True if it's selected, otherwise false.</returns>
        protected abstract bool IsSelected(SceneGraphNode obj);

        /// <summary>
        /// Called when user starts transforming selected objects.
        /// </summary>
        protected virtual void OnStartTransforming()
        {
            TransformingStarted?.Invoke();
        }

        /// <summary>
        /// Called when gizmo tools wants to apply transformation delta to the selected objects pool.
        /// </summary>
        /// <param name="translationDelta">The translation delta.</param>
        /// <param name="rotationDelta">The rotation delta.</param>
        /// <param name="scaleDelta">The scale delta.</param>
        protected virtual void OnApplyTransformation(ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta)
        {
        }

        /// <summary>
        /// Called when user ends transforming selected objects.
        /// </summary>
        protected virtual void OnEndTransforming()
        {
            TransformingEnded?.Invoke();
        }

        /// <summary>
        /// Called when user duplicates selected objects.
        /// </summary>
        protected virtual void OnDuplicate()
        {
        }

        /// <inheritdoc />
        public override void OnSelectionChanged(List<SceneGraphNode> newSelection)
        {
            EndVertexSnapping();
            UpdateGizmoPosition();
        }
    }
}
