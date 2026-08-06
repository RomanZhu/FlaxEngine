// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        /// <summary>
        /// Scale of the gizmo itself
        /// </summary>
        private const float GizmoScaleFactor = 24;

        /// <summary>
        /// The length of each axis (outwards)
        /// </summary>
        private const float AxisLength = 3.5f;

        /// <summary>
        /// Visible start of the axis shaft, outside the center handle.
        /// </summary>
        private const float AxisVisualStart = 0.35f;

        /// <summary>
        /// Length of the compact polygonal arrow head.
        /// </summary>
        private const float AxisArrowHeadLength = 0.58f;

        /// <summary>
        /// Size of the cube cap used by scale axes.
        /// </summary>
        private const float AxisScaleCubeSize = 0.50f;

        private const float RotateRadiusRaw = 3.2f;
        private const float RotateTrackballSensitivity = 1.0f / 60.0f;

        private Mode _activeMode = Mode.Translate;
        private Axis _activeAxis = Axis.None;
        private TransformSpace _activeTransformSpace = TransformSpace.World;
        private PivotType _activePivotType = PivotType.SelectionCenter;

        /// <summary>
        /// True if enable grid snapping when moving objects
        /// </summary>
        public bool TranslationSnapEnable = false;

        /// <summary>
        /// True if enable grid snapping when rotating objects
        /// </summary>
        public bool RotationSnapEnabled = false;

        /// <summary>
        /// True if enable grid snapping when scaling objects
        /// </summary>
        public bool ScaleSnapEnabled = false;

        /// <summary>
        /// True if enable absolute grid snapping (snaps objects to world-space grid, not the one relative to gizmo location)
        /// </summary>
        public bool AbsoluteSnapEnabled = false;

        /// <summary>
        /// Translation snap value
        /// </summary>
        public float TranslationSnapValue = 10;

        /// <summary>
        /// Rotation snap value
        /// </summary>
        public float RotationSnapValue = 30;

        /// <summary>
        /// Scale snap value
        /// </summary>
        public float ScaleSnapValue = 1.0f;

        /// <summary>
        /// Gets or sets the current pivot type.
        /// </summary>
        public PivotType ActivePivot
        {
            get => _activePivotType;
            set
            {
                if (_activePivotType != value)
                {
                    if (TryQueuePivot(value))
                        return;
                    _isTransforming = false;
                    _isDuplicating = false;
                    _startTransforms.Clear();
                    ClearTransformInteraction(!_isVertexSnapTemporaryPivot);
                    _activePivotType = value;
                    PivotChanged?.Invoke();
                }
            }
        }

        /// <summary>
        /// Gets the current axis type.
        /// </summary>
        public Axis ActiveAxis => _activeAxis;

        /// <summary>
        /// Gets or sets the current gizmo mode.
        /// </summary>
        public Mode ActiveMode
        {
            get => _activeMode;
            set
            {
                if (_activeMode != value)
                {
                    if (TryQueueMode(value))
                        return;
                    _isTransforming = false;
                    _isDuplicating = false;
                    _startTransforms.Clear();
                    ClearTransformInteraction(!_isVertexSnapTemporaryPivot);
                    _activeMode = value;
                    ModeChanged?.Invoke();
                }
            }
        }

        /// <summary>
        /// Event fired when active gizmo mode gets changed.
        /// </summary>
        public Action ModeChanged;

        /// <summary>
        /// Event fired when active gizmo pivot gets changed.
        /// </summary>
        public Action PivotChanged;

        /// <summary>
        /// Gets or sets the current gizmo transform space.
        /// </summary>
        public TransformSpace ActiveTransformSpace
        {
            get => _activeTransformSpace;
            set
            {
                if (_activeTransformSpace != value)
                {
                    if (TryQueueTransformSpace(value))
                        return;
                    _activeTransformSpace = value;
                    TransformSpaceChanged?.Invoke();
                }
            }
        }

        /// <summary>
        /// Event fired when active transform space gets changed.
        /// </summary>
        public Action TransformSpaceChanged;

        /// <summary>
        /// Toggles gizmo transform space
        /// </summary>
        public void ToggleTransformSpace()
        {
            ActiveTransformSpace = _activeTransformSpace == TransformSpace.World ? TransformSpace.Local : TransformSpace.World;
        }

        /// <summary>
        /// Toggles gizmo pivot between selection center and object origin.
        /// </summary>
        public void TogglePivot()
        {
            ActivePivot = _activePivotType == PivotType.SelectionCenter ? PivotType.ObjectCenter : PivotType.SelectionCenter;
        }
    }
}
