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
using FlaxEditor.GUI.ContextMenu;
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
        private bool _discardContentAssetChanges;
        private readonly List<PinnedTab> _pinnedTabs = new List<PinnedTab>();
        private readonly ContentAssetEditor _contentAssetEditor = new ContentAssetEditor();
        private IDisposable _contentAssetState;
        private bool _lockObjects = false;
        private bool _showContentSelection;
        private bool _isApplyingContentAssetChanges;
        private SearchBox _searchBox;
        private ContainerControl _filtersPanel;
        private ContainerControl _groupFilterPanel;
        private Panel _scrollingPanel;
        private Tabs _tabs;
        private Tab _selectionTab;
        private float _tabsBarHeight;
        private string _selectedGroupFilter = string.Empty;
        private readonly List<GroupFilterButton> _groupFilterButtons = new List<GroupFilterButton>();

        private const int MaxTabTitleLength = 24;
        private const float FilterHorizontalPadding = 6.0f;
        private const float FilterTopPadding = 4.0f;
        private const float FilterBottomPadding = 5.0f;
        private const float FilterVerticalSpacing = 4.0f;
        private const float GroupFilterButtonHeight = 22.0f;
        private const float GroupFilterButtonSpacing = 4.0f;
        private const float GroupFilterButtonHorizontalPadding = 18.0f;
        private const float GroupFilterButtonMinWidth = 34.0f;
        private const float GroupFilterButtonMaxWidth = 180.0f;
        private const float TabCloseButtonSize = 14.0f;
        private const float TabCloseButtonHitSize = 18.0f;
        private const float TabCloseButtonMargin = 5.0f;
        private const float TabHorizontalPadding = 10.0f;
        private const float TabTextCloseGap = 4.0f;
        private const float TabMinWidth = 86.0f;
        private const float TabMaxWidth = 168.0f;
        private const float TabSelectedLineHeight = 2.0f;
        private const float PropertiesScrollbarWidthReduction = 4.0f;
        private const int TextPreviewMaxCharacters = 1024 * 1024;
        private const int TextFileDetectionSampleSize = 4096;

        private sealed class ContentAssetEditor : ScriptingObjectEditor
        {
            public override void Initialize(LayoutElementsContainer layout)
            {
                if (IsSingleObject && Values[0] is Asset asset && asset.LastLoadFailed &&
                    Editor.Instance.ContentEditing.TryGetBinaryAssetStorageId(asset.Path, out var storageId) &&
                    storageId != asset.ID)
                {
                    var registeredId = asset.ID;
                    var group = layout.Group("Asset ID Mismatch");
                    group.Panel.Open();
                    group.Label("The asset cannot load because its file ID differs from the registered ID.").Label.TextColor = Color.Red;
                    group.Label("Registered ID", registeredId.ToString("N"));
                    group.Label("File ID", storageId.ToString("N"));
                    group.Button("Fix Asset ID", "Rewrites the file ID to the registered ID so existing references remain valid.").Button.Clicked += () => RepairAssetId(asset, storageId, registeredId);
                    return;
                }

                base.Initialize(layout);
            }

            private static void RepairAssetId(Asset asset, Guid storageId, Guid registeredId)
            {
                var result = MessageBox.Show(
                    $"Rewrite the file ID from {storageId:N} to {registeredId:N}? Existing references to {registeredId:N} will remain valid.",
                    "Fix Asset ID", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (result != DialogResult.Yes)
                    return;

                if (!Editor.Instance.ContentEditing.RepairBinaryAssetStorageId(asset.Path, storageId, registeredId))
                {
                    MessageBox.Show("Failed to repair the asset ID. See the output log for details.", "Fix Asset ID", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                Editor.Log($"Repaired asset ID mismatch in '{asset.Path}'.");
                asset.Reload();
            }
        }

        [CustomEditor(typeof(TextFilePropertiesEditor))]
        private sealed class TextFilePropertiesProxy
        {
            public readonly ContentItem Item;
            public readonly string Text;

            public TextFilePropertiesProxy(ContentItem item, string text)
            {
                Item = item;
                Text = text;
            }
        }

        private sealed class TextFilePropertiesEditor : CustomEditor
        {
            public override DisplayStyle Style => DisplayStyle.InlineIntoParent;

            public override void Initialize(LayoutElementsContainer layout)
            {
                if (!IsSingleObject || Values[0] is not TextFilePropertiesProxy proxy)
                {
                    layout.Label("Multiple text files selected.");
                    return;
                }

                var textBox = layout.TextBox(true).TextBox;
                textBox.Text = proxy.Text;
                textBox.Height = TextBox.DefaultHeight * 16.0f;
                textBox.IsReadOnly = true;
                textBox.IsScrollable = true;
            }
        }

        private static float PropertiesPanelPadding => Mathf.Max(4.0f, Style.Current.GetPropertyPanelPadding());

        private static void ApplyPropertiesPanelStyle(CustomEditorPresenter presenter)
        {
            var padding = PropertiesPanelPadding;
            presenter.Panel.Margin = new Margin(padding);
            presenter.Panel.Spacing = padding;
        }

        private static float GetPropertiesTabHeaderWidth(PropertiesTab tab)
        {
            var style = Style.Current;
            var textWidth = style.FontMedium ? style.FontMedium.MeasureText(tab.Text ?? string.Empty).X : 0.0f;
            var closeWidth = tab.Closeable ? TabTextCloseGap + TabCloseButtonHitSize + TabCloseButtonMargin : 0.0f;
            return Mathf.Clamp(textWidth + TabHorizontalPadding * 2.0f + closeWidth, TabMinWidth, TabMaxWidth);
        }

        private static Color GetPinnedTabsBackgroundColor()
        {
            var style = Style.Current;
            var color = style.Background;
            var hsv = color.ToHSV();
            hsv.Z = Mathf.Saturate(hsv.Z - 0.03f);
            return Color.FromHSV(hsv, color.A);
        }

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
                UpdateSize(tabs.TabsSize.Y);
            }

            private Rectangle CloseButtonBounds => new Rectangle(Size.X - TabCloseButtonHitSize - TabCloseButtonMargin, (Size.Y - TabCloseButtonHitSize) * 0.5f, TabCloseButtonHitSize, TabCloseButtonHitSize);

            private Rectangle CloseIconBounds
            {
                get
                {
                    var bounds = CloseButtonBounds;
                    return new Rectangle(bounds.X + (bounds.Width - TabCloseButtonSize) * 0.5f, bounds.Y + (bounds.Height - TabCloseButtonSize) * 0.5f, TabCloseButtonSize, TabCloseButtonSize);
                }
            }

            public void UpdateSize(float height)
            {
                Size = new Float2(GetPropertiesTabHeaderWidth(PropertiesTab), height);
            }

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
                if (headerIndex < 0 || !_closeable)
                    return;

                float pointerX = Location.X + location.X;
                int direction = pointerX < Location.X ? -1 : pointerX > Location.X + Width ? 1 : 0;
                if (direction == 0)
                    return;

                var targetHeader = GetReorderTargetHeader(headerIndex, direction);
                if (targetHeader == null)
                    return;

                var tabIndex = _owner._tabs.Children.IndexOf(PropertiesTab);
                var targetTabIndex = _owner._tabs.Children.IndexOf(targetHeader.PropertiesTab);
                var targetHeaderIndex = _owner._tabs.TabsPanel.Children.IndexOf(targetHeader);
                if (tabIndex < 0 || targetTabIndex < 0 || targetHeaderIndex < 0)
                    return;

                var tabInsertIndex = direction > 0 ? targetTabIndex + 1 : targetTabIndex;
                var headerInsertIndex = direction > 0 ? targetHeaderIndex + 1 : targetHeaderIndex;
                var selectedTab = _owner._tabs.SelectedTab;
                var tab = _owner._tabs.Children[tabIndex];
                var headerControl = _owner._tabs.TabsPanel.Children[headerIndex];

                _owner._tabs.Children.RemoveAt(tabIndex);
                if (tabInsertIndex > tabIndex)
                    tabInsertIndex--;
                _owner._tabs.Children.Insert(tabInsertIndex, tab);

                _owner._tabs.TabsPanel.Children.RemoveAt(headerIndex);
                if (headerInsertIndex > headerIndex)
                    headerInsertIndex--;
                _owner._tabs.TabsPanel.Children.Insert(headerInsertIndex, headerControl);

                _owner._tabs.PerformLayout();
                _owner._tabs.TabsPanel.PerformLayout();
                _owner._tabs.SelectedTab = selectedTab;
            }

            private PropertiesTabHeader GetReorderTargetHeader(int headerIndex, int direction)
            {
                var children = _owner._tabs.TabsPanel.Children;
                for (int i = headerIndex + direction; i >= 0 && i < children.Count; i += direction)
                {
                    if (children[i] is PropertiesTabHeader header)
                        return header._closeable ? header : null;
                }
                return null;
            }

            public override void Draw()
            {
                var style = Style.Current;
                var enabled = EnabledInHierarchy && Tab.EnabledInHierarchy;
                var isSelected = _owner._tabs.SelectedTab == Tab;
                var isMouseOver = IsMouseOver && enabled;
                var tabRect = new Rectangle(Float2.Zero, Size);

                if (isSelected)
                {
                    var cornerRadius = style.GetTabCornerRadius();
                    if (cornerRadius > 0.0f)
                        StyleRendering.FillRoundedRectangle(tabRect, style.BorderSelected, cornerRadius, RoundedCorners.Top);
                    else
                        Render2D.FillRectangle(tabRect, style.BorderSelected);
                }
                else if (isMouseOver)
                {
                    StyleRendering.FillRoundedRectangle(tabRect, style.BackgroundHighlighted.AlphaMultiplied(0.82f), style.GetTabCornerRadius(), RoundedCorners.Top);
                }
                var closeWidth = _closeable ? TabTextCloseGap + TabCloseButtonHitSize + TabCloseButtonMargin : 0.0f;
                var textRect = new Rectangle(TabHorizontalPadding, 0.0f, Mathf.Max(0.0f, Width - TabHorizontalPadding - closeWidth), Height);
                var textColor = !enabled ? style.ForegroundDisabled : isSelected ? Color.White : isMouseOver ? style.Foreground : style.ForegroundGrey;
                Render2D.PushClip(ref textRect);
                Render2D.DrawText(style.FontMedium, Tab.Text, textRect, textColor, TextAlignment.Near, TextAlignment.Center);
                Render2D.PopClip();

                if (_closeable)
                {
                    var bounds = CloseButtonBounds;
                    var mousePosition = RootWindow != null ? PointFromWindow(RootWindow.MousePosition) : Float2.Minimum;
                    bool closeMouseOver = isMouseOver && bounds.Contains(mousePosition);
                    if (closeMouseOver)
                        StyleRendering.FillRoundedRectangle(bounds, style.BackgroundHighlighted * 1.2f, style.GetButtonCornerRadius());
                    Render2D.DrawSprite(style.Cross, CloseIconBounds, closeMouseOver ? style.Foreground : textColor.AlphaMultiplied(isSelected ? 1.0f : 0.75f));
                }
            }
        }

        private sealed class FiltersPanel : ContainerControl
        {
            private readonly PropertiesWindow _owner;

            public FiltersPanel(PropertiesWindow owner)
            {
                _owner = owner;
                ClipChildren = false;
                CullChildren = false;
            }

            protected override void PerformLayoutBeforeChildren()
            {
                _owner.LayoutFilterControls();
                base.PerformLayoutBeforeChildren();
            }
        }

        private sealed class GroupFilterButton : Button
        {
            public readonly string GroupName;

            public GroupFilterButton(string text, string groupName)
            {
                Text = text;
                GroupName = groupName;
                Height = GroupFilterButtonHeight;
                HorizontalAlignment = TextAlignment.Center;
                VerticalAlignment = TextAlignment.Center;
                Margin = new Margin(7.0f, 7.0f, 0.0f, 0.0f);
                ClipText = true;
                HasBorder = false;
                CornerRadius = 3.0f;
                TooltipText = string.IsNullOrEmpty(groupName) ? "Show all property groups." : $"Show only {groupName} properties.";
            }
        }

        private sealed class PinnedTab
        {
            public readonly Tab Tab;
            public readonly Panel Panel;
            public readonly CustomEditorPresenter Presenter;
            public readonly object[] Selection;
            public readonly Guid[] ContentAssetIds;

            public PinnedTab(Tab tab, Panel panel, CustomEditorPresenter presenter, object[] selection, Guid[] contentAssetIds)
            {
                Tab = tab;
                Panel = panel;
                Presenter = presenter;
                Selection = selection;
                ContentAssetIds = contentAssetIds;
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
            var controlHeight = Style.Current.ControlHeight > 0.0f ? Style.Current.ControlHeight : 18.0f;

            _tabs = new Tabs
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                AutoTabsSize = false,
                UseScroll = true,
                Parent = this,
            };
            _tabs.TabStripColor = GetPinnedTabsBackgroundColor();
            _tabsBarHeight = _tabs.TabsSize.Y;
            _selectionTab = _tabs.AddTab(new PropertiesTab(this, "Selection", false));

            _filtersPanel = new FiltersPanel(this)
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Parent = _selectionTab,
                Offsets = new Margin(0.0f, 0.0f, 0.0f, controlHeight + FilterTopPadding + FilterBottomPadding),
            };

            _searchBox = new SearchBox
            {
                Parent = _filtersPanel,
                Bounds = new Rectangle(FilterHorizontalPadding, FilterTopPadding, Width - FilterHorizontalPadding * 2.0f, controlHeight),
                TooltipText = "Search properties.",
            };
            _searchBox.TextChanged += ApplySearchFilter;

            _groupFilterPanel = new ContainerControl
            {
                Parent = _filtersPanel,
                Visible = false,
                ClipChildren = false,
                CullChildren = false,
            };

            _scrollingPanel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0.0f, 0.0f, controlHeight + FilterTopPadding + FilterBottomPadding, 0.0f),
                Parent = _selectionTab,
                ScrollBarsSize = Mathf.Max(1.0f, ScrollBar.DefaultSize - PropertiesScrollbarWidthReduction),
            };

            Presenter = new CustomEditorPresenter(editor.Undo, null, this);
            Presenter.Panel.Parent = _scrollingPanel;
            Presenter.GetUndoObjects += GetUndoObjects;
            Presenter.Features |= FeatureFlags.CacheExpandedGroups;
            Presenter.AfterLayout += OnPresenterAfterLayout;
            Presenter.Modified += OnPresenterModified;
            ApplyPropertiesPanelStyle(Presenter);
            InputActions.Bindings.Insert(0, new FlaxEditor.Options.InputActionsContainer.Binding(
                options => options.Save,
                SaveContentSelection));

            _scrollingPanel.VScrollBar.ValueChanged += OnScrollValueChanged;
            Editor.SceneEditing.SelectionChanged += OnSceneSelectionChanged;
            Editor.Windows.ContentWin.SelectionChanged += OnContentSelectionChanged;
            Editor.ContentDatabase.ItemRemoved += OnContentItemRemoved;
            FlaxEngine.Content.AssetReloading += OnAssetReloading;
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
            if (selected is TextFilePropertiesProxy textFile)
                return TruncateTabTitle(textFile.Item.FileName);
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
            UpdatePropertiesTabHeaderSizes();
        }

        private void UpdateTabsBarVisibility()
        {
            UpdateSelectionTabTitle();
            bool visible = _pinnedTabs.Count != 0;
            _tabs.TabsPanel.Visible = visible;
            _tabs.TabStripColor = GetPinnedTabsBackgroundColor();
            _tabs.TabsSize = new Float2(TabMinWidth, visible ? _tabsBarHeight : 0.0f);
            UpdatePropertiesTabHeaderSizes();
        }

        private void UpdatePropertiesTabHeaderSizes()
        {
            if (_tabs == null)
                return;

            var headerHeight = _tabs.TabsSize.Y;
            for (int i = 0; i < _tabs.TabsPanel.ChildrenCount; i++)
            {
                if (_tabs.TabsPanel.Children[i] is PropertiesTabHeader header)
                    header.UpdateSize(headerHeight);
            }
            _tabs.TabsPanel.PerformLayout();
        }

        /// <summary>
        /// Gets whether the current selection can be pinned safely.
        /// </summary>
        public bool CanPinSelection()
        {
            return Presenter.Selection.Count != 0 && _contentAssetState == null;
        }

        internal bool TryGetInspectedPrefab(Actor instance, out Prefab prefab)
        {
            if (_showContentSelection &&
                _contentAssetState is PrefabContentAssetState state &&
                state.Instance == instance)
            {
                prefab = state.Asset;
                return prefab != null;
            }

            prefab = null;
            return false;
        }

        /// <summary>
        /// Pins the current selection in a separate properties tab.
        /// </summary>
        public void PinSelection()
        {
            if (!CanPinSelection())
                return;

            var selection = Presenter.Selection.ToArray();
            var contentAssetIds = _showContentSelection ? GetSelectedContentAssetIds() : Array.Empty<Guid>();
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
            ApplyPropertiesPanelStyle(presenter);
            presenter.Select(selection);
            presenter.BuildLayout();

            _pinnedTabs.Add(new PinnedTab(tab, panel, presenter, selection, contentAssetIds));
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

        private void ResetFilters()
        {
            if (!string.IsNullOrEmpty(_selectedGroupFilter))
            {
                _selectedGroupFilter = string.Empty;
                Presenter.ApplyGroupFilter(string.Empty);
                UpdateGroupFilterButtonStyles();
            }

            if (_searchBox != null && !string.IsNullOrEmpty(_searchBox.Text))
                _searchBox.Text = string.Empty;
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

        /// <inheritdoc />
        public override void OnShowContextMenu(ContextMenu menu)
        {
            base.OnShowContextMenu(menu);

            bool isPinned = IsSelectionPinned();
            var pin = menu.AddButton(isPinned ? "Unpin" : "Pin");
            pin.Enabled = isPinned || CanPinSelection();
            pin.ButtonClicked += button =>
            {
                if (isPinned)
                    UnpinSelection();
                else
                    PinSelection();
            };
            menu.AddSeparator();
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

        private void OnContentItemRemoved(ContentItem item)
        {
            if (item is not AssetItem assetItem)
                return;

            if (_showContentSelection && ContentSelectionContainsAsset(assetItem.ID))
            {
                _waitingForContentAssets.Clear();
                if (_contentAssetState != null)
                    Presenter.Deselect();
                ClearContentAssetState();
                Presenter.OverrideEditor = null;
                undoRecordObjects = Array.Empty<object>();
                UpdateSelectionTabTitle();
            }

            for (int i = _pinnedTabs.Count - 1; i >= 0; i--)
            {
                var pinned = _pinnedTabs[i];
                if (ContentAssetIdsContain(pinned.ContentAssetIds, assetItem.ID) || SelectionContainsContentAsset(pinned.Selection, assetItem.ID))
                    ClosePinnedTab((PropertiesTab)pinned.Tab);
            }
        }

        private void OnAssetReloading(Asset asset)
        {
            if (!_showContentSelection || asset == null || !ContentSelectionContainsAsset(asset.ID))
                return;
            if (Editor.ContentDatabase.IsAssetSaveInProgress(asset.Path))
                return;

            // Keep displaying the current properties until the replacement data is ready. Once loaded,
            // rebuild the selection from the asset and discard any deferred save owned by the stale proxy.
            _discardContentAssetChanges = true;
            _waitingForContentAssets.Clear();
            _waitingForContentAssets.Add(asset);
        }

        private Guid[] GetSelectedContentAssetIds()
        {
            var selection = Editor.Windows.ContentWin.Selection;
            if (selection.Count == 0)
                return Array.Empty<Guid>();

            var result = new List<Guid>(selection.Count);
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is AssetItem assetItem)
                    result.Add(assetItem.ID);
            }
            return result.Count != 0 ? result.ToArray() : Array.Empty<Guid>();
        }

        private bool ContentSelectionContainsAsset(Guid assetId)
        {
            var selection = Editor.Windows.ContentWin.Selection;
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is AssetItem assetItem && assetItem.ID == assetId)
                    return true;
            }
            return false;
        }

        private static bool ContentAssetIdsContain(IReadOnlyList<Guid> contentAssetIds, Guid assetId)
        {
            for (int i = 0; i < contentAssetIds.Count; i++)
            {
                if (contentAssetIds[i] == assetId)
                    return true;
            }
            return false;
        }

        private static bool SelectionContainsContentAsset(IReadOnlyList<object> selection, Guid assetId)
        {
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] is Asset asset && asset.ID == assetId)
                    return true;
                if (selection[i] is MaterialAssetPropertiesProxy materialProxy && materialProxy.Material != null && materialProxy.Material.ID == assetId)
                    return true;
            }
            return false;
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
            if (_contentAssetState != null)
                Presenter.Deselect();
            ClearContentAssetState();
            Presenter.OverrideEditor = null;

            // Update selected objects
            // TODO: use cached collection for less memory allocations
            undoRecordObjects = Editor.SceneEditing.Selection.ConvertAll(x => x.UndoRecordObject).Distinct();
            var objects = Editor.SceneEditing.Selection.ConvertAll(x => x.EditableObject).Distinct().ToArray();
            if (!SelectionsMatch(objects, Presenter.Selection))
                ResetFilters();
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
            if (!_showContentSelection && !HasInspectableContentSelection(selection))
                return;

            _showContentSelection = true;
            SelectContentObjects();
        }

        private static bool HasInspectableContentSelection(IReadOnlyList<ContentItem> selection)
        {
            for (int i = 0; i < selection.Count; i++)
            {
                var item = selection[i];
                if ((item is AssetItem or ScriptItem or ShaderSourceItem) || (item is FileItem && item is not VideoItem))
                    return true;
            }
            return false;
        }

        private void SelectContentObjects(bool forceRebuild = false)
        {
            _waitingForContentAssets.Clear();
            if (_contentAssetState != null)
                Presenter.Deselect();
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
                {
                    if (TryCreateTextFilePropertiesProxy(selection[i], out var textFile))
                        objects.Add(textFile);
                    continue;
                }

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
                    objects.Add(GetJsonAssetObject(jsonAsset));
                }
                else
                {
                    objects.Add(asset);
                }
            }

            undoRecordObjects = objects;
            Presenter.OverrideEditor = objects.Count != 0 && objects.All(x => x is Asset) ? _contentAssetEditor : null;
            if (!forceRebuild && !SelectionsMatch(objects, Presenter.Selection))
                ResetFilters();
            Presenter.Select(objects);
            UpdateSelectionTabTitle();
            if (forceRebuild)
                Presenter.BuildLayout();
        }

        private static bool TryCreateTextFilePropertiesProxy(ContentItem item, out TextFilePropertiesProxy proxy)
        {
            proxy = null;
            bool isText = item is ScriptItem or ShaderSourceItem;
            if (!isText && (item is not FileItem || item is VideoItem))
                return false;

            try
            {
                using var stream = System.IO.File.Open(item.Path, System.IO.FileMode.Open, System.IO.FileAccess.Read, System.IO.FileShare.ReadWrite | System.IO.FileShare.Delete);
                if (!isText)
                {
                    var sample = new byte[Math.Min(stream.Length, TextFileDetectionSampleSize)];
                    int sampleLength = stream.Read(sample, 0, sample.Length);
                    if (IsBinaryFileSample(sample, sampleLength))
                        return false;
                    isText = true;
                    stream.Position = 0;
                }

                using var reader = new System.IO.StreamReader(stream, System.Text.Encoding.UTF8, true, TextFileDetectionSampleSize);
                var buffer = new char[TextPreviewMaxCharacters + 1];
                int length = reader.ReadBlock(buffer, 0, buffer.Length);
                var text = new string(buffer, 0, Math.Min(length, TextPreviewMaxCharacters));
                if (length > TextPreviewMaxCharacters)
                    text += $"\r\n\r\n[Preview truncated to the first {TextPreviewMaxCharacters:N0} characters.]";
                proxy = new TextFilePropertiesProxy(item, text);
                return true;
            }
            catch (Exception ex)
            {
                if (!isText)
                    return false;
                proxy = new TextFilePropertiesProxy(item, $"Unable to read the file.\r\n\r\n{ex.Message}");
                return true;
            }
        }

        private static bool IsBinaryFileSample(byte[] data, int length)
        {
            if (length == 0 ||
                (length >= 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff))) ||
                (length >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) ||
                (length >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xfe && data[3] == 0xff))
                return false;

            int controlCharacters = 0;
            for (int i = 0; i < length; i++)
            {
                byte value = data[i];
                if (value == 0)
                    return true;
                if (value < 32 && value != '\t' && value != '\n' && value != '\r' && value != '\f')
                    controlCharacters++;
            }
            return controlCharacters * 20 > length;
        }

        private static object GetJsonAssetObject(JsonAsset jsonAsset)
        {
            if (jsonAsset is SceneAsset || string.Equals(jsonAsset.DataTypeName, Scene.AssetTypename, StringComparison.Ordinal))
                return jsonAsset;

            return jsonAsset.Instance ?? jsonAsset;
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
                    var materialState = new MaterialAssetPropertiesProxy(material);
                    state = materialState;
                    return materialState;
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
                    var jsonObject = GetJsonAssetObject(jsonAsset);
                    if (!ReferenceEquals(jsonObject, jsonAsset))
                        state = new JsonAssetContentAssetState(jsonAsset, jsonObject);
                    return jsonObject;
                }
            }

            return asset;
        }

        private void ClearContentAssetState()
        {
            if (_contentAssetState == null)
            {
                _discardContentAssetChanges = false;
                return;
            }
            if (_discardContentAssetChanges)
            {
                if (_contentAssetState is MaterialAssetPropertiesProxy materialState)
                    materialState.DiscardPendingChanges();
                else if (_contentAssetState is ParticleAssetPropertiesProxy particleState)
                    particleState.DiscardPendingChanges();
                else if (_contentAssetState is JsonAssetContentAssetState jsonAssetState)
                    jsonAssetState.DiscardPendingChanges();
                _discardContentAssetChanges = false;
            }
            _contentAssetState.Dispose();
            _contentAssetState = null;
        }

        private void SaveContentSelection()
        {
            if (_showContentSelection && _contentAssetState is JsonAssetContentAssetState jsonAssetState)
                jsonAssetState.SaveChanges();
            Editor.Instance.SaveAll();
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

            if (_showContentSelection && _contentAssetState is MaterialAssetPropertiesProxy materialState)
                materialState.UpdateDeferredSave(deltaTime, Root?.GetMouseButton(MouseButton.Left) ?? false);
            else if (_showContentSelection && _contentAssetState is ParticleAssetPropertiesProxy particleState)
                particleState.UpdateDeferredSave(deltaTime, Root?.GetMouseButton(MouseButton.Left) ?? false);
            else if (_showContentSelection && _contentAssetState is JsonAssetContentAssetState jsonAssetState)
                jsonAssetState.UpdateDeferredSave(deltaTime, Root?.GetMouseButton(MouseButton.Left) ?? false);

            base.Update(deltaTime);
        }

        private void OnPresenterAfterLayout(LayoutElementsContainer layout)
        {
            UpdateGroupFilterButtons();
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
            else if (_contentAssetState is JsonAssetContentAssetState jsonAssetState)
            {
                jsonAssetState.RequestSave();
            }
        }

        private void ApplySearchFilter()
        {
            Presenter.ApplySearchFilter(_searchBox.Text);
        }

        private void UpdateGroupFilterButtons()
        {
            if (_groupFilterPanel == null)
                return;

            var groupNames = new List<string>();
            Presenter.GetRootGroupNames(groupNames);

            if (groupNames.Count <= 1)
            {
                if (!string.IsNullOrEmpty(_selectedGroupFilter))
                {
                    _selectedGroupFilter = string.Empty;
                    Presenter.ApplyGroupFilter(string.Empty);
                }
                _groupFilterButtons.Clear();
                _groupFilterPanel.DisposeChildren();
                _groupFilterPanel.Visible = false;
                LayoutFilterControls();
                return;
            }

            if (!string.IsNullOrEmpty(_selectedGroupFilter) && !groupNames.Contains(_selectedGroupFilter, StringComparer.OrdinalIgnoreCase))
            {
                _selectedGroupFilter = string.Empty;
                Presenter.ApplyGroupFilter(string.Empty);
            }

            _groupFilterButtons.Clear();
            _groupFilterPanel.DisposeChildren();
            _groupFilterPanel.Visible = true;

            AddGroupFilterButton("All", string.Empty);
            foreach (var groupName in groupNames)
                AddGroupFilterButton(groupName, groupName);

            UpdateGroupFilterButtonStyles();
            LayoutFilterControls();
        }

        private void AddGroupFilterButton(string text, string groupName)
        {
            var filterName = groupName ?? string.Empty;
            var button = new GroupFilterButton(text, filterName)
            {
                Parent = _groupFilterPanel,
            };
            button.Clicked += () => SetGroupFilter(filterName);
            _groupFilterButtons.Add(button);
        }

        private void SetGroupFilter(string groupName)
        {
            groupName ??= string.Empty;
            if (string.Equals(_selectedGroupFilter, groupName, StringComparison.OrdinalIgnoreCase))
                return;

            _selectedGroupFilter = groupName;
            Presenter.ApplyGroupFilter(_selectedGroupFilter);
            UpdateGroupFilterButtonStyles();
            LayoutFilterControls();
        }

        private void UpdateGroupFilterButtonStyles()
        {
            var style = Style.Current;
            foreach (var button in _groupFilterButtons)
            {
                bool selected = string.Equals(button.GroupName, _selectedGroupFilter, StringComparison.OrdinalIgnoreCase);
                if (selected)
                {
                    button.BackgroundColor = style.BorderSelected;
                    button.BackgroundColorHighlighted = style.BorderSelected.RGBMultiplied(1.08f);
                    button.BackgroundColorSelected = style.BorderSelected.RGBMultiplied(0.92f);
                    button.TextColor = Color.White;
                }
                else
                {
                    button.BackgroundColor = style.BackgroundNormal;
                    button.BackgroundColorHighlighted = style.BackgroundHighlighted;
                    button.BackgroundColorSelected = style.BorderSelected;
                    button.TextColor = style.ForegroundGrey;
                }

                button.BorderColor = Color.Transparent;
                button.BorderColorHighlighted = Color.Transparent;
                button.BorderColorSelected = Color.Transparent;
            }
        }

        private void LayoutFilterControls()
        {
            if (_filtersPanel == null || _searchBox == null || _scrollingPanel == null)
                return;

            _filtersPanel.UpdateBounds();
            var controlHeight = Style.Current.ControlHeight > 0.0f ? Style.Current.ControlHeight : 18.0f;
            var width = Mathf.Max(0.0f, _filtersPanel.Width);
            var innerWidth = Mathf.Max(0.0f, width - FilterHorizontalPadding * 2.0f);
            var y = FilterTopPadding;

            _searchBox.Bounds = new Rectangle(FilterHorizontalPadding, y, innerWidth, controlHeight);
            y += controlHeight;

            if (_groupFilterPanel != null && _groupFilterPanel.Visible && _groupFilterButtons.Count != 0)
            {
                y += FilterVerticalSpacing;
                var groupButtonsHeight = LayoutGroupFilterButtons(innerWidth);
                _groupFilterPanel.Bounds = new Rectangle(FilterHorizontalPadding, y, innerWidth, groupButtonsHeight);
                y += groupButtonsHeight;
            }

            y += FilterBottomPadding;

            var filterOffsets = _filtersPanel.Offsets;
            if (!Mathf.NearEqual(filterOffsets.Left, 0.0f) ||
                !Mathf.NearEqual(filterOffsets.Right, 0.0f) ||
                !Mathf.NearEqual(filterOffsets.Top, 0.0f) ||
                !Mathf.NearEqual(filterOffsets.Bottom, y))
            {
                _filtersPanel.Offsets = new Margin(0.0f, 0.0f, 0.0f, y);
            }

            var offsets = _scrollingPanel.Offsets;
            if (!Mathf.NearEqual(offsets.Top, y))
            {
                _scrollingPanel.Offsets = new Margin(0.0f, 0.0f, y, 0.0f);
                _scrollingPanel.UpdateBounds();
            }
        }

        private float LayoutGroupFilterButtons(float width)
        {
            if (_groupFilterButtons.Count == 0)
                return 0.0f;

            var availableWidth = Mathf.Max(GroupFilterButtonMinWidth, width);
            float x = 0.0f;
            float y = 0.0f;
            for (int i = 0; i < _groupFilterButtons.Count; i++)
            {
                var button = _groupFilterButtons[i];
                var buttonWidth = Mathf.Min(GetGroupFilterButtonWidth(button.Text?.ToString()), availableWidth);
                if (x > 0.0f && x + buttonWidth > availableWidth)
                {
                    x = 0.0f;
                    y += GroupFilterButtonHeight + GroupFilterButtonSpacing;
                }

                button.Bounds = new Rectangle(x, y, buttonWidth, GroupFilterButtonHeight);
                x += buttonWidth + GroupFilterButtonSpacing;
            }

            return y + GroupFilterButtonHeight;
        }

        private static float GetGroupFilterButtonWidth(string text)
        {
            var style = Style.Current;
            var textWidth = style.FontMedium ? style.FontMedium.MeasureText(text ?? string.Empty).X : (text?.Length ?? 0) * 7.0f;
            return Mathf.Clamp(textWidth + GroupFilterButtonHorizontalPadding, GroupFilterButtonMinWidth, GroupFilterButtonMaxWidth);
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
        private sealed class MaterialAssetPropertiesProxy : IDisposable
        {
            private const float MaterialSaveDelay = 0.15f;
            private bool _hasPendingSave;
            private float _pendingSaveDelay;

            [HideInEditor]
            public MaterialBase Material { get; }

            [HideInEditor]
            public bool IsMaterial => Material is Material;

            [HideInEditor]
            public bool IsMaterialInstance => Material is MaterialInstance;

            /// <summary>
            /// The material parameter values collection. Used to record undo changes.
            /// </summary>
            /// <remarks>
            /// Contains only items with raw values excluding Flax Objects.
            /// </remarks>
            [HideInEditor]
            public object[] Values
            {
                get => Material?.Parameters != null ? Material.Parameters.Select(x => x.Value).ToArray() : null;
                set
                {
                    if (Material == null)
                        return;
                    var parameters = Material.Parameters;
                    if (value == null || parameters == null || value.Length != parameters.Length)
                        return;

                    for (int i = 0; i < value.Length; i++)
                    {
                        var p = parameters[i].Value;
                        if (p is FlaxEngine.Object || p == null)
                            continue;

                        parameters[i].Value = value[i];
                    }
                    RequestSave();
                }
            }

            /// <summary>
            /// The material parameter reference values collection. Used to record undo changes.
            /// </summary>
            /// <remarks>
            /// Contains only items with references to Flax Objects.
            /// </remarks>
            [HideInEditor]
            public FlaxEngine.Object[] ValuesRef
            {
                get => Material?.Parameters != null ? Material.Parameters.Select(x => x.Value as FlaxEngine.Object).ToArray() : null;
                set
                {
                    if (Material == null)
                        return;
                    var parameters = Material.Parameters;
                    if (value == null || parameters == null || value.Length != parameters.Length)
                        return;

                    for (int i = 0; i < value.Length; i++)
                    {
                        var p = parameters[i].Value;
                        if (!(p is FlaxEngine.Object || p == null))
                            continue;

                        parameters[i].Value = value[i];
                    }
                    RequestSave();
                }
            }

            /// <summary>
            /// The material parameter override flags. Used to record undo changes.
            /// </summary>
            [HideInEditor]
            public bool[] Overrides
            {
                get => Material?.Parameters != null ? Material.Parameters.Select(x => x.IsOverride).ToArray() : null;
                set
                {
                    if (Material == null)
                        return;
                    var parameters = Material.Parameters;
                    if (value == null || parameters == null || value.Length != parameters.Length)
                        return;

                    for (int i = 0; i < value.Length; i++)
                        parameters[i].IsOverride = value[i];
                    RequestSave();
                }
            }

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
                    RequestSave();
                    Editor.Instance.Windows.PropertiesWin.Presenter.BuildLayoutOnUpdate();
                }
            }

            public MaterialAssetPropertiesProxy(MaterialBase material)
            {
                Material = material;
            }

            public void RequestSave()
            {
                _hasPendingSave = true;
                _pendingSaveDelay = MaterialSaveDelay;
            }

            public void UpdateDeferredSave(float deltaTime, bool isDragging)
            {
                if (!_hasPendingSave)
                    return;

                if (isDragging)
                {
                    _pendingSaveDelay = MaterialSaveDelay;
                    return;
                }

                _pendingSaveDelay -= deltaTime;
                if (_pendingSaveDelay <= 0.0f)
                    SavePendingChanges();
            }

            public void SavePendingChanges()
            {
                if (!_hasPendingSave)
                    return;

                _hasPendingSave = false;
                if (Material != null && Material.IsLoaded && Material.Save())
                    Editor.LogError("Cannot save asset.");
            }

            public void DiscardPendingChanges()
            {
                _hasPendingSave = false;
            }

            public void Dispose()
            {
                SavePendingChanges();
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
                                                            proxy.RequestSave();
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
                proxy.RequestSave();
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
            private bool _hasPendingParticleEmitterSave;
            private float _pendingParticleSystemSaveDelay;
            private float _pendingParticleEmitterSaveDelay;

            private const float ParticleSaveDelay = 0.15f;

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
                _pendingParticleSystemSaveDelay = ParticleSaveDelay;
            }

            public void RequestEmitterSurfaceSave()
            {
                _hasPendingParticleEmitterSave = true;
                _pendingParticleEmitterSaveDelay = ParticleSaveDelay;
            }

            public void UpdateDeferredSave(float deltaTime, bool isDragging)
            {
                if (!_hasPendingParticleSystemSave && !_hasPendingParticleEmitterSave)
                    return;

                if (isDragging)
                {
                    if (_hasPendingParticleSystemSave)
                        _pendingParticleSystemSaveDelay = ParticleSaveDelay;
                    if (_hasPendingParticleEmitterSave)
                        _pendingParticleEmitterSaveDelay = ParticleSaveDelay;
                    return;
                }

                if (_hasPendingParticleSystemSave)
                {
                    _pendingParticleSystemSaveDelay -= deltaTime;
                    if (_pendingParticleSystemSaveDelay <= 0.0f)
                        SavePendingParticleSystemChanges();
                }
                if (_hasPendingParticleEmitterSave)
                {
                    _pendingParticleEmitterSaveDelay -= deltaTime;
                    if (_pendingParticleEmitterSaveDelay <= 0.0f)
                        SavePendingEmitterSurfaceChanges();
                }
            }

            public void SavePendingChanges(bool rebuildLayout = true)
            {
                if (_hasPendingParticleSystemSave)
                    SavePendingParticleSystemChanges(rebuildLayout);
                if (_hasPendingParticleEmitterSave)
                    SavePendingEmitterSurfaceChanges(rebuildLayout);
            }

            public void DiscardPendingChanges()
            {
                _hasPendingParticleSystemSave = false;
                _hasPendingParticleEmitterSave = false;
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

            public void SavePendingEmitterSurfaceChanges(bool rebuildLayout = true)
            {
                if (!_hasPendingParticleEmitterSave)
                    return;

                _hasPendingParticleEmitterSave = false;
                SaveEmitterSurface(rebuildLayout);
            }

            public void SaveEmitterSurface()
            {
                SaveEmitterSurface(false);
            }

            private void SaveEmitterSurface(bool rebuildLayout)
            {
                if (_particleEmitter && _emitterSurface != null && _emitterSurface.Save())
                    Editor.LogError("Failed to save Particle Emitter surface.");
                if (rebuildLayout)
                    Editor.Instance.Windows.PropertiesWin.Presenter.BuildLayoutOnUpdate();
            }

            public void Dispose()
            {
                SavePendingChanges(false);
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
                proxy.RequestEmitterSurfaceSave();
            }
        }

        private sealed class PrefabContentAssetState : IDisposable
        {
            public Prefab Asset { get; }

            public Actor Instance { get; private set; }

            public PrefabContentAssetState(Prefab prefab)
            {
                Asset = prefab;
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

        private sealed class JsonAssetContentAssetState : IDisposable
        {
            private const float JsonAssetSaveDelay = 0.15f;

            private readonly JsonAsset _asset;
            private readonly object _instance;
            private bool _hasPendingSave;
            private float _pendingSaveDelay;

            public JsonAssetContentAssetState(JsonAsset asset, object instance)
            {
                _asset = asset;
                _instance = instance;
            }

            public void RequestSave()
            {
                _hasPendingSave = true;
                _pendingSaveDelay = JsonAssetSaveDelay;
            }

            public void UpdateDeferredSave(float deltaTime, bool isDragging)
            {
                if (!_hasPendingSave)
                    return;

                if (isDragging)
                {
                    _pendingSaveDelay = JsonAssetSaveDelay;
                    return;
                }

                _pendingSaveDelay -= deltaTime;
                if (_pendingSaveDelay <= 0.0f)
                    SavePendingChanges();
            }

            public void SavePendingChanges()
            {
                if (!_hasPendingSave)
                    return;

                _hasPendingSave = false;
                SaveChanges();
            }

            public void DiscardPendingChanges()
            {
                _hasPendingSave = false;
            }

            public void SaveChanges()
            {
                _hasPendingSave = false;
                if (_asset == null || !_asset.IsLoaded)
                    return;

                _asset.SetInstance(_instance);
                if (Editor.SaveJsonAsset(_asset.Path, _instance))
                    Editor.LogError("Cannot save asset.");
            }

            public void Dispose()
            {
                SavePendingChanges();
            }
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            Editor.SceneEditing.SelectionChanged -= OnSceneSelectionChanged;
            if (Editor.Windows.ContentWin != null)
                Editor.Windows.ContentWin.SelectionChanged -= OnContentSelectionChanged;
            if (Editor.ContentDatabase != null)
                Editor.ContentDatabase.ItemRemoved -= OnContentItemRemoved;
            FlaxEngine.Content.AssetReloading -= OnAssetReloading;
            Presenter.Modified -= OnPresenterModified;
            if (_contentAssetState != null)
                Presenter.Deselect();
            ClearContentAssetState();

            base.OnDestroy();
        }
    }
}
