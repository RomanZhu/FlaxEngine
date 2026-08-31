// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Gizmo;
using FlaxEditor.Options;
using FlaxEditor.Viewport.Modes;
using FlaxEngine;

namespace FlaxEditor.Tools.CSG
{
    /// <summary>
    /// CSG authoring viewport mode. Owns the isolated CSG tool context, persistent options, and interaction services.
    /// </summary>
    [HideInEditor]
    public sealed class CSGAuthoringGizmoMode : EditorGizmoMode
    {
        private const string ToolCacheKey = "CSGAuthoring.Tool";
        private const string OperationCacheKey = "CSGAuthoring.Operation";
        private const string WorkingPlaneLockCacheKey = "CSGAuthoring.WorkingPlaneLocked";
        private const string SnappingCacheKey = "CSGAuthoring.SnappingEnabled";
        private const string BrushAlignmentSnappingCacheKey = "CSGAuthoring.BrushAlignmentSnappingEnabled";
        private const string SnapIncrementCacheKey = "CSGAuthoring.SnapIncrement";
        private const string VisibilityCacheKey = "CSGAuthoring.Visibility";
        private const string RayPlacementAlignmentCacheKey = "CSGAuthoring.RayPlacementAlignment";
        private const string RayPlacementFrontCacheKey = "CSGAuthoring.RayPlacementFront";
        private const string BrushMaterialCacheKey = "CSGAuthoring.BrushMaterial";
        private const string BrushMaterialAutoPickCacheKey = "CSGAuthoring.BrushMaterialAutoPick";

        /// <summary>
        /// Gets the CSG tool state controller.
        /// </summary>
        public CSGToolController Controller { get; private set; }

        /// <summary>
        /// Gets the CSG viewport gizmo.
        /// </summary>
        public CSGAuthoringGizmo Gizmo { get; private set; }

        /// <inheritdoc />
        public override void Init(IGizmoOwner owner)
        {
            base.Init(owner);
            Controller = new CSGToolController();
            LoadState();
            Controller.Changed += OnControllerChanged;
            Gizmo = new CSGAuthoringGizmo(owner, Controller);
        }

        /// <inheritdoc />
        public override void Dispose()
        {
            if (Controller != null)
                Controller.Changed -= OnControllerChanged;
            Gizmo?.Destroy();
            Gizmo = null;
            Controller = null;
            base.Dispose();
        }

        /// <inheritdoc />
        public override void OnActivated()
        {
            Owner.Gizmos.Active = Gizmo;
            ReportInputConflicts();
            base.OnActivated();
        }

