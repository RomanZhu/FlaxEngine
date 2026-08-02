// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Xml;
using FlaxEditor.Content;
using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Dedicated;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.CustomEditors.Elements;
using FlaxEditor.CustomEditors.GUI;
using FlaxEditor.GUI.Input;
using FlaxEditor.GUI.Tabs;
using FlaxEditor.GUI.Timeline;
using FlaxEditor.GUI.Timeline.Tracks;
using FlaxEditor.SceneGraph;
using FlaxEditor.Surface;
using FlaxEditor.Viewport;
using FlaxEditor.Viewport.Previews;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Window used to present collection of selected object(s) properties in a grid. Supports Undo/Redo operations.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.EditorWindow" />
    /// <seealso cref="FlaxEditor.Windows.SceneEditorWindow" />
    public class PropertiesWindow : SceneEditorWindow, IPresenterOwner
    {
        private IEnumerable<object> undoRecordObjects;

        private readonly Dictionary<Guid, float> _actorScrollValues = new Dictionary<Guid, float>();
        private readonly List<Asset> _waitingForContentAssets = new List<Asset>();
        private readonly List<PinnedTab> _pinnedTabs = new List<PinnedTab>();
        private readonly ScriptingObjectEditor _contentAssetEditor = new ScriptingObjectEditor();
        private IDisposable _contentAssetState;
        private bool _lockObjects = false;
        private bool _showContentSelection;
        private bool _isApplyingContentAssetChanges;
        private SearchBox _searchBox;
        private Panel _scrollingPanel;
        private Tabs _tabs;
        private Tab _selectionTab;
        private float _tabsBarHeight;

        private const int MaxTabTitleLength = 24;
        private const float TabCloseButtonSize = 14.0f;
        private sealed class PropertiesTab : Tab
        {
            private readonly PropertiesWindow _owner;
            public readonly bool Closeable;

            public PropertiesTab(PropertiesWindow owner, string text, bool closeable)
            : base(text)
            {
                _owner = owner;
                Closeable = closeable;
            }

            public override Tabs.TabHeader CreateHeader()
            {
                return new PropertiesTabHeader((Tabs)Parent, this, _owner);
            }
        }

        private sealed class PropertiesTabHeader : Tabs.TabHeader
        {
            private readonly PropertiesWindow _owner;
            private readonly bool _closeable;
            private bool _mouseDown;
            private bool _closeMouseDown;
            private bool _dragging;
            private Float2 _mouseDownLocation;

            private PropertiesTab PropertiesTab => (PropertiesTab)Tab;

            public PropertiesTabHeader(Tabs tabs, PropertiesTab tab, PropertiesWindow owner)
            : base(tabs, tab)
            {
                _owner = owner;
                _closeable = tab.Closeable;
            }

            private Rectangle CloseButtonBounds => new Rectangle(Size.X - TabCloseButtonSize, 0.0f, TabCloseButtonSize, Size.Y);

            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button != MouseButton.Left || !EnabledInHierarchy || !Tab.Enabled)
                    return true;

                Focus();
                StartMouseCapture();
                _closeMouseDown = _closeable && CloseButtonBounds.Contains(ref location);
                _mouseDown = !_closeMouseDown;
                _dragging = false;
                _mouseDownLocation = location;
                return true;
            }

            public override void OnMouseMove(Float2 location)
            {
                if (_mouseDown && !_closeMouseDown && Tab != _owner._selectionTab)
                {
                    if (!_dragging && Mathf.Abs(location.X - _mouseDownLocation.X) > 4.0f)
                        _dragging = true;
                    if (_dragging)
                        ReorderTab(location);
                }

                base.OnMouseMove(location);
            }

            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button != MouseButton.Left)
                    return true;

                bool close = _closeMouseDown && CloseButtonBounds.Contains(ref location);
                bool select = _mouseDown && !_dragging;
                _mouseDown = false;
                _closeMouseDown = false;
                EndMouseCapture();

                if (close)
                {
                    _owner.ClosePinnedTab(PropertiesTab);
                }
                else if (select && EnabledInHierarchy && Tab.Enabled)
                {
                    _owner._tabs.SelectedTab = Tab;
                    Tab.PerformLayout(true);
                    _owner._tabs.Focus();
                }

                return true;
            }

            public override void OnEndMouseCapture()
            {
                _mouseDown = false;
                _closeMouseDown = false;
                _dragging = false;
                base.OnEndMouseCapture();
            }

            private void ReorderTab(Float2 location)
            {
                int headerIndex = _owner._tabs.TabsPanel.Children.IndexOf(this);
                if (headerIndex <= 0)
                    return;

                float pointerX = Location.X + location.X;
                int direction = pointerX < Location.X ? -1 : pointerX > Location.X + Width ? 1 : 0;
                if (direction == 0)
                    return;

                int targetIndex = headerIndex + direction;
                if (targetIndex < 1 || targetIndex >= _owner._tabs.TabsPanel.Children.Count)
                    return;

                var selectedTab = _owner._tabs.SelectedTab;
                var tab = _owner._tabs.Children[headerIndex + 1];
                var headerControl = _owner._tabs.TabsPanel.Children[headerIndex];
                _owner._tabs.Children.RemoveAt(headerIndex + 1);
                _owner._tabs.Children.Insert(targetIndex + 1, tab);
                _owner._tabs.TabsPanel.Children.RemoveAt(headerIndex);
                _owner._tabs.TabsPanel.Children.Insert(targetIndex, headerControl);
                _owner._tabs.PerformLayout();
                _owner._tabs.TabsPanel.PerformLayout();
                _owner._tabs.SelectedTab = selectedTab;
            }

            public override void Draw()
            {
                base.Draw();

                if (_closeable)
                {
                    var style = Style.Current;
                    var bounds = CloseButtonBounds;
                    Render2D.DrawSprite(style.Cross, bounds.MakeExpanded(-2.0f), IsMouseOver ? style.Foreground : style.ForegroundGrey);
                }
            }
        }

        private sealed class PinnedTab
        {
            public readonly Tab Tab;
            public readonly Panel Panel;
            public readonly CustomEditorPresenter Presenter;
            public readonly object[] Selection;

            public PinnedTab(Tab tab, Panel panel, CustomEditorPresenter presenter, object[] selection)
            {
                Tab = tab;
                Panel = panel;
                Presenter = presenter;
                Selection = selection;
            }
        }

        /// <inheritdoc />
        public override bool UseLayoutData => true;

        /// <summary>
        /// The editor.
        /// </summary>
        public readonly CustomEditorPresenter Presenter;

        /// <summary>
        /// Indication of if the scale is locked.
        /// </summary>
        public bool ScaleLinked = false;

        /// <summary>
        /// Indication of if UI elements should size relative to the pivot point.
        /// </summary>
        public bool UIPivotRelative = true;

        /// <summary>
        /// Indication of if the properties window is locked on specific objects.
        /// </summary>
        public bool LockSelection
        {
            get => _lockObjects;
            set
            {
                if (value == _lockObjects)
                    return;
                _lockObjects = value;
                if (!value)
                    RefreshSelection();
            }
        }

        /// <inheritdoc />
        public ISceneEditingContext SceneContext => Editor.Windows.EditWin;

        /// <summary>
        /// Initializes a new instance of the <see cref="PropertiesWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public PropertiesWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Properties";
            Icon = editor.Icons.Build64;
            AutoFocus = true;

            _tabs = new Tabs
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                AutoTabsSize = true,
                Parent = this,
            };
            _tabsBarHeight = _tabs.TabsSize.Y;
            _selectionTab = _tabs.AddTab(new PropertiesTab(this, "Selection", false));

            _searchBox = new SearchBox
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Parent = _selectionTab,
                Bounds = new Rectangle(4, 2, Width - 8, 18),
                Visible = false,
            };
            _searchBox.TextChanged += ApplySearchFilter;

            _scrollingPanel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = _selectionTab,
            };

            Presenter = new CustomEditorPresenter(editor.Undo, null, this);
            Presenter.Panel.Parent = _scrollingPanel;
            Presenter.GetUndoObjects += GetUndoObjects;
            Presenter.Features |= FeatureFlags.CacheExpandedGroups;
            Presenter.AfterLayout += OnPresenterAfterLayout;
            Presenter.Modified += OnPresenterModified;

            _scrollingPanel.VScrollBar.ValueChanged += OnScrollValueChanged;
            Editor.SceneEditing.SelectionChanged += OnSceneSelectionChanged;
            Editor.Windows.ContentWin.SelectionChanged += OnContentSelectionChanged;
            UpdateTabsBarVisibility();
        }

        /// <inheritdoc />
        public override void OnSceneLoaded(Scene scene, Guid sceneId)
        {
            base.OnSceneLoaded(scene, sceneId);

            // Clear scroll values if new scene is loaded non additively
            if (Level.ScenesCount > 1)
                return;
            _actorScrollValues.Clear();
            if (LockSelection)
            {
                LockSelection = false;
                Presenter.Deselect();
            }
        }

        private void OnScrollValueChanged()
        {
            if (_showContentSelection || Editor.SceneEditing.SelectionCount != 1)
                return;

            // Clear first 10 scroll values to keep the memory down. Dont need to cache very single value in a scene. We could expose this as a editor setting in the future.
            if (_actorScrollValues.Count >= 20)
            {
                int i = 0;
                foreach (var e in _actorScrollValues)
                {
                    if (i >= 10)
                        break;
                    _actorScrollValues.Remove(e.Key);
                    i += 1;
                }
            }
            
            if (_scrollingPanel.VScrollBar != null)
                _actorScrollValues[Editor.SceneEditing.Selection[0].ID] = _scrollingPanel.VScrollBar.TargetValue;
        }

        private IEnumerable<object> GetUndoObjects(CustomEditorPresenter customEditorPresenter)
        {
            return undoRecordObjects;
        }

        private static string TruncateTabTitle(string text)
        {
            if (string.IsNullOrEmpty(text) || text.Length <= MaxTabTitleLength)
                return text;
            return text.Substring(0, MaxTabTitleLength - 1) + "…";
        }

        private string GetSelectionTabTitle()
        {
            int selectionCount = Presenter.Selection.Count;
            if (selectionCount == 0)
                return "Selection";
            if (selectionCount > 1)
                return TruncateTabTitle($"{selectionCount} Objects");

            var selected = Presenter.Selection[0];
            var actor = selected as Actor;
            if (actor != null && !string.IsNullOrEmpty(actor.Name))
                return TruncateTabTitle(actor.Name);
            var asset = selected as Asset;
            if (asset != null && !string.IsNullOrEmpty(asset.Path))
                return TruncateTabTitle(System.IO.Path.GetFileNameWithoutExtension(asset.Path));
            return TruncateTabTitle(selected?.GetType().Name ?? "Selection");
        }

        private void UpdateSelectionTabTitle()
        {
            _selectionTab.Text = GetSelectionTabTitle();
        }

        private void UpdateTabsBarVisibility()
        {
            UpdateSelectionTabTitle();
            bool visible = _pinnedTabs.Count != 0;
            _tabs.TabsPanel.Visible = visible;
            _tabs.TabsSize = new Float2(_tabs.TabsSize.X, visible ? _tabsBarHeight : 0.0f);
        }

        /// <summary>
        /// Gets whether the current selection can be pinned safely.
        /// </summary>
        public bool CanPinSelection()
        {
            return Presenter.Selection.Count != 0 && _contentAssetState == null;
        }

        /// <summary>
        /// Pins the current selection in a separate properties tab.
        /// </summary>
        public void PinSelection()
        {
            if (!CanPinSelection())
                return;

            var selection = Presenter.Selection.ToArray();
            var pinnedUndoObjects = Presenter.GetUndoObjects?.Invoke(Presenter)?.ToArray() ?? Array.Empty<object>();
            var tab = new PropertiesTab(this, TruncateTabTitle(GetSelectionTabTitle()), true);
            var presenter = new CustomEditorPresenter(Editor.Undo)
            {
                GetUndoObjects = _ => pinnedUndoObjects,
            };
            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = tab,
            };
            presenter.Panel.AnchorPreset = AnchorPresets.StretchAll;
            presenter.Panel.Offsets = Margin.Zero;
            presenter.Panel.Parent = panel;
            presenter.Select(selection);
            presenter.BuildLayout();

            _pinnedTabs.Add(new PinnedTab(tab, panel, presenter, selection));
            _tabs.AddTab(tab);
            UpdateTabsBarVisibility();
            _tabs.SelectedTab = tab;
        }

        private static bool SelectionsMatch(IReadOnlyList<object> first, IReadOnlyList<object> second)
        {
            if (first.Count != second.Count)
                return false;

            var matched = new bool[second.Count];
            for (int i = 0; i < first.Count; i++)
            {
                bool found = false;
                for (int j = 0; j < second.Count; j++)
                {
                    if (!matched[j] && ReferenceEquals(first[i], second[j]))
                    {
                        matched[j] = true;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
            return true;
        }

        /// <summary>
        /// Gets whether the current selection has a pinned properties tab.
        /// </summary>
        public bool IsSelectionPinned()
        {
            return _pinnedTabs.Any(x => SelectionsMatch(x.Selection, Presenter.Selection));
        }

        /// <summary>
        /// Unpins the current selection properties tab.
        /// </summary>
        public void UnpinSelection()
        {
            var pinned = _pinnedTabs.FirstOrDefault(x => SelectionsMatch(x.Selection, Presenter.Selection));
            if (pinned != null)
                ClosePinnedTab((PropertiesTab)pinned.Tab);
        }

        private void ClosePinnedTab(PropertiesTab tab)
        {
            var pinned = _pinnedTabs.FirstOrDefault(x => x.Tab == tab);
            if (pinned == null)
                return;

            var selected = _tabs.SelectedTab;
            Tab fallback = null;
            if (selected == tab)
            {
                int index = _tabs.Children.IndexOf(tab);
                if (index > 1)
                    fallback = _tabs.Children[index - 1] as Tab;
                if (fallback == null && index + 1 < _tabs.Children.Count)
                    fallback = _tabs.Children[index + 1] as Tab;
                fallback ??= _selectionTab;
            }

            _pinnedTabs.Remove(pinned);
            _tabs.RemoveChild(tab);
            tab.Dispose();
            UpdateTabsBarVisibility();

            _tabs.SelectedTab = selected == tab ? fallback ?? _selectionTab : selected;
        }


        private void RefreshSelection()
        {
            if (_showContentSelection)
                SelectContentObjects();
            else
                SelectSceneObjects();
        }

        private void OnSceneSelectionChanged()
        {
            if (LockSelection)
                return;

            _showContentSelection = false;
            SelectSceneObjects();
        }

        private void SelectSceneObjects()
        {
            _waitingForContentAssets.Clear();
            ClearContentAssetState();
            Presenter.OverrideEditor = null;

            // Update selected objects
            // TODO: use cached collection for less memory allocations
            undoRecordObjects = Editor.SceneEditing.Selection.ConvertAll(x => x.UndoRecordObject).Distinct();
            var objects = Editor.SceneEditing.Selection.ConvertAll(x => x.EditableObject).Distinct();
            Presenter.Select(objects);
            UpdateSelectionTabTitle();

            // Set scroll value of window if it exists
            if (Editor.SceneEditing.SelectionCount == 1 && _scrollingPanel.VScrollBar != null)
                _scrollingPanel.VScrollBar.TargetValue = _actorScrollValues.GetValueOrDefault(Editor.SceneEditing.Selection[0].ID, 0);
        }

        private void OnContentSelectionChanged()
        {
            if (LockSelection)
                return;

            var selection = Editor.Windows.ContentWin.Selection;
            if (!_showContentSelection && !HasAssetSelection(selection))
                return;

            _showContentSelection = true;
            SelectContentObjects();
        }

        private static bool HasAssetSelection(IReadOnlyList<ContentItem> selection)
        {
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is AssetItem)
                    return true;
            }
            return false;
        }

        private void SelectContentObjects(bool forceRebuild = false)
        {
            _waitingForContentAssets.Clear();
            ClearContentAssetState();

            var objects = new List<object>();
            var selection = Editor.Windows.ContentWin.Selection;
            int assetItemsCount = 0;
            AssetItem singleAssetItem = null;
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is AssetItem assetItem)
                {
                    assetItemsCount++;
                    singleAssetItem = assetItem;
                    if (assetItemsCount > 1)
                        break;
                }
            }

            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is not AssetItem assetItem)
                    continue;

                var asset = assetItem.LoadAsync();
                if (asset == null)
                    continue;

                if (!asset.IsLoaded && !asset.LastLoadFailed)
                    _waitingForContentAssets.Add(asset);

                if (assetItemsCount == 1 && assetItem == singleAssetItem)
                {
                    var contentObject = GetContentAssetObject(asset, out _contentAssetState);
                    objects.Add(contentObject);
                }
                else if (asset is JsonAsset jsonAsset && jsonAsset.IsLoaded)
                {
                    var instance = jsonAsset.Instance;
                    objects.Add(instance ?? asset);
                }
                else
                {
                    objects.Add(asset);
                }
            }

            undoRecordObjects = objects;
            Presenter.OverrideEditor = objects.Count != 0 && objects.All(x => x is Asset) ? _contentAssetEditor : null;
            Presenter.Select(objects);
            UpdateSelectionTabTitle();
            if (forceRebuild)
                Presenter.BuildLayout();
        }

        private object GetContentAssetObject(Asset asset, out IDisposable state)
        {
            state = null;

            if (asset.IsLoaded)
            {
                if (asset is Prefab prefab)
                {
                    var prefabState = new PrefabContentAssetState(prefab);
                    if (prefabState.Instance)
                    {
                        state = prefabState;
                        return prefabState.Instance;
                    }
                    prefabState.Dispose();
                }
                if (asset is MaterialBase material)
                {
                    return new MaterialAssetPropertiesProxy(material);
                }
                if (asset is ParticleSystem particleSystem)
                {
                    var particleState = new ParticleAssetPropertiesProxy(particleSystem);
                    state = particleState;
                    return particleState;
                }
                if (asset is ParticleEmitter particleEmitter)
                {
                    var particleState = new ParticleAssetPropertiesProxy(particleEmitter);
                    state = particleState;
                    return particleState;
                }
                if (asset is JsonAsset jsonAsset)
                {
                    var instance = jsonAsset.Instance;
                    return instance ?? asset;
                }
            }

            return asset;
        }

        private void ClearContentAssetState()
        {
            if (_contentAssetState == null)
                return;
            _contentAssetState.Dispose();
            _contentAssetState = null;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            if (_showContentSelection && _waitingForContentAssets.Count != 0)
            {
                for (int i = 0; i < _waitingForContentAssets.Count; i++)
                {
                    var asset = _waitingForContentAssets[i];
                    if (asset == null || asset.IsLoaded || asset.LastLoadFailed)
                    {
                        SelectContentObjects(true);
                        break;
                    }
                }
            }

            if (_showContentSelection && _contentAssetState is ParticleAssetPropertiesProxy particleState)
                particleState.UpdateDeferredSave(deltaTime, Root?.GetMouseButton(MouseButton.Left) ?? false);

            base.Update(deltaTime);
        }

        private void OnPresenterAfterLayout(LayoutElementsContainer layout)
        {
            ApplySearchFilter();
        }

        private void OnPresenterModified()
        {
            if (!_showContentSelection || _isApplyingContentAssetChanges)
                return;

            if (_contentAssetState is PrefabContentAssetState prefabState)
            {
                _isApplyingContentAssetChanges = true;
                try
                {
                    prefabState.Apply(Editor);
                }
                finally
                {
                    _isApplyingContentAssetChanges = false;
                }
            }
        }

        private void ApplySearchFilter()
        {
            Presenter.ApplySearchFilter(_searchBox.Text);
        }

        /// <inheritdoc />
        public override void OnLayoutSerialize(XmlWriter writer)
        {
            writer.WriteAttributeString("ScaleLinked", ScaleLinked.ToString());
            writer.WriteAttributeString("UIPivotRelative", UIPivotRelative.ToString());
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            if (bool.TryParse(node.GetAttribute("ScaleLinked"), out bool value1))
                ScaleLinked = value1;
            if (bool.TryParse(node.GetAttribute("UIPivotRelative"), out value1))
                UIPivotRelative = value1;
        }

        /// <inheritdoc />
        public EditorViewport PresenterViewport => Editor.Windows.EditWin.Viewport;

        /// <inheritdoc />
        public void Select(List<SceneGraphNode> nodes)
        {
            Editor.SceneEditing.Select(nodes);
        }

        [CustomEditor(typeof(MaterialAssetPropertiesEditor))]
        private sealed class MaterialAssetPropertiesProxy
        {
            [HideInEditor]
            public MaterialBase Material { get; }

            [HideInEditor]
            public bool IsMaterial => Material is Material;

            [HideInEditor]
            public bool IsMaterialInstance => Material is MaterialInstance;

            [EditorOrder(10), EditorDisplay("General"), VisibleIf(nameof(IsMaterial))]
            public MaterialDomain Domain => Material != null ? Material.Info.Domain : MaterialDomain.Surface;

            [EditorOrder(20), EditorDisplay("General"), VisibleIf(nameof(IsMaterial))]
            public MaterialShadingModel ShadingModel => Material != null ? Material.Info.ShadingModel : MaterialShadingModel.Lit;

            [EditorOrder(30), EditorDisplay("General"), VisibleIf(nameof(IsMaterial))]
            public MaterialBlendMode BlendMode => Material != null ? Material.Info.BlendMode : MaterialBlendMode.Opaque;

            [EditorOrder(10), EditorDisplay("General"), VisibleIf(nameof(IsMaterialInstance)), Tooltip("The base material used to override it's properties")]
            public MaterialBase BaseMaterial
            {
                get => Material is MaterialInstance instance ? instance.BaseMaterial : null;
                set
                {
                    if (Material is not MaterialInstance instance || value == instance)
                        return;
                    instance.BaseMaterial = value;
                    Save();
                    Editor.Instance.Windows.PropertiesWin.Presenter.BuildLayoutOnUpdate();
                }
            }

            public MaterialAssetPropertiesProxy(MaterialBase material)
            {
                Material = material;
            }

            public void Save()
            {
                if (Material != null && Material.IsLoaded && Material.Save())
                    Editor.LogError("Cannot save asset.");
            }
        }

        private sealed class MaterialAssetPropertiesEditor : GenericEditor
        {
            public override void Initialize(LayoutElementsContainer layout)
            {
                var proxy = (MaterialAssetPropertiesProxy)Values[0];
                var material = proxy.Material;
                if (material == null)
                {
                    layout.Label("No material", TextAlignment.Center);
                    return;
                }
                if (!material.IsLoaded)
                {
                    layout.Label("Loading...", TextAlignment.Center);
                    return;
                }

                base.Initialize(layout);

                var parameters = material.Parameters;
                var parametersGroup = SurfaceUtils.InitGraphParametersGroup(layout);
                if (parameters == null || parameters.Length == 0)
                {
                    parametersGroup.Label("No parameters", TextAlignment.Center);
                    return;
                }

                var sourceMaterial = GetSourceMaterial(material);
                var data = SurfaceUtils.InitGraphParameters(parameters, sourceMaterial);
                var materialInstance = material as MaterialInstance;
                var baseMaterial = materialInstance != null ? materialInstance.BaseMaterial : null;
                SurfaceUtils.DisplayGraphParameters(parametersGroup, data,
                                                    MaterialParameterGet,
                                                    MaterialParameterSet,
                                                    Values,
                                                    null,
                                                    materialInstance != null ? (LayoutElementsContainer itemLayout, ValueContainer valueContainer, ref SurfaceUtils.GraphParameterData e) =>
                                                    {
                                                        var parameter = (MaterialParameter)e.Tag;
                                                        var baseParameter = baseMaterial != null ? baseMaterial.GetParameter(parameter.Name) : null;
                                                        if (baseParameter != null && baseParameter.ParameterType == parameter.ParameterType)
                                                            valueContainer.SetDefaultValue(baseParameter.Value);

                                                        var label = new CheckablePropertyNameLabel(e.DisplayName);
                                                        label.CheckBox.Checked = parameter.IsOverride;
                                                        label.CheckBox.Tag = parameter;
                                                        label.CheckChanged += nameLabel =>
                                                        {
                                                            var materialParameter = (MaterialParameter)nameLabel.CheckBox.Tag;
                                                            materialParameter.IsOverride = nameLabel.CheckBox.Checked;
                                                            proxy.Save();
                                                            nameLabel.UpdateStyle();
                                                        };
                                                        itemLayout.Property(label, valueContainer, null, e.Tooltip?.Text);
                                                        label.UpdateStyle();
                                                    } : null);
            }

            private static Material GetSourceMaterial(MaterialBase material)
            {
                while (material is MaterialInstance instance)
                    material = instance.BaseMaterial;
                return material as Material;
            }

            private static object MaterialParameterGet(object instance, GraphParameter parameter, object tag)
            {
                return ((MaterialParameter)tag).Value;
            }

            private static void MaterialParameterSet(object instance, object value, GraphParameter parameter, object tag)
            {
                var proxy = (MaterialAssetPropertiesProxy)instance;
                ((MaterialParameter)tag).Value = value;
                proxy.Save();
            }
        }

        [CustomEditor(typeof(ParticleAssetPropertiesEditor))]
        private sealed class ParticleAssetPropertiesProxy : IDisposable
        {
            private readonly ParticleSystem _particleSystem;
            private readonly ParticleEmitter _particleEmitter;
            private readonly ParticleEmitterSurfaceOwner _surfaceOwner;
            private ParticleSystemPreview _preview;
            private ParticleSystemTimeline _timeline;
            private ParticleEmitterSurface _emitterSurface;
            private bool _hasPendingParticleSystemSave;
            private float _pendingParticleSystemSaveDelay;

            private const float ParticleSystemSaveDelay = 0.15f;

            public ParticleEffect Effect => _preview?.PreviewActor;

            public ParticleEmitterSurface EmitterSurface => _emitterSurface;

            public bool IsEmitterAsset { get; }

            public ParticleAssetPropertiesProxy(ParticleSystem particleSystem)
            {
                _particleSystem = particleSystem;
                _preview = new ParticleSystemPreview(false)
                {
                    System = particleSystem,
                };
                _timeline = new ParticleSystemTimeline(_preview);
                _timeline.Load(particleSystem);
            }

            public ParticleAssetPropertiesProxy(ParticleEmitter particleEmitter)
            {
                _particleEmitter = particleEmitter;
                IsEmitterAsset = true;
                _surfaceOwner = new ParticleEmitterSurfaceOwner(particleEmitter);
                _emitterSurface = new ParticleEmitterSurface(_surfaceOwner, SaveEmitterSurface, null);
                if (_emitterSurface.Load())
                    Editor.LogError("Failed to load Particle Emitter surface.");
            }

            public void SaveParticleSystemParameter(ParticleEffectParameter effectParameter, GraphParameter parameter, object value)
            {
                if (!_particleSystem || _timeline == null || !effectParameter || !parameter)
                    return;

                var track = _timeline.FindTrack(effectParameter.TrackName) as ParticleEmitterTrack;
                if (track == null)
                    return;

                Effect.SetParameterValue(effectParameter.TrackName, parameter.Name, value);
                track.ParametersOverrides[parameter.Identifier] = value;
                _timeline.OnEmittersParametersOverridesEdited();
                _timeline.MarkAsEdited();
                _hasPendingParticleSystemSave = true;
                _pendingParticleSystemSaveDelay = ParticleSystemSaveDelay;
            }

            public void UpdateDeferredSave(float deltaTime, bool isDragging)
            {
                if (!_hasPendingParticleSystemSave)
                    return;

                if (isDragging)
                {
                    _pendingParticleSystemSaveDelay = ParticleSystemSaveDelay;
                    return;
                }

                _pendingParticleSystemSaveDelay -= deltaTime;
                if (_pendingParticleSystemSaveDelay <= 0.0f)
                    SavePendingParticleSystemChanges();
            }

            public void SavePendingParticleSystemChanges(bool rebuildLayout = true)
            {
                if (!_hasPendingParticleSystemSave || !_particleSystem || _timeline == null)
                    return;

                _hasPendingParticleSystemSave = false;
                _timeline.Save(_particleSystem);
                _particleSystem.WaitForLoaded();
                if (rebuildLayout)
                    Editor.Instance.Windows.PropertiesWin.Presenter.BuildLayoutOnUpdate();
            }

            public void SaveEmitterSurface()
            {
                if (_particleEmitter && _emitterSurface != null && _emitterSurface.Save())
                    Editor.LogError("Failed to save Particle Emitter surface.");
            }

            public void Dispose()
            {
                SavePendingParticleSystemChanges(false);
                _timeline?.Dispose();
                _timeline = null;
                _emitterSurface?.Dispose();
                _emitterSurface = null;
                _preview?.Dispose();
                _preview = null;
            }

            private sealed class ParticleEmitterSurfaceOwner : IVisjectSurfaceOwner
            {
                private readonly ParticleEmitter _asset;

                public ParticleEmitterSurfaceOwner(ParticleEmitter asset)
                {
                    _asset = asset;
                }

                public Asset SurfaceAsset => _asset;
                public string SurfaceName => "Particle Emitter";
                public FlaxEditor.Undo Undo => null;
                public VisjectSurfaceContext ParentContext => null;

                public byte[] SurfaceData
                {
                    get => _asset.LoadSurface(true);
                    set
                    {
                        if (_asset.SaveSurface(value))
                        {
                            Editor.LogError("Failed to save Particle Emitter surface.");
                            return;
                        }
                        _asset.Reload();
                        _asset.WaitForLoaded();
                    }
                }

                public void OnContextCreated(VisjectSurfaceContext context)
                {
                }

                public void OnSurfaceEditedChanged()
                {
                }

                public void OnSurfaceGraphEdited()
                {
                }

                public void OnSurfaceClose()
                {
                }
            }
        }

        private sealed class ParticleAssetPropertiesEditor : GenericEditor
        {
            public override void Initialize(LayoutElementsContainer layout)
            {
                var proxy = (ParticleAssetPropertiesProxy)Values[0];
                var group = layout.Group("Parameters");
                group.Panel.Open();

                if (proxy.IsEmitterAsset)
                {
                    var surface = proxy.EmitterSurface;
                    if (surface == null)
                    {
                        group.Label("Loading...", TextAlignment.Center);
                        return;
                    }

                    var surfaceParameters = surface.Parameters.Where(x => x.IsPublic).ToArray();
                    if (surfaceParameters.Length == 0)
                    {
                        group.Label("No parameters", TextAlignment.Center);
                        return;
                    }

                    var data = InitSurfaceParameters(surfaceParameters);
                    SurfaceUtils.DisplayGraphParameters(group, data, SurfaceParameterGet, SurfaceParameterSet, Values);
                    return;
                }

                var effect = proxy.Effect;
                if (!effect || !effect.ParticleSystem || !effect.ParticleSystem.IsLoaded)
                {
                    group.Label("Loading...", TextAlignment.Center);
                    return;
                }

                var parameters = effect.Parameters.Where(x => x != null && x.IsPublic).ToArray();
                if (parameters.Length == 0)
                {
                    group.Label("No parameters", TextAlignment.Center);
                    return;
                }

                foreach (var parametersGroup in parameters.GroupBy(x => x.EmitterIndex))
                {
                    var trackName = parametersGroup.First().TrackName;
                    var trackGroup = group.Group(string.IsNullOrEmpty(trackName) ? "Emitter" : trackName);
                    trackGroup.Panel.Open();
                    DisplayParticleParameters(trackGroup, parametersGroup, Values);
                }
            }

            private static SurfaceUtils.GraphParameterData[] InitSurfaceParameters(IReadOnlyList<SurfaceParameter> parameters)
            {
                var data = new SurfaceUtils.GraphParameterData[parameters.Count];
                for (int i = 0; i < parameters.Count; i++)
                {
                    var parameter = parameters[i];
                    var attributes = parameter.Meta.GetAttributes() ?? FlaxEngine.Utils.GetEmptyArray<Attribute>();
                    data[i] = new SurfaceUtils.GraphParameterData(null, i, parameter.Name, parameter.IsPublic, parameter.Type.Type, attributes, parameter);
                }
                Array.Sort(data, SurfaceUtils.GraphParameterData.Compare);
                return data;
            }

            private static void DisplayParticleParameters(LayoutElementsContainer layout, IEnumerable<ParticleEffectParameter> parameters, ValueContainer values)
            {
                var data = SurfaceUtils.InitGraphParameters(parameters);
                SurfaceUtils.DisplayGraphParameters(layout, data, ParticleParameterGet, ParticleParameterSet, values, ParticleParameterDefaultValue);
            }

            private static object ParticleParameterGet(object instance, GraphParameter parameter, object tag)
            {
                var proxy = (ParticleAssetPropertiesProxy)instance;
                var effectParameter = (ParticleEffectParameter)tag;
                return proxy.Effect.GetParameterValue(effectParameter.TrackName, parameter.Name);
            }

            private static void ParticleParameterSet(object instance, object value, GraphParameter parameter, object tag)
            {
                var proxy = (ParticleAssetPropertiesProxy)instance;
                var effectParameter = (ParticleEffectParameter)tag;
                proxy.SaveParticleSystemParameter(effectParameter, parameter, value);
            }

            private static object ParticleParameterDefaultValue(object instance, GraphParameter parameter, object tag)
            {
                return ((ParticleEffectParameter)tag).DefaultValue;
            }

            private static object SurfaceParameterGet(object instance, GraphParameter parameter, object tag)
            {
                return ((SurfaceParameter)tag).Value;
            }

            private static void SurfaceParameterSet(object instance, object value, GraphParameter parameter, object tag)
            {
                var proxy = (ParticleAssetPropertiesProxy)instance;
                ((SurfaceParameter)tag).Value = value;
                proxy.SaveEmitterSurface();
            }
        }

        private sealed class PrefabContentAssetState : IDisposable
        {
            public Actor Instance { get; private set; }

            public PrefabContentAssetState(Prefab prefab)
            {
                Instance = PrefabManager.SpawnPrefab(prefab, null);
            }

            public void Apply(Editor editor)
            {
                if (Instance)
                    editor.Prefabs.ApplyAll(Instance);
            }

            public void Dispose()
            {
                if (!Instance)
                    return;
                var instance = Instance;
                Instance = null;
                FlaxEngine.Object.Destroy(instance);
            }
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            Editor.SceneEditing.SelectionChanged -= OnSceneSelectionChanged;
            if (Editor.Windows.ContentWin != null)
                Editor.Windows.ContentWin.SelectionChanged -= OnContentSelectionChanged;
            Presenter.Modified -= OnPresenterModified;
            ClearContentAssetState();

            base.OnDestroy();
        }
    }
}
