// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Content;
using FlaxEditor.Content.Settings;
using FlaxEditor.CustomEditors;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.GUI.Tabs;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Json;
using FlaxEngine.Utilities;

namespace FlaxEditor.Windows.Assets
{
    /// <summary>
    /// Editor window to view/modify <see cref="JsonAsset"/> asset.
    /// </summary>
    /// <seealso cref="JsonAsset" />
    /// <seealso cref="FlaxEditor.Windows.Assets.AssetEditorWindow" />
    public sealed class JsonAssetWindow : AssetEditorWindowBase<JsonAsset>
    {
        private sealed class GameSettingsGeneral
        {
            private readonly GameSettings _settings;

            public GameSettingsGeneral(GameSettings settings)
            {
                _settings = settings;
            }

            [EditorOrder(0), EditorDisplay("General")]
            public string ProductName
            {
                get => _settings.ProductName;
                set => _settings.ProductName = value;
            }

            [EditorOrder(10), EditorDisplay("General")]
            public string CompanyName
            {
                get => _settings.CompanyName;
                set => _settings.CompanyName = value;
            }

            [EditorOrder(15), EditorDisplay("General")]
            public string CopyrightNotice
            {
                get => _settings.CopyrightNotice;
                set => _settings.CopyrightNotice = value;
            }

            [EditorOrder(20), EditorDisplay("General")]
            public string Version
            {
                get => _settings.Version;
                set => _settings.Version = value;
            }

            [EditorOrder(30), EditorDisplay("General"), Tooltip("The default icon of the application.")]
            public Texture Icon
            {
                get => _settings.Icon;
                set => _settings.Icon = value;
            }

            [EditorOrder(900), EditorDisplay("Startup"), Tooltip("Reference to the first scene to load on a game startup.")]
            public SceneReference FirstScene
            {
                get => _settings.FirstScene;
                set => _settings.FirstScene = value;
            }

            [EditorOrder(910), EditorDisplay("Startup", "No Splash Screen"), Tooltip("True if skip showing splash screen image on the game startup.")]
            public bool NoSplashScreen
            {
                get => _settings.NoSplashScreen;
                set => _settings.NoSplashScreen = value;
            }

            [EditorOrder(920), EditorDisplay("Startup"), Tooltip("Reference to the splash screen image to show on a game startup.")]
            public Texture SplashScreen
            {
                get => _settings.SplashScreen;
                set => _settings.SplashScreen = value;
            }

        }

        private sealed class GameSettingsPage
        {
            public Tab Tab;
            public CustomEditorPresenter Presenter;
            public JsonAsset Asset;
            public object Value;
            public bool EditsMainObject;
        }

        private class ObjectPasteUndo : IUndoAction
        {
            /// <inheritdoc />
            public string ActionString => "Object Paste Undo";

            private JsonAssetWindow _window;
            private string _oldObject;
            private string _newObject;

            public ObjectPasteUndo(object oldObject, object newObject, JsonAssetWindow window)
            {
                _oldObject = JsonSerializer.Serialize(oldObject);
                _newObject = JsonSerializer.Serialize(newObject);
                _window = window;
            }

            /// <inheritdoc />
            public void Dispose()
            {
                _oldObject = null;
                _newObject = null;
                _window = null;
            }

            /// <inheritdoc />
            public void Do()
            {
                if (!string.IsNullOrEmpty(_newObject))
                {
                    _window._object = JsonSerializer.Deserialize(_newObject, TypeUtils.GetType(_window.Asset.DataTypeName).Type);
                    _window.SelectMainObject();
                    _window.OnMainObjectModified();
                }
            }

            /// <inheritdoc />
            public void Undo()
            {
                if (!string.IsNullOrEmpty(_oldObject))
                {
                    _window._object = JsonSerializer.Deserialize(_oldObject, TypeUtils.GetType(_window.Asset.DataTypeName).Type);
                    _window.SelectMainObject();
                    _window.OnMainObjectModified();
                }
            }
        }
        
