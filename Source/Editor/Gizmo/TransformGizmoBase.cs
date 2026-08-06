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
        private bool _isDrawingRotationDrag;
        private Vector3 _rotationDragStartPointWorld;
        private Vector3 _rotationDragCurrentPointWorld;
        private Vector3 _rotationDragMousePointWorld;
        private bool _isDrawingTranslationDistance;
        private Vector3 _translationDragStartPosition;
        private Vector3 _scaleDelta;
        private float _screenScale;

        private Vector3 _tDelta;
        private Vector3 _translationDelta;
        private Vector3 _translationScaleSnapDelta;

        private SceneGraphNode _vertexSnapObject, _vertexSnapObjectTo;
        private Vector3 _vertexSnapPoint, _vertexSnapPointTo;
        private Float2 _vertexSnapDragStartMousePosition;
        private bool _isVertexSnapPivotLocked;
        private bool _isVertexSnapTemporaryPivot;
        private bool _isVertexSnapDragPending;
        private bool _wasLeftMouseButtonDown;
        private bool _suppressSelectionRelease;
        private bool _wasSnapToVertex;
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
            switch (_activePivotType)
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
                return;

            // Set positions of the gizmo
            UpdateGizmoPosition();

            // Scale gizmo to fit on-screen
            Vector3 position = Position;
            if (Owner.Viewport.UseOrthographicProjection)
            {
                //[hack] this is far form ideal the View Position is in wrong location, any think using the View Position will have problem
                //the camera system needs rewrite the to be a camera on springarm, similar how the ArcBallCamera is handled
                //the ortho projection cannot exist with fps camera because there is no
                // - focus point to calculate correct View Position with Orthographic Scale as a reference and Orthographic Scale from View Position
                // with make the camera jump
                // - and deaph so w and s movment in orto mode moves the cliping plane now
                float gizmoSize = Editor.Instance.Options.Options.Visual.GizmoSize;
                _screenScale = gizmoSize * (50 * Owner.Viewport.OrthographicScale);
            }
            else
            {
                Vector3 vLength = Owner.ViewPosition - position;
                float gizmoSize = Editor.Instance.Options.Options.Visual.GizmoSize;
                _screenScale = (float)(vLength.Length / GizmoScaleFactor * gizmoSize);
            }

            // Setup world
            Quaternion orientation = GetSelectedTransform(0).Orientation;
            _gizmoWorld = new Transform(position, orientation, new Float3(_screenScale));
            if (_activeTransformSpace == TransformSpace.World && _activeMode != Mode.Scale)
            {
                _gizmoWorld.Orientation = Quaternion.Identity;
            }
        }

        private void UpdateTranslateScale()
        {
            bool isScaling = _activeMode == Mode.Scale;
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
            var dir = Vector3.Normalize(ray.Position - position);
            var planeDotXY = Mathf.Abs(Vector3.Dot(planeXY.Normal, dir));
            var planeDotYZ = Mathf.Abs(Vector3.Dot(planeYZ.Normal, dir));
            var planeDotZX = Mathf.Abs(Vector3.Dot(planeZX.Normal, dir));

            Real intersection;
            switch (_activeAxis)
            {
            case Axis.X:
            {
                var plane = planeDotXY > planeDotZX ? planeXY : planeZX;
                if (ray.Intersects(ref plane, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref plane, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(_tDelta.X, 0, 0);
                }
                break;
            }
            case Axis.Y:
            {
                var plane = planeDotXY > planeDotYZ ? planeXY : planeYZ;
                if (ray.Intersects(ref plane, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref plane, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(0, _tDelta.Y, 0);
                }
                break;
            }
            case Axis.Z:
            {
                var plane = planeDotZX > planeDotYZ ? planeZX : planeYZ;
                if (ray.Intersects(ref plane, out intersection))
                {
                    _intersectPosition = ray.GetPoint(intersection);
                    if (interactionAnchor != null && _lastIntersectionPosition.IsZero)
                    {
                        var anchorRay = interactionAnchor.PointerRay;
                        anchorRay.Position = Vector3.Transform(anchorRay.Position, invRotationMatrix);
                        Vector3.TransformNormal(ref anchorRay.Direction, ref invRotationMatrix, out anchorRay.Direction);
                        if (anchorRay.Intersects(ref plane, out Real anchorIntersection))
                            _lastIntersectionPosition = anchorRay.GetPoint(anchorIntersection);
                    }
                    if (!_lastIntersectionPosition.IsZero)
                        _tDelta = _intersectPosition - _lastIntersectionPosition;
                    delta = new Vector3(0, 0, _tDelta.Z);
                }
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
            if (Owner.IsAltKeyDown)
                delta *= 0.5f;
            if ((isScaling ? ScaleSnapEnabled : TranslationSnapEnable) || Owner.UseSnapping)
            {
                var snapValue = new Vector3(isScaling ? ScaleSnapValue : TranslationSnapValue);

                _translationScaleSnapDelta += delta;
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

                Vector3 absoluteDelta = Vector3.Zero;
                if (AbsoluteSnapEnabled && ActiveTransformSpace == TransformSpace.World)
                {
                    // Remove delta to offset local-space grid into the world-space grid
                    Vector3 currentTranslationScale = isScaling ? GetSelectedTransform(0).Scale : GetSelectedTransform(0).Translation; 
                    absoluteDelta = currentTranslationScale - new Vector3(
                        Mathr.Round(currentTranslationScale.X / snapValue.X) * snapValue.X,
                        Mathr.Round(currentTranslationScale.Y / snapValue.Y) * snapValue.Y,
                        Mathr.Round(currentTranslationScale.Z / snapValue.Z) * snapValue.Z);
                }

                delta = new Vector3(
                                    (int)(_translationScaleSnapDelta.X / snapValue.X) * snapValue.X,
                                    (int)(_translationScaleSnapDelta.Y / snapValue.Y) * snapValue.Y,
                                    (int)(_translationScaleSnapDelta.Z / snapValue.Z) * snapValue.Z);
                _translationScaleSnapDelta -= delta;
                delta -= absoluteDelta;
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

        private void ResetTranslationScale()
        {
            ClearTransformInteraction(!_isVertexSnapTemporaryPivot);
        }

        private static bool IsTranslateAxis(Axis axis)
        {
            return axis == Axis.X || axis == Axis.Y || axis == Axis.Z;
        }

        private void ClearTransformInteraction(bool clearVertexSnapping = true)
        {
            _accMoveDelta = Vector3.Zero;
            _lastIntersectionPosition = _intersectPosition = Vector3.Zero;
            _tDelta = Vector3.Zero;
            _translationDelta = Vector3.Zero;
            _scaleDelta = Vector3.Zero;
            _translationScaleSnapDelta = Vector3.Zero;
            _rotationDelta = Quaternion.Identity;
            _rotationGizmoDelta = Quaternion.Identity;
            _rotationSnapDelta = 0.0f;
            _isDrawingRotationDrag = false;
            _isDrawingTranslationDistance = false;
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
        }

        private void StartRotationDrag(Vector3 startPoint)
        {
            _isDrawingRotationDrag = true;
            _rotationDragStartPointWorld = _gizmoWorld.LocalToWorld(startPoint);
            _rotationDragCurrentPointWorld = _rotationDragStartPointWorld;
            _rotationDragMousePointWorld = _rotationDragStartPointWorld;
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
            if (!_isDrawingRotationDrag)
            {
                if (!GetRotateTrackballPointLocal(out var startPoint))
                    startPoint = GetRotateToViewLocal() * _rotationTrackballRadiusRaw;
                StartRotationDrag(startPoint);
            }

            Float2 mouseDelta = Owner.MouseDelta;
            Float3 viewRight = Float3.Right * Owner.ViewOrientation;
            Float3 viewUp = Float3.Up * Owner.ViewOrientation;
            Float3 axis = -viewUp * mouseDelta.X - viewRight * mouseDelta.Y;
            float delta = axis.Length * RotateTrackballSensitivity;
            if (delta <= Mathf.Epsilon)
            {
                _rotationDelta = Quaternion.Identity;
                return;
            }
            axis.Normalize();

            if (RotationSnapEnabled || Owner.UseSnapping)
            {
                float snapValue = RotationSnapValue * Mathf.DegreesToRadians;
                _rotationSnapDelta += delta;

                float snapped = Mathf.Round(_rotationSnapDelta / snapValue) * snapValue;
                _rotationSnapDelta -= snapped;
                delta = snapped;
                if (Mathf.IsZero(delta))
                {
                    _rotationDelta = Quaternion.Identity;
                    return;
                }
            }

            Quaternion.RotationAxis(ref axis, delta, out _rotationDelta);
            UpdateRotationDragPoint();
            AccumulateRotationGizmoDelta();
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
            if (_activeAxis == Axis.Screen)
            {
                UpdateRotateScreen();
                return;
            }

            if (!UpdateRotateRing(out var dir, out var delta))
            {
                _rotationDelta = Quaternion.Identity;
                return;
            }

            if (RotationSnapEnabled || Owner.UseSnapping)
            {
                float snapValue = RotationSnapValue * Mathf.DegreesToRadians;

                float absoluteDelta = 0.0f;
                if (AbsoluteSnapEnabled && ActiveTransformSpace == TransformSpace.World)
                {
                    // Remove delta to offset world-space grid into the local-space grid
                    float currentAngle = 0.0f;
                    switch (_activeAxis)
                    {
                        case Axis.X: currentAngle = GetSelectedTransform(0).Orientation.EulerAngles.X; break;
                        case Axis.Y: currentAngle = GetSelectedTransform(0).Orientation.EulerAngles.Y; break;
                        case Axis.Z: currentAngle = GetSelectedTransform(0).Orientation.EulerAngles.Z; break;
                    }
                    absoluteDelta = currentAngle - (Mathf.Round(currentAngle / RotationSnapValue) * RotationSnapValue);
                }

                _rotationSnapDelta += delta;

                float snapped = Mathf.Round(_rotationSnapDelta / snapValue) * snapValue;
                _rotationSnapDelta -= snapped;

                delta = snapped;
                delta -= absoluteDelta * Mathf.DegreesToRadians;
            }

            switch (_activeAxis)
            {
            case Axis.X:
            case Axis.Y:
            case Axis.Z:
            {
                Quaternion.RotationAxis(ref dir, delta, out _rotationDelta);
                UpdateRotationDragPoint();
                AccumulateRotationGizmoDelta();
                break;
            }

            default:
                _rotationDelta = Quaternion.Identity;
                break;
            }
        }

        /// <inheritdoc />
        public override bool IsControllingMouse => HasActiveTransaction && Owner.IsLeftMouseButtonDown;

        /// <inheritdoc />
        public override void Update(float dt)
        {
            LastDelta = Transform.Identity;
            bool wasLeftBtnDown = _wasLeftMouseButtonDown;
            bool isLeftBtnDown = Owner.IsLeftMouseButtonDown;
            bool isLeftMouseButtonPressed = isLeftBtnDown && !wasLeftBtnDown;
            bool isLeftMouseButtonReleased = !isLeftBtnDown && wasLeftBtnDown;
            _wasLeftMouseButtonDown = isLeftBtnDown;
            if (!IsActive)
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

            if (!HasActiveTransaction && isLeftMouseButtonPressed && _activeAxis != Axis.None)
                ArmInteraction();

            bool snapToVertex = Owner.SnapToVertex;
            bool snapToVertexPressed = snapToVertex && !_wasSnapToVertex;
            _wasSnapToVertex = snapToVertex;
            bool cancelVertexSnapPivot = (snapToVertexPressed && _isVertexSnapTemporaryPivot) ||
                                         (Owner.Viewport.Root != null && Owner.Viewport.Root.GetKeyDown(KeyboardKeys.Escape));
            if (cancelVertexSnapPivot)
                EndVertexSnapping();
            bool skipVertexSnapSelection = cancelVertexSnapPivot;
            if (_isVertexSnapDragPending)
            {
                var mouseDelta = Owner.Viewport.ViewMousePosition - _vertexSnapDragStartMousePosition;
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
                            _vertexSnapDragStartMousePosition = Owner.Viewport.ViewMousePosition;
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

                        if (ActiveAxis == Axis.Center)
                            scaleDelta = new Vector3(scaleDelta.X);
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
            var mousePosition = Owner.Viewport.ViewMousePosition;
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
            TryFindClosestVertexSnapPoint(Owner.SceneGraphRoot, ref rayCast, excludeObjects, Owner.Viewport, Owner.Viewport.ViewMousePosition, ref closestScreenDistance, ref closestRayDistance, ref hit, ref pointSnapped);
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
