// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Xml;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.Content.GUI;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.GUI.Tree;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.Assertions;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// One of the main editor windows used to present workspace content and user scripts.
    /// Provides various functionalities for asset operations.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.EditorWindow" />
    public sealed partial class ContentWindow : EditorWindow
    {
        private sealed class ScalableContentTreePanel : TreeViewPanel
        {
            private readonly ContentWindow _owner;

            public ScalableContentTreePanel(ContentWindow owner)
            {
                _owner = owner;
            }

            public override bool OnMouseWheel(Float2 location, float delta)
            {
                if (Root.GetKey(KeyboardKeys.Control))
                {
                    _owner.View.Zoom(delta);
                    return true;
                }
                return base.OnMouseWheel(location, delta);
            }
        }

        private const string ProjectDataLastViewedFolder = "LastViewedFolder";
        private const string ProjectDataExpandedFolders = "ExpandedFolders";
        private bool _isWorkspaceDirty;
        private string _workspaceRebuildLocation;
        private string _lastViewedFolderBeforeReload;
        private SplitPanel _split;
        private TreeViewPanel _treeOnlyPanel;
        private ContainerControl _treePanelRoot;
        private ContainerControl _treeHeaderPanel;
        private Panel _contentItemsSearchPanel;
        private Panel _contentViewPanel;
        private Panel _contentTreePanel;
        private ContentView _view;

        private readonly ToolStrip _toolStrip;
        private readonly ToolStripButton _importButton;
        private readonly ToolStripButton _createNewButton;
        private readonly SearchHintsPanel _searchHintsPanel;

        private NavigationBar _navigationBar;
        private Panel _viewDropdownPanel;
        private Tree _tree;
        private TextBox _foldersSearchBox;
        private TextBox _itemsSearchBox;
        private ViewDropdown _viewDropdown;
        private SortType _sortType;
        private bool _showEngineFiles = true, _showPluginsFiles = true, _showAllFiles = true, _showGeneratedFiles = false;
        private bool _showAllContentInTree;
        private bool _suppressExpandedStateSave;
        private bool _isClearingSelection;
        private bool _suppressContentSelectionNavigation;
        private bool _suppressContentOpenNavigation;
        private readonly HashSet<string> _expandedFolderPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private bool _renameInTree;
        private RenamePopup _activeRenamePopup;
        private bool _workspaceRebuildPending;

        private RootContentFolderTreeNode _root;
        private readonly List<ContentItem> _treeSelectionCache = new List<ContentItem>();
        private string[] _lastContentSelectionPaths = Array.Empty<string>();
        private string _lastContentOpenPath;

        private bool _navigationUnlocked;
        private readonly Stack<ContentFolderTreeNode> _navigationUndo = new Stack<ContentFolderTreeNode>(32);
        private readonly Stack<ContentFolderTreeNode> _navigationRedo = new Stack<ContentFolderTreeNode>(32);

        private NewItem _newElement;
        private List<string> _newFilesCache;
        private int _newFilesCacheSize;
        private readonly object _importUndoLock = new object();
        private readonly HashSet<string> _importExistingOutputPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly List<ContentItem> _importedItemsForUndo = new List<ContentItem>();

        /// <summary>
        /// Gets the toolstrip.
        /// </summary>
        public ToolStrip Toolstrip => _toolStrip;

        /// <summary>
        /// Gets the assets view.
        /// </summary>
        public ContentView View => _view;

        /// <summary>
        /// Occurs when selected content items collection gets changed.
        /// </summary>
        public event Action SelectionChanged;

        /// <summary>
        /// Allows a contextual tool to keep one Content selection alongside scene selection.
        /// </summary>
        public event Func<ContentItem, bool> SelectionCoexistenceRequested;

        /// <summary>
        /// Gets the selected content items.
        /// </summary>
        public IReadOnlyList<ContentItem> Selection
        {
            get
            {
                if (!_showAllContentInTree)
                    return _view.Selection;

                _treeSelectionCache.Clear();
                for (int i = 0; i < _tree.Selection.Count; i++)
                {
                    if (_tree.Selection[i] is ContentItemTreeNode itemNode)
                        _treeSelectionCache.Add(itemNode.Item);
                    else if (_tree.Selection[i] is ContentFolderTreeNode folderNode)
                        _treeSelectionCache.Add(folderNode.Folder);
                }
                return _treeSelectionCache;
            }
        }

        /// <summary>
        /// Clears the selected content items.
        /// </summary>
        /// <param name="recordUndo">True if record the selection change in edit and navigation history.</param>
        public void ClearSelection(bool recordUndo = true)
        {
            if (_showAllContentInTree)
            {
                _isClearingSelection = !recordUndo;
                try
                {
                    _tree.Deselect();
                    _view.ClearSelection();
                }
                finally
                {
                    if (!recordUndo)
                        _lastContentSelectionPaths = GetContentSelectionPaths();
                    _isClearingSelection = false;
                }
                return;
            }

            if (recordUndo)
            {
                _view.ClearSelection();
                return;
            }

            _isClearingSelection = true;
            try
            {
                _view.ClearSelection();
            }
            finally
            {
                _isClearingSelection = false;
            }
        }

        internal bool ShowEngineFiles
        {
            get => _showEngineFiles;
            set
            {
                if (_showEngineFiles != value)
                {
                    _showEngineFiles = value;
                    if (Editor.ContentDatabase.Engine != null)
                    {
                        Editor.ContentDatabase.Engine.Visible = value;
                        Editor.ContentDatabase.Engine.Folder.Visible = value;
                        RefreshView();
                        _tree.PerformLayout();
                    }
                }
            }
        }

        internal bool ShowPluginsFiles
        {
            get => _showPluginsFiles;
            set
            {
                if (_showPluginsFiles != value)
                {
                    _showPluginsFiles = value;
                    foreach (var project in Editor.ContentDatabase.Projects)
                    {
                        if (project == Editor.ContentDatabase.Game || project == Editor.ContentDatabase.Engine)
                            continue;
                        project.Visible = value;
                        project.Folder.Visible = value;
                        RefreshView();
                        _tree.PerformLayout();
                    }
                }
            }
        }

        internal bool ShowGeneratedFiles
        {
            get => _showGeneratedFiles;
            set
            {
                if (_showGeneratedFiles != value)
                {
                    _showGeneratedFiles = value;
                    RefreshView();
                }
            }
        }

        internal bool ShowAllFiles
        {
            get => _showAllFiles;
            set
            {
                if (_showAllFiles != value)
                {
                    _showAllFiles = value;
                    RefreshView();
                }
            }
        }

        internal bool IsTreeOnlyMode => _showAllContentInTree;
        internal SortType CurrentSortType => _sortType;

        /// <summary>
        /// Initializes a new instance of the <see cref="ContentWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public ContentWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Content";
            Icon = editor.Icons.Folder32;
            var style = Style.Current;
            var controlHeight = style.ControlHeight > 0.0f ? style.ControlHeight : 18.0f;

            FlaxEditor.Utilities.Utils.SetupCommonInputActions(this);

            var options = Editor.Options;
            options.OptionsChanged += OnOptionsChanged;

            // Toolstrip
            _toolStrip = new ToolStrip(ToolStrip.GetCompactHeaderHeight(controlHeight))
            {
                Parent = this,
                BackgroundColor = style.Background,
                ItemsMargin = ToolStrip.CompactHeaderItemsMargin,
                UseCompactButtonStyle = true,
            };
            _createNewButton = (ToolStripButton)_toolStrip.AddGlyphButton(ToolStripGlyph.Add, ToolStripAnchor.Left, "Flax.Content.Create", OnCreateNewItemButtonClicked).LinkTooltip("Create a new asset. Shift + left click to create a new folder.");
            _importButton = (ToolStripButton)_toolStrip.AddGlyphButton(ToolStripGlyph.Import, ToolStripAnchor.Left, "Flax.Content.Import", () => Editor.ContentImporting.ShowImportFileDialog(CurrentViewFolder)).LinkTooltip("Import content.");

            // A single search field serves both the folder tree and active content view.
            _foldersSearchBox = new ContentSearchBox
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Parent = _toolStrip,
                Bounds = new Rectangle(58.0f, ToolStrip.CompactHeaderPadding, Width - 98.0f, controlHeight),
                TooltipText = "Search content. Use t: to filter by asset type.",
            };
            _itemsSearchBox = _foldersSearchBox;
            _foldersSearchBox.TextChanged += OnContentSearchBoxTextChanged;
            var contentSearchBox = (ContentSearchBox)_foldersSearchBox;
            contentSearchBox.SearchFocused += ShowSearchHints;
            contentSearchBox.SearchSubmitted += OnSearchSubmitted;
            contentSearchBox.SearchCanceled += HideSearchHints;
            contentSearchBox.SearchNavigation += OnSearchNavigation;
            _searchHintsPanel = new SearchHintsPanel
            {
                Parent = this,
                Visible = false,
            };
            _searchHintsPanel.TypeSelected += OnTypeSuggestionSelected;

            // Split panel
            _split = new SplitPanel(options.Options.Interface.ContentWindowOrientation, ScrollBars.None, ScrollBars.None)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, _toolStrip.Bottom, 0),
                SplitterValue = 0.2f,
                Parent = this,
            };

            // Tree-only panel (used when showing all content in the tree)
            _treeOnlyPanel = new ScalableContentTreePanel(this)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = new Margin(0, 0, _toolStrip.Bottom, 0),
                Visible = false,
                Parent = this,
            };

            // Tree host panel
            _treePanelRoot = new ContainerControl
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = _split.Panel1,
            };

            // The search field now lives in the toolbar; retain a zero-height host for layout compatibility.
            _treeHeaderPanel = new ContainerControl
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                BackgroundColor = style.Background,
                IsScrollable = false,
                Offsets = Margin.Zero,
                Visible = false,
                Parent = _treePanelRoot,
            };

            // Content tree panel
            _contentTreePanel = new Panel
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                IsScrollable = true,
                ScrollBars = ScrollBars.Both,
                Parent = _treePanelRoot,
            };

            // Content structure tree
            _tree = new Tree(true)
            {
                Parent = _contentTreePanel,
            };
            _tree.SelectedChanged += OnTreeSelectionChanged;
            _treeOnlyPanel.ContentTree = _tree;

            // Search is hosted by the toolbar; keep this panel only as a compatibility anchor.
            _contentItemsSearchPanel = new Panel
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                IsScrollable = true,
                Offsets = Margin.Zero,
                Visible = false,
                Parent = _split.Panel2,
            };

            _viewDropdownPanel = new Panel
            {
                Width = controlHeight,
                BackgroundColor = Color.Transparent,
            };
            _toolStrip.AddItem(_viewDropdownPanel, ToolStripAnchor.Right, "Flax.Content.View");

            _viewDropdown = new ViewDropdown
            {
                SupportMultiSelect = true,
                TooltipText = "Change content view and filter options",
                Offsets = Margin.Zero,
                Width = controlHeight,
                Height = controlHeight,
                BackgroundColor = Color.Transparent,
                BackgroundColorHighlighted = style.BackgroundHighlighted.AlphaMultiplied(0.82f),
                BackgroundColorSelected = style.BorderSelected,
                BorderColor = Color.Transparent,
                BorderColorHighlighted = Color.Transparent,
                BorderColorSelected = Color.Transparent,
                TextColor = style.Foreground,
                TextColorHighlighted = style.Foreground,
                Parent = _viewDropdownPanel,
            };
            _viewDropdown.SelectedIndexChanged += e => UpdateItemsSearch();
            for (int i = 0; i <= (int)ContentItemSearchFilter.Other; i++)
                _viewDropdown.Items.Add(((ContentItemSearchFilter)i).ToString());
            _viewDropdown.PopupCreate += OnViewDropdownPopupCreate;

            // Navigation history remains available to shortcuts, but breadcrumbs no longer consume toolbar space.
            _navigationBar = null;

            // Content view panel
            _contentViewPanel = new Panel
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                IsScrollable = true,
                ScrollBars = ScrollBars.Vertical,
                Parent = _split.Panel2,
            };

            // Content View
            _view = new ContentView
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(0, 0, 0, 0),
                IsScrollable = true,
                Parent = _contentViewPanel,
            };
            _view.OnOpen += Open;
            _view.OnNavigateBack += NavigateBackward;
            _view.OnRename += Rename;
            _view.OnDelete += Delete;
            _view.OnDuplicate += Duplicate;
            _view.OnPaste += Paste;
            _view.ViewScaleChanged += ApplyTreeViewScale;
            _view.SelectionChanged += OnContentViewSelectionChanged;

            _view.InputActions.Add(options => options.Search, () => _itemsSearchBox.Focus());
            InputActions.Add(options => options.Search, () => _itemsSearchBox.Focus());
            Editor.SceneEditing.SelectionChanged += OnSceneSelectionChanged;

            // The split/tree hosts are created after the toolbar. Keep the overlay search
            // above those dock content layers, matching SceneTreeWindow's late-bound header.
            _foldersSearchBox.IndexInParent = int.MaxValue;
            LoadExpandedFolders();
            UpdateViewDropdownBounds();
            ApplyTreeViewScale();
        }

        private void OnSceneSelectionChanged()
        {
            if (Editor.SceneEditing.SelectionCount != 0 && !ShouldPreserveContentSelection())
                ClearSelection(false);
        }

        private void OnContentViewSelectionChanged()
        {
            RecordContentSelectionNavigation();
            if (!IsContentSelectionHistorySuppressed && !ShouldPreserveContentSelection())
                ClearSceneSelection();
            SelectionChanged?.Invoke();
        }

        private bool ShouldPreserveContentSelection()
        {
            var callback = SelectionCoexistenceRequested;
            if (callback == null || Selection.Count != 1)
                return false;
            var item = Selection[0];
            foreach (Func<ContentItem, bool> handler in callback.GetInvocationList())
            {
                if (handler(item))
                    return true;
            }
            return false;
        }

        private void ClearSceneSelection()
        {
            if (!_isClearingSelection && Selection.Count != 0 && Editor.SceneEditing.SelectionCount != 0)
                Editor.SceneEditing.Deselect(false);
        }

        private void OnCreateNewItemButtonClicked()
        {
            if (Input.GetKey(KeyboardKeys.Shift) && CanCreateFolder())
            {
                NewFolder();
                return;
            }

            var menu = new ContextMenu();
            
            InterfaceOptions interfaceOptions = Editor.Instance.Options.Options.Interface;
            bool disableUnavaliable = interfaceOptions.UnavaliableContentCreateOptions == InterfaceOptions.DisabledHidden.Disabled;

            CreateNewFolderMenu(menu, CurrentViewFolder, disableUnavaliable);
            CreateNewModuleMenu(menu, CurrentViewFolder, disableUnavaliable);
            menu.AddSeparator();
            CreateNewContentItemMenu(menu, CurrentViewFolder, false, disableUnavaliable);
            var searchableMenu = new SearchableContextMenu(menu, "New");
            searchableMenu.Show(this, _createNewButton.BottomLeft);
        }

        private ContextMenu OnViewDropdownPopupCreate(ComboBox comboBox)
        {
            var menu = new ContextMenu();

            var alternatingRows = menu.AddButton("Alternating rows", () =>
            {
                Editor.Options.Options.Interface.AlternatingTreeRows = !Editor.Options.Options.Interface.AlternatingTreeRows;
                Editor.Options.Apply(Editor.Options.Options);
            });
            alternatingRows.Checked = Editor.Options.Options.Interface.AlternatingTreeRows;

            var treeRowHeight = menu.AddButton("Tree Row Height");
            treeRowHeight.CloseMenuOnClick = false;
            var treeRowHeightValue = new FloatValueBox(Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 18.0f, 135, 2, 55.0f, 12.0f, 96.0f, 1.0f)
            {
                Parent = treeRowHeight
            };
            treeRowHeightValue.ValueChanged += () =>
            {
                var value = Mathf.Clamp(treeRowHeightValue.Value, 12.0f, 96.0f);
                Editor.Options.Options.Interface.TreeRowHeight = value;
                Style.Current.TreeRowHeight = value;
                Editor.Options.SaveOptions();
                ApplyTreeViewScale();
            };

            var treeIconSize = menu.AddButton("Tree Icon Size");
            treeIconSize.CloseMenuOnClick = false;
            var treeIconSizeValue = new FloatValueBox(Style.Current.GetContentTreeIconSize(), 135, 2, 55.0f, 0.0f, 96.0f, 1.0f)
            {
                Parent = treeIconSize
            };
            treeIconSizeValue.ValueChanged += () =>
            {
                var value = Mathf.Clamp(treeIconSizeValue.Value, 0.0f, 96.0f);
                Editor.Options.Options.Interface.ContentTreeIconSize = value;
                Style.Current.ContentTreeIconSize = value;
                Editor.Options.SaveOptions();
                ApplyTreeViewScale();
            };

            menu.AddSeparator();

            var viewScale = menu.AddButton("View Scale");
            viewScale.CloseMenuOnClick = false;
            var scaleValue = new FloatValueBox(1, 75, 2, 50.0f, 0.8f, 3.0f, 0.01f)
            {
                Parent = viewScale
            };
            scaleValue.ValueChanged += () => View.ViewScale = scaleValue.Value;
            menu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                treeRowHeightValue.Value = Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 18.0f;
                treeIconSizeValue.Value = Style.Current.GetContentTreeIconSize();
                scaleValue.Value = View.ViewScale;
            };

            var viewType = menu.AddChildMenu("View Type");
            viewType.ContextMenu.AddButton("Tiles", OnViewTypeButtonClicked).Tag = ContentViewType.Tiles;
            viewType.ContextMenu.AddButton("List", OnViewTypeButtonClicked).Tag = ContentViewType.List;
            viewType.ContextMenu.AddButton("Tree View", OnViewTypeButtonClicked).Tag = "Tree";
            viewType.ContextMenu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                foreach (var item in ((ContextMenu)control).Items)
                {
                    if (item is ContextMenuButton button)
                    {
                        if (button.Tag is ContentViewType type)
                            button.Checked = View.ViewType == type && !_showAllContentInTree;
                        else
                            button.Checked = _showAllContentInTree;
                    }
                }
            };

            var show = menu.AddChildMenu("Show");
            {
                var b = show.ContextMenu.AddButton("File extensions", () =>
                {
                    View.ShowFileExtensions = !View.ShowFileExtensions;
                    if (_showAllContentInTree)
                        UpdateTreeItemNames(_root);
                });
                b.TooltipText = "Shows all files with extensions";
                b.Checked = View.ShowFileExtensions;
                b.CloseMenuOnClick = false;
                b.AutoCheck = true;

                b = show.ContextMenu.AddButton("Engine files", () => ShowEngineFiles = !ShowEngineFiles);
                b.TooltipText = "Shows in-built engine content";
                b.Checked = ShowEngineFiles;
                b.CloseMenuOnClick = false;
                b.AutoCheck = true;

                b = show.ContextMenu.AddButton("Plugins files", () => ShowPluginsFiles = !ShowPluginsFiles);
                b.TooltipText = "Shows plugin projects content";
                b.Checked = ShowPluginsFiles;
                b.CloseMenuOnClick = false;
                b.AutoCheck = true;

                b = show.ContextMenu.AddButton("Generated files", () => ShowGeneratedFiles = !ShowGeneratedFiles);
                b.TooltipText = "Shows generated files";
                b.Checked = ShowGeneratedFiles;
                b.CloseMenuOnClick = false;
                b.AutoCheck = true;

                b = show.ContextMenu.AddButton("All files", () => ShowAllFiles = !ShowAllFiles);
                b.TooltipText = "Shows all files including other than assets and source code";
                b.Checked = ShowAllFiles;
                b.CloseMenuOnClick = false;
                b.AutoCheck = true;
            }

            var filters = menu.AddChildMenu("Filters");
            for (int i = 0; i < _viewDropdown.Items.Count; i++)
            {
                var filterButton = filters.ContextMenu.AddButton(_viewDropdown.Items[i], OnFilterClicked);
                filterButton.CloseMenuOnClick = false;
                filterButton.Tag = i;
            }
            filters.ContextMenu.ButtonClicked += button =>
            {
                foreach (var item in (filters.ContextMenu).Items)
                {
                    if (item is ContextMenuButton filterButton)
                        filterButton.Checked = _viewDropdown.IsSelected(filterButton.Text);
                }
            };
            filters.ContextMenu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                foreach (var item in ((ContextMenu)control).Items)
                {
                    if (item is ContextMenuButton filterButton)
                        filterButton.Checked = _viewDropdown.IsSelected(filterButton.Text);
                }
            };

            var sortBy = menu.AddChildMenu("Sort by");
            sortBy.ContextMenu.AddButton("Alphabetic Order", OnSortByButtonClicked).Tag = SortType.AlphabeticOrder;
            sortBy.ContextMenu.AddButton("Alphabetic Reverse", OnSortByButtonClicked).Tag = SortType.AlphabeticReverse;
            sortBy.ContextMenu.VisibleChanged += control =>
            {
                if (!control.Visible)
                    return;
                foreach (var item in ((ContextMenu)control).Items)
                {
                    if (item is ContextMenuButton button)
                        button.Checked = _sortType == (SortType)button.Tag;
                }
            };

            return menu;
        }

        private void OnOptionsChanged(EditorOptions options)
        {
            _split.Orientation = options.Interface.ContentWindowOrientation;

            RefreshView();
            ApplyTreeViewScale();
        }

        private void SetShowAllContentInTree(bool value)
        {
            if (_showAllContentInTree == value)
                return;

            _showAllContentInTree = value;
            ApplyTreeViewMode();
        }

        private void ApplyTreeViewMode()
        {
            if (_treeOnlyPanel == null || _split == null || _treePanelRoot == null)
                return;

            if (_showAllContentInTree)
            {
                _split.Visible = false;
                _treeOnlyPanel.Visible = true;
                _treePanelRoot.Parent = _treeOnlyPanel;
                _treePanelRoot.Offsets = Margin.Zero;
                _contentItemsSearchPanel.Visible = false;
                // The folder and item search are the same shared header control. Tree
                // View still needs search, so never hide it with the legacy item panel.
                _itemsSearchBox.Visible = true;
                _contentViewPanel.Visible = false;
                RunWithContentSelectionHistorySuppressed(() =>
                {
                    RefreshTreeItems();
                    if (Editor.SceneEditing.SelectionCount != 0)
                        ClearSelection(false);
                });
            }
            else
            {
                _treeOnlyPanel.Visible = false;
                _split.Visible = true;
                _treePanelRoot.Parent = _split.Panel1;
                _treePanelRoot.Offsets = Margin.Zero;
                _contentItemsSearchPanel.Visible = true;
                _itemsSearchBox.Visible = true;
                _contentViewPanel.Visible = true;
                RunWithContentSelectionHistorySuppressed(() =>
                {
                    if (_tree.SelectedNode is ContentItemTreeNode itemNode && itemNode.Parent is TreeNode parentNode)
                        _tree.Select(parentNode);
                    if (_root != null)
                        RemoveTreeAssetNodes(_root);
                    RefreshView(SelectedNode);
                });
            }

            PerformLayout();
            ApplyTreeViewScale();
            _tree.PerformLayout();
        }

        private void OnViewTypeButtonClicked(ContextMenuButton button)
        {
            if (button.Tag is ContentViewType viewType)
            {
                SetShowAllContentInTree(false);
                View.ViewType = viewType;
            }
            else
            {
                SetShowAllContentInTree(true);
            }
        }

        private void OnFilterClicked(ContextMenuButton filterButton)
        {
            var i = (int)filterButton.Tag;
            _viewDropdown.OnClicked(i);
        }

        private void OnSortByButtonClicked(ContextMenuButton button)
        {
            switch ((SortType)button.Tag)
            {
            case SortType.AlphabeticOrder:
                _sortType = SortType.AlphabeticOrder;
                break;
            case SortType.AlphabeticReverse:
                _sortType = SortType.AlphabeticReverse;
                break;
            }
            RefreshView(SelectedNode);
        }

        /// <summary>
        ///  Enables or disables vertical and horizontal scrolling on the content tree panel
        /// </summary>
        /// <param name="enabled">The state to set scrolling to</param>
        public void ScrollingOnTreeView(bool enabled)
        {
            if (_contentTreePanel.VScrollBar != null)
                _contentTreePanel.VScrollBar.ThumbEnabled = enabled;
            if (_contentTreePanel.HScrollBar != null)
                _contentTreePanel.HScrollBar.ThumbEnabled = enabled;
        }

        /// <summary>
        ///  Enables or disables vertical and horizontal scrolling on the content view panel
        /// </summary>
        /// <param name="enabled">The state to set scrolling to</param>
        public void ScrollingOnContentView(bool enabled)
        {
            if (_contentViewPanel.VScrollBar != null)
                _contentViewPanel.VScrollBar.ThumbEnabled = enabled;
            if (_contentViewPanel.HScrollBar != null)
                _contentViewPanel.HScrollBar.ThumbEnabled = enabled;
        }

        /// <summary>
        /// Shows popup dialog with UI to rename content item.
        /// </summary>
        /// <param name="item">The item to rename.</param>
        /// <returns>The created renaming popup.</returns>
        public void Rename(ContentItem item)
        {
            // Ignore duplicate rename requests (for example, a delayed item-click rename
            // firing after the keyboard command). Replacing the popup would select all text
            // again and discard the edit already in progress.
            if (!CanStartRename(_activeRenamePopup != null))
            {
                ContentMutationDiagnostics.Log("rename.begin-rejected", $"reason=already-active; requested='{item?.Path}'; active='{(_activeRenamePopup?.Tag as ContentItem)?.Path}'");
                return;
            }

            if (!item.CanRename)
            {
                ContentMutationDiagnostics.Log("rename.begin-rejected", $"reason=item-cannot-rename; item='{item.Path}'");
                return;
            }

            ContentMutationDiagnostics.Log("rename.begin", $"item='{item.Path}'; inTree={_showAllContentInTree}; selection={_view.Selection.Count}");

            // Show element in the view
            Select(item, true);

            // Disable scrolling in proper view
            _renameInTree = _showAllContentInTree;
            if (_renameInTree)
                ScrollingOnTreeView(false);
            else
                ScrollingOnContentView(false);

            // Show rename popup
            RenamePopup popup;
            if (_renameInTree)
            {
                TreeNode node = null;
                if (item is ContentFolder folder)
                    node = folder.Node;
                else if (item.ParentFolder != null)
                    node = FindTreeItemNode(item.ParentFolder.Node, item);
                if (node == null)
                {
                    // Fallback to content view rename
                    popup = RenamePopup.Show(item, item.TextRectangle, item.ShortName, true);
                }
                else
                {
                    var area = node.TextRect;
                    const float minRenameWidth = 220.0f;
                    if (area.Width < minRenameWidth)
                        area.Width = minRenameWidth;
                    area.Y -= 2;
                    area.Height += 4.0f;
                    popup = RenamePopup.Show(node, area, item.ShortName, true);
                }
            }
            else
            {
                popup = RenamePopup.Show(item, item.TextRectangle, item.ShortName, true);
            }
            popup.Tag = item;
            _activeRenamePopup = popup;
            ContentMutationDiagnostics.Log("rename.popup-focused", $"item='{item.Path}'; inputFocused={popup.InputField.IsFocused}; initial='{ContentMutationDiagnostics.Sanitize(item.ShortName)}'");
            popup.Validate += OnRenameValidate;
            popup.Renamed += renamePopup => Rename((ContentItem)renamePopup.Tag, renamePopup.Text);
            popup.Closed += OnRenameClosed;

            // For new asset we want to mock the initial value so user can press just Enter to use default name
            if (_newElement != null)
            {
                popup.InitialValue = "?";
            }
        }

        private bool OnRenameValidate(RenamePopup popup, string value)
        {
            var item = (ContentItem)popup.Tag;
            var valid = Editor.ContentEditing.IsValidAssetName(item, value, out var hint);
            ContentMutationDiagnostics.Log("rename.validate", $"item='{item.Path}'; text='{ContentMutationDiagnostics.Sanitize(value)}'; valid={valid}; hint='{ContentMutationDiagnostics.Sanitize(hint)}'");
            return valid;
        }

        private void OnRenameClosed(RenamePopup popup)
        {
            ContentMutationDiagnostics.Log("rename.popup-closed", $"item='{(popup.Tag as ContentItem)?.Path}'; wasActive={_activeRenamePopup == popup}");
            if (_activeRenamePopup == popup)
                _activeRenamePopup = null;

            // Restore scrolling in proper view
            if (_renameInTree)
                ScrollingOnTreeView(true);
            else
                ScrollingOnContentView(true);
            _renameInTree = false;

            // Check if was creating new element
            if (_newElement != null)
            {
                var parentFolder = _newElement.ParentFolder;

                // Destroy mock control
                _newElement.ParentFolder = null;
                _newElement.Dispose();
                _newElement = null;

                if (_showAllContentInTree && parentFolder?.Node != null)
                {
                    RefreshView();
                    _tree.Select(parentFolder.Node);
                }
            }
        }

        internal static bool CanStartRename(bool renameActive)
        {
            return !renameActive;
        }

        /// <summary>
        /// Renames the specified item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="newShortName">New name (without extension, just the filename).</param>
        public void Rename(ContentItem item, string newShortName)
        {
            if (item == null)
                throw new ArgumentNullException();

            // Check if can rename this item
            if (!item.CanRename)
            {
                ContentMutationDiagnostics.Log("mutation.rename.rejected", $"reason=item-cannot-rename; item='{item.Path}'; requested='{ContentMutationDiagnostics.Sanitize(newShortName)}'");
                MessageBox.Show("Cannot rename this item.", "Cannot rename", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Renaming a file to an extension it already has
            if (!item.IsFolder && StringUtils.NormalizeExtension(Path.GetExtension(newShortName)) == StringUtils.NormalizeExtension(Path.GetExtension(item.Path)))
            {
                newShortName = StringUtils.GetPathWithoutExtension(newShortName);
            }

            // Check if name is valid
            if (!Editor.ContentEditing.IsValidAssetName(item, newShortName, out string hint))
            {
                ContentMutationDiagnostics.Log("mutation.rename.rejected", $"reason=invalid-name; item='{item.Path}'; requested='{ContentMutationDiagnostics.Sanitize(newShortName)}'; hint='{ContentMutationDiagnostics.Sanitize(hint)}'");
                MessageBox.Show("Given asset name is invalid. " + hint, "Invalid name", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            newShortName = newShortName.Trim();

            // Ensure has parent
            if (item.ParentFolder == null)
            {
                Editor.LogWarning("Cannot rename root items. " + item.Path);
                return;
            }

            // Cache data
            string extension = item.IsFolder ? "" : Path.GetExtension(item.Path);
            var newPath = StringUtils.CombinePaths(item.ParentFolder.Path, newShortName + extension);
            var oldPath = item.Path;
            var wasNewElement = _newElement == item;
            ContentMutationDiagnostics.Log(wasNewElement ? "mutation.create.begin" : "mutation.rename.begin", $"source='{oldPath}'; destination='{newPath}'");

            // Check if was renaming mock element
            // Note: we create `_newElement` and then rename it to create new asset
            var itemFolder = item.ParentFolder;
            Action<ContentItem> endEvent = null;
            bool lazyCreation = false;
            if (wasNewElement)
            {
                bool creationSucceeded = false;
                try
                {
                    endEvent = (Action<ContentItem>)_newElement.Tag;

                    // Create new asset
                    var proxy = _newElement.Proxy;
                    Editor.Log(string.Format("Creating asset {0} in {1}", proxy.Name, newPath));
                    var createResult = Editor.ContentDatabase.CreatePath(newPath, false, () => proxy.Create(newPath, _newElement.Argument), true);
                    if (!createResult.Succeeded)
                        throw new IOException(createResult.Message ?? "The Content creation transaction failed.");

                    // When creating item with options dialog deffer processing
                    lazyCreation = !File.Exists(newPath);
                    creationSucceeded = lazyCreation || File.Exists(newPath) || Directory.Exists(newPath);
                }
                catch (Exception ex)
                {
                    Editor.LogWarning(ex);
                    Editor.LogError("Failed to create asset.");
                }
                if (!creationSucceeded)
                {
                    ContentMutationDiagnostics.Log("mutation.create.failed", $"destination='{newPath}'; lazy={lazyCreation}");
                    _newElement.ParentFolder = null;
                    _newElement.Dispose();
                    _newElement = null;
                    RefreshView();
                    return;
                }
            }
            else
            {
                // Validate state
                Assert.IsNull(_newElement);

                // Rename asset
                Editor.Log(string.Format("Renaming asset {0} to {1}", item.Path, newShortName));
                if (!Editor.ContentDatabase.Move(item, newPath))
                {
                    ContentMutationDiagnostics.Log("mutation.rename.failed", $"source='{oldPath}'; destination='{newPath}'");
                    return;
                }
            }

            if (_newElement != null)
            {
                // Trigger compilation if need to
                if (_newElement.Proxy is ScriptProxy && Editor.Instance.Options.Options.General.AutoReloadScriptsOnMainWindowFocus)
                    ScriptsBuilder.MarkWorkspaceDirty();

                // Cache new file to be auto-selected after actual creation
                _newFilesCache?.Clear();
                _newFilesCacheSize = 0;
                if (lazyCreation)
                {
                    _newFilesCache ??= new List<string>();
                    _newFilesCache.Add(newPath);
                    _newFilesCacheSize = 1;
                }

                // Destroy mock control
                _newElement.ParentFolder = null;
                _newElement.Dispose();
                _newElement = null;

                // Focus content window
                Focus();
                RootWindow?.Focus();
            }

            // Refresh database and view now
            Editor.ContentDatabase.RefreshFolder(itemFolder, true);
            RefreshView();
            var newItem = itemFolder.FindChild(newPath);
            if (newItem == null)
            {
                ContentMutationDiagnostics.Log(wasNewElement ? "mutation.create.verification-failed" : "mutation.rename.verification-failed", $"source='{oldPath}'; destination='{newPath}'; lazy={lazyCreation}");
                if (!lazyCreation)
                    Editor.LogWarning("Failed to find the created new item.");
                return;
            }

            // Auto-select item
            Select(newItem, true);

            if (wasNewElement)
            {
                Editor.Undo.AddAction(ContentItemFilesystemAction.Create(Editor, newItem));
            }
            else if (!oldPath.Equals(newPath, StringComparison.Ordinal))
            {
                Editor.Undo.AddAction(new MoveContentItemAction(Editor, oldPath, newPath, "Rename " + newItem.FileName));
            }

            // Custom post-action
            endEvent?.Invoke(newItem);
            ContentMutationDiagnostics.Log(wasNewElement ? "mutation.create.committed" : "mutation.rename.committed", $"source='{oldPath}'; destination='{newItem.Path}'; undoRecorded=True");
        }


        /// <summary>
        /// Deletes the specified item. Asks user first and uses some GUI.
        /// </summary>
        /// <param name="item">The item to delete.</param>
        public void Delete(ContentItem item)
        {
            var items = View.Selection;
            if (items.Count == 0)
                items = new List<ContentItem>() { item };
            Delete(items);
        }

        /// <summary>
        /// Deletes the specified items. Asks user first and uses some GUI.
        /// </summary>
        /// <param name="items">The items to delete.</param>
        public void Delete(List<ContentItem> items)
        {
            if (items.Count == 0)
                return;

            // Sort items to remove files first, then folders
            var toDelete = new List<ContentItem>(items);
            toDelete.Sort((a, b) => a.IsFolder ? 1 : b.IsFolder ? -1 : a.Compare(b));

            string singularPlural = toDelete.Count > 1 ? "s" : "";

            string msg = toDelete.Count == 1
                         ? string.Format("Delete \'{0}\'?\n\nThis action can be undone from the edit history.", items[0].Path)
                         : string.Format("Delete {0} selected items?\n\nThis action can be undone from the edit history.", items.Count);

            // Ask user
            if (MessageBox.Show(msg, "Delete asset" + singularPlural, MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK)
                return;

            // Clear navigation
            // TODO: just remove invalid locations from the history (those are removed)
            NavigationClearHistory();

            // Delete items
            var action = ContentItemFilesystemAction.Delete(Editor, toDelete);
            if (action != null)
                Editor.Undo.AddAction(action);

            RefreshView();
        }

        private string GetClonedAssetPath(ContentItem item, ISet<string> reservedPaths = null)
        {
            string sourcePath = item.Path;
            string sourceFolder = Path.GetDirectoryName(sourcePath);
            bool IsAvailable(string path)
            {
                path = StringUtils.NormalizePath(path);
                return !PathExists(path) && (reservedPaths == null || !reservedPaths.Contains(path));
            }

            // Find new name for clone
            string destinationName;
            if (item.IsFolder)
            {
                destinationName = Utilities.Utils.IncrementNameNumber(item.ShortName, x => IsAvailable(StringUtils.CombinePaths(sourceFolder, x)));
            }
            else
            {
                string extension = Path.GetExtension(sourcePath);
                destinationName = Utilities.Utils.IncrementNameNumber(item.ShortName, x => IsAvailable(StringUtils.CombinePaths(sourceFolder, x + extension))) + extension;
            }

            return StringUtils.NormalizePath(StringUtils.CombinePaths(sourceFolder, destinationName));
        }

        private static bool PathExists(string path)
        {
            return File.Exists(path) || Directory.Exists(path);
        }

        private static bool PathsEquivalent(string left, string right)
        {
            var comparison = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), comparison);
        }

        private static List<ContentItem> RemoveSelectedDescendants(IReadOnlyList<ContentItem> items)
        {
            var result = new List<ContentItem>(items.Count);
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i];
                if (item == null || result.Contains(item))
                    continue;
                bool coveredByFolder = false;
                for (int j = 0; j < items.Count; j++)
                {
                    if (i != j && items[j] is ContentFolder folder && folder.Find(item))
                    {
                        coveredByFolder = true;
                        break;
                    }
                }
                if (!coveredByFolder)
                    result.Add(item);
            }
            return result;
        }

        /// <summary>
        /// Clones the specified item.
        /// </summary>
        /// <param name="item">The item.</param>
        public void Duplicate(ContentItem item)
        {
            // Skip null
            if (item == null)
                return;

            // TODO: don't allow to duplicate items without ParentFolder - like root items (Content, Source, Engine and Editor dirs)

            // Clone item
            var targetPath = GetClonedAssetPath(item);
            var copyResult = Editor.ContentDatabase.Copy(item, targetPath);
            if (!copyResult.Succeeded)
            {
                Editor.LogError(copyResult.Message ?? "Failed to duplicate content item.");
                return;
            }

            // Refresh this folder now and try to find duplicated item
            var parentFolder = item.ParentFolder;
            Editor.ContentDatabase.RefreshFolder(parentFolder, true);
            ClearItemsSearch();
            RefreshView();
            RunWithContentSelectionHistorySuppressed(RefreshTreeItems);
            var targetItem = Editor.ContentDatabase.Find(targetPath) ?? parentFolder.FindChild(targetPath);

            // Select the duplicate without entering rename mode. Duplication is already a
            // complete filesystem mutation and must not create an extra focus-sensitive edit.
            if (targetItem != null)
            {
                Editor.Undo.AddAction(ContentItemFilesystemAction.Create(Editor, targetItem));
                Select(targetItem, true);
                var visuallySelected = _showAllContentInTree ? Selection.Contains(targetItem) : targetItem.Parent == _view && _view.IsSelected(targetItem);
                ContentMutationDiagnostics.Log("mutation.duplicate.committed", $"source='{item.Path}'; destination='{targetItem.Path}'; renameStarted=False; selected={visuallySelected}; focused={targetItem.ContainsFocus || _view.ContainsFocus}");
            }
            else
            {
                // Never leave a stale, invisible source selection active when indexing the
                // duplicate fails. The file remains on disk and a watcher refresh can recover it.
                ClearSelection(false);
                Editor.LogWarning("Duplicated content item was not indexed: " + targetPath);
                ContentMutationDiagnostics.Log("mutation.duplicate.selection-failed", $"source='{item.Path}'; destination='{targetPath}'; staleSelectionCleared=True");
            }
        }

        /// <summary>
        /// Duplicates the specified items.
        /// </summary>
        /// <param name="items">The items.</param>
        public void Duplicate(List<ContentItem> items)
        {
            // Skip empty or null case
            if (items == null || items.Count == 0)
                return;

            // TODO: don't allow to duplicate items without ParentFolder - like root items (Content, Source, Engine and Editor dirs)

            // Check if it's just a single item
            if (items.Count == 1)
            {
                Duplicate(items[0]);
            }
            else
            {
                var toDuplicate = RemoveSelectedDescendants(items);
                var createdItems = new List<ContentItem>(items.Count);
                var createdPaths = new List<string>(items.Count);
                var duplicatePlans = new List<(ContentItem Item, string Destination)>(toDuplicate.Count);
                var reservedPaths = new HashSet<string>(RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

                for (int i = 0; i < toDuplicate.Count; i++)
                {
                    var item = toDuplicate[i];
                    var targetPath = GetClonedAssetPath(item, reservedPaths);
                    reservedPaths.Add(targetPath);
                    var preflightResult = Editor.ContentDatabase.PreflightCopy(item, targetPath);
                    if (!preflightResult.Succeeded)
                    {
                        Editor.LogError(preflightResult.Message ?? "Failed to preflight content duplication.");
                        return;
                    }
                    duplicatePlans.Add((item, targetPath));
                }

                var copyResult = Editor.ContentDatabase.Copy(duplicatePlans);
                if (!copyResult.Succeeded)
                {
                    Editor.LogError(copyResult.Message ?? "Failed to duplicate Content items.");
                    return;
                }
                createdPaths.AddRange(duplicatePlans.Select(x => x.Destination));
                foreach (var parent in duplicatePlans.Select(x => x.Item.ParentFolder).Distinct())
                    Editor.ContentDatabase.RefreshFolder(parent, true);

                ClearItemsSearch();
                RefreshView();
                RunWithContentSelectionHistorySuppressed(RefreshTreeItems);
                for (int i = 0; i < createdPaths.Count; i++)
                {
                    var targetItem = Editor.ContentDatabase.Find(createdPaths[i]);
                    if (targetItem != null)
                        createdItems.Add(targetItem);
                }
                var action = ContentItemFilesystemAction.Create(Editor, createdItems);
                if (action != null)
                    Editor.Undo.AddAction(action);

                if (createdItems.Count != 0)
                {
                    RunWithContentSelectionHistorySuppressed(() =>
                    {
                        ClearSelection(false);
                        for (int i = 0; i < createdItems.Count; i++)
                            Select(createdItems[i], true, i != 0);
                    });
                    ContentMutationDiagnostics.Log("mutation.duplicate.batch-committed", $"created={createdItems.Count}; requested={createdPaths.Count}; renameStarted=False; selected={Selection.Count}");
                }
                else
                {
                    ClearSelection(false);
                    Editor.LogWarning("Duplicated content items were not indexed. Stale selection was cleared.");
                    ContentMutationDiagnostics.Log("mutation.duplicate.batch-selection-failed", $"requested={createdPaths.Count}; staleSelectionCleared=True");
                }
            }
        }

        /// <summary>
        /// Pastes the specified files.
        /// </summary>
        /// <param name="files">The files paths to import.</param>
        /// <param name="isCutting">Whether a cutting action is occuring.</param>
        public bool Paste(string[] files, bool isCutting)
        {
            var importFiles = new List<string>();
            var createdItems = new List<ContentItem>();
            var items = RemoveSelectedDescendants(files.Select(Editor.ContentDatabase.Find).Where(x => x != null).ToList());
            var plans = new List<(ContentItem Item, string Destination)>(items.Count);
            var comparer = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            var destinations = new HashSet<string>(comparer);
            foreach (var item in items)
            {
                var newPath = StringUtils.NormalizePath(Path.Combine(CurrentViewFolder.Path, item.FileName));
                if (PathsEquivalent(item.Path, newPath))
                {
                    if (isCutting)
                        continue;
                    newPath = GetClonedAssetPath(item, destinations);
                }
                else if (PathExists(newPath))
                {
                    Editor.LogError($"Cannot paste '{item.Path}' because destination '{newPath}' already exists.");
                    return false;
                }
                if (!destinations.Add(Path.GetFullPath(newPath)))
                {
                    Editor.LogError($"Cannot paste because multiple items target '{newPath}'.");
                    return false;
                }
                plans.Add((item, newPath));
            }
            if (!isCutting)
            {
                foreach (var plan in plans)
                {
                    var preflightResult = Editor.ContentDatabase.PreflightCopy(plan.Item, plan.Destination);
                    if (!preflightResult.Succeeded)
                    {
                        Editor.LogError(preflightResult.Message ?? "Failed to preflight content paste.");
                        return false;
                    }
                }
            }
            foreach (var sourcePath in files)
            {
                if (Editor.ContentDatabase.Find(sourcePath) == null)
                    importFiles.Add(sourcePath);
            }
            if (isCutting)
            {
                var oldPaths = plans.Select(x => x.Item.Path).ToList();
                var moveResult = Editor.ContentDatabase.TryMove(plans);
                if (!moveResult.Succeeded)
                {
                    Editor.LogError(moveResult.Message ?? "Failed to move pasted Content items.");
                    return false;
                }
                if (plans.Count == 1)
                    Editor.Undo.AddAction(new MoveContentItemAction(Editor, oldPaths[0], plans[0].Destination, "Move " + plans[0].Item.FileName));
                else if (plans.Count > 1)
                    Editor.Undo.AddAction(new MoveContentItemsAction(Editor, oldPaths, plans.Select(x => x.Destination).ToList(), "Move " + plans.Count + " items"));
            }
            else if (plans.Count != 0)
            {
                var copyResult = Editor.ContentDatabase.Copy(plans);
                if (!copyResult.Succeeded)
                {
                    Editor.LogError(copyResult.Message ?? "Failed to paste Content items.");
                    return false;
                }
                Editor.ContentDatabase.RefreshFolder(CurrentViewFolder, false);
                for (int i = 0; i < plans.Count; i++)
                {
                    var newItem = Editor.ContentDatabase.Find(plans[i].Destination) ?? CurrentViewFolder.FindChild(plans[i].Destination);
                    if (newItem != null)
                        createdItems.Add(newItem);
                }
            }
            var action = ContentItemFilesystemAction.Create(Editor, createdItems);
            if (action != null)
                Editor.Undo.AddAction(action);
            Editor.ContentImporting.Import(importFiles, CurrentViewFolder);
            return true;
        }

        /// <summary>
        /// Starts creating the folder.
        /// </summary>
        public void NewFolder()
        {
            // Construct path
            var parentFolder = SelectedNode.Folder;
            string destinationPath;
            int i = 0;
            do
            {
                destinationPath = StringUtils.CombinePaths(parentFolder.Path, string.Format("New Folder ({0})", i++));
            } while (PathExists(destinationPath));

            // Create new folder
            var createResult = Editor.ContentDatabase.CreatePath(destinationPath, true, () => Directory.CreateDirectory(destinationPath));
            if (!createResult.Succeeded)
            {
                Editor.LogWarning(createResult.Message ?? "Folder creation transaction failed.");
                MessageBox.Show($"Cannot create folder '{destinationPath}'. {createResult.Message}", "Cannot create folder", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Refresh parent folder now and try to find duplicated item
            // Note: we should spawn new items directly, content database should do it to propagate events in a proper way
            Editor.ContentDatabase.RefreshFolder(parentFolder, true);
            RefreshView();
            var targetItem = parentFolder.FindChild(destinationPath);

            // Start renaming it
            if (targetItem != null)
            {
                Editor.Undo.AddAction(ContentItemFilesystemAction.Create(Editor, targetItem));
                Rename(targetItem);
            }
        }

        /// <summary>
        /// Starts creating new item.
        /// </summary>
        /// <param name="proxy">The new item proxy.</param>
        /// <param name="argument">The argument passed to the proxy for the item creation. In most cases it is null.</param>
        /// <param name="created">The event called when the item is crated by the user. The argument is the new item.</param>
        /// <param name="initialName">The initial item name.</param>
        /// <param name="withRenaming">True if start initial item renaming by user, or true to skip it.</param>
        /// <param name="destinationFolder">Optional explicit destination folder. If null, uses the current view folder.</param>
        public void NewItem(ContentProxy proxy, object argument = null, Action<ContentItem> created = null, string initialName = null, bool withRenaming = true, ContentFolder destinationFolder = null)
        {
            Assert.IsNull(_newElement);
            if (proxy == null)
                throw new ArgumentNullException(nameof(proxy));

            // Setup name
            string name = initialName ?? proxy.NewItemName;
            if (!proxy.IsFileNameValid(name) || Utilities.Utils.HasInvalidPathChar(name))
                name = proxy.NewItemName;

            ContentFolder parentFolder;
            if (destinationFolder != null && proxy.CanCreate(destinationFolder))
            {
                parentFolder = destinationFolder;
            }
            else
            {
                // If the proxy can not be created in the current folder, then navigate to the content folder
                if (CurrentViewFolder == null || !proxy.CanCreate(CurrentViewFolder))
                    Navigate(Editor.Instance.ContentDatabase.Game.Content);
                parentFolder = CurrentViewFolder;
            }
            string parentFolderPath = parentFolder.Path;

            // Create asset name
            string extension = '.' + proxy.FileExtension;
            string path = StringUtils.CombinePaths(parentFolderPath, name + extension);
            if (parentFolder.FindChild(path) != null || PathExists(path))
            {
                int i = 0;
                do
                {
                    path = StringUtils.CombinePaths(parentFolderPath, string.Format("{0} {1}", name, i++) + extension);
                } while (parentFolder.FindChild(path) != null || PathExists(path));
            }

            if (withRenaming)
            {
                // Create new asset proxy, add to view and rename it
                _newElement = new NewItem(path, proxy, argument)
                {
                    ParentFolder = parentFolder,
                    Tag = created,
                };
                RefreshView();
                if (_showAllContentInTree)
                {
                    // The mock item is not part of the content database yet, so the regular
                    // tree refresh can omit it when the Other filter is hidden. Add a
                    // temporary node for the rename popup only when one wasn't generated.
                    var parentNode = parentFolder.Node;
                    parentNode.Expand(true);
                    if (FindTreeItemNode(parentNode, _newElement) == null)
                    {
                        new ContentItemTreeNode(_newElement)
                        {
                            Parent = parentNode,
                        };
                    }
                    parentNode.SortChildren();
                    _tree.PerformLayout(true);
                    _contentTreePanel.PerformLayout(true);
                }
                Rename(_newElement);
            }
            else
            {
                // Create new asset
                Editor.Log(string.Format("Creating asset {0} in {1}", proxy.Name, path));
                var createResult = Editor.ContentDatabase.CreatePath(path, false, () => proxy.Create(path, argument), true);
                if (!createResult.Succeeded)
                {
                    Editor.LogError(createResult.Message ?? "Failed to create asset.");
                    return;
                }

                // Focus content window
                Focus();
                RootWindow?.Focus();

                // Refresh database and view now
                Editor.ContentDatabase.RefreshFolder(parentFolder, false);
                RefreshView();
                var newItem = parentFolder.FindChild(path);
                if (newItem == null)
                {
                    Editor.LogWarning("Failed to find the created new item.");
                    return;
                }

                // Auto-select item
                Select(newItem, true);

                Editor.Undo.AddAction(ContentItemFilesystemAction.Create(Editor, newItem));

                // Custom post-action
                created?.Invoke(newItem);
            }
        }

        /// <summary>
        /// Moves the content items and records the operation in the global edit history.
        /// </summary>
        /// <param name="items">The items.</param>
        /// <param name="newParent">The new parent folder.</param>
        internal void MoveWithUndo(List<ContentItem> items, ContentFolder newParent)
        {
            if (items == null)
                throw new ArgumentNullException(nameof(items));
            if (newParent == null)
                throw new ArgumentNullException(nameof(newParent));

            // A selected folder already moves all of its children. Exclude selected descendants
            // so a multi-selection does not move them a second time outside the folder.
            var itemsToMove = new List<ContentItem>(items.Count);
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i];
                if (item == null)
                    throw new ArgumentNullException(nameof(items));
                if (item.ParentFolder == newParent || itemsToMove.Contains(item))
                    continue;

                bool isChildOfSelectedFolder = false;
                for (int j = 0; j < items.Count; j++)
                {
                    if (i != j && items[j] is ContentFolder folder && folder.Find(item))
                    {
                        isChildOfSelectedFolder = true;
                        break;
                    }
                }
                if (!isChildOfSelectedFolder)
                    itemsToMove.Add(item);
            }

            var oldPaths = new List<string>(itemsToMove.Count);
            var newPaths = new List<string>(itemsToMove.Count);
            for (int i = 0; i < itemsToMove.Count; i++)
            {
                var item = itemsToMove[i];
                oldPaths.Add(item.Path);
                newPaths.Add(StringUtils.CombinePaths(newParent.Path, item.FileName));
            }
            if (oldPaths.Count == 0 || !Editor.ContentDatabase.Move(itemsToMove, newParent))
                return;
            Editor.Undo.AddAction(oldPaths.Count == 1
                ? new MoveContentItemAction(Editor, oldPaths[0], newPaths[0], "Move " + itemsToMove[0].FileName)
                : new MoveContentItemsAction(Editor, oldPaths, newPaths, "Move " + oldPaths.Count + " items"));
        }

        internal bool CanMoveWithPreflight(IReadOnlyList<ContentItem> items, ContentFolder newParent)
        {
            if (items == null || newParent == null || items.Count == 0)
                return false;
            var moves = new List<(ContentItem Item, string Destination)>(items.Count);
            for (int i = 0; i < items.Count; i++)
            {
                if (items[i] != null)
                    moves.Add((items[i], StringUtils.CombinePaths(newParent.Path, items[i].FileName)));
            }
            var result = Editor.ContentDatabase.PreflightMove(moves);
            ContentMutationDiagnostics.Log("mutation.drag-preflight", $"target='{newParent.Path}'; items={moves.Count}; succeeded={result.Succeeded}; failure={result.Failure}; message='{ContentMutationDiagnostics.Sanitize(result.Message)}'");
            return result.Succeeded;
        }

        /// <summary>
        /// Moves the content item and records the operation in the global edit history.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="newParent">The new parent folder.</param>
        internal void MoveWithUndo(ContentItem item, ContentFolder newParent)
        {
            if (item == null || newParent == null)
                throw new ArgumentNullException();

            if (item.ParentFolder == newParent)
                return;

            var oldPath = item.Path;
            if (!Editor.ContentDatabase.Move(item, newParent))
                return;
            var newPath = item.Path;
            if (!oldPath.Equals(newPath, StringComparison.Ordinal))
                Editor.Undo.AddAction(new MoveContentItemAction(Editor, oldPath, newPath, "Move " + item.FileName));
        }

        /// <summary>
        /// Moves the content item and records the operation in the global edit history.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <param name="newPath">The new path.</param>
        internal void MoveWithUndo(ContentItem item, string newPath)
        {
            if (item == null || string.IsNullOrEmpty(newPath))
                throw new ArgumentNullException();

            var oldPath = item.Path;
            if (!Editor.ContentDatabase.Move(item, newPath))
                return;
            newPath = StringUtils.NormalizePath(newPath);
            if (Editor.ContentDatabase.Find(newPath) != null && !oldPath.Equals(newPath, StringComparison.Ordinal))
                Editor.Undo.AddAction(new MoveContentItemAction(Editor, oldPath, newPath, "Move " + item.FileName));
        }

        private void OnContentDatabaseItemRemoved(ContentItem contentItem)
        {
            Editor.NavigationHistory.RemoveActions(x =>
                x is ContentSelectionNavigationAction selectionAction && selectionAction.Contains(contentItem.Path) ||
                x is ContentOpenNavigationAction openAction && openAction.Contains(contentItem.Path));
            if (contentItem is ContentFolder folder)
            {
                var node = folder.Node;

                // Check if current location contains it as a parent
                if (contentItem.Find(CurrentViewFolder))
                {
                    // Navigate to root to prevent leaks
                    RunWithContentSelectionHistorySuppressed(ShowRoot);
                }

                // Check if folder is in navigation
                Editor.NavigationHistory.RemoveActions(x => x is ContentFolderNavigationAction action && action.Contains(node));
                if (_navigationRedo.Contains(node) || _navigationUndo.Contains(node))
                {
                    // Clear all to prevent leaks
                    NavigationClearHistory();
                }
            }
        }

        private void OnContentDatabaseItemAdded(ContentItem contentItem)
        {
            if (contentItem is ContentFolder folder && _expandedFolderPaths.Contains(StringUtils.NormalizePath(folder.Path)))
            {
                _suppressExpandedStateSave = true;
                folder.Node?.Expand(true);
                _suppressExpandedStateSave = false;
            }
        }

        /// <summary>
        /// Opens the specified content item.
        /// </summary>
        /// <param name="item">The content item.</param>
        public void Open(ContentItem item)
        {
            if (item == null)
                throw new ArgumentNullException();

            // Check if it's a folder
            if (item.IsFolder)
            {
                // Show folder
                var folder = (ContentFolder)item;
                folder.Node.Expand();
                RunWithContentSelectionHistorySuppressed(() =>
                {
                    _tree.Select(folder.Node);
                    if (!_showAllContentInTree)
                        _view.SelectFirstItem();
                });
                return;
            }

            // Open it
            Editor.Windows.OnWindowFocused(this);
            RecordContentOpenNavigation(item);
            Assets.AssetEditorWindow.SuppressNextDocumentNavigation(item.Path);
            Editor.ContentEditing.Open(item);
        }

        /// <summary>
        /// Selects the specified asset in the content view.
        /// </summary>
        /// <param name="asset">The asset to select.</param>
        public void Select(Asset asset)
        {
            if (asset == null)
                throw new ArgumentNullException();

            var item = Editor.ContentDatabase.Find(asset.ID);
            if (item != null)
                Select(item);
        }

        /// <summary>
        /// Selects the specified item in the content view. Does nothing if the current view doesn't show the folder containing that item.
        /// </summary>
        /// <param name="item">The item to select.</param>
        /// <param name="fastScroll">True of scroll to the item quickly without smoothing.</param>
        /// <param name="additive">True of select item in additive mode with existing selection preservation, otherwise current selection will be cleared.</param>
        public void Select(ContentItem item, bool fastScroll = false, bool additive = false)
        {
            if (item == null)
                throw new ArgumentNullException();

            if (!_navigationUnlocked)
                return;
            var parent = item.ParentFolder;
            if (parent == null || !parent.Visible)
                return;

            // Ensure that window is visible
            FocusOrShow();

            if (_showAllContentInTree)
            {
                var targetNode = item is ContentFolder folder ? folder.Node : parent.Node;
                if (targetNode != null)
                {
                    targetNode.ExpandAllParents();
                    if (item is ContentFolder)
                    {
                        _tree.Select(targetNode, additive);
                        _contentTreePanel.ScrollViewTo(targetNode, fastScroll);
                        targetNode.Focus();
                    }
                    else
                    {
                        var itemNode = FindTreeItemNode(targetNode, item);
                        if (itemNode != null)
                        {
                            _tree.Select(itemNode, additive);
                            _contentTreePanel.ScrollViewTo(itemNode, fastScroll);
                            itemNode.Focus();
                        }
                        else
                        {
                            _tree.Select(targetNode, additive);
                        }
                    }
                }
                return;
            }

            // Navigate to the parent directory
            Navigate(parent.Node);

            // Select and scroll to cover in view
            _view.Select(item, additive);
            _contentViewPanel.ScrollViewTo(item, fastScroll);

            // Focus
            _view.Focus();
        }

        /// <summary>
        /// Reveals and highlights the specified item in the content view without selecting it.
        /// </summary>
        /// <param name="item">The item to highlight.</param>
        /// <param name="fastScroll">True of scroll to the item quickly without smoothing.</param>
        public void Highlight(ContentItem item, bool fastScroll = false)
        {
            if (item == null)
                throw new ArgumentNullException();

            if (!_navigationUnlocked)
                return;
            var parent = item.ParentFolder;
            if (parent == null || !parent.Visible)
                return;

            ClearItemsSearch();
            FocusOrShow();

            if (_showAllContentInTree)
            {
                var targetNode = item is ContentFolder folder ? folder.Node : parent.Node;
                if (targetNode != null)
                {
                    targetNode.ExpandAllParents();
                    TreeNode nodeToHighlight = targetNode;
                    if (item is not ContentFolder)
                    {
                        targetNode.Expand(true);
                        var itemNode = FindTreeItemNode(targetNode, item);
                        if (itemNode != null)
                            nodeToHighlight = itemNode;
                    }

                    _contentTreePanel.ScrollViewTo(nodeToHighlight, fastScroll);
                    nodeToHighlight.StartHighlight();
                    nodeToHighlight.Focus();
                }
                return;
            }

            Navigate(parent.Node);
            _contentViewPanel.ScrollViewTo(item, fastScroll);
            item.StartHighlight();
            _view.Focus();
        }

        private ContentItemTreeNode FindTreeItemNode(ContentFolderTreeNode parentNode, ContentItem item)
        {
            if (parentNode == null || item == null)
                return null;
            for (int i = 0; i < parentNode.ChildrenCount; i++)
            {
                if (parentNode.GetChild(i) is ContentItemTreeNode itemNode && itemNode.Item == item)
                    return itemNode;
            }
            return null;
        }

        /// <summary>
        /// Refreshes the current view items collection.
        /// </summary>
        public void RefreshView()
        {
            if (_showAllContentInTree)
                RunWithContentSelectionHistorySuppressed(RefreshTreeItems);
            else if (_view.IsSearching)
                UpdateItemsSearch();
            else
                RefreshView(SelectedNode);
        }

        /// <summary>
        /// Refreshes the view.
        /// </summary>
        /// <param name="target">The target location.</param>
        public void RefreshView(ContentFolderTreeNode target)
        {
            if (_showAllContentInTree)
            {
                RunWithContentSelectionHistorySuppressed(RefreshTreeItems);
                return;
            }

            _view.IsSearching = false;
            if (target == _root)
            {
                // Special case for root folder
                var items = new List<ContentItem>(8);
                for (int i = 0; i < _root.ChildrenCount; i++)
                {
                    if (_root.GetChild(i) is ContentFolderTreeNode node)
                    {
                        items.Add(node.Folder);
                    }
                }
                RunWithContentSelectionHistorySuppressed(() => _view.ShowItems(items, _sortType, false, true));
            }
            else if (target != null)
            {
                // Show folder contents
                var items = target.Folder.Children;
                if (!_showAllFiles)
                    items = items.Where(x => !(x is FileItem)).ToList();
                if (!_showGeneratedFiles)
                    items = items.Where(x => !(x.Path.EndsWith(".Gen.cs", StringComparison.Ordinal) || x.Path.EndsWith(".Gen.h", StringComparison.Ordinal) || x.Path.EndsWith(".Gen.cpp", StringComparison.Ordinal) || x.Path.EndsWith(".csproj", StringComparison.Ordinal) || x.Path.Contains(".CSharp"))).ToList();
                RunWithContentSelectionHistorySuppressed(() => _view.ShowItems(items, _sortType, false, true));
            }
        }

        private void RefreshTreeItems()
        {
            if (!_showAllContentInTree || _root == null)
                return;

            // Asset tree nodes are destroyed and recreated below. Preserve selection by
            // stable content path, never by the old node reference. Keeping disposed nodes
            // in Tree.Selection makes Ctrl+D keep operating while no row is highlighted.
            var selectedPaths = new List<string>(_tree.Selection.Count);
            for (int i = 0; i < _tree.Selection.Count; i++)
            {
                if (_tree.Selection[i] is ContentItemTreeNode itemNode && itemNode.Item != null)
                    selectedPaths.Add(itemNode.Item.Path);
                else if (_tree.Selection[i] is ContentFolderTreeNode folderNode && folderNode.Folder != null)
                    selectedPaths.Add(folderNode.Folder.Path);
            }
            var restoreFocus = _tree.ContainsFocus;
            _tree.Deselect();

            _root.LockChildrenRecursive();
            RemoveTreeAssetNodes(_root);
            AddTreeAssetNodes(_root);
            var query = _foldersSearchBox?.Text;
            _root.UpdateFilter(query);
            _root.UnlockChildrenRecursive();
            ApplyTreeViewScale();
            _tree.PerformLayout(true);
            _contentTreePanel.PerformLayout(true);

            if (selectedPaths.Count != 0)
            {
                var selectedNodes = new List<TreeNode>(selectedPaths.Count);
                for (int i = 0; i < selectedPaths.Count; i++)
                {
                    var item = Editor.ContentDatabase.Find(selectedPaths[i]);
                    TreeNode node = null;
                    if (item is ContentFolder folder)
                        node = folder.Node;
                    else if (item?.ParentFolder?.Node != null)
                        node = FindTreeItemNode(item.ParentFolder.Node, item);
                    if (node != null && !selectedNodes.Contains(node))
                        selectedNodes.Add(node);
                }
                _tree.Select(selectedNodes);
                if (restoreFocus && selectedNodes.Count != 0)
                    selectedNodes[selectedNodes.Count - 1].Focus();
                ContentMutationDiagnostics.Log("selection.tree-restored", $"requested={selectedPaths.Count}; restored={selectedNodes.Count}; focus={restoreFocus}");
            }
        }

        private void UpdateTreeItemNames(ContentFolderTreeNode node)
        {
            if (node == null)
                return;

            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is ContentFolderTreeNode childFolder)
                {
                    UpdateTreeItemNames(childFolder);
                }
                else if (node.GetChild(i) is ContentItemTreeNode itemNode)
                {
                    itemNode.UpdateDisplayedName();
                }
            }
        }

        internal void OnContentTreeNodeExpandedChanged(ContentFolderTreeNode node, bool isExpanded)
        {
            if (_suppressExpandedStateSave || node == null || node == _root)
                return;

            var path = node.Path;
            if (string.IsNullOrEmpty(path))
                return;
            path = StringUtils.NormalizePath(path);

            if (isExpanded)
                _expandedFolderPaths.Add(path);
            else
                // Remove all sub paths if parent folder is closed.
                _expandedFolderPaths.RemoveWhere(x => x.Contains(path));

            SaveExpandedFolders();
        }

        internal void TryAutoExpandContentNode(ContentFolderTreeNode node)
        {
            if (node == null || node == _root)
                return;

            var path = node.Path;
            if (string.IsNullOrEmpty(path))
                return;
            path = StringUtils.NormalizePath(path);

            if (!_expandedFolderPaths.Contains(path))
                return;

            _suppressExpandedStateSave = true;
            node.Expand(true);
            _suppressExpandedStateSave = false;
        }

        private void LoadExpandedFolders()
        {
            _expandedFolderPaths.Clear();
            if (Editor.ProjectCache.TryGetCustomData(ProjectDataExpandedFolders, out string data) && !string.IsNullOrWhiteSpace(data))
            {
                var entries = data.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
                for (int i = 0; i < entries.Length; i++)
                {
                    var path = entries[i].Trim();
                    if (path.Length == 0)
                        continue;
                    _expandedFolderPaths.Add(StringUtils.NormalizePath(path));
                }
            }
        }

        private void SaveExpandedFolders()
        {
            if (_expandedFolderPaths.Count == 0)
            {
                Editor.ProjectCache.RemoveCustomData(ProjectDataExpandedFolders);
                return;
            }

            var data = string.Join("\n", _expandedFolderPaths);
            Editor.ProjectCache.SetCustomData(ProjectDataExpandedFolders, data);
        }

        private void ApplyExpandedFolders()
        {
            if (_root == null || _expandedFolderPaths.Count == 0)
                return;

            _suppressExpandedStateSave = true;
            foreach (var path in _expandedFolderPaths)
            {
                if (Editor.ContentDatabase.Find(path) is ContentFolder folder)
                {
                    folder.Node.ExpandAllParents(true);
                    folder.Node.Expand(true);
                }
            }
            _suppressExpandedStateSave = false;
        }

        private void RemoveTreeAssetNodes(ContentFolderTreeNode node)
        {
            for (int i = node.ChildrenCount - 1; i >= 0; i--)
            {
                if (node.GetChild(i) is ContentItemTreeNode itemNode)
                {
                    node.RemoveChild(itemNode);
                    itemNode.Dispose();
                }
                else if (node.GetChild(i) is ContentFolderTreeNode childFolder)
                {
                    RemoveTreeAssetNodes(childFolder);
                }
            }
        }

        private void AddTreeAssetNodes(ContentFolderTreeNode node)
        {
            if (node.Folder != null)
            {
                var children = node.Folder.Children;
                for (int i = 0; i < children.Count; i++)
                {
                    var child = children[i];
                    if (child is ContentFolder)
                        continue;
                    if (!ShouldShowTreeItem(child))
                        continue;

                    var itemNode = new ContentItemTreeNode(child)
                    {
                        Parent = node,
                    };
                }
            }

            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is ContentFolderTreeNode childFolder)
                {
                    AddTreeAssetNodes(childFolder);
                }
            }

            node.SortChildren();
        }

        private bool ShouldShowTreeItem(ContentItem item)
        {
            if (item == null || !item.Visible)
                return false;
            if (_viewDropdown != null && _viewDropdown.HasSelection)
            {
                var filterIndex = (int)item.SearchFilter;
                if (!_viewDropdown.Selection.Contains(filterIndex))
                    return false;
            }
            if (!_showAllFiles && item is FileItem)
                return false;
            if (!_showGeneratedFiles && IsGeneratedFile(item.Path))
                return false;
            return true;
        }

        private static bool IsGeneratedFile(string path)
        {
            return path.EndsWith(".Gen.cs", StringComparison.Ordinal) ||
                   path.EndsWith(".Gen.h", StringComparison.Ordinal) ||
                   path.EndsWith(".Gen.cpp", StringComparison.Ordinal) ||
                   path.EndsWith(".csproj", StringComparison.Ordinal) ||
                   path.Contains(".CSharp");
        }

        private void UpdateUI()
        {
            UpdateToolstrip();
            UpdateNavigationBar();
        }

        private void ApplyTreeViewScale()
        {
            if (_tree == null)
                return;

            var scale = _showAllContentInTree ? View.ViewScale : 1.0f;
            var baseHeaderHeight = Style.Current.TreeRowHeight > 0.0f ? Style.Current.TreeRowHeight : 18.0f;
            var headerHeight = Mathf.Clamp(baseHeaderHeight * scale, 12.0f, 96.0f);
            var style = Style.Current;
            // Density changes row geometry and previews, never editor typography.
            var fontRef = new FontReference(style.FontSmall.Asset, style.FontSmall.Size);
            var iconSize = Mathf.Min(Mathf.Max(0.0f, style.GetContentTreeIconSize() * scale), Mathf.Max(0.0f, headerHeight - 2.0f));
            var textMarginLeft = Mathf.Clamp(2.0f * scale + Mathf.Max(0.0f, iconSize - 12.0f), 2.0f, 16.0f);
            var rowPadding = Mathf.Clamp(2.0f * scale, 1.0f, 5.0f);
            var childrenIndent = Mathf.Clamp(12.0f * scale, 8.0f, 24.0f);
            ApplyTreeNodeScale(_root, headerHeight, fontRef, textMarginLeft, rowPadding, childrenIndent);
            _root?.PerformLayout(true);
            _tree.PerformLayout();
        }

        private void ApplyTreeNodeScale(ContentFolderTreeNode node, float headerHeight, FontReference fontRef, float textMarginLeft, float rowPadding, float childrenIndent)
        {
            if (node == null)
                return;

            var isWorkspaceRoot = node is RootContentFolderTreeNode;
            var margin = node.TextMargin;
            margin.Left = textMarginLeft;
            margin.Top = rowPadding;
            margin.Right = rowPadding;
            margin.Bottom = rowPadding;
            node.TextMargin = margin;
            if (isWorkspaceRoot)
            {
                // This blank container row is clipped by the tree margin below. Keep the
                // content project itself as a regular, visible top-level item.
                node.ChildrenIndent = 0.0f;
            }
            else
            {
                node.HeaderHeight = headerHeight;
                node.ChildrenIndent = childrenIndent;
                node.CustomArrowRect = GetTreeArrowRect(node, headerHeight);
            }
            node.TextFont = fontRef;
            for (int i = 0; i < node.ChildrenCount; i++)
            {
                if (node.GetChild(i) is ContentFolderTreeNode child)
                    ApplyTreeNodeScale(child, headerHeight, fontRef, textMarginLeft, rowPadding, childrenIndent);
                else if (node.GetChild(i) is ContentItemTreeNode itemNode)
                {
                    var itemMargin = itemNode.TextMargin;
                    itemMargin.Left = textMarginLeft;
                    itemMargin.Top = rowPadding;
                    itemMargin.Right = rowPadding;
                    itemMargin.Bottom = rowPadding;
                    itemNode.TextMargin = itemMargin;
                    itemNode.HeaderHeight = headerHeight;
                    itemNode.TextFont = fontRef;
                }
            }
        }

        private static Rectangle GetTreeArrowRect(ContentFolderTreeNode node, float headerHeight)
        {
            if (node == null)
                return Rectangle.Empty;

            var scale = Editor.Instance?.Windows?.ContentWin?.IsTreeOnlyMode == true
                ? Editor.Instance.Windows.ContentWin.View.ViewScale
                : 1.0f;
            var maximumIconSize = Mathf.Max(10.0f, headerHeight - 1.0f);
            var arrowSize = Mathf.Min(12.0f * scale, maximumIconSize);
            var iconSize = Mathf.Min(Mathf.Max(0.0f, Style.Current.GetContentTreeIconSize() * scale), maximumIconSize);
            var textRect = node.TextRect;
            var iconLeft = textRect.Left - iconSize - 2.0f;
            var x = iconLeft - arrowSize - 2.0f;
            var y = (headerHeight - arrowSize) * 0.5f;
            return new Rectangle(Mathf.Max(x, 0.0f), Mathf.Max(y, 0.0f), arrowSize, arrowSize);
        }

        private void UpdateToolstrip()
        {
            if (_toolStrip == null)
                return;

            // Update buttons
            var folder = CurrentViewFolder;
            _importButton.Enabled = folder != null && folder.CanHaveAssets;
        }

        private void UpdateNavigationBarBounds()
        {
            if (_toolStrip == null)
                return;

            if (_navigationBar != null)
            {
                var bottomPrev = _toolStrip.Bottom;
                _navigationBar.UpdateBounds(_toolStrip);
                if (_viewDropdownPanel != null && _viewDropdownPanel.Visible)
                {
                    var reserved = _viewDropdownPanel.Width + 8.0f;
                    _navigationBar.Width = Mathf.Max(_navigationBar.Width - reserved, 0.0f);
                }
                if (bottomPrev != _toolStrip.Bottom)
                {
                    // Navigation bar changed toolstrip height
                    _split.Offsets = new Margin(0, 0, _toolStrip.Bottom, 0);
                    if (_treeOnlyPanel != null)
                        _treeOnlyPanel.Offsets = new Margin(0, 0, _toolStrip.Bottom, 0);
                    PerformLayout();
                }
            }
            UpdateViewDropdownBounds();
        }

        private void UpdateViewDropdownBounds()
        {
            if (_viewDropdownPanel == null || _toolStrip == null)
                return;

            var itemHeight = _toolStrip.ItemsHeight;
            _viewDropdownPanel.Size = new Float2(itemHeight, itemHeight);
            if (_viewDropdown != null)
                _viewDropdown.Bounds = new Rectangle(0.0f, 0.0f, itemHeight, itemHeight);
        }

        private void UpdateHeaderBounds()
        {
            if (_toolStrip == null || _foldersSearchBox == null)
                return;

            var controlHeight = Style.Current.ControlHeight > 0.0f ? Style.Current.ControlHeight : 18.0f;
            var left = Mathf.Max(_importButton?.Right ?? 0.0f, _createNewButton?.Right ?? 0.0f) + 6.0f;
            var right = (_viewDropdownPanel?.Left ?? _toolStrip.Width) - 6.0f;
            var width = Mathf.Max(40.0f, right - left);
            _foldersSearchBox.Bounds = new Rectangle(left, (_toolStrip.Height - controlHeight) * 0.5f, width, controlHeight);
            _foldersSearchBox.IndexInParent = int.MaxValue;
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            // Content events
            Editor.ContentDatabase.WorkspaceModified += () => _isWorkspaceDirty = true;
            Editor.ContentDatabase.ItemAdded += OnContentDatabaseItemAdded;
            Editor.ContentDatabase.ItemRemoved += OnContentDatabaseItemRemoved;
            Editor.ContentDatabase.WorkspaceRebuilding += () => { _workspaceRebuildLocation = SelectedNode?.Path; };
            Editor.ContentDatabase.WorkspaceRebuilt += OnContentDatabaseWorkspaceRebuilt;
            Editor.ContentImporting.ImportFileBegin += OnImportFileBegin;
            Editor.ContentImporting.ImportFileEnd += OnImportFileEnd;
            Editor.ContentImporting.ImportingQueueBegin += OnImportingQueueBegin;
            Editor.ContentImporting.ImportingQueueEnd += OnImportingQueueEnd;

            LoadExpandedFolders();
            Refresh();

            // Load last viewed folder
            if (Editor.ProjectCache.TryGetCustomData(ProjectDataLastViewedFolder, out string lastViewedFolder))
            {
                if (Editor.ContentDatabase.Find(lastViewedFolder) is ContentFolder folder)
                    RunWithContentSelectionHistorySuppressed(() => _tree.Select(folder.Node));
            }

            ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
            ScriptsBuilder.ScriptsReloadEnd += OnScriptsReloadEnd;
        }

        private void OnContentDatabaseWorkspaceRebuilt()
        {
            if (_activeRenamePopup != null)
            {
                _workspaceRebuildPending = true;
                return;
            }

            _workspaceRebuildPending = false;
            var selected = Editor.ContentDatabase.Find(_workspaceRebuildLocation);
            if (selected is ContentFolder selectedFolder)
            {
                RunWithContentSelectionHistorySuppressed(() =>
                {
                    _navigationUnlocked = false;
                    RefreshView(selectedFolder.Node);
                    _tree.Select(selectedFolder.Node);
                    UpdateItemsSearch();
                    _navigationUnlocked = true;
                });
                UpdateUI();
            }
            else if (_root != null)
                RunWithContentSelectionHistorySuppressed(ShowRoot);
        }

        private void OnScriptsReloadBegin()
        {
            var lastViewedFolder = _tree.Selection.Count == 1 ? _tree.SelectedNode as ContentFolderTreeNode : null;
            _lastViewedFolderBeforeReload = lastViewedFolder?.Path ?? string.Empty;

            _tree.RemoveChild(_root);
            _root = null;
        }

        private void OnScriptsReloadEnd()
        {
            Refresh();

            if (!string.IsNullOrEmpty(_lastViewedFolderBeforeReload))
            {
                if (Editor.ContentDatabase.Find(_lastViewedFolderBeforeReload) is ContentFolder folder)
                    RunWithContentSelectionHistorySuppressed(() => _tree.Select(folder.Node));
            }

            OnFoldersSearchBoxTextChanged();
        }

        private void OnImportFileBegin(IFileEntryAction entry)
        {
            // Add to auto-select cache
            _newFilesCache ??= new List<string>();
            _newFilesCache.Add(entry.ResultUrl);
            _newFilesCacheSize++;

            var path = StringUtils.NormalizePath(entry.ResultUrl);
            if (File.Exists(path) || Directory.Exists(path))
            {
                lock (_importUndoLock)
                {
                    _importExistingOutputPaths.Add(path);
                }
            }
        }

        private void OnImportFileEnd(IFileEntryAction entry, bool failed)
        {
            if (failed)
                return;
            if (!Platform.IsInMainThread)
            {
                FlaxEngine.Scripting.InvokeOnUpdate(() => OnImportFileEnd(entry, false));
                return;
            }

            // Refresh view (gives faster response than waiting for filesystem event)
            //RefreshView(); // TODO: is this still needed?

            // Auto-select pending items
            if (_newFilesCache != null && _newFilesCache.Contains(entry.ResultUrl))
            {
                var item = EnsureItem(entry.ResultUrl);
                if (item != null)
                {
                    bool additive = _newFilesCache.Count != _newFilesCacheSize;
                    Select(item, true, additive);
                    TrackImportedItemForUndo(item);
                }
                _newFilesCache.Remove(entry.ResultUrl);
            }
        }

        private void OnImportingQueueBegin()
        {
            // Clear cache to auto-select all imported files
            _newFilesCache?.Clear();
            _newFilesCacheSize = 0;
            lock (_importUndoLock)
            {
                _importExistingOutputPaths.Clear();
                _importedItemsForUndo.Clear();
            }
        }

        private void OnImportingQueueEnd()
        {
            if (!Platform.IsInMainThread)
            {
                FlaxEngine.Scripting.InvokeOnUpdate(OnImportingQueueEnd);
                return;
            }

            List<ContentItem> items;
            lock (_importUndoLock)
            {
                if (_importedItemsForUndo.Count == 0)
                {
                    _importExistingOutputPaths.Clear();
                    return;
                }

                items = new List<ContentItem>(_importedItemsForUndo);
                _importedItemsForUndo.Clear();
                _importExistingOutputPaths.Clear();
            }

            for (int i = items.Count - 1; i >= 0; i--)
            {
                var item = items[i];
                if (item == null || Editor.ContentDatabase.Find(item.Path) == null)
                    items.RemoveAt(i);
            }

            var action = ContentItemFilesystemAction.Create(Editor, items);
            if (action != null)
                Editor.Undo.AddAction(action);
        }

        private void TrackImportedItemForUndo(ContentItem item)
        {
            var path = StringUtils.NormalizePath(item.Path);
            lock (_importUndoLock)
            {
                if (_importExistingOutputPaths.Contains(path))
                    return;
                for (int i = 0; i < _importedItemsForUndo.Count; i++)
                {
                    if (_importedItemsForUndo[i].Path.Equals(path, StringComparison.OrdinalIgnoreCase))
                        return;
                }
                _importedItemsForUndo.Add(item);
            }
        }

        private ContentItem EnsureItem(string path)
        {
            var item = Editor.ContentDatabase.Find(path);
            if (item == null)
            {
                // Cannot find the item (eg. just created file, content database event not yet handled) so refresh to take effect quickly
                var parentPath = Path.GetDirectoryName(path);
                var parentItem = Editor.ContentDatabase.Find(parentPath);
                if (parentItem != null)
                {
                    Editor.ContentDatabase.RefreshFolder(parentItem, false);
                    item = Editor.ContentDatabase.Find(path);
                }
            }
            return item;
        }

        private void Refresh()
        {
            // Setup content root node
            _root = new RootContentFolderTreeNode
            {
                ChildrenIndent = 0,
            };
            _root.Expand(true);

            // Add game project on top, plugins in the middle and engine at bottom
            _root.AddChild(Editor.ContentDatabase.Game);
            Editor.ContentDatabase.Projects.Sort();
            foreach (var project in Editor.ContentDatabase.Projects)
            {
                project.SortChildrenRecursive();
                if (project == Editor.ContentDatabase.Game || project == Editor.ContentDatabase.Engine)
                    continue;
                project.Visible = _showPluginsFiles;
                project.Folder.Visible = _showPluginsFiles;
                _root.AddChild(project);
            }
            Editor.ContentDatabase.Engine.Visible = _showEngineFiles;
            Editor.ContentDatabase.Engine.Folder.Visible = _showEngineFiles;
            _root.AddChild(Editor.ContentDatabase.Engine);

            Editor.ContentDatabase.Game?.Expand(true);
            // The internal workspace container is non-presenting; only real project folders
            // contribute visible rows.
            _tree.Margin = new Margin(0.0f, 0.0f, 0.0f, ScrollBar.DefaultSize + 2);
            _tree.AddChild(_root);

            // Setup navigation
            _navigationUnlocked = true;
            RunWithContentSelectionHistorySuppressed(ShowRoot);
            NavigationClearHistory();

            // Update UI layout
            _isLayoutLocked = false;
            PerformLayout();
            ApplyExpandedFolders();
            ApplyTreeViewMode();
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            // Handle workspace modification events but only once per frame
            if (_workspaceRebuildPending && _activeRenamePopup == null)
            {
                OnContentDatabaseWorkspaceRebuilt();
                _isWorkspaceDirty = false;
            }
            else if (ShouldRefreshWorkspace(_isWorkspaceDirty, _activeRenamePopup != null))
            {
                _isWorkspaceDirty = false;
                RefreshView();
            }

            base.Update(deltaTime);
        }

        internal static bool ShouldRefreshWorkspace(bool workspaceDirty, bool renameActive)
        {
            return workspaceDirty && !renameActive;
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            // Save last viewed folder
            ContentFolderTreeNode lastViewedFolder = null;
            if (_tree.Selection.Count == 1)
            {
                var selectedNode = _tree.SelectedNode;
                if (selectedNode is ContentItemTreeNode itemNode)
                    lastViewedFolder = itemNode.Item?.ParentFolder?.Node;
                else
                    lastViewedFolder = selectedNode as ContentFolderTreeNode;
            }
            Editor.ProjectCache.SetCustomData(ProjectDataLastViewedFolder, lastViewedFolder?.Path ?? string.Empty);

            // Clear view
            RunWithContentSelectionHistorySuppressed(() => _view.ClearItems());

            // Unlink used directories
            if (_root != null)
            {
                while (_root.HasChildren)
                {
                    _root.RemoveChild((ContentFolderTreeNode)_root.GetChild(0));
                }
            }
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (ContentMutationDiagnostics.Enabled)
            {
                var target = GetChildAtRecursive(location);
                ContentMutationDiagnostics.Log("input.window.mouse-down", $"button={button}; location={location}; target={target?.GetType().Name ?? "<none>"}; renameActive={_activeRenamePopup != null}; viewSelection={_view.Selection.Count}; treeSelection={_tree.Selection.Count}");
            }
            if (button == MouseButton.Extended1 || button == MouseButton.Extended2)
                return true;

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (ContentMutationDiagnostics.Enabled)
            {
                var target = GetChildAtRecursive(location);
                ContentMutationDiagnostics.Log("input.window.mouse-up", $"button={button}; location={location}; target={target?.GetType().Name ?? "<none>"}; renameActive={_activeRenamePopup != null}; viewSelection={_view.Selection.Count}; treeSelection={_tree.Selection.Count}");
            }
            if (button == MouseButton.Extended1 || button == MouseButton.Extended2)
                return true;

            if (button == MouseButton.Right)
            {
                // Find control that is under the mouse
                var c = GetChildAtRecursive(location);

                if (c is ContentItem item)
                {
                    if (!_view.IsSelected(item))
                    {
                        _view.Select(item);
                        _view.Focus();
                    }
                    ShowContextMenuForItem(item, ref location, false);
                }
                else if (c is ContentView)
                {
                    ShowContextMenuForItem(null, ref location, false);
                }
                else if (c is ContentItemTreeNode itemNode)
                {
                    if (!_tree.Selection.Contains(itemNode))
                        _tree.Select(itemNode);
                    ShowContextMenuForItem(itemNode.Item, ref location, false);
                }
                else if (c is ContentFolderTreeNode node)
                {
                    if (!_tree.Selection.Contains(node))
                        _tree.Select(node);
                    ShowContextMenuForItem(node.Folder, ref location, true);
                }

                return true;
            }

            if (button == MouseButton.Left)
            {
                // Find control that is under the mouse
                var c = GetChildAtRecursive(location);
                if (c is ContentView)
                {
                    _view.ClearSelection();
                    return true;
                }
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        protected override void PerformLayoutAfterChildren()
        {
            base.PerformLayoutAfterChildren();

            UpdateNavigationBarBounds();
            UpdateHeaderBounds();
            // Dock proxies can rebuild their child order after resizing or view-density
            // changes. Keep toolbar overlays above the split content on every layout pass.
            if (_foldersSearchBox != null)
                _foldersSearchBox.IndexInParent = int.MaxValue;
            if (_searchHintsPanel != null && _searchHintsPanel.Visible)
                _searchHintsPanel.IndexInParent = int.MaxValue;
        }

        /// <inheritdoc />
        public override bool UseLayoutData => true;

        /// <inheritdoc />
        public override void OnLayoutSerialize(XmlWriter writer)
        {
            LayoutSerializeSplitter(writer, "Split", _split);
            writer.WriteAttributeString("Scale", _view.ViewScale.ToString(CultureInfo.InvariantCulture));
            writer.WriteAttributeString("ShowFileExtensions", _view.ShowFileExtensions.ToString());
            writer.WriteAttributeString("ShowEngineFiles", ShowEngineFiles.ToString());
            writer.WriteAttributeString("ShowPluginsFiles", ShowPluginsFiles.ToString());
            writer.WriteAttributeString("ShowAllFiles", ShowAllFiles.ToString());
            writer.WriteAttributeString("ShowGeneratedFiles", ShowGeneratedFiles.ToString());
            writer.WriteAttributeString("ViewType", _view.ViewType.ToString());
            writer.WriteAttributeString("TreeViewAllContent", _showAllContentInTree.ToString());
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            LayoutDeserializeSplitter(node, "Split", _split);
            if (float.TryParse(node.GetAttribute("Scale"), CultureInfo.InvariantCulture, out var value1))
                _view.ViewScale = value1;
            if (bool.TryParse(node.GetAttribute("ShowFileExtensions"), out bool value2))
                _view.ShowFileExtensions = value2;
            if (bool.TryParse(node.GetAttribute("ShowEngineFiles"), out value2))
                ShowEngineFiles = value2;
            if (bool.TryParse(node.GetAttribute("ShowPluginsFiles"), out value2))
                ShowPluginsFiles = value2;
            if (bool.TryParse(node.GetAttribute("ShowAllFiles"), out value2))
                ShowAllFiles = value2;
            if (bool.TryParse(node.GetAttribute("ShowGeneratedFiles"), out value2))
                ShowGeneratedFiles = value2;
            if (Enum.TryParse(node.GetAttribute("ViewType"), out ContentViewType viewType))
                _view.ViewType = viewType;
            if (bool.TryParse(node.GetAttribute("TreeViewAllContent"), out value2))
                _showAllContentInTree = value2;
            ApplyTreeViewMode();
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize()
        {
            _split.SplitterValue = 0.2f;
            _view.ViewScale = 1.0f;
            _showAllContentInTree = false;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (_view != null)
                _view.SelectionChanged -= OnContentViewSelectionChanged;
            if (Editor?.SceneEditing != null)
                Editor.SceneEditing.SelectionChanged -= OnSceneSelectionChanged;

            _foldersSearchBox = null;
            _itemsSearchBox = null;
            _viewDropdown = null;
            _viewDropdownPanel = null;
            _treePanelRoot = null;
            _treeHeaderPanel = null;
            _treeOnlyPanel = null;
            _contentItemsSearchPanel = null;
            _newFilesCache = null;

            Editor.Options.OptionsChanged -= OnOptionsChanged;
            ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;
            ScriptsBuilder.ScriptsReloadEnd -= OnScriptsReloadEnd;
            if (Editor?.ContentDatabase != null)
            {
                Editor.ContentDatabase.ItemAdded -= OnContentDatabaseItemAdded;
                Editor.ContentImporting.ImportFileBegin -= OnImportFileBegin;
                Editor.ContentImporting.ImportFileEnd -= OnImportFileEnd;
                Editor.ContentImporting.ImportingQueueBegin -= OnImportingQueueBegin;
                Editor.ContentImporting.ImportingQueueEnd -= OnImportingQueueEnd;
            }

            base.OnDestroy();
        }
    }
}