        private const float AutoSavePanelWidth = 96.0f;
        private const float AutoSaveLabelWidth = 70.0f;
        private const string AutoSaveTooltip = "Automatically saves modified settings 300 ms after values are applied.";

        private readonly CustomEditorPresenter _presenter;
        private SearchBox _searchBox;
        private Panel _scrollingPanel;
        private readonly ToolStripButton _saveButton;
        private readonly ToolStripButton _undoButton;
        private readonly ToolStripButton _redoButton;
        private readonly Undo _undo;
        private readonly bool _isSettingsAsset;
        private readonly bool _isGameSettingsAsset;
        private readonly CheckBox _autoSaveCheckBox;
        private readonly Tabs _gameSettingsTabs;
        private readonly List<GameSettingsPage> _gameSettingsPages = new List<GameSettingsPage>();
        private readonly Dictionary<JsonAsset, object> _dirtyGameSettingsAssets = new Dictionary<JsonAsset, object>();
        private object _object;
        private bool _isMainObjectDirty;
        private bool _gameSettingsPagesRefreshPending;
        private int _gameSettingsPagesSignature;
        private bool _isRegisteredForScriptsReload;
        private bool _pendingAutoSave;
        private bool _isAutoSaving;
        private Label _typeText;
        private ToolStripButton _optionsButton;
        private ContextMenu _optionsCM;

        /// <summary>
        /// Gets the instance of the Json asset object that is being edited.
        /// </summary>
        public object Instance => _object;

        /// <inheritdoc />
        public JsonAssetWindow(Editor editor, AssetItem item)
        : base(editor, item)
        {
            var inputOptions = Editor.Options.Options.Input;
            _isSettingsAsset = editor.ContentDatabase.GetProxy(item) is SettingsProxy;
            _isGameSettingsAsset = item is JsonAssetItem jsonAssetItem && jsonAssetItem.TypeName == typeof(GameSettings).FullName;

            // Undo
            _undo = new Undo(Editor.Undo, this);
            _undo.UndoDone += OnUndoRedo;
            _undo.RedoDone += OnUndoRedo;
            _undo.ActionDone += OnUndoRedo;

            // Toolstrip
            _saveButton = _toolstrip.AddButton(editor.Icons.Save64, Save).LinkTooltip("Save", ref inputOptions.Save);
            _toolstrip.AddSeparator();
            _undoButton = _toolstrip.AddButton(Editor.Icons.Undo64, _undo.PerformUndo).LinkTooltip("Undo", ref inputOptions.Undo);
            _redoButton = _toolstrip.AddButton(Editor.Icons.Redo64, _undo.PerformRedo).LinkTooltip("Redo", ref inputOptions.Redo);

            // Header panel for search
            var headerPanel = new ContainerControl
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                BackgroundColor = Style.Current.Background,
                IsScrollable = false,
                Offsets = new Margin(0, 0, _toolstrip.Bottom, 18 + 6),
                Parent = this,
            };
            float searchRightMargin = _isSettingsAsset ? AutoSavePanelWidth : 4.0f;
            _searchBox = new SearchBox
            {
                AnchorPreset = AnchorPresets.HorizontalStretchMiddle,
                Parent = headerPanel,
                Bounds = new Rectangle(4, 4, Mathf.Max(32.0f, headerPanel.Width - 4 - searchRightMargin), 18),
            };
            _searchBox.TextChanged += ApplySearchFilter;
            if (_isSettingsAsset)
            {
                var autoSaveLabel = new Label
                {
                    AnchorPreset = AnchorPresets.TopRight,
                    Bounds = new Rectangle(0, 4, AutoSaveLabelWidth, 18),
                    HorizontalAlignment = TextAlignment.Far,
                    Parent = headerPanel,
                    Text = "Auto Save",
                    TooltipText = AutoSaveTooltip,
                };
                autoSaveLabel.LocalX -= AutoSavePanelWidth;
                _autoSaveCheckBox = new CheckBox(0, 4, true, 18)
                {
                    AnchorPreset = AnchorPresets.TopRight,
                    Parent = headerPanel,
                    TooltipText = AutoSaveTooltip,
                };
                _autoSaveCheckBox.LocalX -= (_autoSaveCheckBox.Width + 4);
                _autoSaveCheckBox.StateChanged += OnAutoSaveCheckBoxStateChanged;
            }

