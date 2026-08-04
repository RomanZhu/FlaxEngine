// Copyright (c) Wojciech Figat. All rights reserved.

using System.ComponentModel;
using FlaxEngine;

namespace FlaxEditor.Options
{
    /// <summary>
    /// Editor viewport options data container.
    /// </summary>
    [CustomEditor(typeof(Editor<ViewportOptions>))]
    public class ViewportOptions
    {
        /// <summary>
        /// The default Alt+right mouse button drag zoom speed for viewport cameras.
        /// </summary>
        public const float DefaultAltRightMouseZoomSpeed = 12.5f;

        /// <summary>
        /// Gets or sets the mouse movement sensitivity scale applied when using the viewport camera.
        /// </summary>
        [DefaultValue(1.0f), Limit(0.01f, 100.0f)]
        [EditorDisplay("General"), EditorOrder(100), Tooltip("The mouse movement sensitivity scale applied when using the viewport camera.")]
        public float MouseSensitivity { get; set; } = 1.0f;

        /// <summary>
        /// Gets or sets the mouse wheel sensitivity applied to zoom in orthographic mode.
        /// </summary>
        [DefaultValue(1.0f), Limit(0.01f, 100.0f)]
        [EditorDisplay("General"), EditorOrder(101), Tooltip("The mouse wheel sensitivity applied to zoom in orthographic mode.")]
        public float MouseWheelSensitivity { get; set; } = 1.0f;
        
        /// <summary>
        /// Gets or sets whether to invert the Y rotation of the mouse in the editor viewport.
        /// </summary>
        [DefaultValue(false)]
        [EditorDisplay("General"), EditorOrder(102), Tooltip("Whether to invert the Y rotation of the mouse in the editor viewport.")]
        public bool InvertMouseYAxisRotation { get; set; } = false;

        /// <summary>
        /// Gets or sets the total amount of steps the camera needs to go from minimum to maximum speed.
        /// </summary>
        [DefaultValue(64), Limit(1, 128)]
        [EditorDisplay("Camera"), EditorOrder(110), Tooltip("The total amount of steps the camera needs to go from minimum to maximum speed.")]
        public int TotalCameraSpeedSteps { get; set; } = 64;

        /// <summary>
        /// Gets or sets the degree to which the camera will be eased when using camera flight in the editor window.
        /// </summary>
        [DefaultValue(3.0f), Limit(1.0f, 8.0f)]
        [EditorDisplay("Camera"), EditorOrder(111), Tooltip("The degree to which the camera will be eased when using camera flight in the editor window (ignored if camera easing degree is enabled).")]
        public float CameraEasingDegree { get; set; } = 3.0f;

        /// <summary>
        /// Gets or sets the Alt+right mouse button drag zoom speed for viewport cameras.
        /// </summary>
        [DefaultValue(DefaultAltRightMouseZoomSpeed), Limit(0.01f, 100.0f, 0.1f)]
        [EditorDisplay("Camera"), EditorOrder(112), Tooltip("The Alt+right mouse button drag zoom speed for viewport cameras.")]
        public float AltRightMouseZoomSpeed { get; set; } = DefaultAltRightMouseZoomSpeed;

        /// <summary>
        /// Gets or sets the default movement speed for the viewport camera (must be in range between minimum and maximum movement speed values).
        /// </summary>
        [DefaultValue(1.0f), Limit(0.05f, 32.0f)]
        [EditorDisplay("Defaults"), EditorOrder(120), Tooltip("The default movement speed for the viewport camera (must be in range between minimum and maximum movement speed values).")]
        public float MovementSpeed { get; set; } = 1.0f;

        /// <summary>
        /// Gets or sets the default minimum camera movement speed.
        /// </summary>
        [DefaultValue(0.05f), Limit(0.05f, 32.0f)]
        [EditorDisplay("Defaults"), EditorOrder(121), Tooltip("The default minimum movement speed for the viewport camera.")]
        public float MinMovementSpeed { get; set; } = 0.05f;

