// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Viewport.Cameras
{
    /// <summary>
    /// Editor viewport camera that controls a transient physics character in the edited scene.
    /// </summary>
    /// <seealso cref="ViewportCamera" />
    [HideInEditor]
    public class InSceneCharacterControllerCamera : ViewportCamera
    {
        private const int HelperLayer = 31;
        private const string MovementSpeedCacheKey = "CharacterControllerMovementSpeedValue";

        private CharacterController _controller;
        private Vector3 _gravityVelocity;
        private float _movementSpeed;

        /// <summary>
        /// Gets a value indicating whether this camera has spawned its transient character.
        /// </summary>
        public bool IsActive => _controller;

        /// <inheritdoc />
        public override bool UseMovementSpeed => false;

        private static ViewportOptions Options => Editor.Instance.Options.Options.Viewport;

        private static void GetMovementSpeedRange(ViewportOptions options, out float minSpeed, out float maxSpeed)
        {
            minSpeed = Mathf.Max(1.0f, options.CharacterControllerMinWalkSpeed);
            maxSpeed = Mathf.Max(minSpeed, options.CharacterControllerMaxWalkSpeed);
        }

        /// <summary>
        /// Spawns the transient character at the current viewport camera location.
        /// </summary>
        /// <returns>True if mode was activated.</returns>
        public bool Enter()
        {
            if (IsActive)
                return true;
            if (Viewport == null || !Level.IsAnySceneLoaded)
                return false;

            var parent = Level.GetScene(0);
            if (!parent)
                return false;

            var options = Options;
            GetMovementSpeedRange(options, out var minSpeed, out var maxSpeed);
            _movementSpeed = Mathf.Clamp(options.CharacterControllerWalkSpeed, minSpeed, maxSpeed);
            if (Editor.Instance.ProjectCache.TryGetCustomData(MovementSpeedCacheKey, out float cachedSpeed))
                _movementSpeed = Mathf.Clamp(cachedSpeed, minSpeed, maxSpeed);

            _controller = new CharacterController
            {
                Name = "__EditorInSceneCharacterController",
                HideFlags = HideFlags.FullyHidden,
                StaticFlags = StaticFlags.None,
                Layer = HelperLayer,
                Radius = options.CharacterControllerRadius,
                Height = options.CharacterControllerHeight,
                StepOffset = options.CharacterControllerStepOffset,
                SlopeLimit = options.CharacterControllerSlopeLimit,
                OriginMode = CharacterController.OriginModes.Base,
                AutoGravity = false,
                Position = Viewport.ViewPosition - Vector3.Up * options.CharacterControllerEyeHeight,
                Orientation = Quaternion.Identity,
            };
            _controller.SetParent(parent, false, false);
            _gravityVelocity = Vector3.Zero;

            return true;
        }

        /// <summary>
        /// Removes all transient actors owned by this camera.
        /// </summary>
        public void Exit()
        {
            Object.Destroy(ref _controller);
            _gravityVelocity = Vector3.Zero;
        }

        /// <summary>
        /// Adjusts current walking speed by mouse wheel input.
        /// </summary>
        /// <param name="wheelDelta">The mouse wheel delta.</param>
        public void AdjustMovementSpeed(float wheelDelta)
        {
            if (Mathf.Abs(wheelDelta) <= Mathf.Epsilon)
                return;

            var options = Options;
            GetMovementSpeedRange(options, out var minSpeed, out var maxSpeed);
            var step = Mathf.Max(1.0f, options.CharacterControllerScrollSpeedStep) * options.MouseWheelSensitivity;
            _movementSpeed = Mathf.Clamp(_movementSpeed + wheelDelta * step, minSpeed, maxSpeed);
            Editor.Instance.ProjectCache.SetCustomData(MovementSpeedCacheKey, _movementSpeed);
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            if (!IsActive)
                return;

            if (!_controller.IsDuringPlay)
            {
                Exit();
                return;
            }

            var dt = Mathf.Clamp(deltaTime, 0.0f, 0.1f);
            UpdateCharacter(dt);

            Viewport.ViewPosition = _controller.Position + Vector3.Up * Options.CharacterControllerEyeHeight;
        }

        private void UpdateCharacter(float dt)
        {
            if (dt <= Mathf.Epsilon)
                return;

            var root = Viewport.Root as WindowRootControl;
            var canUseInput = root != null && root.Window != null && root.Window.IsFocused && root.Window.IsForegroundWindow;
            if (canUseInput)
            {
                Viewport.GetInput(out var viewportInput);
                canUseInput = root.GetMouseButton(MouseButton.Right) || viewportInput.IsMouseRightDown;
            }
            var move = Vector3.Zero;
            if (canUseInput)
            {
                var input = Editor.Instance.Options.Options.Input;
                if (root.GetKey(input.Forward.Key))
                    move += Vector3.Forward;
                if (root.GetKey(input.Backward.Key))
                    move += Vector3.Backward;
                if (root.GetKey(input.Right.Key))
                    move += Vector3.Right;
                if (root.GetKey(input.Left.Key))
                    move += Vector3.Left;
                if (move.LengthSquared > Mathf.Epsilon)
                    move.Normalize();
            }

            var rotation = Viewport.ViewOrientation;
            var forward = Vector3.Forward * rotation;
            forward.Y = 0.0f;
            if (forward.LengthSquared > Mathf.Epsilon)
                forward.Normalize();
            else
                forward = Vector3.Forward;

            var right = Vector3.Right * rotation;
            right.Y = 0.0f;
            if (right.LengthSquared > Mathf.Epsilon)
                right.Normalize();
            else
                right = Vector3.Right;

            var options = Options;
            var speed = _movementSpeed;
            if (canUseInput && root.GetKey(KeyboardKeys.Shift))
                speed *= options.CharacterControllerSprintMultiplier;

            var displacement = (right * move.X + forward * move.Z) * speed * dt;
            var isGrounded = _controller.IsGrounded;
            if (isGrounded)
            {
                if (canUseInput && root.GetKeyDown(KeyboardKeys.Spacebar))
                    _gravityVelocity = Vector3.Up * options.CharacterControllerJumpSpeed;
                else if (_gravityVelocity.Y < 0.0f)
                    _gravityVelocity = Vector3.Zero;
            }
            _gravityVelocity += Physics.Gravity * dt;
            displacement += _gravityVelocity * dt;

            var flags = _controller.Move(displacement);
            if ((flags & CharacterController.CollisionFlags.Below) != CharacterController.CollisionFlags.None && _gravityVelocity.Y < 0.0f)
                _gravityVelocity = Vector3.Zero;
            if ((flags & CharacterController.CollisionFlags.Above) != CharacterController.CollisionFlags.None && _gravityVelocity.Y > 0.0f)
                _gravityVelocity = Vector3.Zero;
        }

        /// <inheritdoc />
        public override void UpdateView(float dt, ref Vector3 moveDelta, ref Float2 mouseDelta, out bool centerMouse)
        {
            centerMouse = true;
            if (!mouseDelta.IsZero)
            {
                Viewport.Yaw += mouseDelta.X;
                Viewport.Pitch += mouseDelta.Y;
            }

            Viewport.GetInput(out var input);
            AdjustMovementSpeed(input.MouseWheelDelta);
        }
    }
}
