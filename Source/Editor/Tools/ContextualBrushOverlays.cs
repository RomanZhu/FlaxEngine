// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Input;
using FlaxEditor.CustomEditors;
using FlaxEditor.Tools.Foliage;
using FlaxEditor.Tools.Terrain;
using FlaxEditor.Tools.Terrain.Brushes;
using FlaxEditor.Viewport.Overlays;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Tools
{
    /// <summary>Compact contextual controls shared by terrain sculpt and terrain paint modes.</summary>
    [HideInEditor]
    public sealed class TerrainBrushContextOverlay : ContainerControl, IViewportOverlayResponsiveContent
    {
        /// <summary>Preferred overlay content size.</summary>
        public static readonly Float2 PreferredSize = new Float2(580.0f, 28.0f);

        private readonly SculptTerrainGizmoMode _sculpt;
        private readonly PaintTerrainGizmoMode _paint;
        private readonly Label _modeLabel;
        private readonly Label _sizeLabel;
        private readonly Label _strengthLabel;
        private readonly Label _falloffLabel;
        private readonly ComboBox _mode;
        private readonly FloatValueBox _size;
        private readonly FloatValueBox _strength;
        private readonly FloatValueBox _falloff;
        private bool _syncing;

        /// <summary>Creates terrain brush controls for sculpt mode.</summary>
        public TerrainBrushContextOverlay(SculptTerrainGizmoMode mode)
        : this()
        {
            _sculpt = mode;
            _mode.AddItems(System.Enum.GetNames(typeof(SculptTerrainGizmoMode.ModeTypes)));
            _mode.SelectedIndexChanged += OnModeChanged;
            Sync();
        }

        /// <summary>Creates terrain brush controls for paint mode.</summary>
        public TerrainBrushContextOverlay(PaintTerrainGizmoMode mode)
        : this()
        {
            _paint = mode;
            _modeLabel.Text = "Layer";
            _mode.AddItems(System.Enum.GetNames(typeof(Terrain.Paint.SingleLayerMode.Layers)));
            _mode.SelectedIndexChanged += OnModeChanged;
            Sync();
        }

        private TerrainBrushContextOverlay()
        : base(0, 0, PreferredSize.X, PreferredSize.Y)
        {
            _modeLabel = new Label(6, 5, 38, 20) { Parent = this, Text = "Mode", HorizontalAlignment = TextAlignment.Near };
            _mode = new ComboBox(46, 3, 104) { Parent = this };
            _sizeLabel = AddLabel("Size", 158, 5, 30);
            _size = new FloatValueBox(100.0f, 190, 3, 74, 0.0001f, 1000000.0f, 10.0f) { Parent = this };
            _strengthLabel = AddLabel("Strength", 272, 5, 50);
            _strength = new FloatValueBox(1.0f, 324, 3, 96, 0.0f, 1000.0f, 0.01f) { Parent = this };
            _falloffLabel = AddLabel("Falloff", 6, 32, 42);
            _falloff = new FloatValueBox(0.5f, 50, 29, 100, 0.0f, 1.0f, 0.01f) { Parent = this };
            _size.ValueChanged += OnValuesChanged;
            _strength.ValueChanged += OnValuesChanged;
            _falloff.ValueChanged += OnValuesChanged;
        }

        /// <inheritdoc />
        public float GetDesiredHeight(float width)
        {
            float x = 6.0f;
            int rows = 1;
            var widths = new[] { 144.0f, 106.0f, 148.0f, 144.0f };
            for (int i = 0; i < widths.Length; i++)
            {
                if (x > 6.0f && x + widths[i] > width - 6.0f)
                {
                    rows++;
                    x = 6.0f;
                }
                x += widths[i] + 8.0f;
            }
            return rows * 26.0f + 2.0f;
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            float x = 6.0f;
            float y = 0.0f;
            PlaceGroup(_modeLabel, _mode, 38.0f, 104.0f, ref x, ref y);
            PlaceGroup(_sizeLabel, _size, 30.0f, 74.0f, ref x, ref y);
            PlaceGroup(_strengthLabel, _strength, 50.0f, 96.0f, ref x, ref y);
            PlaceGroup(_falloffLabel, _falloff, 42.0f, 100.0f, ref x, ref y);
            base.PerformLayoutBeforeChildren();
        }

        private void PlaceGroup(Label label, Control input, float labelWidth, float inputWidth, ref float x, ref float y)
        {
            float groupWidth = labelWidth + 2.0f + inputWidth;
            if (x > 6.0f && x + groupWidth > Width - 6.0f)
            {
                x = 6.0f;
                y += 26.0f;
            }
            label.Bounds = new Rectangle(x, y + 4.0f, labelWidth, 20.0f);
            input.Bounds = new Rectangle(x + labelWidth + 2.0f, y + 2.0f, inputWidth, 22.0f);
            x += groupWidth + 8.0f;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            Sync();
            base.Update(deltaTime);
        }

        private void OnModeChanged(ComboBox box)
        {
            if (_syncing || box.SelectedIndex < 0)
                return;
            if (_sculpt != null)
                _sculpt.ToolModeType = (SculptTerrainGizmoMode.ModeTypes)box.SelectedIndex;
            else if (_paint != null)
                _paint.SingleLayerMode.Layer = (Terrain.Paint.SingleLayerMode.Layers)box.SelectedIndex;
            Sync();
        }

        private void OnValuesChanged()
        {
            if (_syncing)
                return;
            var brush = CurrentBrush;
            if (brush != null)
                brush.Size = _size.Value;
            CurrentStrength = _strength.Value;
            if (brush is CircleBrush circle)
                circle.Falloff = _falloff.Value;
        }

        private void Sync()
        {
            var brush = CurrentBrush;
            if (brush == null)
                return;
            _syncing = true;
            _mode.SelectedIndex = _sculpt != null ? (int)_sculpt.ToolModeType : (int)_paint.SingleLayerMode.Layer;
            if (!_size.IsFocused)
                _size.Value = brush.Size;
            if (!_strength.IsFocused)
                _strength.Value = CurrentStrength;
            _falloff.Enabled = brush is CircleBrush;
            if (brush is CircleBrush circle && !_falloff.IsFocused)
                _falloff.Value = circle.Falloff;
            _syncing = false;
        }

        private FlaxEditor.Tools.Terrain.Brushes.Brush CurrentBrush => _sculpt != null ? _sculpt.CurrentBrush : _paint?.CurrentBrush;

        private float CurrentStrength
        {
            get => _sculpt != null ? _sculpt.CurrentMode.Strength : _paint.CurrentMode.Strength;
            set
            {
                if (_sculpt != null)
                    _sculpt.CurrentMode.Strength = value;
                else if (_paint != null)
                    _paint.CurrentMode.Strength = value;
            }
        }

        private Label AddLabel(string text, float x, float y, float width)
        {
            return new Label(x, y, width, 20)
            {
                Parent = this,
                Text = text,
                HorizontalAlignment = TextAlignment.Near,
            };
        }
    }

    /// <summary>Compact contextual controls for foliage painting.</summary>
    [HideInEditor]
    public sealed class FoliageBrushContextOverlay : ContainerControl, IViewportOverlayResponsiveContent
    {
        /// <summary>Preferred overlay content size.</summary>
        public static readonly Float2 PreferredSize = new Float2(350.0f, 28.0f);

        private readonly PaintFoliageGizmoMode _mode;
        private readonly Label _sizeLabel;
        private readonly Label _densityLabel;
        private readonly Label _singleClickLabel;
        private readonly FloatValueBox _size;
        private readonly FloatValueBox _density;
        private readonly CheckBox _singleClick;
        private bool _syncing;

        /// <summary>Creates foliage brush controls.</summary>
        public FoliageBrushContextOverlay(PaintFoliageGizmoMode mode)
        : base(0, 0, PreferredSize.X, PreferredSize.Y)
        {
            _mode = mode;
            _sizeLabel = AddLabel("Size", 6, 5, 30);
            _size = new FloatValueBox(mode.CurrentBrush.Size, 38, 3, 82, 0.0001f, 1000000.0f, 10.0f) { Parent = this };
            _densityLabel = AddLabel("Density", 128, 5, 45);
            _density = new FloatValueBox(mode.CurrentBrush.DensityScale, 175, 3, 72, 0.0f, 1000.0f, 0.01f) { Parent = this };
            _singleClick = new CheckBox(256, 5, mode.CurrentBrush.SingleClick) { Parent = this };
            _singleClickLabel = new Label(274, 5, 70, 20) { Parent = this, Text = "Single click", HorizontalAlignment = TextAlignment.Near };
            _size.ValueChanged += OnValuesChanged;
            _density.ValueChanged += OnValuesChanged;
            _singleClick.StateChanged += OnSingleClickChanged;
        }

        /// <inheritdoc />
        public float GetDesiredHeight(float width)
        {
            float required = 6.0f;
            int rows = 1;
            var widths = new[] { 114.0f, 119.0f, 88.0f };
            for (int i = 0; i < widths.Length; i++)
            {
                if (required > 6.0f && required + widths[i] > width - 6.0f)
                {
                    rows++;
                    required = 6.0f;
                }
                required += widths[i] + 8.0f;
            }
            return rows * 26.0f + 2.0f;
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            float x = 6.0f;
            float y = 0.0f;
            PlaceGroup(_sizeLabel, _size, 30.0f, 82.0f, ref x, ref y);
            PlaceGroup(_densityLabel, _density, 45.0f, 72.0f, ref x, ref y);
            const float singleWidth = 88.0f;
            if (x > 6.0f && x + singleWidth > Width - 6.0f)
            {
                x = 6.0f;
                y += 26.0f;
            }
            _singleClick.Bounds = new Rectangle(x, y + 5.0f, 14.0f, 14.0f);
            _singleClickLabel.Bounds = new Rectangle(x + 18.0f, y + 4.0f, 70.0f, 20.0f);
            base.PerformLayoutBeforeChildren();
        }

        private void PlaceGroup(Label label, Control input, float labelWidth, float inputWidth, ref float x, ref float y)
        {
            float groupWidth = labelWidth + 2.0f + inputWidth;
            if (x > 6.0f && x + groupWidth > Width - 6.0f)
            {
                x = 6.0f;
                y += 26.0f;
            }
            label.Bounds = new Rectangle(x, y + 4.0f, labelWidth, 20.0f);
            input.Bounds = new Rectangle(x + labelWidth + 2.0f, y + 2.0f, inputWidth, 22.0f);
            x += groupWidth + 8.0f;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            _syncing = true;
            if (!_size.IsFocused)
                _size.Value = _mode.CurrentBrush.Size;
            if (!_density.IsFocused)
                _density.Value = _mode.CurrentBrush.DensityScale;
            _singleClick.Checked = _mode.CurrentBrush.SingleClick;
            _syncing = false;
            base.Update(deltaTime);
        }

        private void OnValuesChanged()
        {
            if (_syncing)
                return;
            _mode.CurrentBrush.Size = _size.Value;
            _mode.CurrentBrush.DensityScale = _density.Value;
        }

        private void OnSingleClickChanged(CheckBox box)
        {
            if (!_syncing)
                _mode.CurrentBrush.SingleClick = box.Checked;
        }

        private Label AddLabel(string text, float x, float y, float width)
        {
            return new Label(x, y, width, 20) { Parent = this, Text = text, HorizontalAlignment = TextAlignment.Near };
        }
    }

    /// <summary>Contextual foliage-type selection and property editing.</summary>
    [HideInEditor]
    public sealed class FoliageTypeContextOverlay : ContainerControl
    {
        /// <summary>Preferred overlay content size.</summary>
        public static readonly Float2 PreferredSize = new Float2(390.0f, 460.0f);

        private readonly FoliageTab _tab;
        private readonly ComboBox _types;
        private readonly CheckBox _paintEnabled;
        private readonly Button _addType;
        private readonly Button _removeType;
        private readonly Panel _propertiesPanel;
        private readonly CustomEditorPresenter _presenter;
        private bool _syncing;

        /// <summary>Creates the foliage type selector and selected-type property editor.</summary>
        public FoliageTypeContextOverlay(FoliageTab tab)
        : base(0, 0, PreferredSize.X, PreferredSize.Y)
        {
            _tab = tab;
            new Label(6, 5, 34, 20) { Parent = this, Text = "Type", HorizontalAlignment = TextAlignment.Near };
            _types = new ComboBox(42, 3, 246) { Parent = this };
            _paintEnabled = new CheckBox(298, 5, true) { Parent = this };
            new Label(318, 5, 66, 20) { Parent = this, Text = "Paint", HorizontalAlignment = TextAlignment.Near };
            _addType = new Button(6, 32, 190, 24) { Parent = this, Text = "Add Foliage Type" };
            _removeType = new Button(202, 32, 182, 24) { Parent = this, Text = "Remove Selected" };
            _propertiesPanel = new Panel(ScrollBars.Vertical)
            {
                Parent = this,
            };
            _presenter = new CustomEditorPresenter(null, "No foliage type selected");
            _presenter.Panel.Parent = _propertiesPanel;
            _presenter.Modified += OnModified;
            _types.SelectedIndexChanged += OnTypeChanged;
            _paintEnabled.StateChanged += OnPaintEnabledChanged;
            _addType.Clicked += OnAddType;
            _removeType.Clicked += OnRemoveType;
            _tab.SelectedFoliageChanged += RefreshTypes;
            _tab.SelectedFoliageTypesChanged += RefreshTypes;
            _tab.SelectedFoliageTypeIndexChanged += OnSelectedTypeChanged;
            RefreshTypes();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            _types.Width = Mathf.Max(80.0f, Width - 144.0f);
            _paintEnabled.X = Width - 92.0f;
            _addType.Width = Mathf.Max(80.0f, Width * 0.5f - 9.0f);
            _removeType.X = _addType.Right + 6.0f;
            _removeType.Width = Mathf.Max(80.0f, Width - _removeType.X - 6.0f);
            _propertiesPanel.Bounds = new Rectangle(0, 62, Width, Mathf.Max(0.0f, Height - 62.0f));
            base.PerformLayoutBeforeChildren();
        }

        private void RefreshTypes()
        {
            _syncing = true;
            _types.ClearItems();
            var foliage = _tab.SelectedFoliage;
            _addType.Enabled = foliage != null;
            if (foliage)
            {
                for (int i = 0; i < foliage.FoliageTypesCount; i++)
                {
                    var model = foliage.GetFoliageType(i).Model;
                    var asset = model ? _tab.Editor.ContentDatabase.FindAsset(model.ID) : null;
                    _types.AddItem(asset?.NamePath ?? $"Foliage Type {i + 1}");
                }
            }
            int selected = foliage && _tab.SelectedFoliageTypeIndex >= 0 && _tab.SelectedFoliageTypeIndex < foliage.FoliageTypesCount
                ? _tab.SelectedFoliageTypeIndex
                : -1;
            _types.SelectedIndex = selected;
            SyncSelectedType(selected);
            _syncing = false;
        }

        private void OnSelectedTypeChanged(int previousIndex, int currentIndex)
        {
            if (_syncing)
                return;
            _syncing = true;
            _types.SelectedIndex = currentIndex;
            SyncSelectedType(currentIndex);
            _syncing = false;
        }

        private void OnTypeChanged(ComboBox box)
        {
            if (!_syncing)
                _tab.SelectedFoliageTypeIndex = box.SelectedIndex;
        }

        private void SyncSelectedType(int index)
        {
            var foliage = _tab.SelectedFoliage;
            if (!foliage || index < 0 || index >= foliage.FoliageTypesCount)
            {
                _paintEnabled.Enabled = false;
                _paintEnabled.Checked = false;
                _removeType.Enabled = false;
                _presenter.Deselect();
                return;
            }

            var type = foliage.GetFoliageType(index);
            _paintEnabled.Enabled = true;
            _removeType.Enabled = true;
            _paintEnabled.Checked = !type.Model || !_tab.FoliageTypeModelIdsToPaint.TryGetValue(type.Model.ID, out bool enabled) || enabled;
            _presenter.Select(type);
            _presenter.BuildLayoutOnUpdate();
        }

        private void OnPaintEnabledChanged(CheckBox box)
        {
            if (_syncing)
                return;
            var foliage = _tab.SelectedFoliage;
            int index = _tab.SelectedFoliageTypeIndex;
            if (foliage && index >= 0 && index < foliage.FoliageTypesCount)
            {
                var model = foliage.GetFoliageType(index).Model;
                if (model)
                    _tab.FoliageTypeModelIdsToPaint[model.ID] = box.Checked;
            }
        }

        private void OnModified()
        {
            Editor.Instance.Scene.MarkSceneEdited(_tab.SelectedFoliage?.Scene);
        }

        private void OnAddType()
        {
            AssetSearchPopup.Show(_addType, new Float2(_addType.Width * 0.5f, _addType.Height),
                item => item is BinaryAssetItem binary && binary.Type == typeof(Model), OnTypeAssetSelected);
        }

        private void OnTypeAssetSelected(AssetItem item)
        {
            var foliage = _tab.SelectedFoliage;
            if (!foliage || item == null)
                return;
            var model = FlaxEngine.Content.LoadAssetAsync<Model>(item.ObjectID);
            var action = new FlaxEditor.Tools.Foliage.Undo.EditFoliageAction(foliage);
            foliage.AddFoliageType(model);
            Editor.Instance.Scene.MarkSceneEdited(foliage.Scene);
            action.RecordEnd();
            _tab.Editor.Undo.AddAction(action);
            _tab.OnSelectedFoliageTypesChanged();
            _tab.SelectedFoliageTypeIndex = foliage.FoliageTypesCount - 1;
        }

        private void OnRemoveType()
        {
            var foliage = _tab.SelectedFoliage;
            int index = _tab.SelectedFoliageTypeIndex;
            if (!foliage || index < 0 || index >= foliage.FoliageTypesCount)
                return;
            _tab.SelectedFoliageTypeIndex = -1;
            var action = new FlaxEditor.Tools.Foliage.Undo.EditFoliageAction(foliage);
            foliage.RemoveFoliageType(index);
            action.RecordEnd();
            _tab.Editor.Undo.AddAction(action);
            _tab.OnSelectedFoliageTypesChanged();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _tab.SelectedFoliageChanged -= RefreshTypes;
            _tab.SelectedFoliageTypesChanged -= RefreshTypes;
            _tab.SelectedFoliageTypeIndexChanged -= OnSelectedTypeChanged;
            _presenter.Modified -= OnModified;
            base.OnDestroy();
        }
    }
}