            if (_isGameSettingsAsset)
            {
                _gameSettingsTabs = new Tabs
                {
                    Orientation = Orientation.Vertical,
                    AnchorPreset = AnchorPresets.StretchAll,
                    Offsets = new Margin(0, 0, headerPanel.Bottom, 0),
                    TabsSize = new Float2(120, 32),
                    UseScroll = true,
                    Parent = this,
                };
                _gameSettingsTabs.SelectedTabChanged += OnGameSettingsSelectedTabChanged;

                var generalTab = _gameSettingsTabs.AddTab(new Tab("General"));
                _scrollingPanel = new Panel(ScrollBars.Vertical)
                {
                    AnchorPreset = AnchorPresets.StretchAll,
                    Offsets = Margin.Zero,
                    Parent = generalTab,
                };
            }
            else
            {
                _scrollingPanel = new Panel(ScrollBars.Vertical)
                {
                    AnchorPreset = AnchorPresets.StretchAll,
                    Offsets = new Margin(0, 0, headerPanel.Bottom, 0),
                    Parent = this,
                };
            }

            // Properties
            _presenter = new CustomEditorPresenter(_undo, "Loading...");
            _presenter.Panel.Parent = _scrollingPanel;
            _presenter.Modified += OnMainObjectModified;
            _presenter.AfterLayout += OnPresenterAfterLayout;

            // Setup input actions
            InputActions.Add(options => options.Undo, _undo.PerformUndo);
            InputActions.Add(options => options.Redo, _undo.PerformRedo);
        }

        private void OnUndoRedo(IUndoAction action)
        {
            if (!UndoActionMetadata.IsSelectionOnly(action))
            {
                if (_isGameSettingsAsset)
                {
                    if (_gameSettingsTabs.SelectedTab?.Text == "General")
                    {
                        _isMainObjectDirty = true;
                    }
                    else
                    {
                        foreach (var page in _gameSettingsPages)
                        {
                            if (page.Tab == _gameSettingsTabs.SelectedTab)
                            {
                                if (page.EditsMainObject)
                                {
                                    _isMainObjectDirty = true;
                                    _gameSettingsPagesRefreshPending = true;
                                }
                                else if (page.Asset && page.Value != null)
                                    _dirtyGameSettingsAssets[page.Asset] = page.Value;
                                break;
                            }
                        }
                    }
                }
                OnObjectModified();
            }
            UpdateToolstrip();
        }

        private void OnMainObjectModified()
        {
            _isMainObjectDirty = true;
            OnObjectModified();
        }

        private void OnGameSettingsPageModified(GameSettingsPage page)
        {
            if (page.Asset && page.Value != null)
                _dirtyGameSettingsAssets[page.Asset] = page.Value;
            OnObjectModified();
        }

        private void OnGameSettingsAssignmentsModified()
        {
            _gameSettingsPagesRefreshPending = true;
            OnMainObjectModified();
        }

        private GameSettingsPage AddGameSettingsPage(string name, JsonAsset asset, bool showIfMissing = true)
        {
            if (!asset && !showIfMissing)
                return null;

            var tab = _gameSettingsTabs.AddTab(new Tab(name));
            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = tab,
            };
            var presenter = new CustomEditorPresenter(_undo, asset ? "Loading..." : $"Missing {name} settings asset.");
            presenter.Panel.Parent = panel;

            var page = new GameSettingsPage
            {
                Tab = tab,
                Presenter = presenter,
                Asset = asset,
            };
            presenter.Modified += () => OnGameSettingsPageModified(page);
            presenter.AfterLayout += layout =>
            {
                if (_gameSettingsTabs.SelectedTab == tab)
                    ApplySearchFilter();
            };

            if (asset && !asset.WaitForLoaded())
            {
                page.Value = asset.Instance;
                presenter.Select(page.Value);
            }

