// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Viewport.Modes;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG
{
    /// <summary>
    /// The active CSG authoring tool.
    /// </summary>
    public enum CSGTool
    {
        /// <summary>
        /// Selects existing brushes or places a primitive selected elsewhere in the editor.
        /// </summary>
        SelectPlace,

        /// <summary>
        /// Draws a new brush footprint and extrusion.
        /// </summary>
        Draw,

        /// <summary>
        /// Edits brush topology.
        /// </summary>
        Edit,

        /// <summary>
        /// Edits brush surface properties.
        /// </summary>
        Surface,

        /// <summary>
        /// Clips brush geometry. Reserved for a later milestone.
        /// </summary>
        Clip,
    }

    /// <summary>
    /// CSG brush operation used for newly authored brushes.
    /// </summary>
    public enum CSGOperation
    {
        /// <summary>
        /// Adds brush volume.
        /// </summary>
        Additive,

        /// <summary>
        /// Removes brush volume.
        /// </summary>
        Subtractive,

        /// <summary>
        /// Keeps the intersection with brush volume. Reserved for a later milestone.
        /// </summary>
        Intersecting,
    }

    /// <summary>
    /// CSG visualization categories shown in the viewport.
    /// </summary>
    [Flags]
    public enum CSGVisibility
    {
        /// <summary>
        /// No CSG visualization category.
        /// </summary>
        None = 0,

        /// <summary>
        /// Source brush wireframes and handles.
        /// </summary>
        SourceBrushes = 1,

        /// <summary>
        /// Built CSG result geometry.
        /// </summary>
        BuiltGeometry = 2,

        /// <summary>
        /// Hidden or filtered source brushes.
        /// </summary>
        HiddenBrushes = 4,

        /// <summary>
        /// Default CSG visualization categories.
        /// </summary>
        Default = SourceBrushes | BuiltGeometry,
    }

    /// <summary>
    /// Serializable snapshot of the persistent CSG authoring tool state.
    /// </summary>
    public struct CSGToolState
    {
        /// <summary>
        /// The selected tool.
        /// </summary>
        public CSGTool Tool;

        /// <summary>
        /// The selected brush operation.
        /// </summary>
        public CSGOperation Operation;

        /// <summary>
        /// Whether the working plane is locked.
        /// </summary>
        public bool WorkingPlaneLocked;

        /// <summary>
        /// Whether snapping is enabled.
        /// </summary>
        public bool SnappingEnabled;

        /// <summary>
        /// The current linear snap increment.
        /// </summary>
        public float SnapIncrement;

        /// <summary>
        /// The visible CSG categories.
        /// </summary>
        public CSGVisibility Visibility;
    }

    /// <summary>
    /// Owns the state and lifecycle of CSG authoring tools. Milestone 0 intentionally performs no scene mutations.
    /// </summary>
    [HideInEditor]
    public sealed class CSGToolController
    {
        /// <summary>
        /// Supported snap increments in engine units.
        /// </summary>
        public static readonly float[] SnapIncrements = { 1.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f };

        private CSGTool _tool = CSGTool.SelectPlace;
        private CSGOperation _operation = CSGOperation.Additive;
        private bool _workingPlaneLocked;
        private bool _snappingEnabled = true;
        private float _snapIncrement = 10.0f;
        private CSGVisibility _visibility = CSGVisibility.Default;

        /// <summary>
        /// Occurs whenever persistent or transient tool state changes.
        /// </summary>
        public event Action Changed;

        /// <summary>
        /// Occurs when a future milestone should pick a working plane from the viewport.
        /// </summary>
        public event Action PickWorkingPlaneRequested;

        /// <summary>
        /// Occurs when a future milestone should reset the working plane.
        /// </summary>
        public event Action ResetWorkingPlaneRequested;

        /// <summary>
        /// Gets the active CSG tool.
        /// </summary>
        public CSGTool Tool => _tool;

        /// <summary>
        /// Gets the active CSG operation.
        /// </summary>
        public CSGOperation Operation => _operation;

        /// <summary>
        /// Gets a value indicating whether the working plane is locked.
        /// </summary>
        public bool WorkingPlaneLocked => _workingPlaneLocked;

        /// <summary>
        /// Gets a value indicating whether snapping is enabled.
        /// </summary>
        public bool SnappingEnabled => _snappingEnabled;

        /// <summary>
        /// Gets the active snap increment in engine units.
        /// </summary>
        public float SnapIncrement => _snapIncrement;

        /// <summary>
        /// Gets the visible CSG categories.
        /// </summary>
        public CSGVisibility Visibility => _visibility;

        /// <summary>
        /// Gets a value indicating whether the snapping override key is held.
        /// </summary>
        public bool SnapOverrideActive { get; private set; }

        /// <summary>
        /// Gets the effective snapping state after applying the transient override.
        /// </summary>
        public bool EffectiveSnappingEnabled => SnappingEnabled != SnapOverrideActive;

        /// <summary>
        /// Gets a value indicating whether the square constraint key is held.
        /// </summary>
        public bool SquareConstraintActive { get; private set; }

        /// <summary>
        /// Gets a value indicating whether the symmetric constraint key is held.
        /// </summary>
        public bool SymmetricConstraintActive { get; private set; }

        /// <summary>
        /// Gets a value indicating whether the duplicate modifier key is held.
        /// </summary>
        public bool DuplicateModifierActive { get; private set; }

        /// <summary>
        /// Gets a value indicating whether a CSG interaction is active.
        /// </summary>
        public bool HasActiveInteraction { get; private set; }

        /// <summary>
        /// Gets the reason for the most recent interaction cancellation.
        /// </summary>
        public EditorGizmoModeCancelReason LastCancelReason { get; private set; }

        /// <summary>
        /// Selects a supported tool, cancelling an active interaction first.
        /// </summary>
        /// <param name="tool">The tool to activate.</param>
        /// <returns>True if the tool is available in this milestone.</returns>
        public bool SetTool(CSGTool tool)
        {
            if (tool == CSGTool.Clip)
                return false;
            if (_tool == tool)
                return true;

            TryCancel(EditorGizmoModeCancelReason.ToolChanged);
            _tool = tool;
            Changed?.Invoke();
            return true;
        }

        /// <summary>
        /// Selects a supported brush operation.
        /// </summary>
        /// <param name="operation">The operation to select.</param>
        /// <returns>True if the operation is available in this milestone.</returns>
        public bool SetOperation(CSGOperation operation)
        {
            if (operation == CSGOperation.Intersecting)
                return false;
            if (_operation == operation)
                return true;

            _operation = operation;
            Changed?.Invoke();
            return true;
        }

        /// <summary>
        /// Sets the working plane lock state.
        /// </summary>
        /// <param name="value">The new state.</param>
        public void SetWorkingPlaneLocked(bool value)
        {
            if (_workingPlaneLocked == value)
                return;
            _workingPlaneLocked = value;
            Changed?.Invoke();
        }

        /// <summary>
        /// Requests working-plane picking from a future authoring implementation.
        /// </summary>
        public void RequestPickWorkingPlane()
        {
            PickWorkingPlaneRequested?.Invoke();
        }

        /// <summary>
        /// Requests working-plane reset from a future authoring implementation and unlocks the plane.
        /// </summary>
        public void ResetWorkingPlane()
        {
            SetWorkingPlaneLocked(false);
            ResetWorkingPlaneRequested?.Invoke();
        }

        /// <summary>
        /// Sets the persistent snapping state.
        /// </summary>
        /// <param name="value">The new state.</param>
        public void SetSnappingEnabled(bool value)
        {
            if (_snappingEnabled == value)
                return;
            _snappingEnabled = value;
            Changed?.Invoke();
        }

        /// <summary>
        /// Sets the linear snap increment.
        /// </summary>
        /// <param name="value">The new positive increment.</param>
        public void SetSnapIncrement(float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value))
                value = 10.0f;
            value = Mathf.Max(value, 0.0001f);
            if (Mathf.NearEqual(_snapIncrement, value))
                return;
            _snapIncrement = value;
            Changed?.Invoke();
        }

        /// <summary>
        /// Cycles to the next supported linear snap increment.
        /// </summary>
        public void CycleSnapIncrement()
        {
            int next = 0;
            for (int i = 0; i < SnapIncrements.Length; i++)
            {
                if (Mathf.NearEqual(SnapIncrements[i], _snapIncrement))
                {
                    next = (i + 1) % SnapIncrements.Length;
                    break;
                }
                if (SnapIncrements[i] > _snapIncrement)
                {
                    next = i;
                    break;
                }
            }
            SetSnapIncrement(SnapIncrements[next]);
        }

        /// <summary>
        /// Sets the visible CSG categories.
        /// </summary>
        /// <param name="value">The visibility flags.</param>
        public void SetVisibility(CSGVisibility value)
        {
            if (_visibility == value)
                return;
            _visibility = value;
            Changed?.Invoke();
        }

        /// <summary>
        /// Toggles a CSG visibility category.
        /// </summary>
        /// <param name="value">The category to toggle.</param>
        public void ToggleVisibility(CSGVisibility value)
        {
            SetVisibility(_visibility ^ value);
        }

        /// <summary>
        /// Starts a transaction shell for lifecycle and cancellation testing. No scene data is changed.
        /// </summary>
        public void BeginInteraction()
        {
            if (HasActiveInteraction)
                return;
            HasActiveInteraction = true;
            Changed?.Invoke();
        }

        /// <summary>
        /// Commits the transaction shell. No scene data is changed.
        /// </summary>
        /// <returns>True if an interaction was committed.</returns>
        public bool TryCommit()
        {
            if (!HasActiveInteraction)
                return false;
            HasActiveInteraction = false;
            ClearTransientModifiers();
            Changed?.Invoke();
            return true;
        }

        /// <summary>
        /// Cancels the transaction shell. No scene data is changed.
        /// </summary>
        /// <param name="reason">The cancellation reason.</param>
        /// <returns>True if an interaction was cancelled.</returns>
        public bool TryCancel(EditorGizmoModeCancelReason reason)
        {
            bool changed = HasActiveInteraction || SnapOverrideActive || SquareConstraintActive || SymmetricConstraintActive || DuplicateModifierActive;
            if (!changed)
                return false;

            HasActiveInteraction = false;
            LastCancelReason = reason;
            ClearTransientModifiers();
            Changed?.Invoke();
            return true;
        }

        /// <summary>
        /// Sets transient authoring modifier state.
        /// </summary>
        public void SetTransientModifiers(bool snapOverride, bool square, bool symmetric, bool duplicate)
        {
            if (SnapOverrideActive == snapOverride && SquareConstraintActive == square && SymmetricConstraintActive == symmetric && DuplicateModifierActive == duplicate)
                return;
            SnapOverrideActive = snapOverride;
            SquareConstraintActive = square;
            SymmetricConstraintActive = symmetric;
            DuplicateModifierActive = duplicate;
            Changed?.Invoke();
        }

        /// <summary>
        /// Gets a snapshot of persistent state.
        /// </summary>
        public CSGToolState CaptureState()
        {
            return new CSGToolState
            {
                Tool = Tool,
                Operation = Operation,
                WorkingPlaneLocked = WorkingPlaneLocked,
                SnappingEnabled = SnappingEnabled,
                SnapIncrement = SnapIncrement,
                Visibility = Visibility,
            };
        }

        /// <summary>
        /// Applies a persistent state snapshot, sanitizing features unavailable in Milestone 0.
        /// </summary>
        /// <param name="state">The state to apply.</param>
        public void ApplyState(CSGToolState state)
        {
            switch (state.Tool)
            {
            case CSGTool.SelectPlace:
            case CSGTool.Draw:
            case CSGTool.Edit:
            case CSGTool.Surface:
                _tool = state.Tool;
                break;
            default:
                _tool = CSGTool.SelectPlace;
                break;
            }
            _operation = state.Operation == CSGOperation.Subtractive ? CSGOperation.Subtractive : CSGOperation.Additive;
            _workingPlaneLocked = state.WorkingPlaneLocked;
            _snappingEnabled = state.SnappingEnabled;
            _snapIncrement = float.IsNaN(state.SnapIncrement) || float.IsInfinity(state.SnapIncrement) ? 10.0f : Mathf.Max(state.SnapIncrement, 0.0001f);
            _visibility = state.Visibility & (CSGVisibility.SourceBrushes | CSGVisibility.BuiltGeometry | CSGVisibility.HiddenBrushes);
            TryCancel(EditorGizmoModeCancelReason.SceneChanged);
            Changed?.Invoke();
        }

        private void ClearTransientModifiers()
        {
            SnapOverrideActive = false;
            SquareConstraintActive = false;
            SymmetricConstraintActive = false;
            DuplicateModifierActive = false;
        }
    }
}
