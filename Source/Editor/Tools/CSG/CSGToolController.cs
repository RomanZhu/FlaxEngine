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
        /// Draws a new brush footprint and extrusion, then keeps the created box available for editing.
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
        /// Paints the active material onto brush surfaces.
        /// </summary>
        Brush,
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

    /// <summary>Orientation behavior used while Shift-dragging brushes onto scene surfaces.</summary>
    public enum CSGRayPlacementAlignment
    {
        /// <summary>Rotates the configured source side to follow the complete surface normal.</summary>
        AlignToSurface,

        /// <summary>Keeps the destination direction horizontal so placed brushes remain consistently upright.</summary>
        AlignSurfaceUp,

        /// <summary>Moves the brush onto the surface without changing its orientation.</summary>
        KeepRotation,
    }

    /// <summary>Local brush side treated as the front during ray placement.</summary>
    public enum CSGRayPlacementFront
    {
        /// <summary>The brush front side.</summary>
        Front,

        /// <summary>The brush back side.</summary>
        Back,

        /// <summary>The brush left side.</summary>
        Left,

        /// <summary>The brush right side.</summary>
        Right,

        /// <summary>The brush top side.</summary>
        Top,

        /// <summary>The brush bottom side.</summary>
        Bottom,
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
        /// CSG authoring status text overlay.
        /// </summary>
        StatusText = 8,

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
        /// Whether face edits may snap to aligned faces on other CSG brushes.
        /// </summary>
        public bool BrushAlignmentSnappingEnabled;

        /// <summary>
        /// The current linear snap increment.
        /// </summary>
        public float SnapIncrement;

        /// <summary>
        /// The visible CSG categories.
        /// </summary>
        public CSGVisibility Visibility;

        /// <summary>The active ray-placement orientation behavior.</summary>
        public CSGRayPlacementAlignment RayPlacementAlignment;

        /// <summary>The local brush side used as the ray-placement front.</summary>
        public CSGRayPlacementFront RayPlacementFront;

        /// <summary>The material painted by the brush tool, or null to use the default CSG material.</summary>
        public MaterialBase BrushMaterial;

        /// <summary>Whether selecting a material in Content updates the surface brush material.</summary>
        public bool BrushMaterialAutoPick;
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
        public static readonly float[] SnapIncrements = { 5.0f, 15.0f, 25.0f, 50.0f, 100.0f, 200.0f };

        private CSGTool _tool = CSGTool.Draw;
        private CSGOperation _operation = CSGOperation.Additive;
        private bool _workingPlaneLocked;
        private bool _snappingEnabled = true;
        private bool _brushAlignmentSnappingEnabled;
        private float _snapIncrement = 15.0f;
        private CSGVisibility _visibility = CSGVisibility.Default;
        private CSGRayPlacementAlignment _rayPlacementAlignment = CSGRayPlacementAlignment.AlignToSurface;
        private CSGRayPlacementFront _rayPlacementFront = CSGRayPlacementFront.Top;
        private MaterialBase _brushMaterial;
        private bool _brushMaterialPickArmed;
        private bool _brushMaterialAutoPick;

        /// <summary>
        /// Occurs whenever persistent or transient tool state changes.
        /// </summary>
        public event Action Changed;

        /// <summary>
        /// Occurs when a CSG authoring transaction begins.
        /// </summary>
        public event Action InteractionStarted;

        /// <summary>
        /// Occurs when the active CSG authoring transaction commits.
        /// </summary>
        public event Action InteractionCommitted;

        /// <summary>
        /// Occurs when the active CSG authoring transaction is invalidated or cancelled.
        /// </summary>
        public event Action<EditorGizmoModeCancelReason> InteractionCancelled;

        /// <summary>
        /// Occurs when the viewport should pick and lock the hovered working plane.
        /// </summary>
        public event Action PickWorkingPlaneRequested;

        /// <summary>
        /// Occurs when the viewport should reset the working plane.
        /// </summary>
        public event Action ResetWorkingPlaneRequested;

        /// <summary>
        /// Occurs when the working plane should be offset along its normal.
        /// </summary>
        public event Action<float> OffsetWorkingPlaneRequested;

        /// <summary>
        /// Occurs when the working-plane grid basis should rotate around its normal.
        /// </summary>
        public event Action<float> RotateWorkingPlaneRequested;

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
        /// Gets whether face edits may snap to aligned faces on other CSG brushes.
        /// Passive alignment guides remain visible regardless of this setting.
        /// </summary>
        public bool BrushAlignmentSnappingEnabled => _brushAlignmentSnappingEnabled;

        /// <summary>
        /// Gets the active snap increment in engine units.
        /// </summary>
        public float SnapIncrement => _snapIncrement;

        /// <summary>
        /// Gets the visible CSG categories.
        /// </summary>
        public CSGVisibility Visibility => _visibility;

        /// <summary>Gets the orientation behavior used for Shift surface placement.</summary>
        public CSGRayPlacementAlignment RayPlacementAlignment => _rayPlacementAlignment;

        /// <summary>Gets the local side treated as front for Shift surface placement.</summary>
        public CSGRayPlacementFront RayPlacementFront => _rayPlacementFront;

        /// <summary>Gets the material painted by the brush tool. Null resolves to the default CSG material.</summary>
        public MaterialBase BrushMaterial => _brushMaterial;

        /// <summary>Gets whether the material eyedropper will sample the next clicked CSG surface.</summary>
        public bool BrushMaterialPickArmed => _brushMaterialPickArmed;

        /// <summary>Gets whether Content material selection automatically updates the brush material.</summary>
        public bool BrushMaterialAutoPick => _brushMaterialAutoPick;

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
        /// Gets a value indicating whether explicit surface-normal alignment is requested.
        /// </summary>
        public bool AlignNormalModifierActive { get; private set; }

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
            // Select and Edit are legacy aliases for the unified Draw/Edit workflow.
            if (tool is CSGTool.SelectPlace or CSGTool.Edit)
                tool = CSGTool.Draw;
            if (tool is not (CSGTool.Draw or CSGTool.Surface or CSGTool.Brush))
                return false;
            if (_tool == tool)
                return true;

            TryCancel(EditorGizmoModeCancelReason.ToolChanged);
            _brushMaterialPickArmed = false;
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
        /// Requests working-plane picking from the viewport.
        /// </summary>
        public void RequestPickWorkingPlane()
        {
            PickWorkingPlaneRequested?.Invoke();
        }

        /// <summary>
        /// Requests working-plane reset and unlocks the plane.
        /// </summary>
        public void ResetWorkingPlane()
        {
            SetWorkingPlaneLocked(false);
            ResetWorkingPlaneRequested?.Invoke();
        }

        /// <summary>
        /// Requests a working-plane offset in world units.
        /// </summary>
        public void OffsetWorkingPlane(float distance)
        {
            if (float.IsNaN(distance) || float.IsInfinity(distance) || Mathf.IsZero(distance))
                return;
            OffsetWorkingPlaneRequested?.Invoke(distance);
        }

        /// <summary>
        /// Requests a working-plane grid rotation in degrees.
        /// </summary>
        public void RotateWorkingPlane(float angleDegrees)
        {
            if (float.IsNaN(angleDegrees) || float.IsInfinity(angleDegrees) || Mathf.IsZero(angleDegrees))
                return;
            RotateWorkingPlaneRequested?.Invoke(angleDegrees);
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
        /// Sets brush-to-brush face alignment snapping.
        /// </summary>
        public void SetBrushAlignmentSnappingEnabled(bool value)
        {
            if (_brushAlignmentSnappingEnabled == value)
                return;
            _brushAlignmentSnappingEnabled = value;
            Changed?.Invoke();
        }

        /// <summary>
        /// Sets the linear snap increment.
        /// </summary>
        /// <param name="value">The new positive increment.</param>
        public void SetSnapIncrement(float value)
        {
            if (float.IsNaN(value) || float.IsInfinity(value))
                value = 15.0f;
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
        /// Steps to the next larger or smaller supported linear snap increment.
        /// </summary>
        /// <param name="direction">Positive to increase, negative to decrease.</param>
        public void StepSnapIncrement(int direction)
        {
            if (direction == 0)
                return;

            if (direction > 0)
            {
                int next = SnapIncrements.Length - 1;
                for (int i = 0; i < SnapIncrements.Length; i++)
                {
                    if (SnapIncrements[i] > _snapIncrement && !Mathf.NearEqual(SnapIncrements[i], _snapIncrement))
                    {
                        next = i;
                        break;
                    }
                }
                SetSnapIncrement(SnapIncrements[next]);
            }
            else
            {
                int next = 0;
                for (int i = SnapIncrements.Length - 1; i >= 0; i--)
                {
                    if (SnapIncrements[i] < _snapIncrement && !Mathf.NearEqual(SnapIncrements[i], _snapIncrement))
                    {
                        next = i;
                        break;
                    }
                }
                SetSnapIncrement(SnapIncrements[next]);
            }
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

        /// <summary>Sets the orientation behavior used for Shift surface placement.</summary>
        public void SetRayPlacementAlignment(CSGRayPlacementAlignment value)
        {
            if (_rayPlacementAlignment == value)
                return;
            _rayPlacementAlignment = value;
            Changed?.Invoke();
        }

        /// <summary>Sets the local brush side treated as front for Shift surface placement.</summary>
        public void SetRayPlacementFront(CSGRayPlacementFront value)
        {
            if (_rayPlacementFront == value)
                return;
            _rayPlacementFront = value;
            Changed?.Invoke();
        }

        /// <summary>Sets the material used for subsequent CSG surface brush strokes.</summary>
        public void SetBrushMaterial(MaterialBase value)
        {
            if (_brushMaterial == value)
                return;
            _brushMaterial = value;
            Changed?.Invoke();
        }

        /// <summary>Resets the CSG surface brush to the engine's default CSG material.</summary>
        public void ResetBrushMaterial()
        {
            SetBrushMaterial(null);
        }

        /// <summary>Arms or cancels the surface-material eyedropper.</summary>
        public void SetBrushMaterialPickArmed(bool value)
        {
            if (_brushMaterialPickArmed == value)
                return;
            _brushMaterialPickArmed = value;
            Changed?.Invoke();
        }

        /// <summary>Enables or disables automatic material picking from Content selection.</summary>
        public void SetBrushMaterialAutoPick(bool value)
        {
            if (_brushMaterialAutoPick == value)
                return;
            _brushMaterialAutoPick = value;
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
            InteractionStarted?.Invoke();
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
            InteractionCommitted?.Invoke();
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
            bool changed = HasActiveInteraction || SnapOverrideActive || SquareConstraintActive || SymmetricConstraintActive || DuplicateModifierActive || AlignNormalModifierActive;
            if (!changed)
                return false;

            HasActiveInteraction = false;
            LastCancelReason = reason;
            ClearTransientModifiers();
            InteractionCancelled?.Invoke(reason);
            Changed?.Invoke();
            return true;
        }

        /// <summary>
        /// Sets transient authoring modifier state.
        /// </summary>
        public void SetTransientModifiers(bool snapOverride, bool square, bool symmetric, bool duplicate, bool alignNormal = false)
        {
            if (SnapOverrideActive == snapOverride && SquareConstraintActive == square && SymmetricConstraintActive == symmetric && DuplicateModifierActive == duplicate && AlignNormalModifierActive == alignNormal)
                return;
            SnapOverrideActive = snapOverride;
            SquareConstraintActive = square;
            SymmetricConstraintActive = symmetric;
            DuplicateModifierActive = duplicate;
            AlignNormalModifierActive = alignNormal;
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
                BrushAlignmentSnappingEnabled = BrushAlignmentSnappingEnabled,
                SnapIncrement = SnapIncrement,
                Visibility = Visibility,
                RayPlacementAlignment = RayPlacementAlignment,
                RayPlacementFront = RayPlacementFront,
                BrushMaterial = BrushMaterial,
                BrushMaterialAutoPick = BrushMaterialAutoPick,
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
                _tool = CSGTool.Draw;
                break;
            case CSGTool.Surface:
            case CSGTool.Brush:
                _tool = state.Tool;
                break;
            default:
                _tool = CSGTool.Draw;
                break;
            }
            _operation = state.Operation == CSGOperation.Subtractive ? CSGOperation.Subtractive : CSGOperation.Additive;
            _workingPlaneLocked = state.WorkingPlaneLocked;
            _snappingEnabled = state.SnappingEnabled;
            _brushAlignmentSnappingEnabled = state.BrushAlignmentSnappingEnabled;
            _snapIncrement = float.IsNaN(state.SnapIncrement) || float.IsInfinity(state.SnapIncrement) ? 15.0f : Mathf.Max(state.SnapIncrement, 0.0001f);
            _visibility = state.Visibility & (CSGVisibility.SourceBrushes | CSGVisibility.BuiltGeometry | CSGVisibility.HiddenBrushes | CSGVisibility.StatusText);
            _rayPlacementAlignment = state.RayPlacementAlignment is CSGRayPlacementAlignment.AlignToSurface or CSGRayPlacementAlignment.AlignSurfaceUp or CSGRayPlacementAlignment.KeepRotation
                ? state.RayPlacementAlignment
                : CSGRayPlacementAlignment.AlignToSurface;
            _rayPlacementFront = state.RayPlacementFront is CSGRayPlacementFront.Front or CSGRayPlacementFront.Back or CSGRayPlacementFront.Left or CSGRayPlacementFront.Right or CSGRayPlacementFront.Top or CSGRayPlacementFront.Bottom
                ? state.RayPlacementFront
                : CSGRayPlacementFront.Top;
            _brushMaterial = state.BrushMaterial;
            _brushMaterialPickArmed = false;
            _brushMaterialAutoPick = state.BrushMaterialAutoPick;
            TryCancel(EditorGizmoModeCancelReason.SceneChanged);
            Changed?.Invoke();
        }

        private void ClearTransientModifiers()
        {
            SnapOverrideActive = false;
            SquareConstraintActive = false;
            SymmetricConstraintActive = false;
            DuplicateModifierActive = false;
            AlignNormalModifierActive = false;
        }
    }
}
