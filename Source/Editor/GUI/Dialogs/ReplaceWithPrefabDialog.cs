// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.CustomEditors;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Dialogs;
using FlaxEditor.SceneGraph;
using FlaxEditor.Scripting;
using FlaxEditor.Tools;
using FlaxEditor.Viewport.Previews;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.Dialogs
{
    /// <summary>
    /// Dialog for replacing selected actors with a prefab with live in-scene preview.
    /// </summary>
    public class ReplaceWithPrefabDialog : Dialog
    {
        private readonly List<ActorNode> _targetNodes;
        private readonly ReplaceOptions _options = new ReplaceOptions();
        private readonly AssetPicker _assetPicker;
        private readonly PrefabPreview _preview;
        private readonly Button _applyButton;
        private readonly CustomEditorPresenter _editor;
        private readonly List<Actor> _previewActors = new List<Actor>();
        private readonly Dictionary<Actor, bool> _originalActiveState = new Dictionary<Actor, bool>();

        /// <summary>
        /// Initializes a new instance of the <see cref="ReplaceWithPrefabDialog"/> class.
        /// </summary>
        /// <param name="targetNodes">The actor nodes to replace.</param>
        public ReplaceWithPrefabDialog(List<ActorNode> targetNodes)
            : base("Replace with Prefab")
        {
            _targetNodes = targetNodes ?? new List<ActorNode>();

            const float TotalWidth = 720;
            const float TotalHeight = 520;
            const float LeftWidth = 370;
            const float RightWidth = 320;
            const float ContentHeight = 400;

            _dialogSize = new Float2(TotalWidth, TotalHeight);
            Size = _dialogSize;

            // Header
            var headerLabel = new Label
            {
                Text = "Replace with Prefab",
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(10, 10, 10, 30),
                Parent = this,
                Font = new FontReference(Style.Current.FontTitle),
                HorizontalAlignment = TextAlignment.Near,
            };

            string actorNames = _targetNodes.Count == 1
                ? $"'{_targetNodes[0].Actor?.Name ?? "Actor"}'"
                : $"{_targetNodes.Count} selected actors";
            var infoLabel = new Label
            {
                Text = $"Select a prefab to replace {actorNames}.",
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(10, 10, 42, 20),
                Parent = this,
                HorizontalAlignment = TextAlignment.Near,
                TextColor = Style.Current.ForegroundGrey,
            };

            // Left panel (Asset Picker + Options)
            var leftPanel = new Panel
            {
                AnchorPreset = AnchorPresets.TopLeft,
                Offsets = new Margin(10, 0, 70, ContentHeight),
                Width = LeftWidth,
                Parent = this,
            };

            var pickerLabel = new Label
            {
                Text = "Replacement Prefab:",
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0, 0, 0, 20),
                Parent = leftPanel,
                HorizontalAlignment = TextAlignment.Near,
            };

            _assetPicker = new AssetPicker(new ScriptType(typeof(Prefab)), new Float2(0, 22))
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0, 0, 22, 36),
                UseCompactField = true,
                ShowCompactPreview = true,
                Parent = leftPanel,
            };
            _assetPicker.SelectedItemChanged += UpdateScenePreview;

            // Options Presenter
            _editor = new CustomEditorPresenter(null);
            _editor.Panel.AnchorPreset = AnchorPresets.HorizontalStretchTop;
            _editor.Panel.Offsets = new Margin(0, 0, 64, ContentHeight - 64);
            _editor.Panel.Parent = leftPanel;
            _editor.Modified += UpdateScenePreview;
            _editor.Select(_options);

            // Right panel (3D Preview)
            var rightPanel = new Panel
            {
                AnchorPreset = AnchorPresets.TopLeft,
                Offsets = new Margin(LeftWidth + 25, 0, 70, ContentHeight),
                Width = RightWidth,
                Parent = this,
            };

            var previewLabel = new Label
            {
                Text = "Prefab Preview:",
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0, 0, 0, 20),
                Parent = rightPanel,
                HorizontalAlignment = TextAlignment.Near,
            };

            _preview = new PrefabPreview(true)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, 22, 0),
                Parent = rightPanel,
            };

            // Bottom Buttons
            const float ButtonWidth = 80;
            const float ButtonHeight = 26;
            const float ButtonMargin = 12;

            _applyButton = new Button
            {
                Text = "Apply",
                AnchorPreset = AnchorPresets.BottomRight,
                Offsets = new Margin(-ButtonWidth - ButtonMargin - ButtonWidth - ButtonMargin, ButtonWidth, -ButtonHeight - ButtonMargin, ButtonHeight),
                Parent = this,
                Enabled = false,
            };
            _applyButton.Clicked += OnApply;

            var cancelButton = new Button
            {
                Text = "Cancel",
                AnchorPreset = AnchorPresets.BottomRight,
                Offsets = new Margin(-ButtonWidth - ButtonMargin, ButtonWidth, -ButtonHeight - ButtonMargin, ButtonHeight),
                Parent = this,
            };
            cancelButton.Clicked += OnCancel;
        }

        /// <inheritdoc />
        protected override void SetupWindowSettings(ref CreateWindowSettings settings)
        {
            base.SetupWindowSettings(ref settings);
            settings.HasSizingFrame = true;
        }

        private void UpdateScenePreview()
        {
            ClearScenePreview();

            var prefab = _assetPicker.Validator.SelectedAsset as Prefab;
            _applyButton.Enabled = prefab != null;
            _preview.Prefab = prefab;

            if (prefab == null || prefab.WaitForLoaded())
                return;

            foreach (var targetNode in _targetNodes)
            {
                if (targetNode == null || !targetNode.Actor || targetNode.Actor is Scene)
                    continue;

                var oldActor = targetNode.Actor;
                var parent = oldActor.Parent;
                if (parent == null || oldActor.Scene == null)
                    continue;

                // Remember original active state and temporarily disable old actor
                if (!_originalActiveState.ContainsKey(oldActor))
                    _originalActiveState[oldActor] = oldActor.IsActive;
                oldActor.IsActive = false;

                // Spawn preview instance in scene
                var previewActor = PrefabManager.SpawnPrefab(prefab, parent);
                if (previewActor == null)
                    continue;

                // Prevent saving or polluting scene hierarchy during preview
                SetHideFlagsRecursive(previewActor, HideFlags.DontSave | HideFlags.HideInHierarchy | HideFlags.DontSelect);

                // Transfer transform
                var oldTransform = oldActor.Transform;
                var newTransform = previewActor.Transform;

                if (_options.ApplyPosition)
                    newTransform.Translation = oldTransform.Translation;
                if (_options.ApplyRotation)
                    newTransform.Orientation = oldTransform.Orientation;
                if (_options.ApplyScale)
                    newTransform.Scale = oldTransform.Scale;

                previewActor.Transform = newTransform;
                previewActor.OrderInParent = oldActor.OrderInParent;
                previewActor.Layer = oldActor.Layer;
                previewActor.Tags = oldActor.Tags;
                previewActor.StaticFlags = oldActor.StaticFlags;

                if (_options.KeepName)
                    previewActor.Name = oldActor.Name;

                // Transfer materials
                if (_options.TransferMaterials)
                {
                    ActorReplacement.TransferMaterials(oldActor, previewActor);
                }

                _previewActors.Add(previewActor);
            }
        }

        private void ClearScenePreview()
        {
            foreach (var previewActor in _previewActors)
            {
                if (previewActor)
                {
                    FlaxEngine.Object.Destroy(previewActor);
                }
            }
            _previewActors.Clear();

            foreach (var kvp in _originalActiveState)
            {
                if (kvp.Key)
                {
                    kvp.Key.IsActive = kvp.Value;
                }
            }
            _originalActiveState.Clear();

            FlaxEngine.Scripting.FlushRemovedObjects();
        }

        private static void SetHideFlagsRecursive(Actor actor, HideFlags flags)
        {
            if (actor == null)
                return;
            actor.HideFlags |= flags;
            for (int i = 0; i < actor.ChildrenCount; i++)
                SetHideFlagsRecursive(actor.GetChild(i), flags);
        }

        private void OnApply()
        {
            var prefab = _assetPicker.Validator.SelectedAsset as Prefab;
            if (prefab == null)
                return;

            // Clear temporary preview actors and restore original state before committing real replacement
            ClearScenePreview();

            if (ActorReplacement.Replace(_targetNodes, prefab, _options))
            {
                Close(DialogResult.OK);
            }
        }

        /// <inheritdoc />
        public override void OnCancel()
        {
            ClearScenePreview();
            base.OnCancel();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            ClearScenePreview();
            base.OnDestroy();
        }
    }
}