        /// <summary>
        /// Gets or sets the default maximum camera movement speed.
        /// </summary>
        [DefaultValue(32.0f), Limit(16.0f, 1000.0f)]
        [EditorDisplay("Defaults"), EditorOrder(122), Tooltip("The default maximum movement speed for the viewport camera.")]
        public float MaxMovementSpeed { get; set; } = 32f;

        /// <summary>
        /// Gets or sets the default camera easing mode.
        /// </summary>
        [DefaultValue(true)]
        [EditorDisplay("Defaults"), EditorOrder(130), Tooltip("Enables eased camera speed steps and RMB fly input inertia.")]
        public bool UseCameraEasing { get; set; } = true;

        /// <summary>
        /// Gets or sets the default near clipping plane distance for the viewport camera.
        /// </summary>
        [DefaultValue(10.0f), Limit(0.001f, 1000.0f)]
        [EditorDisplay("Defaults"), EditorOrder(140), Tooltip("The default near clipping plane distance for the viewport camera.")]
        public float NearPlane { get; set; } = 10.0f;

        /// <summary>
        /// Gets or sets the default far clipping plane distance for the viewport camera.
        /// </summary>
        [DefaultValue(40000.0f), Limit(10.0f)]
        [EditorDisplay("Defaults"), EditorOrder(150), Tooltip("The default far clipping plane distance for the viewport camera.")]
        public float FarPlane { get; set; } = 40000.0f;

        /// <summary>
        /// Gets or sets the default field of view angle (in degrees) for the viewport camera.
        /// </summary>
        [DefaultValue(60.0f), Limit(35.0f, 160.0f, 0.1f)]
        [EditorDisplay("Defaults"), EditorOrder(160), Tooltip("The default field of view angle (in degrees) for the viewport camera.")]
        public float FieldOfView { get; set; } = 60.0f;

        /// <summary>
        /// Gets or sets the default camera orthographic mode.
        /// </summary>
        [DefaultValue(false)]
        [EditorDisplay("Defaults"), EditorOrder(170), Tooltip("The default camera orthographic mode.")]
        public bool UseOrthographicProjection { get; set; } = false;

        /// <summary>
        /// Gets or sets the default camera orthographic scale (if camera uses orthographic mode).
        /// </summary>
        [DefaultValue(5.0f), Limit(0.001f, 100000.0f, 0.1f)]
        [EditorDisplay("Defaults"), EditorOrder(180), Tooltip("The default camera orthographic scale (if camera uses orthographic mode).")]
        public float OrthographicScale { get; set; } = 5.0f;

        /// <summary>
        /// Gets or sets the default panning direction for the viewport camera.
        /// </summary>
        [DefaultValue(false)]
        [EditorDisplay("Defaults"), EditorOrder(190), Tooltip("The default panning direction for the viewport camera.")]
        public bool InvertPanning { get; set; } = false;

        /// <summary>
        /// Gets or sets the default relative panning mode.
        /// </summary>
        [DefaultValue(true)]
        [EditorDisplay("Defaults"), EditorOrder(200), Tooltip("The default relative panning mode. Uses distance between camera and target to determine panning speed.")]
        public bool UseRelativePanning { get; set; } = true;

        /// <summary>
        /// Gets or sets the default panning speed (ignored if relative panning is speed enabled).
        /// </summary>
        [DefaultValue(0.8f), Limit(0.01f, 128.0f, 0.1f)]
        [EditorDisplay("Defaults"), EditorOrder(210), Tooltip("The default camera panning speed (ignored if relative panning is enabled).")]
        public float PanningSpeed { get; set; } = 0.8f;

        /// <summary>
        /// Gets or sets the default editor viewport grid scale.
        /// </summary>
        [DefaultValue(50.0f), Limit(25.0f, 500.0f, 5.0f)]
        [EditorDisplay("Defaults"), EditorOrder(220), Tooltip("The default editor viewport grid scale.")]
        public float ViewportGridScale { get; set; } = 50.0f;
        
        /// <summary>
        /// Gets or sets the use persistence over defaults setting
        /// </summary>
        [DefaultValue(true)]
        [EditorDisplay("Defaults"), EditorOrder(230), Tooltip("Allow persistence setting from last session to override default settings")]
        public bool UsePersistenceOverDefaults { get; set; } = true;

