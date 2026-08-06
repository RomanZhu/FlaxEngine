// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using System;
using System.Collections.Generic;
using FlaxEditor.SceneGraph;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    /// <summary>
    /// Immutable scene snapshot captured at the beginning of a transform transaction.
    /// </summary>
    [HideInEditor]
    public sealed class TransactionOrigin
    {
        /// <summary>
        /// Initializes a new transaction origin.
        /// </summary>
        public TransactionOrigin(IReadOnlyList<SceneGraphNode> objects, IReadOnlyList<Transform> transforms, BoundingBox bounds, TransformGizmoBase.Mode mode, TransformGizmoBase.TransformSpace transformSpace, TransformGizmoBase.PivotType pivot, Quaternion basis, SemanticHandle handle)
        : this(objects, transforms, bounds, mode, transformSpace, pivot, basis, handle, Vector3.Zero)
        {
        }

        /// <summary>
        /// Initializes a new transaction origin with its world-space pivot.
        /// </summary>
        public TransactionOrigin(IReadOnlyList<SceneGraphNode> objects, IReadOnlyList<Transform> transforms, BoundingBox bounds, TransformGizmoBase.Mode mode, TransformGizmoBase.TransformSpace transformSpace, TransformGizmoBase.PivotType pivot, Quaternion basis, SemanticHandle handle, Vector3 pivotPosition)
        {
            Objects = CopyObjects(objects);
            OriginalTransforms = CopyTransforms(transforms);
            OriginalBounds = bounds;
            InitialMode = mode;
            InitialTransformSpace = transformSpace;
            InitialPivot = pivot;
            InitialBasis = basis;
            Handle = handle;
            PivotPosition = pivotPosition;
            CreatedObjects = Array.Empty<SceneGraphNode>();
        }

        private TransactionOrigin(TransactionOrigin source, IReadOnlyList<SceneGraphNode> createdObjects, IUndoAction duplicateUndoAction)
        {
            Objects = source.Objects;
            OriginalTransforms = source.OriginalTransforms;
            OriginalBounds = source.OriginalBounds;
            InitialMode = source.InitialMode;
            InitialTransformSpace = source.InitialTransformSpace;
            InitialPivot = source.InitialPivot;
            InitialBasis = source.InitialBasis;
            Handle = source.Handle;
            PivotPosition = source.PivotPosition;
            CreatedObjects = CopyObjects(createdObjects);
            DuplicateUndoAction = duplicateUndoAction;
        }

        /// <summary>
        /// Gets the top-level selected objects at transaction begin.
        /// </summary>
        public IReadOnlyList<SceneGraphNode> Objects { get; }

        /// <summary>
        /// Gets the exact transforms captured before the first mutation.
        /// </summary>
        public IReadOnlyList<Transform> OriginalTransforms { get; }

        /// <summary>
        /// Gets the original aggregate bounds.
        /// </summary>
        public BoundingBox OriginalBounds { get; }

        /// <summary>
        /// Gets the initial transform mode.
        /// </summary>
        public TransformGizmoBase.Mode InitialMode { get; }

        /// <summary>
        /// Gets the initial transform space.
        /// </summary>
        public TransformGizmoBase.TransformSpace InitialTransformSpace { get; }

        /// <summary>
        /// Gets the initial pivot policy.
        /// </summary>
        public TransformGizmoBase.PivotType InitialPivot { get; }

        /// <summary>
        /// Gets the initial gizmo basis.
        /// </summary>
        public Quaternion InitialBasis { get; }

        /// <summary>
        /// Gets the semantic handle latched for this transaction.
        /// </summary>
        public SemanticHandle Handle { get; }

        /// <summary>
        /// Gets the world-space gizmo pivot captured at transaction begin.
        /// </summary>
        public Vector3 PivotPosition { get; }

        /// <summary>
        /// Gets the objects created by duplication for this transaction.
        /// </summary>
        public IReadOnlyList<SceneGraphNode> CreatedObjects { get; }

        internal IUndoAction DuplicateUndoAction { get; }

        internal TransactionOrigin WithCreatedObjects(IReadOnlyList<SceneGraphNode> createdObjects, IUndoAction duplicateUndoAction)
        {
            return new TransactionOrigin(this, createdObjects, duplicateUndoAction);
        }

        private static IReadOnlyList<SceneGraphNode> CopyObjects(IReadOnlyList<SceneGraphNode> source)
        {
            var result = new SceneGraphNode[source?.Count ?? 0];
            if (source != null)
            {
                for (int i = 0; i < source.Count; i++)
                    result[i] = source[i];
            }
            return Array.AsReadOnly(result);
        }

        private static IReadOnlyList<Transform> CopyTransforms(IReadOnlyList<Transform> source)
        {
            var result = new Transform[source?.Count ?? 0];
            if (source != null)
            {
                for (int i = 0; i < source.Count; i++)
                    result[i] = source[i];
            }
            return Array.AsReadOnly(result);
        }
    }

    /// <summary>
    /// Re-anchorable pointer and camera state for one transform transaction.
    /// </summary>
    [HideInEditor]
    public sealed class InteractionAnchor
    {
        /// <summary>
        /// Initializes a new interaction anchor.
        /// </summary>
        public InteractionAnchor(InteractionResult result, Float2 pointerPosition, Ray pointerRay, Vector3 cameraPosition, Quaternion cameraOrientation, Plane fallbackPlane, SemanticHandle handle)
        {
            result ??= InteractionResult.Empty;
            Result = result;
            PointerPosition = pointerPosition;
            PointerRay = pointerRay;
            CameraPosition = cameraPosition;
            CameraOrientation = cameraOrientation;
            FallbackPlane = fallbackPlane;
            Handle = handle;
            AccumulatedRotation = result.Rotation;
            AccumulatedScale = result.Scale;
        }

        /// <summary>
        /// Gets the result at the instant the anchor was created.
        /// </summary>
        public InteractionResult Result { get; }

        /// <summary>
        /// Gets the pointer position at the anchor.
        /// </summary>
        public Float2 PointerPosition { get; }

        /// <summary>
        /// Gets the pointer ray at the anchor.
        /// </summary>
        public Ray PointerRay { get; }

        /// <summary>
        /// Gets the camera position at the anchor.
        /// </summary>
        public Vector3 CameraPosition { get; }

        /// <summary>
        /// Gets the camera orientation at the anchor.
        /// </summary>
        public Quaternion CameraOrientation { get; }

        /// <summary>
        /// Gets the fallback solving plane at the anchor.
        /// </summary>
        public Plane FallbackPlane { get; }

        /// <summary>
        /// Gets the latched semantic handle.
        /// </summary>
        public SemanticHandle Handle { get; }

        /// <summary>
        /// Gets the accumulated rotation context at the anchor.
        /// </summary>
        public Quaternion AccumulatedRotation { get; }

        /// <summary>
        /// Gets the accumulated scale context at the anchor.
        /// </summary>
        public Vector3 AccumulatedScale { get; }
    }

    /// <summary>
    /// Origin-derived result of a transform interaction.
    /// </summary>
    [HideInEditor]
    public sealed class InteractionResult
    {
        /// <summary>
        /// The empty interaction result.
        /// </summary>
        public static readonly InteractionResult Empty = new InteractionResult(Vector3.Zero, Quaternion.Identity, Vector3.One, Array.Empty<Transform>(), null);

        /// <summary>
        /// Initializes a new interaction result.
        /// </summary>
        public InteractionResult(Vector3 translation, Quaternion rotation, Vector3 scale, IReadOnlyList<Transform> objectTransforms, object snapIdentity)
        {
            Translation = IsFinite(translation) ? translation : Vector3.Zero;
            Quaternion.Normalize(ref rotation, out var normalizedRotation);
            Rotation = IsValidRotation(normalizedRotation) ? normalizedRotation : Quaternion.Identity;
            Scale = IsFinite(scale) ? scale : Vector3.One;
            ObjectTransforms = CopyTransforms(objectTransforms);
            SnapIdentity = snapIdentity;
        }

        /// <summary>
        /// Gets the net translation from the transaction origin.
        /// </summary>
        public Vector3 Translation { get; }

        /// <summary>
        /// Gets the normalized net rotation from the transaction origin.
        /// </summary>
        public Quaternion Rotation { get; }

        /// <summary>
        /// Gets the net scale factors from the transaction origin.
        /// </summary>
        public Vector3 Scale { get; }

        /// <summary>
        /// Gets the current derived object transforms.
        /// </summary>
        public IReadOnlyList<Transform> ObjectTransforms { get; }

        /// <summary>
        /// Gets the semantic snap identity, if any.
        /// </summary>
        public object SnapIdentity { get; }

        /// <summary>
        /// Gets a value indicating whether this result contains a non-identity change.
        /// </summary>
        public bool HasMeaningfulChange => !Translation.IsZero || !Rotation.IsIdentity || Scale != Vector3.One;

        private static IReadOnlyList<Transform> CopyTransforms(IReadOnlyList<Transform> source)
        {
            var result = new Transform[source?.Count ?? 0];
            if (source != null)
            {
                for (int i = 0; i < source.Count; i++)
                    result[i] = source[i];
            }
            return Array.AsReadOnly(result);
        }

        private static bool IsFinite(Vector3 value)
        {
            return !Real.IsNaN(value.X) && !Real.IsInfinity(value.X) &&
                   !Real.IsNaN(value.Y) && !Real.IsInfinity(value.Y) &&
                   !Real.IsNaN(value.Z) && !Real.IsInfinity(value.Z);
        }

        private static bool IsFinite(Quaternion value)
        {
            return !float.IsNaN(value.X) && !float.IsInfinity(value.X) &&
                   !float.IsNaN(value.Y) && !float.IsInfinity(value.Y) &&
                   !float.IsNaN(value.Z) && !float.IsInfinity(value.Z) &&
                   !float.IsNaN(value.W) && !float.IsInfinity(value.W);
        }

        private static bool IsValidRotation(Quaternion value)
        {
            return IsFinite(value) && value.LengthSquared >= Mathf.Epsilon * Mathf.Epsilon;
        }
    }

    public partial class TransformGizmoBase
    {
        private readonly List<SceneGraphNode> _transactionObjects = new List<SceneGraphNode>();
        private readonly List<SceneGraphNode> _createdObjects = new List<SceneGraphNode>();
        private InteractionState _interactionState;
        private TransactionOrigin _transactionOrigin;
        private InteractionAnchor _interactionAnchor;
        private InteractionResult _interactionResult = InteractionResult.Empty;
        private SemanticHandle _latchedHandle = SemanticHandle.None;
        private IUndoAction _duplicateUndoAction;
        private bool _expectingSelectionChange;
        private bool _focusLost;
        private bool _lastUseSnapping;
        private bool _lastPrecision;
        private bool _lastGeometrySnap;
        private Vector3 _anchorTranslationDelta;
        private Quaternion _anchorRotationDelta = Quaternion.Identity;
        private Vector3 _anchorScaleDelta;
        private Mode? _queuedMode;
        private TransformSpace? _queuedTransformSpace;
        private PivotType? _queuedPivot;
        private bool _applyingQueuedSettings;
        private bool _pointerWarpPending;
        private Float2 _pointerWarpTarget;
        private int _pointerWarpFramesRemaining;

        /// <summary>
        /// Gets the explicit transaction state.
        /// </summary>
        public InteractionState State => _interactionState;

        /// <summary>
        /// Gets the immutable origin of the active transaction.
        /// </summary>
        public TransactionOrigin TransactionOrigin => _transactionOrigin;

        /// <summary>
        /// Gets the current replaceable interaction anchor.
        /// </summary>
        public InteractionAnchor InteractionAnchor => _interactionAnchor;

        /// <summary>
        /// Gets the current origin-derived interaction result.
        /// </summary>
        public InteractionResult InteractionResult => _interactionResult;

        /// <summary>
        /// Gets the semantic handle latched at press time.
        /// </summary>
        public SemanticHandle LatchedHandle => _latchedHandle;

        /// <summary>
        /// Gets a value indicating whether a transaction owns the gizmo input.
        /// </summary>
        public bool HasActiveTransaction => IsTransactionState(_interactionState);

        /// <summary>
        /// Gets the pivot used to apply the current transaction preview.
        /// </summary>
        public Vector3 InteractionPivotPosition => _transactionOrigin != null ? _transactionOrigin.PivotPosition : Position;

        /// <summary>
        /// Occurs after an explicit state transition.
        /// </summary>
        public event Action<InteractionState, InteractionState> InteractionStateChanged;

        /// <summary>
        /// Occurs when the lifecycle records a recoverable interaction diagnostic.
        /// </summary>
        public event Action<string> InteractionDiagnostic;

        /// <summary>
        /// Cancels the active transaction and restores its exact origin.
        /// </summary>
        /// <returns>True if a transaction was cancelled.</returns>
        public bool CancelTransforming()
        {
            if (!HasActiveTransaction && !_isTransforming && _transactionOrigin == null)
                return false;

            // Cancelling while LMB still owns the drag must also consume the
            // eventual release. Otherwise the viewport sees that release after
            // the transaction has reset and picks the object under the cursor.
            _suppressSelectionRelease |= Owner != null && (Owner.IsLeftMouseButtonDown || _wasLeftMouseButtonDown);

            _cancelledFeedbackHandle = _latchedHandle.IsValid ? _latchedHandle : new SemanticHandle(_activeMode, _activeAxis);
            _cancelledFeedbackTime = FeedbackCancelledDuration;
            UpdateFeedbackModel(FeedbackHandleState.Cancelled);

            try
            {
                if (HasActiveTransaction && !SetInteractionState(InteractionState.Cancelling))
                    return false;
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform transaction cancellation state change failed.", ex);
            }

            try
            {
                RestoreTransactionOrigin();
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform transaction cancellation failed safely.", ex);
            }
            finally
            {
                try
                {
                    ResetTransactionState();
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Transform transaction reset failed after cancellation.", ex);
                }
            }
            return true;
        }

        /// <summary>
        /// Commits the active transaction, recording at most one logical undo item.
        /// </summary>
        /// <returns>True if a changed transaction was committed.</returns>
        public bool CommitTransforming()
        {
            if (_interactionState == InteractionState.Armed)
            {
                ResetTransactionState();
                return false;
            }
            if (!_isTransforming || !HasActiveTransaction)
                return false;
            if (!SetInteractionState(InteractionState.Committing))
                return false;

            bool committed = false;
            try
            {
                if (HasWorkingTransformChanges())
                {
                    OnEndTransforming();
                    committed = true;
                }
                else
                {
                    RestoreTransactionOrigin();
                }
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform transaction commit failed; restoring the origin.", ex);
                try
                {
                    RestoreTransactionOrigin();
                }
                catch (Exception restoreException)
                {
                    ReportInteractionFailure("Transform transaction origin could not be restored completely.", restoreException);
                }
            }
            finally
            {
                try
                {
                    ResetTransactionState();
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Transform transaction reset failed after commit.", ex);
                }
            }
            return committed;
        }

        /// <summary>
        /// Replaces the current pointer/camera anchor without changing the visible result.
        /// </summary>
        /// <returns>True if an active transaction was re-anchored.</returns>
        public bool ReanchorInteraction()
        {
            if (!HasActiveTransaction || _transactionOrigin == null)
                return false;

            _interactionResult = CreateCurrentInteractionResult();
            _interactionAnchor = CreateInteractionAnchor();
            ResetSolverAnchorState(true);
            UpdateFeedbackModel();
            return true;
        }

        /// <summary>
        /// Enters camera-clutch mode and freezes the current result.
        /// </summary>
        public bool BeginCameraClutch()
        {
            if (_interactionState != InteractionState.Dragging)
                return false;
            return SetInteractionState(InteractionState.Clutched);
        }

        /// <summary>
        /// Leaves camera-clutch mode and re-anchors at the unchanged result.
        /// </summary>
        public bool EndCameraClutch()
        {
            if (_interactionState != InteractionState.Clutched)
                return false;
            if (!ReanchorInteraction())
                return false;
            return SetInteractionState(InteractionState.Dragging);
        }

        /// <summary>
        /// Enters numeric-entry mode.
        /// </summary>
        public bool BeginNumericEntry()
        {
            return _interactionState == InteractionState.Dragging && SetInteractionState(InteractionState.NumericEntry);
        }

        /// <summary>
        /// Applies an exact numeric result to the lifecycle and resumes pointer solving.
        /// </summary>
        /// <param name="translation">The net translation from the transaction origin.</param>
        /// <param name="rotation">The net rotation from the transaction origin.</param>
        /// <param name="scale">The net scale factors from the transaction origin.</param>
        /// <returns>True if numeric entry was active and the result was applied.</returns>
        public bool EndNumericEntry(Vector3 translation, Quaternion rotation, Vector3 scale)
        {
            if (_interactionState != InteractionState.NumericEntry)
                return false;
            if (!IsFinite(translation) || !IsFinite(rotation) || !IsFinite(scale) || rotation.LengthSquared < Mathf.Epsilon * Mathf.Epsilon)
            {
                ReportInteractionFailure("Numeric transform entry produced an invalid result.", null);
                CancelTransforming();
                return false;
            }

            try
            {
                _interactionResult = new InteractionResult(translation, rotation, scale, GetCurrentTransforms(), null);
                if (!SetInteractionState(InteractionState.Dragging))
                    return false;
                _interactionAnchor = CreateInteractionAnchor();
                ResetSolverAnchorState();
                ApplyOriginAuthoritativePreview();
                UpdateFeedbackModel();
                return true;
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Numeric transform entry failed; cancelling the transaction.", ex);
                CancelTransforming();
                return false;
            }
        }

        /// <summary>
        /// Notifies the transaction that the viewport lost focus.
        /// </summary>
        public void OnInteractionFocusLost()
        {
            if (!HasActiveTransaction)
                return;

            _focusLost = true;
            if (_interactionState == InteractionState.Dragging)
                SetInteractionState(InteractionState.Clutched);
        }

        /// <summary>
        /// Notifies the transaction that mouse capture ended unexpectedly.
        /// </summary>
        public void OnInteractionMouseCaptureLost()
        {
            if (!HasActiveTransaction || Owner == null || _focusLost)
                return;
            if (Owner.IsLeftMouseButtonDown)
                CancelTransforming();
            else
                CommitTransforming();
        }

        /// <summary>
        /// Gets the currently transformed top-level objects.
        /// </summary>
        protected IReadOnlyList<SceneGraphNode> TransactionObjects => _transactionObjects;

        /// <summary>
        /// Gets a value indicating whether a selection change is part of the duplicate operation.
        /// </summary>
        protected bool IsExpectingTransactionSelectionChange => _expectingSelectionChange;

        /// <summary>
        /// Gets a value indicating whether the current working transforms differ from their captured state.
        /// </summary>
        protected bool HasTransformChanges => HasWorkingTransformChanges();

        /// <summary>
        /// Registers the result of a transaction-aware duplicate operation.
        /// </summary>
        protected void RegisterDuplicatedObjects(IReadOnlyList<SceneGraphNode> objects, IUndoAction undoAction)
        {
            _createdObjects.Clear();
            if (objects != null)
            {
                for (int i = 0; i < objects.Count; i++)
                {
                    var node = objects[i];
                    bool isOriginObject = false;
                    if (_transactionOrigin != null)
                    {
                        for (int j = 0; j < _transactionOrigin.Objects.Count; j++)
                        {
                            if (_transactionOrigin.Objects[j] == node)
                            {
                                isOriginObject = true;
                                break;
                            }
                        }
                    }
                    if (node != null && !isOriginObject && !_createdObjects.Contains(node))
                        _createdObjects.Add(node);
                }
            }
            _duplicateUndoAction = undoAction;
            if (_transactionOrigin != null)
                _transactionOrigin = _transactionOrigin.WithCreatedObjects(_createdObjects, undoAction);
        }

        /// <summary>
        /// Adds a transform action, composing a transaction-aware duplicate
        /// action when one exists.
        /// </summary>
        protected void AddTransformUndoAction(IUndoAction transformAction)
        {
            if (transformAction == null || Owner?.Undo == null)
                return;

            if (_duplicateUndoAction != null)
            {
                var duplicateAction = _duplicateUndoAction;
                _duplicateUndoAction = null;
                Owner.Undo.AddAction(new MultiUndoAction(new[] { duplicateAction, transformAction }, "Transform object(s)"));
            }
            else
            {
                Owner.Undo.AddAction(transformAction);
            }
        }

        /// <summary>
        /// Records a solver delta in the origin-derived result.
        /// </summary>
        protected void RecordInteractionDelta(Vector3 translationDelta, Quaternion rotationDelta, Vector3 scaleDelta)
        {
            var anchor = _interactionAnchor;
            Vector3 translation;
            Quaternion rotation;
            Vector3 scale;
            if (anchor != null)
            {
                _anchorTranslationDelta += translationDelta;
                _anchorRotationDelta = rotationDelta * _anchorRotationDelta;
                Quaternion.Normalize(ref _anchorRotationDelta, out var normalizedAnchorRotation);
                _anchorRotationDelta = normalizedAnchorRotation;
                _anchorScaleDelta += scaleDelta;

                translation = anchor.Result.Translation + _anchorTranslationDelta;
                rotation = _anchorRotationDelta * anchor.Result.Rotation;
                scale = anchor.Result.Scale + _anchorScaleDelta;
            }
            else
            {
                translation = _interactionResult.Translation + translationDelta;
                rotation = rotationDelta * _interactionResult.Rotation;
                scale = _interactionResult.Scale + scaleDelta;
            }
            Quaternion.Normalize(ref rotation, out var normalizedRotation);
            rotation = normalizedRotation;
            if (!IsFinite(translation) || !IsFinite(rotation) || !IsFinite(scale))
            {
                ReportInteractionFailure("Transform preview produced a non-finite result.", null);
                CancelTransforming();
                return;
            }
            object snapIdentity = _vertexSnapObject != null ? _vertexSnapObjectTo : _interactionResult.SnapIdentity;
            _interactionResult = new InteractionResult(translation, rotation, scale, null, snapIdentity);
            UpdateFeedbackModel();
        }

        /// <summary>
        /// Applies a multiplicative scale delta while keeping the result finite,
        /// bounded, and away from an accidentally singular zero scale.
        /// </summary>
        /// <param name="currentScale">The scale at the transaction origin.</param>
        /// <param name="scaleDelta">The origin-relative scale delta.</param>
        /// <returns>The protected scale value.</returns>
        internal static Float3 ApplyScaleDelta(Float3 currentScale, Vector3 scaleDelta)
        {
            const float scaleLimit = 99_999_999.0f;
            const float minimumScaleMagnitude = 0.0001f;
            var scaleFactor = new Float3((float)(1.0 + scaleDelta.X), (float)(1.0 + scaleDelta.Y), (float)(1.0 + scaleDelta.Z));
            var result = Float3.Clamp(currentScale * scaleFactor, new Float3(-scaleLimit), new Float3(scaleLimit));
            result.X = ProtectScaleComponent(result.X, currentScale.X, minimumScaleMagnitude);
            result.Y = ProtectScaleComponent(result.Y, currentScale.Y, minimumScaleMagnitude);
            result.Z = ProtectScaleComponent(result.Z, currentScale.Z, minimumScaleMagnitude);
            return result;
        }

        private static float ProtectScaleComponent(float value, float previousValue, float minimumMagnitude)
        {
            if (Mathf.Abs(value) >= minimumMagnitude)
                return value;
            if (value < 0.0f || (value == 0.0f && previousValue < 0.0f))
                return -minimumMagnitude;
            return minimumMagnitude;
        }

        /// <summary>
        /// Captures the transaction origin before the first scene mutation.
        /// </summary>
        private bool CaptureTransactionOrigin()
        {
            if (_transactionOrigin != null)
                return true;
            try
            {
                if (!CollectCurrentObjects(_transactionObjects) || _transactionObjects.Count == 0)
                    return false;

                var transforms = new List<Transform>(_transactionObjects.Count);
                for (int i = 0; i < _transactionObjects.Count; i++)
                    transforms.Add(GetSelectedTransform(i));
                GetSelectedObjectsBounds(out var bounds, out _navigationDirty);
                _startBounds = bounds;
                _transactionOrigin = new TransactionOrigin(_transactionObjects, transforms, bounds, _activeMode, _activeTransformSpace, _activePivotType, _gizmoWorld.Orientation, _latchedHandle, Position);
                _interactionResult = new InteractionResult(Vector3.Zero, Quaternion.Identity, Vector3.One, transforms, null);
                return true;
            }
            catch (Exception ex)
            {
                _transactionObjects.Clear();
                ReportInteractionFailure("Transform origin capture failed.", ex);
                return false;
            }
        }

        /// <summary>
        /// Starts a transaction after origin capture and optional transaction-aware duplication.
        /// </summary>
        private bool StartTransformingInternal(bool allowDuplicate)
        {
            if (_isTransforming || !CanTransform || SelectionCount == 0)
                return false;
            try
            {
                if (!_latchedHandle.IsValid && _activeAxis != Axis.None)
                    _latchedHandle = new SemanticHandle(_activeMode, _activeAxis);
                if (_interactionState == InteractionState.Inactive)
                    SetInteractionState(InteractionState.Hovering);
                if (_interactionState == InteractionState.Hovering)
                {
                    if (!CaptureTransactionOrigin())
                        return false;
                    SetInteractionState(InteractionState.Armed);
                }
                else if (_transactionOrigin == null && !CaptureTransactionOrigin())
                {
                    return false;
                }

                // Shift is surface snap for the free/center translation handle.
                // Keep Shift-to-duplicate available for the constrained handles.
                if (allowDuplicate && Owner.UseDuplicate && !IsGeometrySnapActive && !_isDuplicating && CanDuplicate)
                {
                    _isDuplicating = true;
                    _expectingSelectionChange = true;
                    try
                    {
                        OnDuplicate();
                    }
                    finally
                    {
                        _expectingSelectionChange = false;
                    }

                    if (_createdObjects.Count == 0)
                        _isDuplicating = false;
                    if (SelectionCount == 0 || !CollectCurrentObjects(_transactionObjects) || _transactionObjects.Count == 0)
                    {
                        CancelTransforming();
                        return false;
                    }
                }

                if (!CollectCurrentObjects(_transactionObjects) || _transactionObjects.Count == 0)
                {
                    CancelTransforming();
                    return false;
                }

                _startTransforms.Clear();
                for (int i = 0; i < _transactionObjects.Count; i++)
                    _startTransforms.Add(GetSelectedTransform(i));
                if (_createdObjects.Count != 0)
                    _transactionOrigin = _transactionOrigin.WithCreatedObjects(_createdObjects, _duplicateUndoAction);

                _isTransforming = true;
                _lastUseSnapping = Owner.UseSnapping;
                _lastPrecision = Owner.IsAltKeyDown;
                _lastGeometrySnap = IsGeometrySnapActive;
                SetInteractionState(InteractionState.Dragging);
                _interactionAnchor = CreateInteractionAnchor();
                ResetSolverAnchorState();
                _isDrawingTranslationDistance = _activeMode == Mode.Translate && IsTranslateAxis(_activeAxis);
                if (_isDrawingTranslationDistance)
                    _translationDragStartPosition = _transactionOrigin != null ? _transactionOrigin.PivotPosition : Position;
                _cancelledFeedbackTime = 0.0f;
                _feedbackHudQuadrantValid = false;
                _pressedFeedbackTime = FeedbackPressedDuration;
                UpdateFeedbackModel();
                OnStartTransforming();
                return true;
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform transaction start failed; cancelling the transaction.", ex);
                CancelTransforming();
                return false;
            }
        }

        private bool CollectCurrentObjects(List<SceneGraphNode> target)
        {
            target.Clear();
            for (int i = 0; i < SelectionCount; i++)
            {
                var node = GetSelectedObject(i);
                if (!IsNodeUsable(node))
                    return false;
                target.Add(node);
            }
            return true;
        }

        private bool ValidateTransactionObjects()
        {
            if (_transactionOrigin == null)
                return false;
            for (int i = 0; i < _transactionOrigin.Objects.Count; i++)
            {
                var node = _transactionOrigin.Objects[i];
                if (!IsNodeUsable(node))
                    return false;
                if (node != null && !IsTransformFinite(node))
                    return false;
            }
            for (int i = 0; i < _transactionObjects.Count; i++)
            {
                var node = _transactionObjects[i];
                if (!IsNodeUsable(node))
                    return false;
                if (node != null && !IsTransformFinite(node))
                    return false;
            }
            return true;
        }

        private void SetHoveringState(bool hovering)
        {
            if (hovering)
            {
                if (_interactionState == InteractionState.Inactive)
                    SetInteractionState(InteractionState.Hovering);
            }
            else if (_interactionState == InteractionState.Hovering)
            {
                SetInteractionState(InteractionState.Inactive);
            }
        }

        private InteractionAnchor CreateInteractionAnchor()
        {
            var viewDirection = (Vector3)Owner.ViewDirection;
            var fallbackPlane = new Plane(Position, -viewDirection);
            var pointerPosition = Owner.Viewport != null ? Owner.Viewport.ViewMousePosition : default;
            var anchor = new InteractionAnchor(_interactionResult, pointerPosition, Owner.MouseRay, Owner.ViewPosition, Owner.ViewOrientation, fallbackPlane, _latchedHandle);
            _anchorTranslationDelta = Vector3.Zero;
            _anchorRotationDelta = Quaternion.Identity;
            _anchorScaleDelta = _interactionResult.Scale - Vector3.One;
            return anchor;
        }

        private InteractionResult CreateCurrentInteractionResult()
        {
            return new InteractionResult(_interactionResult.Translation, _interactionResult.Rotation, _interactionResult.Scale, GetCurrentTransforms(), _interactionResult.SnapIdentity);
        }

        private List<Transform> GetCurrentTransforms()
        {
            var transforms = new List<Transform>(_transactionObjects.Count);
            for (int i = 0; i < _transactionObjects.Count; i++)
            {
                var node = _transactionObjects[i];
                if (IsNodeUsable(node))
                    transforms.Add(GetSelectedTransform(i));
            }
            return transforms;
        }

        private bool HasWorkingTransformChanges()
        {
            if (_transactionObjects.Count == 0 || _transactionObjects.Count != _startTransforms.Count)
                return false;
            for (int i = 0; i < _transactionObjects.Count; i++)
            {
                var node = _transactionObjects[i];
                if (node == null)
                    return true;
                if (!IsNodeUsable(node))
                    return false;
                if (!AreTransformsEqual(GetSelectedTransform(i), _startTransforms[i]))
                    return true;
            }
            return false;
        }

        private void RestoreTransactionOrigin()
        {
            try
            {
                OnCancelTransforming();
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Derived transform cancellation failed; continuing origin restore.", ex);
            }
            if (_duplicateUndoAction != null)
            {
                var action = _duplicateUndoAction;
                _duplicateUndoAction = null;
                try
                {
                    action.Undo();
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Duplicate rollback failed; deleting known created objects.", ex);
                }
                finally
                {
                    action.Dispose();
                }
                DeleteCreatedObjects();
                return;
            }

            if (_transactionOrigin == null)
                return;
            if (!RestoreTransactionObjectTransforms)
            {
                DeleteCreatedObjects();
                return;
            }
            var count = Math.Min(_transactionOrigin.Objects.Count, _transactionOrigin.OriginalTransforms.Count);
            for (int i = 0; i < count; i++)
            {
                var node = _transactionOrigin.Objects[i];
                if (IsNodeUsable(node))
                {
                    try
                    {
                        ApplyTransactionTransform(node, _transactionOrigin.OriginalTransforms[i]);
                    }
                    catch (Exception ex)
                    {
                        ReportInteractionFailure("Transform origin restore failed.", ex);
                    }
                }
            }
            DeleteCreatedObjects();
        }

        private void DeleteCreatedObjects()
        {
            for (int i = 0; i < _createdObjects.Count; i++)
            {
                var node = _createdObjects[i];
                if (node == null || !IsNodeUsable(node))
                    continue;
                try
                {
                    node.Delete();
                }
                catch (Exception ex)
                {
                    ReportInteractionFailure("Created-object rollback failed.", ex);
                }
            }
        }

        private static bool IsNodeUsable(SceneGraphNode node)
        {
            if (node == null)
                return true;
            try
            {
                return node.IsActive;
            }
            catch
            {
                return false;
            }
        }

        private void ResetTransactionState()
        {
            _isTransforming = false;
            _isDuplicating = false;
            _expectingSelectionChange = false;
            _focusLost = false;
            _createdObjects.Clear();
            _transactionObjects.Clear();
            _duplicateUndoAction = null;
            _transactionOrigin = null;
            _interactionAnchor = null;
            _interactionResult = InteractionResult.Empty;
            _anchorTranslationDelta = Vector3.Zero;
            _anchorRotationDelta = Quaternion.Identity;
            _anchorScaleDelta = Vector3.Zero;
            _latchedHandle = SemanticHandle.None;
            _pressedFeedbackTime = 0.0f;
            _lastGeometrySnap = false;
            _pointerWarpPending = false;
            _pointerWarpFramesRemaining = 0;
            _startTransforms.Clear();
            _activeAxis = Axis.None;
            try
            {
                ClearTransformInteraction();
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform interaction cleanup failed.", ex);
            }
            if (_interactionState != InteractionState.Inactive)
            {
                var previous = _interactionState;
                _interactionState = InteractionState.Inactive;
                NotifyInteractionStateChanged(previous, InteractionState.Inactive);
            }
            try
            {
                ApplyQueuedSettings();
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Queued gizmo settings could not be applied after the transaction.", ex);
            }
            UpdateFeedbackModel();
        }

        private void ResetSolverAnchorState(bool preserveRotationTotal = false)
        {
            _lastIntersectionPosition = _intersectPosition = Vector3.Zero;
            _tDelta = Vector3.Zero;
            _translationScaleSnapDelta = Vector3.Zero;
            _translationSnapAppliedTotal = Vector3.Zero;
            _translationSnapAnchorPosition = Vector3.Zero;
            _translationSnapAnchorInitialized = false;
            _rotationDelta = Quaternion.Identity;
            _rotationSnapDelta = 0.0f;
            if (!preserveRotationTotal)
                _rotationAccumulatedAngle = 0.0f;
            _isDrawingRotationDrag = false;
        }

        private bool SetInteractionState(InteractionState state)
        {
            if (_interactionState == state)
                return true;
            if (!IsAllowedTransition(_interactionState, state))
            {
                ReportInteractionFailure($"Ignored invalid transform lifecycle transition {_interactionState} -> {state}.", null);
                return false;
            }
            var previous = _interactionState;
            _interactionState = state;
            UpdateFeedbackModel();
            NotifyInteractionStateChanged(previous, state);
            return true;
        }

        private void NotifyInteractionStateChanged(InteractionState previous, InteractionState current)
        {
            try
            {
                InteractionStateChanged?.Invoke(previous, current);
            }
            catch (Exception ex)
            {
                ReportInteractionFailure("Transform lifecycle observer failed.", ex);
            }
        }

        private static bool IsTransactionState(InteractionState state)
        {
            return state == InteractionState.Armed || state == InteractionState.Dragging || state == InteractionState.Clutched || state == InteractionState.NumericEntry || state == InteractionState.Committing || state == InteractionState.Cancelling;
        }

        private static bool IsAllowedTransition(InteractionState from, InteractionState to)
        {
            switch (from)
            {
            case InteractionState.Inactive:
                return to == InteractionState.Hovering || to == InteractionState.Armed;
            case InteractionState.Hovering:
                return to == InteractionState.Armed || to == InteractionState.Inactive;
            case InteractionState.Armed:
                return to == InteractionState.Dragging || to == InteractionState.Cancelling || to == InteractionState.Inactive;
            case InteractionState.Dragging:
                return to == InteractionState.Clutched || to == InteractionState.NumericEntry || to == InteractionState.Committing || to == InteractionState.Cancelling;
            case InteractionState.Clutched:
                return to == InteractionState.Dragging || to == InteractionState.Committing || to == InteractionState.Cancelling;
            case InteractionState.NumericEntry:
                return to == InteractionState.Dragging || to == InteractionState.Committing || to == InteractionState.Cancelling;
            case InteractionState.Committing:
                return to == InteractionState.Inactive;
            case InteractionState.Cancelling:
                return to == InteractionState.Inactive;
            default:
                return false;
            }
        }

        private static bool AreTransformsEqual(Transform a, Transform b)
        {
            return a.Translation.Equals(b.Translation) && a.Orientation.Equals(b.Orientation) && a.Scale.Equals(b.Scale);
        }

        private static bool IsTransformFinite(SceneGraphNode node)
        {
            try
            {
                var transform = node.Transform;
                return IsFinite(transform.Translation) && IsFinite(transform.Orientation) && IsFinite(transform.Scale);
            }
            catch
            {
                return false;
            }
        }

        private static bool IsFinite(Vector3 value)
        {
            return !Real.IsNaN(value.X) && !Real.IsInfinity(value.X) &&
                   !Real.IsNaN(value.Y) && !Real.IsInfinity(value.Y) &&
                   !Real.IsNaN(value.Z) && !Real.IsInfinity(value.Z);
        }

        private static bool IsFinite(Quaternion value)
        {
            return !float.IsNaN(value.X) && !float.IsInfinity(value.X) &&
                   !float.IsNaN(value.Y) && !float.IsInfinity(value.Y) &&
                   !float.IsNaN(value.Z) && !float.IsInfinity(value.Z) &&
                   !float.IsNaN(value.W) && !float.IsInfinity(value.W);
        }

        private void ReportInteractionFailure(string message, Exception exception)
        {
            var fullMessage = exception == null ? message : message + " " + exception.Message;
            try
            {
                InteractionDiagnostic?.Invoke(fullMessage);
            }
            catch
            {
            }
            try
            {
                Editor.LogError(fullMessage);
            }
            catch
            {
            }
        }

        private void ApplyQueuedSettings()
        {
            if (_applyingQueuedSettings)
                return;
            _applyingQueuedSettings = true;
            try
            {
                if (_queuedMode.HasValue)
                {
                    var mode = _queuedMode.Value;
                    _queuedMode = null;
                    ActiveMode = mode;
                }
                if (_queuedTransformSpace.HasValue)
                {
                    var space = _queuedTransformSpace.Value;
                    _queuedTransformSpace = null;
                    ActiveTransformSpace = space;
                }
                if (_queuedPivot.HasValue)
                {
                    var pivot = _queuedPivot.Value;
                    _queuedPivot = null;
                    ActivePivot = pivot;
                }
            }
            finally
            {
                _applyingQueuedSettings = false;
            }
        }

        /// <summary>
        /// Applies one solver delta through the transaction result.
        /// </summary>
        protected void ApplyInteractionDelta(ref Vector3 translationDelta, ref Quaternion rotationDelta, ref Vector3 scaleDelta)
        {
            if (UsesOriginAuthoritativePreview)
            {
                RecordInteractionDelta(translationDelta, rotationDelta, scaleDelta);
                ApplyOriginAuthoritativePreview();
            }
            else
            {
                OnApplyTransformation(ref translationDelta, ref rotationDelta, ref scaleDelta);
                RecordInteractionDelta(translationDelta, rotationDelta, scaleDelta);
            }
        }

        /// <summary>
        /// Rebuilds the visible preview from the transaction start transforms and
        /// the current origin-derived result.
        /// </summary>
        private void ApplyOriginAuthoritativePreview()
        {
            if (!UsesOriginAuthoritativePreview || _transactionObjects.Count != _startTransforms.Count)
                return;

            for (int i = 0; i < _transactionObjects.Count; i++)
                ApplyTransactionTransform(_transactionObjects[i], _startTransforms[i]);

            OnApplyInteractionResult(_interactionResult);
            _interactionResult = new InteractionResult(_interactionResult.Translation, _interactionResult.Rotation, _interactionResult.Scale, GetCurrentTransforms(), _interactionResult.SnapIdentity);
        }

        private void ArmInteraction()
        {
            if (_interactionState != InteractionState.Inactive && _interactionState != InteractionState.Hovering)
                return;
            if (_activeMode == Mode.Select || _activeAxis == Axis.None || !CanTransform)
                return;
            _latchedHandle = new SemanticHandle(_activeMode, _activeAxis);
            _cancelledFeedbackTime = 0.0f;
            _feedbackHudQuadrantValid = false;
            _pressedFeedbackTime = FeedbackPressedDuration;
            if (!CaptureTransactionOrigin())
                return;
            WarpTranslationPointerToPivot();
            SetInteractionState(InteractionState.Armed);
        }

        private void WarpTranslationPointerToPivot()
        {
            if (_activeMode != Mode.Translate || Owner?.Viewport?.RootWindow == null)
                return;
            var viewport = Owner.Viewport;
            viewport.ProjectPoint(Position, out var target);
            if (target.X < 0.0f || target.Y < 0.0f || target.X > viewport.Width || target.Y > viewport.Height)
                return;
            _pointerWarpTarget = target;
            _pointerWarpPending = true;
            _pointerWarpFramesRemaining = 4;
            viewport.RootWindow.MousePosition = viewport.PointToWindow(target);
        }

        private bool ConsumePointerWarpFrame()
        {
            if (!_pointerWarpPending)
                return false;
            if (!Owner.IsLeftMouseButtonDown)
            {
                _pointerWarpPending = false;
                _pointerWarpFramesRemaining = 0;
                return false;
            }

            _pointerWarpFramesRemaining--;
            if ((Owner.Viewport.ViewMousePosition - _pointerWarpTarget).LengthSquared <= 4.0f || _pointerWarpFramesRemaining <= 0)
                _pointerWarpPending = false;
            _lastIntersectionPosition = _intersectPosition = Vector3.Zero;
            _tDelta = Vector3.Zero;
            return true;
        }

        private bool IsCameraClutchActive => Owner.IsMiddleMouseButtonDown || Owner.IsRightMouseButtonDown;

        private bool HandleFocusAndClutch()
        {
            if (_focusLost)
            {
                var viewport = Owner.Viewport;
                if (viewport == null || !viewport.ContainsFocus)
                    return true;
                _focusLost = false;
                if (!Owner.IsLeftMouseButtonDown)
                {
                    CancelTransforming();
                    return true;
                }
                if (_interactionState == InteractionState.Clutched)
                    EndCameraClutch();
            }

            if (_interactionState == InteractionState.Dragging && IsCameraClutchActive)
            {
                BeginCameraClutch();
                return true;
            }
            if (_interactionState == InteractionState.Clutched)
            {
                if (IsCameraClutchActive)
                    return true;
                EndCameraClutch();
            }

            if (_interactionState == InteractionState.Dragging)
            {
                bool useSnapping = Owner.UseSnapping;
                bool precision = Owner.IsAltKeyDown;
                bool geometrySnap = IsGeometrySnapActive;
                if (useSnapping != _lastUseSnapping || precision != _lastPrecision || geometrySnap != _lastGeometrySnap)
                {
                    _lastUseSnapping = useSnapping;
                    _lastPrecision = precision;
                    _lastGeometrySnap = geometrySnap;
                    if (!geometrySnap)
                        ClearGeometrySnapTarget();
                    ReanchorInteraction();
                }
            }
            return false;
        }

        /// <summary>
        /// Queues or applies a mode change according to transaction ownership.
        /// </summary>
        internal bool TryQueueMode(Mode value)
        {
            if (!_applyingQueuedSettings && HasActiveTransaction)
            {
                _queuedMode = value;
                return true;
            }
            return false;
        }

        /// <summary>
        /// Queues or applies a transform-space change according to transaction ownership.
        /// </summary>
        internal bool TryQueueTransformSpace(TransformSpace value)
        {
            if (!_applyingQueuedSettings && HasActiveTransaction)
            {
                _queuedTransformSpace = value;
                return true;
            }
            return false;
        }

        /// <summary>
        /// Queues or applies a pivot change according to transaction ownership.
        /// </summary>
        internal bool TryQueuePivot(PivotType value)
        {
            if (!_applyingQueuedSettings && HasActiveTransaction)
            {
                _queuedPivot = value;
                return true;
            }
            return false;
        }

        /// <summary>
        /// Starts a transform transaction from the existing polling solver.
        /// </summary>
        public void StartTransforming(bool allowDuplicate = true)
        {
            StartTransformingInternal(allowDuplicate);
        }

        /// <summary>
        /// Commits a transform transaction, or clears an armed click with no mutation.
        /// </summary>
        public void EndTransforming()
        {
            CommitTransforming();
        }

        /// <summary>
        /// Called before the common origin restore when a derived gizmo owns
        /// additional non-scene transform data.
        /// </summary>
        protected virtual void OnCancelTransforming()
        {
        }

        /// <summary>
        /// Gets a value indicating whether scene preview transforms are rebuilt
        /// from the transaction start rather than applied incrementally.
        /// </summary>
        protected virtual bool UsesOriginAuthoritativePreview => false;

        /// <summary>
        /// Gets a value indicating whether the base class should restore scene
        /// node transforms during cancellation.
        /// </summary>
        protected virtual bool RestoreTransactionObjectTransforms => true;

        /// <summary>
        /// Applies a world-space transaction transform to one selected object.
        /// Derived gizmos can translate the value into their storage space.
        /// </summary>
        protected virtual void ApplyTransactionTransform(SceneGraphNode node, Transform transform)
        {
            if (node != null && IsNodeUsable(node))
                node.Transform = transform;
        }

        /// <summary>
        /// Applies the current origin-derived result to the scene integration.
        /// </summary>
        protected virtual void OnApplyInteractionResult(InteractionResult result)
        {
            var translation = result.Translation;
            var rotation = result.Rotation;
            var scale = result.Scale - Vector3.One;
            OnApplyTransformation(ref translation, ref rotation, ref scale);
        }

        /// <inheritdoc />
        public override void OnDeactivated()
        {
            CancelTransforming();
            base.OnDeactivated();
        }
    }
}
