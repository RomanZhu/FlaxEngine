// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Gizmo;
using FlaxEngine;

namespace FlaxEditor.Viewport.Modes
{
    /// <summary>
    /// Describes why an editor gizmo mode is being asked to cancel its active interaction.
    /// </summary>
    public enum EditorGizmoModeCancelReason
    {
        /// <summary>
        /// The active editor gizmo mode is changing.
        /// </summary>
        ModeChanged,

        /// <summary>
        /// The viewport lost keyboard focus.
        /// </summary>
        FocusLost,

        /// <summary>
        /// The active tool inside the mode is changing.
        /// </summary>
        ToolChanged,

        /// <summary>
        /// The edited scene or authoring context changed.
        /// </summary>
        SceneChanged,

        /// <summary>
        /// The editor is entering play mode.
        /// </summary>
        PlayModeBeginning,

        /// <summary>
        /// The user requested cancellation.
        /// </summary>
        User,
    }

    /// <summary>
    /// Editor viewport gizmo mode descriptor. Defines which gizmo tools to show and how to handle scene editing.
    /// </summary>
    /// <remarks>
    /// Only one gizmo mode can be active at the time. It defines the viewport toolset usage.
    /// </remarks>
    [HideInEditor]
    public abstract class EditorGizmoMode
    {
        private IGizmoOwner _owner;

        /// <summary>
        /// Gets the gizmos owner viewport.
        /// </summary>
        public IGizmoOwner Owner => _owner;

        /// <summary>
        /// Occurs when mode gets activated.
        /// </summary>
        public event Action Activated;

        /// <summary>
        /// Occurs when mode gets deactivated.
        /// </summary>
        public event Action Deactivated;

        /// <summary>
        /// Initializes the specified mode and links it to the viewport.
        /// </summary>
        /// <param name="owner">The gizmos owner.</param>
        public virtual void Init(IGizmoOwner owner)
        {
            _owner = owner;
        }

        /// <summary>
        /// Releases the mode. Called on editor exit or when mode gets removed from the current viewport.
        /// </summary>
        public virtual void Dispose()
        {
            _owner = null;
        }

        /// <summary>
        /// Called when mode gets activated.
        /// </summary>
        public virtual void OnActivated()
        {
            Activated?.Invoke();
        }

        /// <summary>
        /// Called when mode gets deactivated.
        /// </summary>
        public virtual void OnDeactivated()
        {
            Deactivated?.Invoke();
        }

        /// <summary>
        /// Handles viewport mouse movement before the viewport performs mode-independent picking or selection.
        /// </summary>
        /// <param name="location">The mouse location in viewport coordinates.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnMouseMove(Float2 location)
        {
            return false;
        }

        /// <summary>
        /// Handles a viewport mouse button press before the viewport performs mode-independent picking or selection.
        /// </summary>
        /// <param name="location">The mouse location in viewport coordinates.</param>
        /// <param name="button">The pressed mouse button.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnMouseDown(Float2 location, MouseButton button)
        {
            return false;
        }

        /// <summary>
        /// Handles a viewport mouse button release before the viewport performs mode-independent picking or selection.
        /// </summary>
        /// <param name="location">The mouse location in viewport coordinates.</param>
        /// <param name="button">The released mouse button.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnMouseUp(Float2 location, MouseButton button)
        {
            return false;
        }

        /// <summary>
        /// Handles a viewport mouse double-click before the viewport performs mode-independent picking or selection.
        /// </summary>
        /// <param name="location">The mouse location in viewport coordinates.</param>
        /// <param name="button">The clicked mouse button.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            return false;
        }

        /// <summary>
        /// Handles a viewport key press before the viewport performs mode-independent shortcuts.
        /// </summary>
        /// <param name="key">The pressed key.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnKeyDown(KeyboardKeys key)
        {
            return false;
        }

        /// <summary>
        /// Handles a viewport key release before the viewport performs mode-independent shortcuts.
        /// </summary>
        /// <param name="key">The released key.</param>
        /// <returns>True if the input was handled.</returns>
        public virtual bool OnKeyUp(KeyboardKeys key)
        {
            return false;
        }

        /// <summary>
        /// Cancels any transient interaction owned by this mode.
        /// </summary>
        /// <param name="reason">The cancellation reason.</param>
        /// <returns>True if an interaction was cancelled.</returns>
        public virtual bool TryCancel(EditorGizmoModeCancelReason reason)
        {
            return false;
        }
    }
}
