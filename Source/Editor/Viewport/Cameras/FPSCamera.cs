// Copyright (c) Wojciech Figat. All rights reserved.

#if USE_LARGE_WORLDS
using Real = System.Double;
#else
using Real = System.Single;
#endif

using FlaxEditor.Gizmo;
using FlaxEngine;

namespace FlaxEditor.Viewport.Cameras
{
    /// <summary>
    /// Implementation of <see cref="ViewportCamera"/> that simulated the first-person camera which can fly though the scene.
    /// </summary>
    /// <seealso cref="FlaxEditor.Viewport.Cameras.ViewportCamera" />
    [HideInEditor]
    public class FPSCamera : ViewportCamera
    {
        private const float FlyMoveInertiaResponse = 24.0f;
        private const float FlyLookInertiaResponse = 55.0f;
        private const float FlyInertiaStopThresholdSq = 0.000001f;
        private const float AltRightMouseZoomMinDistance = 500.0f;
        private const float MinimumZoomDistance = 100.0f;
        private const float RecenterMinMoveDistanceSq = 0.000001f;

        private Transform _startMove;
        private Transform _endMove;
        private Vector3 _startMoveTargetPoint;
        private Vector3 _endMoveTargetPoint;
        private Vector3 _flyMoveDelta;
        private Float2 _flyMouseDelta;
        private Vector3 _altRightMouseZoomDirection;
        private bool _hasAltRightMouseZoomDirection;
        private bool _animateMoveTargetPoint;
        private float _moveStartTime = -1;
        private float _additionalFOV;

        /// <summary>
        /// Gets a value indicating whether this viewport is animating movement.
        /// </summary>
        public bool IsAnimatingMove => _moveStartTime > Mathf.Epsilon;

        /// <summary>
        /// The target point location. It's used to orbit around it when user clicks Alt+LMB.
        /// </summary>
        public Vector3 TargetPoint = new Vector3(-200);

        /// <summary>
        /// Additional field of view used for zooming the camera in and out.
        /// </summary>
        public float AdditionalZoomFOV
        {
            get => _additionalFOV;
            private set => _additionalFOV = Mathf.Clamp(value, 5 - Viewport.FieldOfView, 160f - Viewport.FieldOfView);
        }

        /// <summary>
        /// Sets view.
        /// </summary>
        /// <param name="position">The view position.</param>
        /// <param name="direction">The view direction.</param>
        public void SetView(Vector3 position, Vector3 direction)
        {
            if (IsAnimatingMove)
                return;

            ResetFlyInertia();

            // Rotate and move
            Viewport.ViewPosition = position;
            Viewport.ViewDirection = direction;
        }

        /// <summary>
        /// Sets view.
        /// </summary>
        /// <param name="position">The view position.</param>
        /// <param name="orientation">The view rotation.</param>
        public void SetView(Vector3 position, Quaternion orientation)
        {
            if (IsAnimatingMove)
                return;

            ResetFlyInertia();

            // Rotate and move
            Viewport.ViewPosition = position;
            Viewport.ViewOrientation = orientation;
        }

        /// <summary>
        /// Start animating viewport movement to the target transformation.
        /// </summary>
        /// <param name="position">The target position.</param>
        /// <param name="orientation">The target orientation.</param>
        public void MoveViewport(Vector3 position, Quaternion orientation)
        {
            MoveViewport(new Transform(position, orientation));
        }

        /// <summary>
        /// Start animating viewport movement to the target transformation.
        /// </summary>
        /// <param name="target">The target transform.</param>
        public void MoveViewport(Transform target)
        {
            ResetFlyInertia();

            _startMove = Viewport.ViewTransform;
            _endMove = target;
            _animateMoveTargetPoint = false;
            _moveStartTime = Time.UnscaledGameTime;
        }

        private static float GetInertiaAmount(float dt, float response)
        {
            return 1.0f - Mathf.Exp(-Mathf.Max(dt, 0.0f) * response);
        }

        private void ResetFlyInertia()
        {
            _flyMoveDelta = Vector3.Zero;
            _flyMouseDelta = Float2.Zero;
        }

