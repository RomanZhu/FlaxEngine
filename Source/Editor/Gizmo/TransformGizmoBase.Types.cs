// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEditor.Gizmo
{
    /// <summary>
    /// Describes the ownership state of a transform interaction.
    /// </summary>
    public enum InteractionState
    {
        /// <summary>
        /// No transform interaction is active.
        /// </summary>
        Inactive,

        /// <summary>
        /// A semantic handle is under the pointer.
        /// </summary>
        Hovering,

        /// <summary>
        /// A handle was pressed and the transaction origin is latched.
        /// </summary>
        Armed,

        /// <summary>
        /// Pointer input is solving a transform preview.
        /// </summary>
        Dragging,

        /// <summary>
        /// The result is frozen while camera navigation owns the pointer.
        /// </summary>
        Clutched,

        /// <summary>
        /// Pointer solving is suspended while numeric input owns the interaction.
        /// </summary>
        NumericEntry,

        /// <summary>
        /// The final result is being committed.
        /// </summary>
        Committing,

        /// <summary>
        /// The transaction origin is being restored.
        /// </summary>
        Cancelling,
    }

    /// <summary>
    /// Identifies the semantic transform handle latched by a transaction.
    /// </summary>
    public readonly struct SemanticHandle : IEquatable<SemanticHandle>
    {
        /// <summary>
        /// The handle that is not selectable.
        /// </summary>
        public static readonly SemanticHandle None = new SemanticHandle(TransformGizmoBase.Axis.None);

        /// <summary>
        /// Initializes a new semantic handle.
        /// </summary>
        /// <param name="axis">The gizmo axis or handle type.</param>
        public SemanticHandle(TransformGizmoBase.Axis axis)
        {
            Axis = axis;
        }

        /// <summary>
        /// Gets the axis represented by this handle.
        /// </summary>
        public TransformGizmoBase.Axis Axis { get; }

        /// <summary>
        /// Gets a value indicating whether this handle can be used for a transaction.
        /// </summary>
        public bool IsValid => Axis != TransformGizmoBase.Axis.None;

        /// <inheritdoc />
        public bool Equals(SemanticHandle other)
        {
            return Axis == other.Axis;
        }

        /// <inheritdoc />
        public override bool Equals(object obj)
        {
            return obj is SemanticHandle other && Equals(other);
        }

        /// <inheritdoc />
        public override int GetHashCode()
        {
            return (int)Axis;
        }

        /// <summary>
        /// Compares two semantic handles.
        /// </summary>
        public static bool operator ==(SemanticHandle left, SemanticHandle right)
        {
            return left.Equals(right);
        }

        /// <summary>
        /// Compares two semantic handles.
        /// </summary>
        public static bool operator !=(SemanticHandle left, SemanticHandle right)
        {
            return !left.Equals(right);
        }
    }

    public partial class TransformGizmoBase
    {
        /// <summary>
        /// Gizmo axis modes.
        /// </summary>
        public enum Axis
        {
            /// <summary>
            /// None.
            /// </summary>
            None = 0,

            /// <summary>
            /// The X axis.
            /// </summary>
            X = 1,

            /// <summary>
            /// The Y axis.
            /// </summary>
            Y = 2,

            /// <summary>
            /// The Z axis.
            /// </summary>
            Z = 4,

            /// <summary>
            /// The XY plane.
            /// </summary>
            XY = X | Y,

            /// <summary>
            /// The ZX plane.
            /// </summary>
            ZX = Z | X,

            /// <summary>
            /// The YZ plane.
            /// </summary>
            YZ = Y | Z,

            /// <summary>
            /// The center point.
            /// </summary>
            Center = 8,

            /// <summary>
            /// Screen-space rotation.
            /// </summary>
            Screen = 16,
        };

        /// <summary>
        /// Gizmo tool mode.
        /// </summary>
        public enum Mode
        {
            /// <summary>
            /// Translate object(s)
            /// </summary>
            Translate = 0,

            /// <summary>
            /// Rotate object(s)
            /// </summary>
            Rotate = 1,

            /// <summary>
            /// Scale object(s)
            /// </summary>
            Scale = 2,

            /// <summary>
            /// Select object(s) without transform handles.
            /// </summary>
            Select = 3
        }

        /// <summary>
        /// Transform object space.
        /// </summary>
        public enum TransformSpace
        {
            /// <summary>
            /// Object local space coordinates
            /// </summary>
            Local,

            /// <summary>
            /// World space coordinates
            /// </summary>
            World
        }

        /// <summary>
        /// Pivot location type.
        /// </summary>
        public enum PivotType
        {
            /// <summary>
            /// First selected object pivot/origin
            /// </summary>
            ObjectCenter,

            /// <summary>
            /// Selection bounds center point
            /// </summary>
            SelectionCenter,

            /// <summary>
            /// World origin
            /// </summary>
            WorldOrigin
        }
    }
}
