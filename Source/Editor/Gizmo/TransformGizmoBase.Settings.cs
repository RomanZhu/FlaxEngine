// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEngine;

namespace FlaxEditor.Gizmo
{
    public partial class TransformGizmoBase
    {
        /// <summary>
        /// Default projected radius of the transform gizmo in logical viewport pixels.
        /// </summary>
        private const float GizmoRadiusPixels = 96.0f;

        /// <summary>
        /// Scale used to convert the unit editor primitive meshes into gizmo-space units.
        /// </summary>
        private const float GizmoModelsScale2RealGizmoSize = 0.075f;

        /// <summary>
        /// Legacy perspective scale factor used by the vertex-snap point marker.
        /// </summary>
        private const float VertexSnapPointScaleFactor = 24.0f;

        /// <summary>
        /// Radius of the authored gizmo basis used to convert the projected radius to
        /// the scale stored in <see cref="_gizmoWorld"/>.
        /// </summary>
        private const float GizmoGeometryRadiusRaw = AxisLength;

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

        /// <summary>
        /// Width of an axis motor target in viewport pixels.
        /// </summary>
        private const float AxisMotorTargetWidthPixels = 16.0f;

        /// <summary>
        /// Width of an arrow or cube cap motor envelope in viewport pixels.
        /// </summary>
        private const float CapMotorTargetWidthPixels = 20.0f;

        /// <summary>
        /// Expansion around the visible plane handle used for acquisition.
        /// </summary>
        private const float PlaneMotorExpansionPixels = 6.0f;

        /// <summary>
        /// Diameter of the center handle motor target in viewport pixels.
        /// </summary>
        private const float CenterMotorTargetSizePixels = 24.0f;

        /// <summary>
        /// Width of the rotation-ring motor band in viewport pixels.
        /// </summary>
        private const float RingMotorTargetWidthPixels = 16.0f;

        /// <summary>
        /// Additional retention margin used by hover hysteresis.
        /// </summary>
        private const float HoverRetentionExpansionPixels = 6.0f;

        /// <summary>
        /// Minimum physical depth used when a perspective pivot is close to the camera.
        /// </summary>
        private const float MinimumProjectionDepth = 0.01f;

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
        public float TranslationSnapValue = 25;

        /// <summary>
        /// Rotation snap value
        /// </summary>
        public float RotationSnapValue = 30;

        /// <summary>
        /// Legacy scale-factor snap value. Scale snapping uses <see cref="TranslationSnapValue"/> as a world-unit grid size.
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
