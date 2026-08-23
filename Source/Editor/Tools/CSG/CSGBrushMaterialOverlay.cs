// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.Scripting;
using FlaxEditor.Viewport.Overlays;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Tools.CSG
{
    /// <summary>
    /// Compact material controls for the CSG surface brush tool.
    /// </summary>
    [HideInEditor]
    public sealed class CSGBrushMaterialOverlay : ContainerControl, IViewportOverlayResponsiveContent
    {
        /// <summary>Preferred content size inside a viewport overlay container.</summary>
        public static readonly Float2 PreferredSize = new Float2(440.0f, 66.0f);

        private readonly CSGToolController _controller;
        private readonly AssetPicker _materialPicker;
        private readonly Button _pickButton;
        private readonly Button _autoPickButton;
        private readonly Button _resetButton;
        private readonly Label _stateLabel;
        private bool _syncing;

        /// <summary>Creates material selection, eyedropper, and reset controls.</summary>
        public CSGBrushMaterialOverlay(CSGToolController controller)
        : base(0, 0, PreferredSize.X, PreferredSize.Y)
        {
            _controller = controller;

            _stateLabel = new Label(6, 3, 356, 18)
            {
                Parent = this,
                Text = "Material painted by subsequent strokes",
                TextColor = Style.Current.ForegroundViewport,
            };
            _materialPicker = new AssetPicker(new ScriptType(typeof(MaterialBase)), new Float2(6, 27))
            {
                Parent = this,
                UseCompactField = true,
                ShowCompactPreview = true,
                Width = 210.0f,
            };
            _materialPicker.SelectedItemChanged += OnMaterialChanged;
            _pickButton = new Button(222, 27, 64, AssetPicker.CompactFieldWithPreviewHeight)
            {
                Parent = this,
                Text = "Pick",
                TooltipText = "Sample the material from the next clicked CSG surface.",
            };
            _pickButton.Clicked += TogglePick;
            _autoPickButton = new Button(292, 27, 76, AssetPicker.CompactFieldWithPreviewHeight)
            {
                Parent = this,
                Text = "Auto",
                TooltipText = "Automatically use a material selected in the Project window while the Brush tool is active.",
            };
            _autoPickButton.Clicked += ToggleAutoPick;
            _resetButton = new Button(374, 27, 60, AssetPicker.CompactFieldWithPreviewHeight)
            {
                Parent = this,
                Text = "Default",
                TooltipText = "Use the engine default CSG material for subsequent strokes.",
            };
            _resetButton.Clicked += _controller.ResetBrushMaterial;
            _controller.Changed += SyncFromController;
            if (Editor.Instance?.Windows?.ContentWin != null)
            {
                Editor.Instance.Windows.ContentWin.SelectionChanged += OnContentSelectionChanged;
                Editor.Instance.Windows.ContentWin.SelectionCoexistenceRequested += CanCoexistWithSceneSelection;
            }
            SyncFromController();
        }

        /// <inheritdoc />
        public float GetDesiredHeight(float width)
        {
            return width >= 360.0f ? 66.0f : 96.0f;
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            _stateLabel.Width = Mathf.Max(0.0f, Width - 12.0f);
            if (Width >= 360.0f)
            {
                _resetButton.Bounds = new Rectangle(Width - 66.0f, 27.0f, 60.0f, AssetPicker.CompactFieldWithPreviewHeight);
                _autoPickButton.Bounds = new Rectangle(_resetButton.Left - 82.0f, 27.0f, 76.0f, AssetPicker.CompactFieldWithPreviewHeight);
                _pickButton.Bounds = new Rectangle(_autoPickButton.Left - 70.0f, 27.0f, 64.0f, AssetPicker.CompactFieldWithPreviewHeight);
                _materialPicker.Width = Mathf.Max(100.0f, _pickButton.Left - 12.0f);
                _materialPicker.Location = new Float2(6.0f, 27.0f);
            }
            else
            {
                _materialPicker.Location = new Float2(6.0f, 27.0f);
                _materialPicker.Width = Mathf.Max(100.0f, Width - 12.0f);
                float buttonWidth = Mathf.Max(62.0f, (Width - 24.0f) / 3.0f);
                _pickButton.Bounds = new Rectangle(6.0f, 57.0f, buttonWidth, AssetPicker.CompactFieldWithPreviewHeight);
                _autoPickButton.Bounds = new Rectangle(12.0f + buttonWidth, 57.0f, buttonWidth, AssetPicker.CompactFieldWithPreviewHeight);
                _resetButton.Bounds = new Rectangle(18.0f + buttonWidth * 2.0f, 57.0f, buttonWidth, AssetPicker.CompactFieldWithPreviewHeight);
            }
            base.PerformLayoutBeforeChildren();
        }

        private void OnMaterialChanged()
        {
            if (!_syncing)
                _controller.SetBrushMaterial(_materialPicker.Validator.SelectedAsset as MaterialBase);
        }

        private void TogglePick()
        {
            _controller.SetBrushMaterialPickArmed(!_controller.BrushMaterialPickArmed);
        }

        private void ToggleAutoPick()
        {
            _controller.SetBrushMaterialAutoPick(!_controller.BrushMaterialAutoPick);
            if (_controller.BrushMaterialAutoPick)
                OnContentSelectionChanged();
        }

        private void OnContentSelectionChanged()
        {
            if (!_controller.BrushMaterialAutoPick || _controller.Tool != CSGTool.Brush)
                return;
            var selection = Editor.Instance?.Windows?.ContentWin?.Selection;
            if (selection == null || selection.Count != 1 || !_materialPicker.Validator.IsValid(selection[0]))
                return;
            _materialPicker.Validator.SelectedItem = selection[0];
        }

        private bool CanCoexistWithSceneSelection(ContentItem item)
        {
            if (!_controller.BrushMaterialAutoPick || item == null || !_materialPicker.Validator.IsValid(item))
                return false;
            if (_controller.Tool == CSGTool.Brush)
                return true;
            return item is AssetItem assetItem && _controller.BrushMaterial != null &&
                   assetItem.ID == _controller.BrushMaterial.ID;
        }

        private void SyncFromController()
        {
            if (IsDisposing)
                return;
            _syncing = true;
            _materialPicker.Validator.SelectedAsset = _controller.BrushMaterial;
            _syncing = false;
            _pickButton.Text = _controller.BrushMaterialPickArmed ? "Cancel" : "Pick";
            _autoPickButton.Text = _controller.BrushMaterialAutoPick ? "Auto On" : "Auto";
            _stateLabel.Text = _controller.BrushMaterial == null
                ? "Default CSG material"
                : "Material painted by subsequent strokes";
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _controller.Changed -= SyncFromController;
            if (Editor.Instance?.Windows?.ContentWin != null)
            {
                Editor.Instance.Windows.ContentWin.SelectionChanged -= OnContentSelectionChanged;
                Editor.Instance.Windows.ContentWin.SelectionCoexistenceRequested -= CanCoexistWithSceneSelection;
            }
            _materialPicker.SelectedItemChanged -= OnMaterialChanged;
            _pickButton.Clicked -= TogglePick;
            _autoPickButton.Clicked -= ToggleAutoPick;
            _resetButton.Clicked -= _controller.ResetBrushMaterial;
            base.OnDestroy();
        }
    }
}