        private bool HasFlyInertia()
        {
            return _flyMoveDelta.LengthSquared >= FlyInertiaStopThresholdSq || _flyMouseDelta.LengthSquared >= FlyInertiaStopThresholdSq;
        }

        private void ApplyFlyInertia(float dt, ref Vector3 moveDelta, ref Float2 mouseDelta)
        {
            var moveAmount = GetInertiaAmount(dt, FlyMoveInertiaResponse);
            var lookAmount = GetInertiaAmount(dt, FlyLookInertiaResponse);
            _flyMoveDelta = Vector3.Lerp(_flyMoveDelta, moveDelta, moveAmount);
            _flyMouseDelta = Float2.Lerp(_flyMouseDelta, mouseDelta, lookAmount);

            if (moveDelta.IsZero && _flyMoveDelta.LengthSquared < FlyInertiaStopThresholdSq)
                _flyMoveDelta = Vector3.Zero;
            if (mouseDelta.IsZero && _flyMouseDelta.LengthSquared < FlyInertiaStopThresholdSq)
                _flyMouseDelta = Float2.Zero;

            moveDelta = _flyMoveDelta;
            mouseDelta = _flyMouseDelta;
        }

        /// <inheritdoc />
        public override void ShowSphere(ref BoundingSphere sphere, ref Quaternion orientation)
        {
            Vector3 position;
            if (Viewport.UseOrthographicProjection)
            {
                position = sphere.Center + Vector3.Backward * orientation * (sphere.Radius * 5.0f);
                Viewport.OrthographicScale = (float)Vector3.Distance(position, sphere.Center) / 1000;
            }
            else
            {
                // Calculate the distance so that the sphere fits roughly 70% in FOV.
                // Keep tiny targets usable and clip to the far plane so large objects do not disappear.
                var distance = Mathf.Clamp(1.4f * sphere.Radius / Mathf.Tan(Mathf.DegreesToRadians * Viewport.FieldOfView / 2), MinimumZoomDistance, Viewport.FarPlane);
                position = sphere.Center - Vector3.Forward * orientation * distance;
            }
            TargetPoint = sphere.Center;
            MoveViewport(position, orientation);
        }

        /// <inheritdoc />
        public override void SetArcBallView(Quaternion orientation, Vector3 orbitCenter, Real orbitRadius)
        {
            base.SetArcBallView(orientation, orbitCenter, orbitRadius);

            TargetPoint = orbitCenter;
        }

        /// <summary>
        /// Moves the camera center to the given world-space hit without moving the view forward or backward.
        /// </summary>
        /// <param name="hitPoint">The target hit point.</param>
        public void RecenterView(Vector3 hitPoint)
        {
            ResetFlyInertia();

            var viewPosition = Viewport.ViewPosition;
            var rotation = Viewport.ViewOrientation;
            var forward = Vector3.Forward * rotation;
            var up = Vector3.Up * rotation;
            var right = Vector3.Cross(forward, up);
            var toHit = hitPoint - viewPosition;
            if (Vector3.Dot(toHit, forward) <= Viewport.NearPlane)
                return;
            var planarMove = right * Vector3.Dot(toHit, right) + up * Vector3.Dot(toHit, up);
            if (planarMove.LengthSquared < RecenterMinMoveDistanceSq && Vector3.DistanceSquared(TargetPoint, hitPoint) < RecenterMinMoveDistanceSq)
                return;

            _startMove = Viewport.ViewTransform;
            _endMove = new Transform(viewPosition + planarMove, rotation);
            _startMoveTargetPoint = TargetPoint;
            _endMoveTargetPoint = hitPoint;
            _animateMoveTargetPoint = true;
            _moveStartTime = Time.UnscaledGameTime;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // Update animated movement
            if (IsAnimatingMove)
            {
                // Calculate linear progress
                float animationDuration = 0.5f;
                float time = Time.UnscaledGameTime;
                float progress = (time - _moveStartTime) / animationDuration;

                // Check for end
                if (progress >= 1.0f)
                {
                    // Animation has been finished
                    _moveStartTime = -1;
                }

                // Animate camera
                try
                {
                    float a = Mathf.Saturate(progress);
                    a = a * a * a;
                    var targetTransform = Transform.Lerp(_startMove, _endMove, a);
                    if (progress >= 1.0f)
                        targetTransform = _endMove; // Be precise
                    targetTransform.Scale = Vector3.Zero;
                    Viewport.ViewPosition = targetTransform.Translation;
                    Viewport.ViewOrientation = targetTransform.Orientation;
                    if (_animateMoveTargetPoint)
                    {
                        TargetPoint = progress >= 1.0f ? _endMoveTargetPoint : Vector3.Lerp(_startMoveTargetPoint, _endMoveTargetPoint, a);
                        if (progress >= 1.0f)
                            _animateMoveTargetPoint = false;
                    }
                }
                catch
                {
                    // Fix camera if lerp failed (eg. large world with NaNs inside)
                    Viewport.ViewPosition = Vector3.Zero;
                    Viewport.ViewOrientation = Quaternion.Identity;
                    _animateMoveTargetPoint = false;
                }
            }
        }