            _gameSettingsPages.Add(page);
            return page;
        }

        private GameSettingsPage AddGameSettingsObjectPage(string name, object value, string groupFilter)
        {
            var tab = _gameSettingsTabs.AddTab(new Tab(name));
            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                Parent = tab,
            };
            var presenter = new CustomEditorPresenter(_undo);
            presenter.Panel.Parent = panel;

            var page = new GameSettingsPage
            {
                Tab = tab,
                Presenter = presenter,
                Value = value,
                EditsMainObject = true,
            };
            presenter.Modified += OnGameSettingsAssignmentsModified;
            presenter.AfterLayout += layout =>
            {
                if (_gameSettingsTabs.SelectedTab == tab)
                    ApplySearchFilter();
            };
            presenter.Select(value);
            presenter.ApplyGroupFilter(groupFilter);

            _gameSettingsPages.Add(page);
            return page;
        }

        private void ClearGameSettingsPages()
        {
            if (_gameSettingsTabs == null)
                return;

            _gameSettingsTabs.SelectedTabIndex = 0;
            foreach (var page in _gameSettingsPages)
            {
                page.Presenter.Deselect();
                page.Tab.Dispose();
            }
            _gameSettingsPages.Clear();
        }

        private static int GetGameSettingsPagesSignature(GameSettings settings)
        {
            int signature = 17;
            void AddAsset(JsonAsset asset)
            {
                signature = unchecked(signature * 31 + (asset ? asset.ID.GetHashCode() : 0));
            }

            AddAsset(settings.Time);
            AddAsset(settings.Audio);
            AddAsset(settings.LayersAndTags);
            AddAsset(settings.Physics);
            AddAsset(settings.Input);
            AddAsset(settings.Graphics);
            AddAsset(settings.Network);
            AddAsset(settings.Navigation);
            AddAsset(settings.Localization);
            AddAsset(settings.GameCooking);
            AddAsset(settings.Streaming);
            AddAsset(settings.AssetPipeline);
            AddAsset(settings.WindowsPlatform);
            AddAsset(settings.UWPPlatform);
            AddAsset(settings.LinuxPlatform);
            AddAsset(settings.PS4Platform);
            AddAsset(settings.XboxOnePlatform);
            AddAsset(settings.XboxScarlettPlatform);
            AddAsset(settings.AndroidPlatform);
            AddAsset(settings.SwitchPlatform);
            AddAsset(settings.PS5Platform);
            AddAsset(settings.MacPlatform);
            AddAsset(settings.iOSPlatform);
            AddAsset(settings.WebPlatform);

            if (settings.CustomSettings != null)
            {
                foreach (var customSettings in settings.CustomSettings)
                {
                    signature = unchecked(signature * 31 + customSettings.Key.GetHashCode());
                    AddAsset(customSettings.Value);
                }
            }

            return signature;
        }

        private void SetupGameSettingsPages(GameSettings settings, string selectedTabName = null)
        {
            ClearGameSettingsPages();

            AddGameSettingsPage("Time", settings.Time);
            AddGameSettingsPage("Audio", settings.Audio);
            AddGameSettingsPage("Layers and Tags", settings.LayersAndTags);
            AddGameSettingsPage("Physics", settings.Physics);
            AddGameSettingsPage("Input", settings.Input);
            AddGameSettingsPage("Graphics", settings.Graphics);
            AddGameSettingsPage("Network", settings.Network);
            AddGameSettingsPage("Navigation", settings.Navigation);
            AddGameSettingsPage("Localization", settings.Localization);
            AddGameSettingsPage("Game Cooking", settings.GameCooking);
            AddGameSettingsPage("Streaming", settings.Streaming);
            AddGameSettingsPage("Asset Pipeline", settings.AssetPipeline);

            // Keep asset assignment available without duplicating those references on the General page.
            AddGameSettingsObjectPage("Settings Assets", settings, "Other Settings");

            if (settings.CustomSettings != null)
            {
                foreach (var customSettings in settings.CustomSettings)
                    AddGameSettingsPage("Custom: " + customSettings.Key, customSettings.Value);
            }

            AddGameSettingsObjectPage("Platform Assets", settings, "Platform Settings");
            AddGameSettingsPage("Windows", settings.WindowsPlatform, false);
            AddGameSettingsPage("UWP", settings.UWPPlatform, false);
            AddGameSettingsPage("Linux", settings.LinuxPlatform, false);
            AddGameSettingsPage("PlayStation 4", settings.PS4Platform, false);
            AddGameSettingsPage("Xbox One", settings.XboxOnePlatform, false);
            AddGameSettingsPage("Xbox Scarlett", settings.XboxScarlettPlatform, false);
            AddGameSettingsPage("Android", settings.AndroidPlatform, false);
            AddGameSettingsPage("Switch", settings.SwitchPlatform, false);
            AddGameSettingsPage("PlayStation 5", settings.PS5Platform, false);
            AddGameSettingsPage("Mac", settings.MacPlatform, false);
            AddGameSettingsPage("iOS", settings.iOSPlatform, false);
            AddGameSettingsPage("Web", settings.WebPlatform, false);

            if (!string.IsNullOrEmpty(selectedTabName))
            {
                for (int i = 0; i < _gameSettingsTabs.ChildrenCount; i++)
                {
                    if (_gameSettingsTabs.GetChild(i) is Tab tab && tab.Text == selectedTabName)
                    {
                        _gameSettingsTabs.SelectedTab = tab;
                        break;
                    }
                }
            }

            _gameSettingsPagesSignature = GetGameSettingsPagesSignature(settings);
        }

        private void SelectMainObject()
        {
            if (_isGameSettingsAsset && _object is GameSettings settings)
            {
                _presenter.Select(new GameSettingsGeneral(settings));
                SetupGameSettingsPages(settings);
            }
            else
            {
                _presenter.Select(_object);
            }
        }

        private void OnGameSettingsSelectedTabChanged(Tabs tabs)
        {
            ApplySearchFilter();
        }

        private bool IsAutoSaveEnabled => _isSettingsAsset && _autoSaveCheckBox != null && _autoSaveCheckBox.Checked;

        /// <inheritdoc />
        public override bool CanRunAutoSave => !IsAutoSaveEnabled || !_pendingAutoSave || IsAutoSaveEditDelayElapsed;

        private void OnObjectModified()
        {
            MarkAsEdited();
            RequestAutoSave();
        }

        private void RequestAutoSave()
        {
            _pendingAutoSave = true;
            MarkAutoSaveEdit();
        }

        private void AutoSaveIfNeeded()
        {
            if (!IsAutoSaveEnabled || !IsEdited || !_pendingAutoSave || _isAutoSaving)
                return;

            if (!IsAutoSaveEditDelayElapsed)
                return;

            _pendingAutoSave = false;
            _isAutoSaving = true;
            try
            {
                Save();
            }
            catch (Exception ex)
            {
                Editor.LogWarning(ex);
                Editor.LogError("Cannot auto-save settings asset. See log for more.");
            }
            finally
            {
                _isAutoSaving = false;
            }
        }

        private void OnAutoSaveCheckBoxStateChanged(CheckBox box)
        {
            if (box.Checked)
                RequestAutoSave();
            else
                _pendingAutoSave = false;
        }

        /// <inheritdoc />
        protected override void OnScriptsReloadBegin()
        {
            base.OnScriptsReloadBegin();
            Close();
        }

        /// <inheritdoc />
        public override void Save()
        {
            if (!IsEdited)
                return;
            if (_asset.WaitForLoaded())
                return;

            var dirtyAssets = _isGameSettingsAsset
                                  ? new List<KeyValuePair<JsonAsset, object>>(_dirtyGameSettingsAssets)
                                  : null;
            bool failed = false;
            if (!_isGameSettingsAsset || _isMainObjectDirty)
            {
                if (Editor.SaveJsonAsset(_item.Path, _object))
                    failed = true;
                else
                    _isMainObjectDirty = false;
            }

            if (_isGameSettingsAsset)
            {
                foreach (var dirtyAsset in dirtyAssets)
                {
                    if (!dirtyAsset.Key || dirtyAsset.Value == null || Editor.SaveJsonAsset(dirtyAsset.Key.Path, dirtyAsset.Value))
                    {
                        failed = true;
                    }
                    else
                    {
                        _dirtyGameSettingsAssets.Remove(dirtyAsset.Key);
                    }
                }
            }

            if (failed)
            {
                Editor.LogError("Cannot save asset.");
                return;
            }

            ClearEditedFlag();
        }

        /// <inheritdoc />
        protected override void UpdateToolstrip()
        {
            _saveButton.Enabled = IsEdited;
            _undoButton.Enabled = _undo.CanUndo;
            _redoButton.Enabled = _undo.CanRedo;

            base.UpdateToolstrip();
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);

            if (_isGameSettingsAsset && _object is GameSettings gameSettings)
            {
                if (_gameSettingsPagesSignature != GetGameSettingsPagesSignature(gameSettings))
                    _gameSettingsPagesRefreshPending = true;
                if (_gameSettingsPagesRefreshPending)
                {
                    _gameSettingsPagesRefreshPending = false;
                    SetupGameSettingsPages(gameSettings, _gameSettingsTabs.SelectedTab?.Text);
                }
            }

            AutoSaveIfNeeded();
        }

        /// <inheritdoc />
        protected override void OnAssetLoaded()
        {
            _object = Asset.Instance;
            if (_object == null)
            {
                // Hint developer about cause of failure
                var dataTypeName = Asset.DataTypeName;
                var type = Type.GetType(dataTypeName);
                if (type != null)
                {
                    try
                    {
                        var obj = Activator.CreateInstance(type);
                        var data = Asset.Data;
                        FlaxEngine.Json.JsonSerializer.Deserialize(obj, data);
                    }
                    catch (Exception ex)
                    {
                        _presenter.NoSelectionText = "Failed to load asset. See log for more. " + ex.Message.Replace('\n', ' ');
                    }
                }
                else if (string.IsNullOrEmpty(dataTypeName))
                {
                    _presenter.NoSelectionText = "Empty data type.";
                }
                else
                {
                    _presenter.NoSelectionText = string.Format("Missing type '{0}'.", dataTypeName);
                }
            }
            SelectMainObject();

            if (_typeText != null)
                _typeText.Dispose();

            // Get content item for options button
            object buttonTag = null;
            var allTypes = Editor.CodeEditing.All.Get();
            foreach (var type in allTypes)
            {
                if (type.TypeName.Equals(Asset.DataTypeName, StringComparison.Ordinal))
                {
                    buttonTag = type.ContentItem;
                    break;
                }
            }

            _optionsButton = new ToolStripButton(_toolstrip.ItemsHeight, ref Editor.Icons.Settings12)
            {
                AnchorPreset = AnchorPresets.TopRight,
                Tag = buttonTag,
                Size = new Float2(18),
                Parent = this,
            };
            _optionsButton.LocalX -= (_optionsButton.Width + 4);
            _optionsButton.LocalY += (_toolstrip.Height - _optionsButton.Height) * 0.5f;
            _optionsButton.Clicked += OpenOptionsContextMenu;

            var typeText = new ClickableLabel
            {
                Text = $"{Asset.DataTypeName}",
                TooltipText = "Asset data type (full name)",
                Pivot = Float2.Zero,
                AnchorPreset = AnchorPresets.TopRight,
                AutoWidth = true,
                Parent = this,
            };
            typeText.LocalX += -(typeText.Width + _optionsButton.Width + 8);
            typeText.LocalY += (_toolstrip.Height - typeText.Height) * 0.5f;
            _typeText = typeText;

            _undo.Clear();
            _isMainObjectDirty = false;
            _gameSettingsPagesRefreshPending = false;
            _dirtyGameSettingsAssets.Clear();
            ClearEditedFlag();

            // Auto-close on scripting reload if json asset is from game scripts (it might be reloaded)
            if ((_object == null || FlaxEngine.Scripting.IsTypeFromGameScripts(_object.GetType())) && !_isRegisteredForScriptsReload)
            {
                _isRegisteredForScriptsReload = true;
                ScriptsBuilder.ScriptsReloadBegin += OnScriptsReloadBegin;
            }

            base.OnAssetLoaded();
        }

        private void OpenOptionsContextMenu()
        {
            if (_optionsCM != null)
            {
                _optionsCM.Hide();
                _optionsCM.Dispose();
            }
            
            _optionsCM = new ContextMenu();
            _optionsCM.AddButton("Copy type name", () => Clipboard.Text = Asset.DataTypeName);
            _optionsCM.AddButton("Copy asset data", () => Clipboard.Text = Asset.Data);
            _optionsCM.AddButton("Paste asset data", () =>
            {
                if (!string.IsNullOrEmpty(Clipboard.Text))
                {
                    var dataTypeName = Asset.DataTypeName;
                    var type = TypeUtils.GetType(dataTypeName);
                    if (type != null)
                    {
                        try
                        {
                            var obj = Activator.CreateInstance(type.Type);
                            var data = Clipboard.Text;
                            JsonSerializer.Deserialize(obj, data);
                            if (obj != null)
                            {
                                var undoAction = new ObjectPasteUndo(_object, obj, this);
                                undoAction.Do();
                                _undo.AddAction(undoAction);
                            }
                            else
                            {
                                Editor.LogWarning("Pasted data is not the correct data type or has incomplete data");
                            }
                        }
                        catch (Exception ex)
                        {
                            Editor.LogWarning($"Pasted data is not the correct data type or has incomplete data. Exception: {ex}");
                        }
                    }
                }
            });
            _optionsCM.Enabled = !string.IsNullOrEmpty(Clipboard.Text);
            _optionsCM.AddSeparator();
            if (_optionsButton.Tag is ContentItem item)
            {
                _optionsCM.AddButton("Edit asset code", () =>
                {
                    Editor.Instance.ContentEditing.Open(item);
                });
                _optionsCM.AddButton("Show asset code item in Project", () =>
                {
                    Editor.Instance.Windows.ContentWin.Select(item);
                });
            }
            
            _optionsCM.Show(_optionsButton, _optionsButton.PointFromScreen(Input.MouseScreenPosition));
        }

        /// <inheritdoc />
        protected override void OnAssetLoadFailed()
        {
            _presenter.NoSelectionText = "Failed to load the asset.";

            base.OnAssetLoadFailed();
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            base.OnLostFocus();
            _optionsCM?.Dispose();
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            base.OnExit();
            _optionsCM?.Dispose();
        }

        /// <inheritdoc />
        public override void OnItemReimported(ContentItem item)
        {
            // Refresh the properties (will get new data in OnAssetLoaded)
            _presenter.Deselect();
            ClearGameSettingsPages();
            _isMainObjectDirty = false;
            _gameSettingsPagesRefreshPending = false;
            _dirtyGameSettingsAssets.Clear();
            ClearEditedFlag();

            base.OnItemReimported(item);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;
            base.OnDestroy();

            if (_isRegisteredForScriptsReload)
            {
                _isRegisteredForScriptsReload = false;
                ScriptsBuilder.ScriptsReloadBegin -= OnScriptsReloadBegin;
            }
            _optionsCM?.Dispose();
            _gameSettingsPages.Clear();
            _dirtyGameSettingsAssets.Clear();
            _typeText = null;
        }

        private void OnPresenterAfterLayout(LayoutElementsContainer layout)
        {
            ApplySearchFilter();
        }

        private void ApplySearchFilter()
        {
            // Adding the initial General tab fires SelectedTabChanged before the presenter is created.
            if (_presenter == null)
                return;

            if (_gameSettingsTabs != null && _gameSettingsTabs.SelectedTab?.Text != "General")
            {
                foreach (var page in _gameSettingsPages)
                {
                    if (page.Tab == _gameSettingsTabs.SelectedTab)
                    {
                        page.Presenter.ApplySearchFilter(_searchBox.Text);
                        return;
                    }
                }
            }

            _presenter.ApplySearchFilter(_searchBox.Text);
        }
    }
}