        /// <inheritdoc />
        public override void OnDeactivated()
        {
            TryCancel(EditorGizmoModeCancelReason.ModeChanged);
            base.OnDeactivated();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            var viewport = Owner?.Viewport;
            var input = Editor.Instance?.Options.Options.Input;
            if (viewport == null || input == null)
                return false;

            if (key == KeyboardKeys.Control && Gizmo?.IsMoveInteractionActive == true)
            {
                SetTransientModifiers(snapOverride: true);
                return true;
            }

            if (Gizmo?.OnKeyDown(key) == true)
                return true;

            if (input.CSGSelectPlaceTool.Process(viewport, key))
                return Controller.SetTool(CSGTool.SelectPlace);
            if (input.CSGDrawTool.Process(viewport, key))
                return Controller.SetTool(CSGTool.Draw);
            if (input.CSGEditTool.Process(viewport, key))
                return Controller.SetTool(CSGTool.Edit);
            if (input.CSGSurfaceTool.Process(viewport, key))
                return Controller.SetTool(CSGTool.Surface);
            if (input.CSGBrushTool.Process(viewport, key))
                return Controller.SetTool(CSGTool.Brush);
            if (input.CSGPickWorkingPlane.Process(viewport, key))
            {
                Controller.RequestPickWorkingPlane();
                return true;
            }
            if (input.CSGToggleWorkingPlaneLock.Process(viewport, key))
            {
                Controller.SetWorkingPlaneLocked(!Controller.WorkingPlaneLocked);
                return true;
            }
            if (input.CSGResetWorkingPlane.Process(viewport, key))
            {
                Controller.ResetWorkingPlane();
                return true;
            }
            if (Controller.HasActiveInteraction && input.CSGSnapOverride.Process(viewport, key))
            {
                SetTransientModifiers(snapOverride: true);
                return true;
            }
            if (Controller.HasActiveInteraction && input.CSGSquareConstraint.Process(viewport, key))
            {
                SetTransientModifiers(square: true);
                return true;
            }
            if (Controller.HasActiveInteraction && input.CSGSymmetricConstraint.Process(viewport, key))
            {
                SetTransientModifiers(symmetric: true);
                return true;
            }
            if ((Controller.HasActiveInteraction || Gizmo?.HasArmedSelectDrag == true) && input.CSGDuplicateModifier.Process(viewport, key))
            {
                SetTransientModifiers(duplicate: true);
                return true;
            }
            if ((Controller.HasActiveInteraction || Gizmo?.HasArmedSelectDrag == true) && input.CSGAlignNormalModifier.Process(viewport, key))
            {
                SetTransientModifiers(alignNormal: true);
                return true;
            }
            if (input.CSGCommit.Process(viewport, key))
                return Gizmo?.TryCommitDrawStage() == true || Controller.TryCommit();
            if (input.CSGCancel.Process(viewport, key))
                return TryCancel(EditorGizmoModeCancelReason.User);
            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseMove(Float2 location)
        {
            return Gizmo?.OnMouseMove(location) ?? false;
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            return Gizmo?.OnMouseDown(location, button) ?? false;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            return Gizmo?.OnMouseUp(location, button) ?? false;
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            return Gizmo?.OnMouseDoubleClick(location, button) ?? false;
        }

        /// <inheritdoc />
        public override bool OnKeyUp(KeyboardKeys key)
        {
            var input = Editor.Instance?.Options.Options.Input;
            if (input == null)
                return false;

            if (key == KeyboardKeys.Control && Controller.SnapOverrideActive)
            {
                SetTransientModifiers(snapOverride: false);
                return true;
            }

            if (key == input.CSGSnapOverride.Key && Controller.SnapOverrideActive)
            {
                SetTransientModifiers(snapOverride: false);
                return true;
            }
            if (key == input.CSGSquareConstraint.Key && Controller.SquareConstraintActive)
            {
                SetTransientModifiers(square: false);
                return true;
            }
            if (key == input.CSGSymmetricConstraint.Key && Controller.SymmetricConstraintActive)
            {
                SetTransientModifiers(symmetric: false);
                return true;
            }
            if (key == input.CSGDuplicateModifier.Key && Controller.DuplicateModifierActive)
            {
                SetTransientModifiers(duplicate: false);
                return true;
            }
            if (key == input.CSGAlignNormalModifier.Key && Controller.AlignNormalModifierActive)
            {
                SetTransientModifiers(alignNormal: false);
                return true;
            }
            return false;
        }

        /// <inheritdoc />
        public override bool TryCancel(EditorGizmoModeCancelReason reason)
        {
            bool gizmoCancelled = Gizmo?.TryCancel(reason) == true;
            bool controllerCancelled = Controller?.TryCancel(reason) ?? false;
            return gizmoCancelled || controllerCancelled;
        }

        /// <summary>
        /// Finds duplicate CSG input bindings that would otherwise depend on evaluation order.
        /// </summary>
        /// <param name="input">The editor input options.</param>
        /// <returns>Human-readable conflict descriptions.</returns>
        public static List<string> FindInputConflicts(InputOptions input)
        {
            var result = new List<string>();
            if (input == null)
                return result;

            var bindings = new[]
            {
                new KeyValuePair<string, InputBinding>("Select / Place Tool", input.CSGSelectPlaceTool),
                new KeyValuePair<string, InputBinding>("Draw Tool", input.CSGDrawTool),
                new KeyValuePair<string, InputBinding>("Edit Tool", input.CSGEditTool),
                new KeyValuePair<string, InputBinding>("Surface Tool", input.CSGSurfaceTool),
                new KeyValuePair<string, InputBinding>("Brush Tool", input.CSGBrushTool),
                new KeyValuePair<string, InputBinding>("Pick Working Plane", input.CSGPickWorkingPlane),
                new KeyValuePair<string, InputBinding>("Toggle Working Plane Lock", input.CSGToggleWorkingPlaneLock),
                new KeyValuePair<string, InputBinding>("Reset Working Plane", input.CSGResetWorkingPlane),
                new KeyValuePair<string, InputBinding>("Snap Override", input.CSGSnapOverride),
                new KeyValuePair<string, InputBinding>("Square Constraint", input.CSGSquareConstraint),
                new KeyValuePair<string, InputBinding>("Symmetric Constraint", input.CSGSymmetricConstraint),
                new KeyValuePair<string, InputBinding>("Duplicate Modifier", input.CSGDuplicateModifier),
                new KeyValuePair<string, InputBinding>("Align to Surface Normal Modifier", input.CSGAlignNormalModifier),
                new KeyValuePair<string, InputBinding>("Commit Interaction", input.CSGCommit),
                new KeyValuePair<string, InputBinding>("Cancel Interaction", input.CSGCancel),
            };

            for (int i = 0; i < bindings.Length; i++)
            {
                if (bindings[i].Value.Key == KeyboardKeys.None)
                    continue;
                if (bindings[i].Value.Key == KeyboardKeys.Control)
                    result.Add($"Ctrl: {bindings[i].Key} conflicts with temporary Draw and the move snap override");
                if (bindings[i].Value.Key == KeyboardKeys.Shift)
                    result.Add($"Shift: {bindings[i].Key} conflicts with component-add marquee selection");
                for (int j = i + 1; j < bindings.Length; j++)
                {
                    if (bindings[i].Value == bindings[j].Value)
                        result.Add($"{bindings[i].Value}: {bindings[i].Key} and {bindings[j].Key}");
                }
            }
            return result;
        }

        private void SetTransientModifiers(bool? snapOverride = null, bool? square = null, bool? symmetric = null, bool? duplicate = null, bool? alignNormal = null)
        {
            Controller.SetTransientModifiers(
                snapOverride ?? Controller.SnapOverrideActive,
                square ?? Controller.SquareConstraintActive,
                symmetric ?? Controller.SymmetricConstraintActive,
                duplicate ?? Controller.DuplicateModifierActive,
                alignNormal ?? Controller.AlignNormalModifierActive);
        }

        private void LoadState()
        {
            var cache = Editor.Instance?.ProjectCache;
            var state = Controller.CaptureState();
            if (cache == null)
                return;

            if (cache.TryGetCustomData(ToolCacheKey, out string text) && Enum.TryParse(text, out CSGTool tool))
                state.Tool = tool;
            if (cache.TryGetCustomData(OperationCacheKey, out text) && Enum.TryParse(text, out CSGOperation operation))
                state.Operation = operation;
            if (cache.TryGetCustomData(WorkingPlaneLockCacheKey, out bool flag))
                state.WorkingPlaneLocked = flag;
            if (cache.TryGetCustomData(SnappingCacheKey, out flag))
                state.SnappingEnabled = flag;
            if (cache.TryGetCustomData(BrushAlignmentSnappingCacheKey, out flag))
                state.BrushAlignmentSnappingEnabled = flag;
            if (cache.TryGetCustomData(SnapIncrementCacheKey, out float value))
                state.SnapIncrement = value;
            if (cache.TryGetCustomData(VisibilityCacheKey, out text) && Enum.TryParse(text, out CSGVisibility visibility))
                state.Visibility = visibility;
            if (cache.TryGetCustomData(RayPlacementAlignmentCacheKey, out text) && Enum.TryParse(text, out CSGRayPlacementAlignment alignment))
                state.RayPlacementAlignment = alignment;
            if (cache.TryGetCustomData(RayPlacementFrontCacheKey, out text) && Enum.TryParse(text, out CSGRayPlacementFront front))
                state.RayPlacementFront = front;
            if (cache.TryGetCustomData(BrushMaterialCacheKey, out text) && Guid.TryParse(text, out var materialId) && materialId != Guid.Empty)
                state.BrushMaterial = FlaxEngine.Content.LoadRuntimeObjectAsync<MaterialBase>(materialId);
            if (cache.TryGetCustomData(BrushMaterialAutoPickCacheKey, out flag))
                state.BrushMaterialAutoPick = flag;
            Controller.ApplyState(state);
        }

        private void OnControllerChanged()
        {
            var cache = Editor.Instance?.ProjectCache;
            if (cache == null)
                return;

            var state = Controller.CaptureState();
            cache.SetCustomData(ToolCacheKey, state.Tool.ToString());
            cache.SetCustomData(OperationCacheKey, state.Operation.ToString());
            cache.SetCustomData(WorkingPlaneLockCacheKey, state.WorkingPlaneLocked);
            cache.SetCustomData(SnappingCacheKey, state.SnappingEnabled);
            cache.SetCustomData(BrushAlignmentSnappingCacheKey, state.BrushAlignmentSnappingEnabled);
            cache.SetCustomData(SnapIncrementCacheKey, state.SnapIncrement);
            cache.SetCustomData(VisibilityCacheKey, state.Visibility.ToString());
            cache.SetCustomData(RayPlacementAlignmentCacheKey, state.RayPlacementAlignment.ToString());
            cache.SetCustomData(RayPlacementFrontCacheKey, state.RayPlacementFront.ToString());
            cache.SetCustomData(BrushMaterialCacheKey, state.BrushMaterial != null ? state.BrushMaterial.ID.ToString() : Guid.Empty.ToString());
            cache.SetCustomData(BrushMaterialAutoPickCacheKey, state.BrushMaterialAutoPick);
        }

        private void ReportInputConflicts()
        {
            var conflicts = FindInputConflicts(Editor.Instance?.Options.Options.Input);
            if (conflicts.Count != 0)
                Debug.LogWarning("CSG authoring input binding conflicts: " + string.Join("; ", conflicts));
        }
    }
}