        /// <inheritdoc />
        public override void EndAltRightMouseZoom()
        {
            if (_hasAltRightMouseZoomDirection)
            {
                var distance = Vector3.Dot(TargetPoint - Viewport.ViewPosition, _altRightMouseZoomDirection);
                if (distance <= 0.0f)
                    TargetPoint += _altRightMouseZoomDirection * (AltRightMouseZoomMinDistance - distance);
                _hasAltRightMouseZoomDirection = false;
            }
        }

        /// <inheritdoc />
        public override void CancelInputInertia()
        {
            ResetFlyInertia();
        }

        /// <inheritdoc />
        public override bool TryGetCameraCenter(out Vector3 center)
        {
            center = TargetPoint;
            return true;
        }

        /// <inheritdoc />
        public override void UpdateView(float dt, ref Vector3 moveDelta, ref Float2 mouseDelta, out bool centerMouse)
        {
            centerMouse = true;

            if (IsAnimatingMove)
                return;

            Viewport.GetInput(out var input);
            Viewport.GetPrevInput(out var prevInput);
            var transformGizmo = (Viewport as EditorGizmoViewport)?.Gizmos.Active as TransformGizmoBase;
            var isUsingGizmo = transformGizmo != null && transformGizmo.ActiveAxis != TransformGizmoBase.Axis.None;

            // Get current view properties
            var yaw = Viewport.Yaw;
            var pitch = Viewport.Pitch;
            var position = Viewport.ViewPosition;
            var rotation = Viewport.ViewOrientation;

            // Compute base vectors for camera movement
            var forward = Vector3.Forward * rotation;
            var up = Vector3.Up * rotation;
            var right = Vector3.Cross(forward, up);
            var targetDistance = (float)Vector3.Dot(TargetPoint - position, forward);
            if (targetDistance < 0.0001f)
                targetDistance = Mathf.Max((float)Vector3.Distance(ref position, ref TargetPoint), 0.0001f);

            var flyMoveDelta = moveDelta;
            var flyMouseDelta = mouseDelta;
            var useFlyInertia = Viewport.UseCameraEasing && (input.IsRotating || HasFlyInertia());
            if (useFlyInertia)
            {
                ApplyFlyInertia(dt, ref flyMoveDelta, ref flyMouseDelta);
                useFlyInertia = HasFlyInertia();
            }
            else
            {
                ResetFlyInertia();
            }

            var useFlyMove = input.IsRotating || (useFlyInertia && !_flyMoveDelta.IsZero);
            var useFlyLook = input.IsRotating || (useFlyInertia && !_flyMouseDelta.IsZero);
            var updateTargetFromView = !input.IsAltRightMouseZooming && (useFlyLook || (input.IsMoving && !mouseDelta.IsZero));

            // Dolly
            if (input.IsPanning || input.IsMoving)
            {
                Vector3.Transform(ref moveDelta, ref rotation, out Vector3 move);
                position += move;
            }
            if (useFlyMove)
            {
                Vector3.Transform(ref flyMoveDelta, ref rotation, out Vector3 move);
                position += move;
            }

            // Pan
            if (input.IsPanning)
            {
                var panningSpeed = (Viewport.RelativePanning)
                    ? Mathf.Abs((position - TargetPoint).Length) * 0.005f
                    : Viewport.PanningSpeed;

                if (Viewport.InvertPanning)
                {
                    position -= up * (mouseDelta.Y * panningSpeed);
                    position -= right * (mouseDelta.X * panningSpeed);
                }
                else
                {
                    position += right * (mouseDelta.X * panningSpeed);
                    position += up * (mouseDelta.Y * panningSpeed);
                }
            }

            // Move
            if (input.IsMoving)
            {
                // Move camera over XZ plane
                var projectedForward = Vector3.Normalize(new Vector3(forward.X, 0, forward.Z));
                position -= projectedForward * mouseDelta.Y;
                yaw += mouseDelta.X;
            }

            // Rotate or orbit
            if (useFlyLook)
            {
                yaw += flyMouseDelta.X;
                pitch += flyMouseDelta.Y;
            }
            else if (input.IsOrbiting && !isUsingGizmo && prevInput.IsOrbiting)
            {
                yaw += mouseDelta.X;
                pitch += mouseDelta.Y;
            }

            // Camera translations should carry the target point, but dolly zoom should not.
            var positionBeforeZoom = position;

            // Zoom in/out with mouse wheel or Alt+RMB horizontal drag
            if (input.IsAltRightMouseZooming)
            {
                var zoomDelta = Viewport.MouseWheelZoomSpeedFactor * EditorViewport.GetAltRightMouseZoomDelta(ref mouseDelta) * Editor.Instance.Options.Options.Viewport.AltRightMouseZoomSpeed;
                if (Mathf.Abs(zoomDelta) > Mathf.Epsilon)
                {
                    if (!_hasAltRightMouseZoomDirection)
                    {
                        var toTarget = TargetPoint - position;
                        _altRightMouseZoomDirection = toTarget.LengthSquared > Mathf.Epsilon ? Vector3.Normalize(toTarget) : forward;
                        _hasAltRightMouseZoomDirection = true;
                    }
                    position += _altRightMouseZoomDirection * zoomDelta;
                }
            }
            else if (input.IsZooming && !input.IsRotating)
            {
                var zoomDelta = Viewport.MouseWheelZoomSpeedFactor * input.MouseWheelDelta * 25.0f;
                if (zoomDelta > 0.0f)
                {
                    var distanceToTarget = (float)Vector3.Dot(TargetPoint - position, forward);
                    if (distanceToTarget > 0.0f)
                        zoomDelta = Mathf.Min(zoomDelta, Mathf.Max(distanceToTarget - MinimumZoomDistance, 0.0f));
                }
                position += forward * zoomDelta;
            }

            // Zoom in and out by changing FOV
            if (input.IsRotating && (input.ZoomInDown || input.ZoomOutDown))
            {
                float delta = (input.ZoomInDown ? -0.8f : 0.8f);
                AdditionalZoomFOV += delta;
            }
            else if (!input.IsRotating)
            {
                AdditionalZoomFOV = 0f;
            }

            // Move camera with the gizmo
            if (input.IsOrbiting && isUsingGizmo)
            {
                centerMouse = false;
                if (Editor.Instance.Options.Options.Viewport.MoveCameraWithAltTransformDrag)
                    Viewport.ViewPosition += transformGizmo.LastDelta.Translation;
                return;
            }

            // Update view
            Viewport.Yaw = yaw;
            Viewport.Pitch = pitch;
            if (input.IsOrbiting)
            {
                float orbitRadius = Mathf.Max((float)Vector3.Distance(ref position, ref TargetPoint), 0.0001f);
                Vector3 localPosition = Viewport.ViewDirection * (-1 * orbitRadius);
                Viewport.ViewPosition = TargetPoint + localPosition;
            }
            else
            {
                if (updateTargetFromView)
                    TargetPoint = positionBeforeZoom + Viewport.ViewDirection * targetDistance;
                else if (!input.IsAltRightMouseZooming)
                    TargetPoint += positionBeforeZoom - Viewport.ViewPosition;
                Viewport.ViewPosition = position;
            }
        }
    }
}