        /// <summary>
        /// Gets or sets the default walking speed for the in-scene character controller camera mode.
        /// </summary>
        [DefaultValue(420.0f), Limit(1.0f, 10000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(240), Tooltip("The default walking speed for the in-scene character controller camera mode.")]
        public float CharacterControllerWalkSpeed { get; set; } = 420.0f;

        /// <summary>
        /// Gets or sets the minimum walking speed used when mouse wheel speed adjustment is active.
        /// </summary>
        [DefaultValue(60.0f), Limit(1.0f, 10000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(241), Tooltip("The minimum walking speed used when mouse wheel speed adjustment is active.")]
        public float CharacterControllerMinWalkSpeed { get; set; } = 60.0f;

        /// <summary>
        /// Gets or sets the maximum walking speed used when mouse wheel speed adjustment is active.
        /// </summary>
        [DefaultValue(2000.0f), Limit(1.0f, 10000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(242), Tooltip("The maximum walking speed used when mouse wheel speed adjustment is active.")]
        public float CharacterControllerMaxWalkSpeed { get; set; } = 2000.0f;

        /// <summary>
        /// Gets or sets the speed change applied per mouse wheel step in the in-scene character controller camera mode.
        /// </summary>
        [DefaultValue(50.0f), Limit(1.0f, 10000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(243), Tooltip("The speed change applied per mouse wheel step in the in-scene character controller camera mode.")]
        public float CharacterControllerScrollSpeedStep { get; set; } = 50.0f;

        /// <summary>
        /// Gets or sets the sprint speed multiplier for the in-scene character controller camera mode.
        /// </summary>
        [DefaultValue(1.85f), Limit(1.0f, 20.0f, 0.05f)]
        [EditorDisplay("Character Controller"), EditorOrder(244), Tooltip("The sprint speed multiplier for the in-scene character controller camera mode.")]
        public float CharacterControllerSprintMultiplier { get; set; } = 1.85f;

        /// <summary>
        /// Gets or sets the jump speed for the in-scene character controller camera mode.
        /// </summary>
        [DefaultValue(560.0f), Limit(0.0f, 10000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(245), Tooltip("The jump speed for the in-scene character controller camera mode.")]
        public float CharacterControllerJumpSpeed { get; set; } = 560.0f;

        /// <summary>
        /// Gets or sets the radius for the in-scene character controller.
        /// </summary>
        [DefaultValue(35.0f), Limit(1.0f, 1000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(246), Tooltip("The radius for the in-scene character controller.")]
        public float CharacterControllerRadius { get; set; } = 35.0f;

        /// <summary>
        /// Gets or sets the height for the in-scene character controller.
        /// </summary>
        [DefaultValue(115.0f), Limit(1.0f, 2000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(247), Tooltip("The height for the in-scene character controller.")]
        public float CharacterControllerHeight { get; set; } = 115.0f;

        /// <summary>
        /// Gets or sets the eye height above the in-scene character controller position.
        /// </summary>
        [DefaultValue(160.0f), Limit(1.0f, 2000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(248), Tooltip("The eye height above the in-scene character controller position.")]
        public float CharacterControllerEyeHeight { get; set; } = 160.0f;

        /// <summary>
        /// Gets or sets the step offset for the in-scene character controller.
        /// </summary>
        [DefaultValue(35.0f), Limit(0.0f, 1000.0f, 1.0f)]
        [EditorDisplay("Character Controller"), EditorOrder(249), Tooltip("The step offset for the in-scene character controller.")]
        public float CharacterControllerStepOffset { get; set; } = 35.0f;

        /// <summary>
        /// Gets or sets the slope limit for the in-scene character controller.
        /// </summary>
        [DefaultValue(50.0f), Limit(0.0f, 90.0f, 0.1f)]
        [EditorDisplay("Character Controller"), EditorOrder(250), Tooltip("The slope limit for the in-scene character controller.")]
        public float CharacterControllerSlopeLimit { get; set; } = 50.0f;

        /// <summary>
        /// Gets or sets the view distance you can see the grid.
        /// </summary>
        [DefaultValue(2500.0f)]
        [EditorDisplay("Grid"), EditorOrder(300), Tooltip("The maximum distance you will be able to see the grid.")]
        public float ViewportGridViewDistance { get; set; } = 2500.0f;

        /// <summary>
        /// Gets or sets the grid color.
        /// </summary>
        [DefaultValue(typeof(Color), "0.5,0.5,0.5,1.0")]
        [EditorDisplay("Grid"), EditorOrder(310), Tooltip("The color for the viewport grid.")]
        public Color ViewportGridColor { get; set; } = new Color(0.5f, 0.5f, 0.5f, 1.0f);

        /// <summary>
        /// Gets or sets the minimum size used for viewport icons.
        /// </summary>
        [DefaultValue(7.0f), Limit(1.0f, 1000.0f, 5.0f)]
        [EditorDisplay("Viewport Icons"), EditorOrder(400)]
        public float IconsMinimumSize { get; set; } = 7.0f;

        /// <summary>
        /// Gets or sets the maximum size used for viewport icons.
        /// </summary>
        [DefaultValue(30.0f), Limit(1.0f, 1000.0f, 5.0f)]
        [EditorDisplay("Viewport Icons"), EditorOrder(410)]
        public float IconsMaximumSize { get; set; } = 30.0f;

        /// <summary>
        /// Gets or sets the distance towards the camera at which the max icon scale will be applied. Set to 0 to disable scaling the icons based on the distance to the camera.
        /// </summary>
        [DefaultValue(1000.0f), Limit(0.0f, 20000.0f, 5.0f)]
        [EditorDisplay("Viewport Icons"), EditorOrder(410)]
        public float MaxSizeDistance { get; set; } = 1000.0f;

        /// <summary>
        /// Gets or sets a value that indicates whether the main viewports <see cref="Gizmo.DirectionGizmo"/> is visible.
        /// </summary>
        [DefaultValue(true)]
        [EditorDisplay("Direction Gizmo"), EditorOrder(500), Tooltip("Sets the visibility of the direction gizmo in the main editor viewport.")]
        public bool ShowDirectionGizmo { get; set; } = true;

        /// <summary>
        /// Gets or sets a value by which the main viewports <see cref="Gizmo.DirectionGizmo"/> size is multiplied with.
        /// </summary>
        [DefaultValue(1f), Limit(0.0f, 2.0f)]
        [EditorDisplay("Direction Gizmo"), EditorOrder(501), Tooltip("The scale of the direction gizmo in the main viewport.")]
        public float DirectionGizmoScale { get; set; } = 1f;

        /// <summary>
        /// Gets or sets a value for the opacity of the main viewports <see cref="Gizmo.DirectionGizmo"/> background. Background will only show when the gizmo is hovered.
        /// </summary>
        [DefaultValue(0.1f), Limit(0.0f, 1.0f)]
        [EditorDisplay("Direction Gizmo"), EditorOrder(502), Tooltip("The background opacity of the of the direction gizmo in the main viewport.")]
        public float DirectionGizmoBackgroundOpacity { get; set; } = 0.1f;

        /// <summary>
        /// Gets or sets a value for the opacity of the main viewports <see cref="Gizmo.DirectionGizmo"/>.
        /// </summary>
        [DefaultValue(0.6f), Limit(0.0f, 1.0f)]
        [EditorDisplay("Direction Gizmo"), EditorOrder(503), Tooltip("The opacity of the of the direction gizmo in the main viewport.")]
        public float DirectionGizmoOpacity { get; set; } = 0.6f;

        /// <summary>
        /// Gets or sets a value for the opacity of the main viewports <see cref="Gizmo.DirectionGizmo"/>.
        /// </summary>
        [DefaultValue(1f), Limit(0.0f, 2.0f)]
        [EditorDisplay("Direction Gizmo"), EditorOrder(504), Tooltip("The brightness of the of the direction gizmo in the main viewport.")]
        public float DirectionGizmoBrightness{ get; set; } = 1f;
    }
}
