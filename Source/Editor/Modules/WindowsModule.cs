// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Xml;
using FlaxEditor.Content;
using FlaxEditor.GUI.Dialogs;
using FlaxEditor.History;
using FlaxEditor.Windows;
using FlaxEditor.Windows.Assets;
using FlaxEditor.Windows.Profiler;
using FlaxEngine;
using FlaxEngine.Assertions;
using FlaxEngine.GUI;
using DockPanel = FlaxEditor.GUI.Docking.DockPanel;
using DockState = FlaxEditor.GUI.Docking.DockState;
using FloatWindowDockPanel = FlaxEditor.GUI.Docking.FloatWindowDockPanel;
using Window = FlaxEngine.Window;

namespace FlaxEditor.Modules
{
    /// <summary>
    /// Manages Editor windows and popups.
    /// </summary>
    /// <seealso cref="FlaxEditor.Modules.EditorModule" />
    public sealed class WindowsModule : EditorModule
    {
        private DateTime _lastLayoutSaveTime;
        private float _projectIconScreenshotTimeout = -1;
        private string _windowsLayoutPath;
        private WindowNavigationContext _lastNavigationWindowContext;
        private WindowNavigationContext _pendingContentBounceSource;
        private WindowNavigationContext _pendingContentBounceTarget;
        private string _contentOpenNavigationTargetTypename;
        private bool _lastAssetFocusWasContentOpen;
        private bool _pendingContentBounceWasContentOpen;
        private bool _suppressWindowNavigation;

        private static readonly bool LogFocusRecovery = false;
        private static readonly bool LogWindowNavigation = true;

        private struct WindowRestoreData
        {
            public string AssemblyName;
            public string TypeName;

            public DockState DockState;
            public DockPanel DockedTo;
            public int DockedTabIndex;
            public float? SplitterValue = null;

            public bool SelectOnShow = false;

            public bool Maximize;
            public bool Minimize;
            public Float2 FloatSize;
            public Float2 FloatPosition;

            public Guid AssetItemID;

            // Constructor, to allow for default values
            public WindowRestoreData()
            {
            }
        }

        private readonly List<WindowRestoreData> _restoreWindows = new List<WindowRestoreData>();
        private readonly List<EditorWindow> _focusedWindows = new List<EditorWindow>(32);
        private readonly NavigationHistory _windowNavigationHistory = new NavigationHistory();

        private struct WindowNavigationContext : IEquatable<WindowNavigationContext>
        {
            public readonly string Typename;
            public readonly string Title;
            public readonly bool IsAssetDocument;

            public bool IsValid => !string.IsNullOrEmpty(Typename);

            public WindowNavigationContext(string typename, string title, bool isAssetDocument)
            {
                Typename = typename;
                Title = title;
                IsAssetDocument = isAssetDocument;
            }

            public bool Equals(WindowNavigationContext other)
            {
                return string.Equals(Typename, other.Typename, StringComparison.OrdinalIgnoreCase);
            }

            public override bool Equals(object obj)
            {
                return obj is WindowNavigationContext other && Equals(other);
            }

            public override int GetHashCode()
            {
                return StringComparer.OrdinalIgnoreCase.GetHashCode(Typename ?? string.Empty);
            }
        }

        private sealed class WindowNavigationAction : INavigationHistoryAction, INavigationHistoryDestination
        {
            private readonly WindowsModule _windows;
            private readonly WindowNavigationContext _source;
            private readonly WindowNavigationContext _target;

            public WindowNavigationAction(WindowsModule windows, WindowNavigationContext source, WindowNavigationContext target)
            {
                _windows = windows;
                _source = source;
                _target = target;
            }

            public object Owner => _windows;

            public string ActionString => string.Format("Window change: {0} -> {1}", _source.Title, _target.Title);

            public WindowNavigationContext Source => _source;

            public WindowNavigationContext Target => _target;

            public bool IsSameDestination(INavigationHistoryAction other)
            {
                return other is WindowNavigationAction action && _target.Equals(action._target);
            }

            public bool Contains(string typename)
            {
                return string.Equals(_source.Typename, typename, StringComparison.OrdinalIgnoreCase) ||
                       string.Equals(_target.Typename, typename, StringComparison.OrdinalIgnoreCase);
            }

            public void NavigateBack()
            {
                _windows.NavigateToWindowContext(_source);
            }

            public void NavigateForward()
            {
                _windows.NavigateToWindowContext(_target);
            }

            public void Dispose()
            {
            }
        }

        /// <summary>
        /// The main editor window.
        /// </summary>
        public Window MainWindow { get; private set; }

        /// <summary>
        /// Occurs when main editor window is being closed.
        /// </summary>
        public event Action MainWindowClosing;

        /// <summary>
        /// Gets a value indicating whether window navigation can travel back.
        /// </summary>
        public bool CanNavigateBack => HasPendingContentBounce || _windowNavigationHistory.CanGoBack;

        /// <summary>
        /// Gets a value indicating whether window navigation can travel forward.
        /// </summary>
        public bool CanNavigateForward => _windowNavigationHistory.CanGoForward;

        /// <summary>
        /// The content window.
        /// </summary>
        public ContentWindow ContentWin;

        /// <summary>
        /// The edit game window.
        /// </summary>
        public EditGameWindow EditWin;

        /// <summary>
        /// The game window.
        /// </summary>
        public GameWindow GameWin;

        /// <summary>
        /// The properties window.
        /// </summary>
        public PropertiesWindow PropertiesWin;

        /// <summary>
        /// The scene tree window.
        /// </summary>
        public SceneTreeWindow SceneWin;

        /// <summary>
        /// The debug log window.
        /// </summary>
        public DebugLogWindow DebugLogWin;

        /// <summary>
        /// The output log window.
        /// </summary>
        public OutputLogWindow OutputLogWin;

        /// <summary>
        /// The toolbox window.
        /// </summary>
        public ToolboxWindow ToolboxWin;

        /// <summary>
        /// The graphics quality window.
        /// </summary>
        public GraphicsQualityWindow GraphicsQualityWin;

        /// <summary>
        /// The game cooker window.
        /// </summary>
        public GameCookerWindow GameCookerWin;

        /// <summary>
        /// The profiler window.
        /// </summary>
        public ProfilerWindow ProfilerWin;

        /// <summary>
        /// The editor options window.
        /// </summary>
        public EditorOptionsWindow EditorOptionsWin;

        /// <summary>
        /// The plugins manager window.
        /// </summary>
        public PluginsWindow PluginsWin;

        /// <summary>
        /// The Visual Script debugger window.
        /// </summary>
        public VisualScriptDebuggerWindow VisualScriptDebuggerWin;

        /// <summary>
        /// The live UI design inspector window.
        /// </summary>
        public UIDesignInspectorWindow UIDesignInspectorWin;

        /// <summary>
        /// List with all created editor windows.
        /// </summary>
        public readonly List<EditorWindow> Windows = new List<EditorWindow>(32);

        /// <summary>
        /// Occurs when new window gets opened and added to the editor windows list.
        /// </summary>
        public event Action<EditorWindow> WindowAdded;

        /// <summary>
        /// Occurs when new window gets closed and removed from the editor windows list.
        /// </summary>
        public event Action<EditorWindow> WindowRemoved;

        internal WindowsModule(Editor editor)
        : base(editor)
        {
            InitOrder = -75;
        }

        /// <summary>
        /// Takes the screenshot of the current viewport.
        /// </summary>
        public void TakeScreenshot()
        {
            // Select task
            SceneRenderTask target = null;
            if (Editor.Windows.EditWin.IsSelected)
            {
                // Use editor window
                target = EditWin.Viewport.Task;
            }
            else
            {
                // Use game window
                GameWin.FocusOrShow();
            }

            // Fire screenshot taking
            Screenshot.Capture(target);
        }

        /// <summary>
        /// Updates the main window title.
        /// </summary>
        public void UpdateWindowTitle()
        {
            var mainWindow = MainWindow;
            if (mainWindow)
            {
                var title = Editor.GameProject?.Name;
                if (string.IsNullOrEmpty(title))
                    title = Path.GetFileNameWithoutExtension(Editor.GameProject?.ProjectPath);
                if (string.IsNullOrEmpty(title))
                    title = "Flax Editor";
                if (Editor.MultiplayerPlayMode.IsActive)
                {
                    if (Editor.MultiplayerPlayMode.IsReplica)
                    {
                        var tags = Editor.MultiplayerPlayMode.InstanceTags;
                        var label = tags.Length != 0 ? string.Join(", ", tags) : $"Player {Editor.MultiplayerPlayMode.InstanceIndex + 1}";
                        title += $" [{label} - Read Only]";
                    }
                    else
                    {
                        title += " [Multiplayer]";
                    }
                }
                mainWindow.Title = title;
            }
        }

        /// <summary>
        /// Flash main editor window to catch user attention
        /// </summary>
        public void FlashMainWindow()
        {
            MainWindow?.FlashWindow();
        }

        /// <summary>
        /// Finds the first window that is using given element to view/edit it.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <returns>Editor window or null if cannot find any window.</returns>
        public EditorWindow FindEditor(ContentItem item)
        {
            if (item == null)
                return null;
            for (int i = 0; i < Windows.Count; i++)
            {
                var win = Windows[i];
                if (win.IsEditingItem(item))
                {
                    return win;
                }
            }
            return null;
        }

        /// <summary>
        /// Gets the focused editor window, if any.
        /// </summary>
        internal EditorWindow FocusedEditorWindow => GetFocusedEditorWindow();

        /// <summary>
        /// Closes all windows that are using given element to view/edit it.
        /// </summary>
        /// <param name="item">The item.</param>
        public void CloseAllEditors(ContentItem item)
        {
            for (int i = 0; i < Windows.Count; i++)
            {
                var win = Windows[i];
                if (win.IsEditingItem(item))
                {
                    win.Close();
                    i--;
                }
            }
        }

        /// <summary>
        /// Saves the current workspace layout.
        /// </summary>
        public void SaveCurrentLayout()
        {
            _lastLayoutSaveTime = DateTime.UtcNow;
            SaveLayout(_windowsLayoutPath);
        }

        /// <summary>
        /// Loads the default workspace layout for the current editor version.
        /// </summary>
        public void LoadDefaultLayout()
        {
            var path = StringUtils.CombinePaths(Globals.EngineContentFolder, "Editor/LayoutDefault.xml");
            if (File.Exists(path))
            {
                LoadLayout(path);
            }
        }

        /// <summary>
        /// Loads the layout from the file.
        /// </summary>
        /// <param name="path">The layout file path.</param>
        /// <returns>True if layout has been loaded otherwise if failed (e.g. missing file).</returns>
        public bool LoadLayout(string path)
        {
            if (Editor.IsHeadlessMode)
                return false;

            Editor.Log(string.Format("Loading editor windows layout from \'{0}\'", path));

            if (!File.Exists(path))
            {
                Editor.LogWarning("Cannot load windows layout. File is missing.");
                return false;
            }

            XmlDocument doc = new XmlDocument();
            var masterPanel = Editor.UI.MasterPanel;

            try
            {
                doc.Load(path);
                var root = doc["DockPanelLayout"];
                if (root == null)
                {
                    Editor.LogWarning("Invalid windows layout file.");
                    return false;
                }

                // Reset existing layout
                masterPanel.ResetLayout();

                // Get metadata
                int version = int.Parse(root.Attributes["Version"].Value, CultureInfo.InvariantCulture);

                switch (version)
                {
                case 4:
                {
                    // Main window info
                    if (MainWindow)
                    {
                        var mainWindowNode = root["MainWindow"];
                        bool isMaximized = true, isMinimized = false;
                        Rectangle bounds = LoadBounds(mainWindowNode, ref isMaximized, ref isMinimized);
                        LoadWindow(MainWindow, ref bounds, isMaximized, false);
                    }

                    // Load master panel structure
                    var masterPanelNode = root["MasterPanel"];
                    if (masterPanelNode != null)
                    {
                        LoadPanel(masterPanelNode, masterPanel);
                    }

                    // Load all floating windows structure
                    var floating = root.SelectNodes("Float");
                    if (floating != null)
                    {
                        foreach (XmlElement child in floating)
                        {
                            if (child == null)
                                continue;

                            // Get window properties
                            bool isMaximized = false, isMinimized = false;
                            Rectangle bounds = LoadBounds(child, ref isMaximized, ref isMinimized);

                            // Create window and floating dock panel
                            var window = FloatWindowDockPanel.CreateFloatWindow(MainWindow.GUI, bounds.Location, bounds.Size, WindowStartPosition.Manual, string.Empty);
                            var panel = new FloatWindowDockPanel(masterPanel, window.GUI);
                            LoadWindow(panel.Window.Window, ref bounds, isMaximized, isMinimized);

                            // Load structure
                            LoadPanel(child, panel);

                            // Check if no child windows loaded (due to file errors or loading problems)
                            if (panel.TabsCount == 0 && panel.ChildPanelsCount == 0)
                            {
                                // Close empty window
                                Editor.LogWarning("Empty floating window inside layout.");
                                window.Close();
                            }
                            else
                            {
                                // Perform layout
                                var windowGUI = window.GUI;
                                windowGUI.IsLayoutLocked = false;
                                windowGUI.PerformLayout();

                                // Show
                                window.Show();
                                window.Focus();

                                // Perform layout again
                                windowGUI.PerformLayout();
                            }
                        }
                    }

                    break;
                }

                default:
                {
                    Editor.LogWarning("Unsupported windows layout version");
                    return false;
                }
                }
            }
            catch (Exception ex)
            {
                Editor.LogWarning("Failed to load windows layout.");
                Editor.LogWarning(ex);
                return false;
            }
            finally
            {
                masterPanel.PerformLayout();
            }

            return true;
        }

        private void SavePanel(XmlWriter writer, DockPanel panel)
        {
            writer.WriteAttributeString("SelectedTab", panel.SelectedTabIndex.ToString());

            for (int i = 0; i < panel.TabsCount; i++)
            {
                var win = panel.Tabs[i];
                writer.WriteStartElement("Window");

                writer.WriteAttributeString("Typename", win.SerializationTypename);

                if (win.UseLayoutData)
                {
                    writer.WriteStartElement("Data");
                    win.OnLayoutSerialize(writer);
                    writer.WriteEndElement();
                }

                writer.WriteEndElement();
            }

            for (int i = 0; i < panel.ChildPanelsCount; i++)
            {
                var p = panel.ChildPanels[i];

                // Skip empty panels
                if (p.TabsCount == 0)
                    continue;

                writer.WriteStartElement("Panel");

                DockState state = p.TryGetDockState(out float splitterValue);

                writer.WriteAttributeString("DockState", ((int)state).ToString());
                writer.WriteAttributeString("SplitterValue", splitterValue.ToString(CultureInfo.InvariantCulture));

                SavePanel(writer, p);

                writer.WriteEndElement();
            }
        }

        private void LoadPanel(XmlElement node, DockPanel panel)
        {
            int selectedTab = int.Parse(node.GetAttribute("SelectedTab"), CultureInfo.InvariantCulture);

            // Load docked windows
            var windows = node.SelectNodes("Window");
            if (windows != null)
            {
                foreach (XmlElement child in windows)
                {
                    if (child == null)
                        continue;

                    var typename = child.GetAttribute("Typename");
                    var window = GetWindow(typename);
                    if (window != null)
                    {
                        if (child.SelectSingleNode("Data") is XmlElement data)
                        {
                            window.OnLayoutDeserialize(data);
                        }
                        else
                        {
                            window.OnLayoutDeserialize();
                        }

                        window.Show(DockState.DockFill, panel);
                    }
                }
            }

            // Load child panels
            var panels = node.SelectNodes("Panel");
            if (panels != null)
            {
                foreach (XmlElement child in panels)
                {
                    if (child == null)
                        continue;

                    // Create child panel
                    DockState state = (DockState)int.Parse(child.GetAttribute("DockState"), CultureInfo.InvariantCulture);
                    float splitterValue = float.Parse(child.GetAttribute("SplitterValue"), CultureInfo.InvariantCulture);
                    var p = panel.CreateChildPanel(state, splitterValue);

                    LoadPanel(child, p);

                    // Check if panel has no docked window (due to loading problems or sth)
                    if (p.TabsCount == 0 && p.ChildPanelsCount == 0)
                    {
                        // Remove empty panel
                        Editor.LogWarning("Empty panel inside layout.");
                        p.RemoveIt();
                    }
                    else
                    {
                        p.CollapseEmptyTabsProxy();
                    }
                }
            }

            panel.SelectTab(selectedTab);
            panel.CollapseEmptyTabsProxy();
        }

        private static void SaveBounds(XmlWriter writer, Window win)
        {
            writer.WriteStartElement("Bounds");
            {
                var bounds = win.ClientBounds;
                writer.WriteAttributeString("X", bounds.X.ToString(CultureInfo.InvariantCulture));
                writer.WriteAttributeString("Y", bounds.Y.ToString(CultureInfo.InvariantCulture));
                writer.WriteAttributeString("Width", bounds.Width.ToString(CultureInfo.InvariantCulture));
                writer.WriteAttributeString("Height", bounds.Height.ToString(CultureInfo.InvariantCulture));
                writer.WriteAttributeString("IsMaximized", win.IsMaximized.ToString());
                writer.WriteAttributeString("IsMinimized", win.IsMinimized.ToString());
            }
            writer.WriteEndElement();
        }

        private static Rectangle LoadBounds(XmlElement node, ref bool isMaximized, ref bool isMinimized)
        {
            var bounds = node["Bounds"];
            var isMaximizedText = bounds.GetAttribute("IsMaximized");
            if (!string.IsNullOrEmpty(isMaximizedText) && bool.TryParse(isMaximizedText, out var tmpBool))
                isMaximized = tmpBool;
            var isMinimizedText = bounds.GetAttribute("IsMinimized");
            if (!string.IsNullOrEmpty(isMinimizedText) && bool.TryParse(isMinimizedText, out tmpBool))
                isMinimized = tmpBool;
            float x = float.Parse(bounds.GetAttribute("X"), CultureInfo.InvariantCulture);
            float y = float.Parse(bounds.GetAttribute("Y"), CultureInfo.InvariantCulture);
            float width = float.Parse(bounds.GetAttribute("Width"), CultureInfo.InvariantCulture);
            float height = float.Parse(bounds.GetAttribute("Height"), CultureInfo.InvariantCulture);
            return new Rectangle(x, y, width, height);
        }

        private static void LoadWindow(Window win, ref Rectangle bounds, bool isMaximized, bool isMinimized)
        {
            var virtualDesktopBounds = Platform.VirtualDesktopBounds;
            var virtualDesktopSafeLeftCorner = virtualDesktopBounds.Location;
            var virtualDesktopSafeRightCorner = virtualDesktopBounds.BottomRight;

            // Clamp position to match current desktop dimensions (if window was on desktop that is now inactive)
            if (bounds.X < virtualDesktopSafeLeftCorner.X || bounds.Y < virtualDesktopSafeLeftCorner.Y || bounds.X > virtualDesktopSafeRightCorner.X || bounds.Y > virtualDesktopSafeRightCorner.Y)
                bounds.Location = virtualDesktopSafeLeftCorner;

            if (isMaximized)
            {
                if (win.IsMaximized)
                    win.Restore();
                win.ClientPosition = bounds.Location;
                win.Maximize();
            }
            else
            {
                if (Mathf.Min(bounds.Size.X, bounds.Size.Y) >= 1)
                {
                    win.ClientBounds = bounds;
                }
                else
                {
                    win.ClientPosition = bounds.Location;
                }
                if (isMinimized)
                    win.Minimize();
            }
        }

        private class LayoutNameDialog : Dialog
        {
            private TextBox _textbox;

            public LayoutNameDialog()
            : base("Enter Layout Name")
            {
                var name = new TextBox(false, 8, 8, 200)
                {
                    WatermarkText = "Enter layout slot name",
                    Parent = this,
                };
                _textbox = name;

                var okButton = new Button(name.Right - 50, name.Bottom + 4, 50)
                {
                    Text = "OK",
                    Parent = this,
                };
                okButton.Clicked += OnSubmit;

                var cancelButton = new Button(okButton.Left - 54, okButton.Y, 50)
                {
                    Text = "Cancel",
                    Parent = this,
                };
                cancelButton.Clicked += OnCancel;

                _dialogSize = okButton.BottomRight + new Float2(8);
            }

            /// <inheritdoc />
            public override void OnSubmit()
            {
                var name = _textbox.Text;
                if (name.Length == 0)
                {
                    MessageBox.Show("Cannot use the empty name.");
                    return;
                }
                if (Utilities.Utils.HasInvalidPathChar(name))
                {
                    MessageBox.Show("Cannot use this name. It contains one or more invalid characters.");
                    return;
                }

                base.OnSubmit();

                var path = StringUtils.CombinePaths(Editor.LocalCachePath, "LayoutsCache", "Layout_" + name + ".xml");
                Editor.Instance.Windows.SaveLayout(path);
            }
        }

        /// <summary>
        /// Asks user for the layout name and saves the current windows layout in the current project cache folder.
        /// </summary>
        public void SaveLayout()
        {
            if (Editor.IsHeadlessMode)
                return;

            new LayoutNameDialog().Show();
        }

        /// <summary>
        /// Saves the layout to the file.
        /// </summary>
        /// <param name="path">The layout file path.</param>
        public void SaveLayout(string path)
        {
            if (Editor.IsHeadlessMode)
                return;

            //Editor.Log(string.Format("Saving editor windows layout to \'{0}\'", path));

            var settings = new XmlWriterSettings
            {
                Indent = true,
                IndentChars = "\t",
                Encoding = Encoding.UTF8,
                OmitXmlDeclaration = true,
            };

            var masterPanel = Editor.UI.MasterPanel;
            if (masterPanel == null)
                return;

            using (XmlWriter writer = XmlWriter.Create(path, settings))
            {
                writer.WriteStartDocument();
                writer.WriteStartElement("DockPanelLayout");

                // Metadata
                writer.WriteAttributeString("Version", "4");

                // Main window info
                if (MainWindow)
                {
                    writer.WriteStartElement("MainWindow");
                    SaveBounds(writer, MainWindow);
                    writer.WriteEndElement();
                }

                // Master panel structure
                writer.WriteStartElement("MasterPanel");
                SavePanel(writer, masterPanel);
                writer.WriteEndElement();

                // Save all floating windows structure
                for (int i = 0; i < masterPanel.FloatingPanels.Count; i++)
                {
                    var panel = masterPanel.FloatingPanels[i];
                    var window = panel.Window;
                    if (window == null)
                        continue;

                    writer.WriteStartElement("Float");
                    SavePanel(writer, panel);
                    SaveBounds(writer, window.Window);
                    writer.WriteEndElement();
                }

                writer.WriteEndElement();
                writer.WriteEndDocument();
            }
        }

        /// <summary>
        /// Opens the specified editor window (shows it with editor options handling for new windows).
        /// </summary>
        /// <param name="window">The window.</param>
        public void Open(EditorWindow window)
        {
            var newLocation = (DockState)Editor.Options.Options.Interface.NewWindowLocation;
            if (newLocation == DockState.Float)
            {
                // Check if there is a floating window that has the same size
                var dpi = (float)Platform.Dpi / 96.0f;
#if PLATFORM_MAC && !PLATFORM_SDL
                dpi = 1.0f; // TODO: refactor DPI support on macOS to skip such hacks
#endif
                var dpiScale = Platform.CustomDpiScale;
                var defaultSize = window.DefaultSize * dpi;
                for (var i = 0; i < Editor.UI.MasterPanel.FloatingPanels.Count; i++)
                {
                    var win = Editor.UI.MasterPanel.FloatingPanels[i];
                    if (Float2.Abs(win.Size - defaultSize).LengthSquared < 100)
                    {
                        window.Show(DockState.DockFill, win);
                        window.Focus();
                        return;
                    }
                }

                window.ShowFloating(defaultSize * dpiScale);
            }
            else
            {
                window.Show(newLocation);
            }
        }

        /// <summary>
        /// Gets <see cref="EditorWindow"/> that is represented by the given serialized typename. Used to restore workspace layout.
        /// </summary>
        /// <param name="typename">The typename.</param>
        /// <returns>The window or null if failed.</returns>
        private EditorWindow GetWindow(string typename)
        {
            // Try use already opened window
            for (int i = 0; i < Windows.Count; i++)
            {
                if (string.Equals(GetWindowNavigationTypename(Windows[i]), typename, StringComparison.OrdinalIgnoreCase))
                    return Windows[i];
            }

            // Check if it's an asset ID
            if (Guid.TryParse(typename, out Guid id))
            {
                var el = Editor.ContentDatabase.Find(id);
                if (el != null)
                {
                    // Open asset
                    return Editor.ContentEditing.Open(el, true);
                }
            }

            // Check if it's a content-backed tool window
            if (AssetReferencesGraphWindow.TryParseSerializationTypename(typename, out id))
            {
                var item = Editor.ContentDatabase.FindAsset(id);
                if (item != null)
                    return new AssetReferencesGraphWindow(Editor, item);

                Editor.LogWarning("Cannot restore asset references graph navigation. Missing asset: " + id);
            }

            return null;
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            Assert.IsNull(MainWindow);

            var layoutName = Editor.MultiplayerPlayMode.IsReplica ? $"WindowsLayout-MPPM-{Editor.MultiplayerPlayMode.InstanceIndex}.xml" : "WindowsLayout.xml";
            _windowsLayoutPath = StringUtils.CombinePaths(Globals.ProjectCacheFolder, layoutName);

            if (!Editor.IsHeadlessMode)
            {
                // Create main window
                var settings = CreateWindowSettings.Default;
                settings.Title = "Flax Editor";
                settings.Size = Platform.DesktopSize * 0.75f;
                settings.MinimumSize = new Float2(200, 150);
                settings.StartPosition = WindowStartPosition.CenterScreen;
                settings.ShowAfterFirstPaint = true;

                if (Utilities.Utils.UseCustomWindowDecorations(isMainWindow: true))
                {
                    settings.HasBorder = false;
#if PLATFORM_WINDOWS && !PLATFORM_SDL
                    // Skip OS sizing frame and implement it using LeftButtonHit
                    settings.HasSizingFrame = false;
#endif
                }
#if PLATFORM_LINUX && !PLATFORM_SDL
                settings.HasBorder = false;
#endif
                MainWindow = Platform.CreateWindow(ref settings);
                if (MainWindow == null)
                {
                    Editor.LogError("Failed to create editor main window!");
                    return;
                }
                UpdateWindowTitle();

                // Link for main window events
                MainWindow.Closing += MainWindow_OnClosing;
                MainWindow.Closed += MainWindow_OnClosed;
                MainWindow.MouseDown += OnNavigationMouseDown;
                MainWindow.MouseUp += OnNavigationMouseUp;
                MainWindow.GUI.UnhandledKeyDown += MainWindow_OnUnhandledKeyDown;
            }

            // Create default editor windows
            ContentWin = new ContentWindow(Editor);
            EditWin = new EditGameWindow(Editor);
            GameWin = new GameWindow(Editor);
            PropertiesWin = new PropertiesWindow(Editor);
            SceneWin = new SceneTreeWindow(Editor);
            DebugLogWin = new DebugLogWindow(Editor);
            OutputLogWin = new OutputLogWindow(Editor);
            ToolboxWin = new ToolboxWindow(Editor);
            GraphicsQualityWin = new GraphicsQualityWindow(Editor);
            GameCookerWin = new GameCookerWindow(Editor);
            ProfilerWin = new ProfilerWindow(Editor);
            EditorOptionsWin = new EditorOptionsWindow(Editor);
            PluginsWin = new PluginsWindow(Editor);
            VisualScriptDebuggerWin = new VisualScriptDebuggerWindow(Editor);
            UIDesignInspectorWin = new UIDesignInspectorWindow(Editor);

            // Bind events
            Level.SceneSaveError += OnSceneSaveError;
            Level.SceneLoaded += OnSceneLoaded;
            Level.SceneLoadError += OnSceneLoadError;
            Level.SceneLoading += OnSceneLoading;
            Level.SceneSaved += OnSceneSaved;
            Level.SceneSaving += OnSceneSaving;
            Level.SceneUnloaded += OnSceneUnloaded;
            Level.SceneUnloading += OnSceneUnloading;
            Editor.ContentDatabase.WorkspaceRebuilt += OnWorkspaceRebuilt;
            Editor.StateMachine.StateChanged += OnEditorStateChanged;
        }

        internal void AddToRestore(AssetEditorWindow win)
        {
            AddToRestore(win, win.GetType(), new WindowRestoreData
            {
                AssetItemID = win.Item.ID,
            });
        }

        internal void AddToRestore(CustomEditorWindow win)
        {
            // Validate if can restore type
            var type = win.GetType();
            var constructor = type.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null, Type.EmptyTypes, null);
            if (constructor == null || type.IsGenericType)
                return;

            AddToRestore(win.Window, type, new WindowRestoreData());
        }

        private void AddToRestore(EditorWindow win, Type type, WindowRestoreData winData)
        {
            // Ensure that this window is only selected following recompilation
            // if it was the active tab in its dock panel. Otherwise, there is a
            // risk of interrupting the user's workflow by potentially selecting
            // background tabs.
            var window = win.RootWindow?.Window;
            var panel = win.ParentDockPanel;
            winData.SelectOnShow = panel.SelectedTab == win;
            winData.DockedTabIndex = 0;
            if (panel is FloatWindowDockPanel && window != null && panel.TabsCount == 1)
            {
                winData.DockState = DockState.Float;
                winData.FloatPosition = window.Position;
                winData.FloatSize = window.ClientSize;
                winData.Maximize = window.IsMaximized;
                winData.Minimize = window.IsMinimized;
                winData.DockedTo = panel;
            }
            else
            {
                for (int i = 0; i < panel.Tabs.Count; i++)
                {
                    if (panel.Tabs[i] == win)
                    {
                        winData.DockedTabIndex = i;
                        break;
                    }
                }
                if (panel.TabsCount > 1)
                {
                    winData.DockState = DockState.DockFill;
                    winData.DockedTo = panel;
                }
                else
                {
                    winData.DockState = panel.TryGetDockState(out var splitterValue);
                    winData.DockedTo = panel.ParentDockPanel;
                    winData.SplitterValue = splitterValue;
                }
            }
            winData.AssemblyName = type.Assembly.GetName().Name;
            winData.TypeName = type.FullName;
            _restoreWindows.Add(winData);
        }

        private void OnWorkspaceRebuilt()
        {
            // Go in reverse order to create floating Prefab windows first before docked windows
            for (int i = _restoreWindows.Count - 1; i >= 0; i--)
            {
                var winData = _restoreWindows[i];

                try
                {
                    var assembly = Utils.GetAssemblyByName(winData.AssemblyName);
                    if (assembly == null)
                        continue;

                    var type = assembly.GetType(winData.TypeName);
                    if (type == null)
                        continue;

                    if (type.IsAssignableTo(typeof(AssetEditorWindow)))
                    {
                        var assetItem = Editor.ContentDatabase.FindAsset(winData.AssetItemID);
                        var assetType = assetItem.GetType();
                        var ctor = type.GetConstructor(new Type[] { typeof(Editor), assetType });
                        var win = (AssetEditorWindow)ctor.Invoke(new object[] { Editor.Instance, assetItem });

                        win.Show(winData.DockState, winData.DockState != DockState.Float ? winData.DockedTo : null, winData.SelectOnShow, winData.SplitterValue);
                        if (winData.DockState == DockState.Float)
                        {
                            var window = win.RootWindow.Window;
                            window.Position = winData.FloatPosition;
                            if (winData.Maximize)
                            {
                                window.Maximize();
                            }
                            else if (winData.Minimize)
                            {
                                window.Minimize();
                            }
                            else
                            {
                                window.ClientSize = winData.FloatSize;
                            }

                            // Update panel reference in other windows docked to this panel
                            foreach (ref var otherData in CollectionsMarshal.AsSpan(_restoreWindows))
                            {
                                if (otherData.DockedTo == winData.DockedTo)
                                    otherData.DockedTo = win.ParentDockPanel;
                            }
                        }
                        var panel = win.ParentDockPanel;
                        int currentTabIndex = 0;
                        for (int pi = 0; pi < panel.TabsCount; pi++)
                        {
                            if (panel.Tabs[pi] == win)
                            {
                                currentTabIndex = pi;
                                break;
                            }
                        }
                        while (currentTabIndex > winData.DockedTabIndex)
                        {
                            win.ParentDockPanel.MoveTabLeft(currentTabIndex);
                            currentTabIndex--;
                        }
                        while (currentTabIndex < winData.DockedTabIndex)
                        {
                            win.ParentDockPanel.MoveTabRight(currentTabIndex);
                            currentTabIndex++;
                        }
                        panel.PerformLayout(true);
                    }
                    else
                    {
                        var win = (CustomEditorWindow)Activator.CreateInstance(type);
                        win.Show(winData.DockState, winData.DockedTo, winData.SelectOnShow, winData.SplitterValue);
                        if (winData.DockState == DockState.Float)
                        {
                            var window = win.Window.RootWindow.Window;
                            window.Position = winData.FloatPosition;
                            if (winData.Maximize)
                            {
                                window.Maximize();
                            }
                            else if (winData.Minimize)
                            {
                                window.Minimize();
                            }
                            else
                            {
                                window.ClientSize = winData.FloatSize;
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    Editor.LogWarning(ex);
                    Editor.LogWarning(string.Format("Failed to restore window {0} (assembly: {1})", winData.TypeName, winData.AssemblyName));
                }
            }

            // Restored windows stole the focus from Editor
            if (_restoreWindows.Count > 0)
                Editor.Instance.Windows.MainWindow.Focus();

            _restoreWindows.Clear();
        }

        private void MainWindow_OnClosing(ClosingReason reason, ref bool cancel)
        {
            Editor.Log("Main window is closing, reason: " + reason);

            if (Editor.StateMachine.IsPlayMode)
            {
                // Cancel closing but leave the play mode
                cancel = true;
                Editor.Log("Skip closing editor and leave the play mode");
                Editor.Simulation.RequestStopPlay();
                return;
            }

            SaveCurrentLayout();

            // Block closing only on user events
            if (reason == ClosingReason.User)
            {
                // Check if cancel action or save scene before exit
                if (Editor.Scene.CheckSaveBeforeClose())
                {
                    // Cancel
                    cancel = true;
                    return;
                }

                // Close all asset editor windows
                for (int i = 0; i < Windows.Count; i++)
                {
                    if (Windows[i] is AssetEditorWindow assetEditorWindow)
                    {
                        if (assetEditorWindow.Close(ClosingReason.User))
                        {
                            // Cancel
                            cancel = true;
                            return;
                        }

                        // Remove it
                        OnWindowRemove(assetEditorWindow);
                        i--;
                    }
                }
            }

            MainWindowClosing?.Invoke();
        }

        private void MainWindow_OnClosed()
        {
            Editor.Log("Main window is closed");
            if (MainWindow != null)
            {
                MainWindow.MouseDown -= OnNavigationMouseDown;
                MainWindow.MouseUp -= OnNavigationMouseUp;
                MainWindow.GUI.UnhandledKeyDown -= MainWindow_OnUnhandledKeyDown;
            }
            MainWindow = null;

            // Capture project icon screenshot (not in play mode and if editor was used for some time)
            if (!Editor.StateMachine.IsPlayMode &&
                Time.TimeSinceStartup >= 5.0f &&
                !Editor.IsHeadlessMode &&
                EditWin.Viewport.Task != null &&
                EditWin.Viewport.Task.LastUsedFrame > 100 &&
                GPUDevice.Instance?.RendererType != RendererType.Null)
            {
                Editor.Log("Capture project icon screenshot");
                _projectIconScreenshotTimeout = Time.TimeSinceStartup + 0.8f; // wait 800ms for a screenshot task
                EditWin.Viewport.SaveProjectIcon();
            }
            else
            {
                // Close editor
                Engine.RequestExit();
            }
        }

        internal void OnWindowAdd(EditorWindow window)
        {
            Windows.Add(window);
            WindowAdded?.Invoke(window);
        }

        internal void OnWindowFocused(EditorWindow window)
        {
            if (window == null || window.IsDisposing || !Windows.Contains(window))
                return;

            _focusedWindows.Remove(window);
            _focusedWindows.Add(window);
            RecordWindowNavigation(window);
        }

        internal void OnWindowRemove(EditorWindow window)
        {
            if (!CanRecordWindowNavigation(window))
                RemoveWindowNavigation(GetWindowNavigationTypename(window));

            Windows.Remove(window);
            _focusedWindows.Remove(window);
            ClearFocusForWindow(window);
            WindowRemoved?.Invoke(window);
            RestoreEditorFocusIfNeeded(window);
        }

        internal void BeginContentWindowOpen(ContentItem item)
        {
            OnWindowFocused(ContentWin);

            if (item is AssetItem assetItem)
            {
                _contentOpenNavigationTargetTypename = assetItem.ID.ToString();
                LogWindowNavigationDebug("Begin content open " + item + " -> " + _contentOpenNavigationTargetTypename);
            }
            else
            {
                ClearContentOpenNavigation();
            }
        }

        internal void EndContentWindowOpen(EditorWindow window)
        {
            if (string.IsNullOrEmpty(_contentOpenNavigationTargetTypename))
                return;

            if (window != null)
                OnWindowFocused(window);
            ClearContentOpenNavigation();
        }

        /// <summary>
        /// Navigates back to the previous editor window.
        /// </summary>
        /// <returns>True if the window history had a back entry to consume, otherwise false.</returns>
        public bool NavigateBack()
        {
            FlushPendingContentBounce("back requested");
            LogWindowNavigationDebug("Back requested. CanGoBack: " + CanNavigateBack);
            if (!CanNavigateBack)
            {
                RestoreEditorFocusIfNeeded(null);
                return false;
            }

            _windowNavigationHistory.GoBack();
            RestoreEditorFocusIfNeeded(null);
            return true;
        }

        /// <summary>
        /// Navigates forward to the next editor window.
        /// </summary>
        /// <returns>True if the window history had a forward entry to consume, otherwise false.</returns>
        public bool NavigateForward()
        {
            FlushPendingContentBounce("forward requested");
            LogWindowNavigationDebug("Forward requested. CanGoForward: " + CanNavigateForward);
            if (!CanNavigateForward)
            {
                RestoreEditorFocusIfNeeded(null);
                return false;
            }

            _windowNavigationHistory.GoForward();
            RestoreEditorFocusIfNeeded(null);
            return true;
        }

        /// <summary>
        /// Removes window navigation actions that reference the specified serialized window typename.
        /// </summary>
        /// <param name="typename">The serialized window typename.</param>
        public void RemoveWindowNavigation(string typename)
        {
            if (string.IsNullOrEmpty(typename))
                return;

            LogWindowNavigationDebug("Remove entries containing " + typename);
            _windowNavigationHistory.RemoveActions(x => x is WindowNavigationAction action && action.Contains(typename));
            if (ContainsWindowNavigationContext(_pendingContentBounceSource, typename) || ContainsWindowNavigationContext(_pendingContentBounceTarget, typename))
                ClearPendingContentBounce();
            if (_lastNavigationWindowContext.IsValid && string.Equals(_lastNavigationWindowContext.Typename, typename, StringComparison.OrdinalIgnoreCase))
                _lastNavigationWindowContext = default;
        }

        private void RecordWindowNavigation(EditorWindow window)
        {
            if (!CanRecordWindowNavigation(window))
            {
                LogWindowNavigationDebug("Skip non-recordable focus: " + DescribeWindowForNavigation(window));
                return;
            }

            var target = GetWindowNavigationContext(window);
            if (!target.IsValid)
            {
                LogWindowNavigationDebug("Skip invalid focus target: " + DescribeWindowForNavigation(window));
                return;
            }

            if (_suppressWindowNavigation || _windowNavigationHistory.IsNavigating)
            {
                LogWindowNavigationDebug("Sync current during restore: " + DescribeWindowNavigationContext(target));
                _lastNavigationWindowContext = target;
                _lastAssetFocusWasContentOpen = false;
                return;
            }

            if (TryRecordContentOpenTarget(target))
                return;

            var source = _lastNavigationWindowContext;
            _lastNavigationWindowContext = target;
            if (!source.IsValid || source.Equals(target))
            {
                LogWindowNavigationDebug("Set current without action. Source: " + DescribeWindowNavigationContext(source) + ", Target: " + DescribeWindowNavigationContext(target));
                if (target.IsAssetDocument)
                    _lastAssetFocusWasContentOpen = false;
                return;
            }

            if (TryCompletePendingContentBounce(source, target))
                return;

            FlushPendingContentBounce("before adding " + DescribeWindowNavigationContext(source) + " -> " + DescribeWindowNavigationContext(target));

            if (source.IsAssetDocument && IsContentWindowContext(target))
            {
                _pendingContentBounceSource = source;
                _pendingContentBounceTarget = target;
                _pendingContentBounceWasContentOpen = _lastAssetFocusWasContentOpen;
                LogWindowNavigationDebug("Hold pending " + DescribeWindowNavigationContext(source) + " -> " + DescribeWindowNavigationContext(target) + ". ContentOpen: " + _pendingContentBounceWasContentOpen);
                return;
            }

            AddWindowNavigationAction(source, target);
            if (target.IsAssetDocument)
                _lastAssetFocusWasContentOpen = false;
        }

        private bool HasPendingContentBounce => _pendingContentBounceSource.IsValid && _pendingContentBounceTarget.IsValid;

        private bool TryRecordContentOpenTarget(WindowNavigationContext target)
        {
            if (string.IsNullOrEmpty(_contentOpenNavigationTargetTypename) || !target.IsAssetDocument)
                return false;

            if (!string.Equals(target.Typename, _contentOpenNavigationTargetTypename, StringComparison.OrdinalIgnoreCase))
            {
                LogWindowNavigationDebug("Skip asset focus " + DescribeWindowNavigationContext(target) + " while waiting for content-open target " + _contentOpenNavigationTargetTypename);
                return true;
            }

            ClearContentOpenNavigation();
            var source = _lastNavigationWindowContext;
            _lastNavigationWindowContext = target;
            if (!source.IsValid || source.Equals(target))
            {
                LogWindowNavigationDebug("Set content-open current without action. Source: " + DescribeWindowNavigationContext(source) + ", Target: " + DescribeWindowNavigationContext(target));
                _lastAssetFocusWasContentOpen = true;
                return true;
            }

            if (TryCompletePendingContentBounce(source, target))
            {
                _lastAssetFocusWasContentOpen = true;
                return true;
            }

            FlushPendingContentBounce("before content-open " + DescribeWindowNavigationContext(target));
            AddWindowNavigationAction(source, target);
            _lastAssetFocusWasContentOpen = true;
            return true;
        }

        private bool TryCompletePendingContentBounce(WindowNavigationContext source, WindowNavigationContext target)
        {
            if (!HasPendingContentBounce || !target.IsAssetDocument || !IsContentWindowContext(source))
                return false;
            if (!_pendingContentBounceWasContentOpen)
            {
                FlushPendingContentBounce("not a content-open chain before " + DescribeWindowNavigationContext(target));
                return false;
            }

            var pendingSource = _pendingContentBounceSource;
            var pendingTarget = _pendingContentBounceTarget;
            ClearPendingContentBounce();

            if (pendingSource.Equals(target))
            {
                LogWindowNavigationDebug("Drop pending bounce back to same asset " + DescribeWindowNavigationContext(pendingSource) + " -> " + DescribeWindowNavigationContext(pendingTarget) + " -> " + DescribeWindowNavigationContext(target));
                return true;
            }

            LogWindowNavigationDebug("Coalesce " + DescribeWindowNavigationContext(pendingSource) + " -> " + DescribeWindowNavigationContext(pendingTarget) + " -> " + DescribeWindowNavigationContext(target));
            AddWindowNavigationAction(pendingSource, target);
            return true;
        }

        private void FlushPendingContentBounce(string reason)
        {
            if (!HasPendingContentBounce)
                return;

            var source = _pendingContentBounceSource;
            var target = _pendingContentBounceTarget;
            ClearPendingContentBounce();
            LogWindowNavigationDebug("Flush pending " + DescribeWindowNavigationContext(source) + " -> " + DescribeWindowNavigationContext(target) + ". Reason: " + reason);
            AddWindowNavigationAction(source, target);
        }

        private void ClearPendingContentBounce()
        {
            _pendingContentBounceSource = default;
            _pendingContentBounceTarget = default;
            _pendingContentBounceWasContentOpen = false;
        }

        private void ClearContentOpenNavigation()
        {
            _contentOpenNavigationTargetTypename = null;
        }

        private void AddWindowNavigationAction(WindowNavigationContext source, WindowNavigationContext target)
        {
            LogWindowNavigationDebug("Add " + DescribeWindowNavigationContext(source) + " -> " + DescribeWindowNavigationContext(target));
            _windowNavigationHistory.AddAction(new WindowNavigationAction(this, source, target));
        }

        private static bool ContainsWindowNavigationContext(WindowNavigationContext context, string typename)
        {
            return context.IsValid && string.Equals(context.Typename, typename, StringComparison.OrdinalIgnoreCase);
        }

        private bool IsContentWindowContext(WindowNavigationContext context)
        {
            return context.IsValid &&
                   ContentWin != null &&
                   string.Equals(context.Typename, GetWindowNavigationTypename(ContentWin), StringComparison.OrdinalIgnoreCase);
        }

        private static WindowNavigationContext GetWindowNavigationContext(EditorWindow window)
        {
            if (window == null)
                return default;

            var title = window.Title;
            if (string.IsNullOrEmpty(title))
                title = window.GetType().Name;

            return new WindowNavigationContext(GetWindowNavigationTypename(window), title, window is AssetEditorWindow);
        }

        private static string GetWindowNavigationTypename(EditorWindow window)
        {
            if (window == null)
                return null;

            var typename = window.SerializationTypename;
            return !string.IsNullOrEmpty(typename) ? typename : window.GetType().FullName;
        }

        private bool CanRecordWindowNavigation(EditorWindow window)
        {
            if (window == null)
                return false;

            if (window is AssetEditorWindow || window is AssetReferencesGraphWindow)
                return true;

            return ReferenceEquals(window, ContentWin) ||
                   ReferenceEquals(window, EditWin) ||
                   ReferenceEquals(window, GameWin) ||
                   ReferenceEquals(window, PropertiesWin) ||
                   ReferenceEquals(window, SceneWin) ||
                   ReferenceEquals(window, DebugLogWin) ||
                   ReferenceEquals(window, OutputLogWin) ||
                   ReferenceEquals(window, ToolboxWin) ||
                   ReferenceEquals(window, GraphicsQualityWin) ||
                   ReferenceEquals(window, GameCookerWin) ||
                   ReferenceEquals(window, ProfilerWin) ||
                   ReferenceEquals(window, EditorOptionsWin) ||
                   ReferenceEquals(window, PluginsWin) ||
                   ReferenceEquals(window, VisualScriptDebuggerWin) ||
                   ReferenceEquals(window, UIDesignInspectorWin);
        }

        private bool NavigateToWindowContext(WindowNavigationContext context)
        {
            if (!context.IsValid)
            {
                LogWindowNavigationDebug("Restore skipped invalid context.");
                return false;
            }

            LogWindowNavigationDebug("Restore " + DescribeWindowNavigationContext(context));
            var window = GetWindow(context.Typename);
            if (window == null)
            {
                Editor.LogWarning("Cannot restore window navigation. Missing window: " + context.Title);
                LogWindowNavigationDebug("Restore failed, missing window " + DescribeWindowNavigationContext(context));
                RemoveWindowNavigation(context.Typename);
                return false;
            }

            var wasSuppressed = _suppressWindowNavigation;
            _suppressWindowNavigation = true;
            try
            {
                if (window.IsHidden)
                {
                    LogWindowNavigationDebug("Restore opening hidden window " + DescribeWindowForNavigation(window));
                    Open(window);
                }
                window.FocusOrShow();
                if (!TryFocusEditorWindow(window))
                {
                    LogWindowNavigationDebug("Restore failed to focus " + DescribeWindowForNavigation(window) + ". Hidden: " + window.IsHidden + ", Visible: " + window.Visible + ", Docked: " + window.IsDocked);
                    return false;
                }
                _lastNavigationWindowContext = GetWindowNavigationContext(window);
                LogWindowNavigationDebug("Restored " + DescribeWindowForNavigation(window));
                return true;
            }
            finally
            {
                _suppressWindowNavigation = wasSuppressed;
            }
        }

        internal void SetWindowNavigationHistoryCapacity(int capacity)
        {
            _windowNavigationHistory.Capacity = Mathf.Max(1, capacity);
        }

        private void LogWindowNavigationDebug(string message)
        {
            if (LogWindowNavigation)
                Editor.Log("[WindowHistory] " + message);
        }

        private static string DescribeWindowNavigationContext(WindowNavigationContext context)
        {
            if (!context.IsValid)
                return "<none>";

            var type = context.IsAssetDocument ? "asset" : "window";
            return string.Format("{0} ({1}, {2})", context.Title, context.Typename, type);
        }

        private static string DescribeWindowForNavigation(EditorWindow window)
        {
            if (window == null)
                return "<null>";

            var context = GetWindowNavigationContext(window);
            return context.IsValid ? DescribeWindowNavigationContext(context) : window.GetType().FullName;
        }

        /// <inheritdoc />
        public override void OnEndInit()
        {
            UpdateWindowTitle();

            // Initialize windows
            for (int i = 0; i < Windows.Count; i++)
            {
                try
                {
                    Windows[i].OnInit();
                }
                catch (Exception ex)
                {
                    Editor.LogWarning(ex);
                    Editor.LogError("Failed to init window " + Windows[i]);
                }
            }

            // Load current workspace layout
            if (!LoadLayout(_windowsLayoutPath))
                LoadDefaultLayout();

            // Clear timer flag
            _lastLayoutSaveTime = DateTime.UtcNow;
        }

        /// <inheritdoc />
        public override void OnUpdate()
        {
            Profiler.BeginEvent("WindowsModule.Update");

            // Auto save workspace layout every few seconds
            var now = DateTime.UtcNow;
            if (_lastLayoutSaveTime.Ticks > 10 && now - _lastLayoutSaveTime >= TimeSpan.FromSeconds(10))
            {
                Profiler.BeginEvent("Save Layout");
                SaveCurrentLayout();
                Profiler.EndEvent();
            }

            // Auto close on project icon saving end
            if (_projectIconScreenshotTimeout > 0 && Time.TimeSinceStartup > _projectIconScreenshotTimeout)
            {
                Editor.Log("Closing Editor after project icon screenshot");
                EditWin.Viewport.SaveProjectIconEnd();
                Engine.RequestExit();
            }

            // Update editor windows
            for (int i = 0; i < Windows.Count; i++)
            {
                Windows[i].OnUpdate();
            }

            Profiler.EndEvent();
        }

        /// <inheritdoc />
        public override void OnExit()
        {
            // Unbind events
            Level.SceneSaveError -= OnSceneSaveError;
            Level.SceneLoaded -= OnSceneLoaded;
            Level.SceneLoadError -= OnSceneLoadError;
            Level.SceneLoading -= OnSceneLoading;
            Level.SceneSaved -= OnSceneSaved;
            Level.SceneSaving -= OnSceneSaving;
            Level.SceneUnloaded -= OnSceneUnloaded;
            Level.SceneUnloading -= OnSceneUnloading;
            Editor.ContentDatabase.WorkspaceRebuilt -= OnWorkspaceRebuilt;
            Editor.StateMachine.StateChanged -= OnEditorStateChanged;
            if (MainWindow != null)
            {
                MainWindow.MouseDown -= OnNavigationMouseDown;
                MainWindow.MouseUp -= OnNavigationMouseUp;
                MainWindow.GUI.UnhandledKeyDown -= MainWindow_OnUnhandledKeyDown;
            }

            // Close main window
            MainWindow?.Close(ClosingReason.EngineExit);
            MainWindow = null;

            // Close all windows
            var windows = Windows.ToArray();
            for (int i = 0; i < windows.Length; i++)
            {
                if (windows[i] != null)
                    windows[i].Close(ClosingReason.EngineExit);
            }
            _windowNavigationHistory.Dispose();
            _lastNavigationWindowContext = default;
            ClearPendingContentBounce();
            _focusedWindows.Clear();
        }

        #region Window Events

        private bool MainWindow_OnUnhandledKeyDown(KeyboardKeys key)
        {
            var mainWindow = MainWindow;
            if (!mainWindow || (!mainWindow.IsFocused && !Platform.HasFocus))
                return false;

            bool logGizmoFocus = IsGizmoFocusDebugKey(key);
            if (logGizmoFocus)
                LogGizmoFocusDebug("Main window received unhandled key", key);

            var input = Editor.Options.Options.Input;
            if (input.Undo.Process(mainWindow, key))
            {
                if (logGizmoFocus)
                    LogGizmoFocusDebug("Global undo before action", key);
                Editor.PerformUndo();
                if (logGizmoFocus)
                    LogGizmoFocusDebug("Global undo after action", key);
                RestoreEditorFocusIfNeeded(null);
                if (logGizmoFocus)
                {
                    LogGizmoFocusDebug("Global undo after focus recovery", key);
                    FlaxEngine.Scripting.InvokeOnUpdate(() => LogGizmoFocusDebug("Global undo next update", key));
                }
                return true;
            }
            if (input.Redo.Process(mainWindow, key))
            {
                if (logGizmoFocus)
                    LogGizmoFocusDebug("Global redo before action", key);
                Editor.PerformRedo();
                if (logGizmoFocus)
                    LogGizmoFocusDebug("Global redo after action", key);
                RestoreEditorFocusIfNeeded(null);
                if (logGizmoFocus)
                {
                    LogGizmoFocusDebug("Global redo after focus recovery", key);
                    FlaxEngine.Scripting.InvokeOnUpdate(() => LogGizmoFocusDebug("Global redo next update", key));
                }
                return true;
            }
            if (IsNavigationBackInput(mainWindow, key))
            {
                NavigateBack();
                return true;
            }
            if (IsNavigationForwardInput(mainWindow, key))
            {
                NavigateForward();
                return true;
            }

            return false;
        }

        private static bool IsGizmoFocusDebugKey(KeyboardKeys key)
        {
            return key == KeyboardKeys.F ||
                   key == KeyboardKeys.Q ||
                   key == KeyboardKeys.W ||
                   key == KeyboardKeys.E ||
                   key == KeyboardKeys.R ||
                   key == KeyboardKeys.Z;
        }

        private void LogGizmoFocusDebug(string point, KeyboardKeys key)
        {
            var mainWindow = MainWindow;
            Editor.Log(string.Format(
                "[GizmoFocusDebug] WindowsModule {0}; Key={1}; Focused={2}; AppFocus={3}; MainWindowFocus={4}; Ctrl={5}; Shift={6}; Alt={7}",
                point,
                key,
                DescribeControl(mainWindow ? mainWindow.GUI?.FocusedControl : null),
                Platform.HasFocus,
                mainWindow && mainWindow.IsFocused,
                mainWindow && mainWindow.GetKey(KeyboardKeys.Control),
                mainWindow && mainWindow.GetKey(KeyboardKeys.Shift),
                mainWindow && mainWindow.GetKey(KeyboardKeys.Alt)));
        }

        internal void OnNavigationMouseDown(ref Float2 mousePosition, MouseButton button, ref bool handled)
        {
            if (handled)
                return;

            if (button == MouseButton.Extended1 || button == MouseButton.Extended2)
                handled = true;
        }

        internal void OnNavigationMouseUp(ref Float2 mousePosition, MouseButton button, ref bool handled)
        {
            if (handled)
                return;

            if (button == MouseButton.Extended1)
            {
                if (CanNavigateBack)
                    NavigateBack();
                else if (GetFocusedEditorWindow() == ContentWin)
                    ContentWin.NavigateBackward();
                handled = true;
            }
            else if (button == MouseButton.Extended2)
            {
                if (CanNavigateForward)
                    NavigateForward();
                else if (GetFocusedEditorWindow() == ContentWin)
                    ContentWin.NavigateForward();
                handled = true;
            }
        }

        internal static bool IsNavigationBackInput(Window window, KeyboardKeys key)
        {
            return key == KeyboardKeys.ArrowLeft &&
                   window.GetKey(KeyboardKeys.Alt) &&
                   !window.GetKey(KeyboardKeys.Control) &&
                   !window.GetKey(KeyboardKeys.Shift);
        }

        internal static bool IsNavigationForwardInput(Window window, KeyboardKeys key)
        {
            return key == KeyboardKeys.ArrowRight &&
                   window.GetKey(KeyboardKeys.Alt) &&
                   !window.GetKey(KeyboardKeys.Control) &&
                   !window.GetKey(KeyboardKeys.Shift);
        }

        private void RestoreEditorFocusIfNeeded(EditorWindow closedWindow)
        {
            if (GetFocusedEditorWindow() != null)
                return;

            if (LogFocusRecovery)
            {
                Editor.Log(string.Format("Editor focus lost{0}. AppFocus: {1}, MainWindowFocus: {2}, MainGuiFocus: {3}.",
                    closedWindow != null ? " after closing " + DescribeWindow(closedWindow) : string.Empty,
                    Platform.HasFocus,
                    MainWindow && MainWindow.IsFocused,
                    DescribeControl(MainWindow ? MainWindow.GUI?.FocusedControl : null)));
            }

            if (!Platform.HasFocus)
                return;

            if (TryFocusPreviousEditorWindow(out var restoredWindow))
            {
                RecordWindowNavigation(restoredWindow);
                if (LogFocusRecovery)
                    Editor.Log("Editor focus restored to " + DescribeWindow(restoredWindow) + ".");
                return;
            }

            if (LogFocusRecovery)
                Editor.Log("Editor focus recovery found no visible editor window to focus.");
        }

        private EditorWindow GetFocusedEditorWindow()
        {
            for (int i = 0; i < Windows.Count; i++)
            {
                var window = Windows[i];
                if (window.IsDisposing || window.IsHidden)
                    continue;
                if (window.RootWindow is WindowRootControl root && root.Window && root.Window.IsFocused && window.ContainsFocus)
                    return window;
            }

            var mainWindow = MainWindow;
            var mainRoot = mainWindow ? mainWindow.GUI : null;
            if (mainWindow && mainWindow.IsFocused)
            {
                var window = GetEditorWindow(mainRoot?.FocusedControl);
                if (window != null && Windows.Contains(window) && !window.IsDisposing && !window.IsHidden)
                    return window;
            }

            return null;
        }

        private static void ClearFocusForWindow(EditorWindow window)
        {
            if (window?.RootWindow is WindowRootControl root && GetEditorWindow(root.FocusedControl) == window)
                root.FocusedControl = null;
        }

        private bool TryFocusPreviousEditorWindow(out EditorWindow restoredWindow)
        {
            for (int i = _focusedWindows.Count - 1; i >= 0; i--)
            {
                var window = _focusedWindows[i];
                if (TryFocusEditorWindow(window))
                {
                    restoredWindow = window;
                    return true;
                }
                _focusedWindows.RemoveAt(i);
            }

            for (int i = Windows.Count - 1; i >= 0; i--)
            {
                var window = Windows[i];
                if (window.IsSelected && TryFocusEditorWindow(window))
                {
                    restoredWindow = window;
                    return true;
                }
            }

            if (TryFocusEditorWindow(EditWin))
            {
                restoredWindow = EditWin;
                return true;
            }

            restoredWindow = null;
            return false;
        }

        private bool TryFocusEditorWindow(EditorWindow window)
        {
            if (window == null || window.IsDisposing || window.IsHidden || !(window.RootWindow is WindowRootControl root) || !root.Window)
                return false;

            var rootWindowFocused = root.Window.IsFocused;
            var focusedControl = root.FocusedControl;
            if (focusedControl != null)
            {
                if (!focusedControl.IsDisposing && focusedControl.RootWindow == root && focusedControl.VisibleInHierarchy && focusedControl.EnabledInHierarchy)
                {
                    var focusedWindow = GetEditorWindow(focusedControl);
                    if (focusedWindow != null && focusedWindow != window)
                    {
                        if (rootWindowFocused && Windows.Contains(focusedWindow) && !focusedWindow.IsDisposing && !focusedWindow.IsHidden)
                            return false;
                        root.FocusedControl = null;
                    }
                }
                else
                {
                    root.FocusedControl = null;
                }
            }

            if (!rootWindowFocused)
                root.Window.Focus();
            window.Focus();
            return true;
        }

        private static EditorWindow GetEditorWindow(Control control)
        {
            while (control != null)
            {
                if (control is EditorWindow window)
                    return window;
                control = control.Parent;
            }
            return null;
        }

        private static string DescribeWindow(EditorWindow window)
        {
            return window != null ? string.Format("'{0}' ({1})", window.Title, window.GetType().Name) : "<none>";
        }

        private static string DescribeControl(Control control)
        {
            if (control == null)
                return "<none>";

            return string.Format("{0} in {1}, Disposing: {2}, Visible: {3}, Enabled: {4}",
                control.GetType().Name,
                DescribeWindow(GetEditorWindow(control)),
                control.IsDisposing,
                control.VisibleInHierarchy,
                control.EnabledInHierarchy);
        }

        private void OnEditorStateChanged()
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnEditorStateChanged();
        }

        private void OnSceneSaveError(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneSaveError(scene, sceneId);
        }

        private void OnSceneLoaded(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneLoaded(scene, sceneId);
        }

        private void OnSceneLoadError(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneLoadError(scene, sceneId);
        }

        private void OnSceneLoading(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneLoading(scene, sceneId);
        }

        private void OnSceneSaved(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneSaved(scene, sceneId);
        }

        private void OnSceneSaving(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneSaving(scene, sceneId);
        }

        private void OnSceneUnloaded(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneUnloaded(scene, sceneId);
        }

        private void OnSceneUnloading(Scene scene, Guid sceneId)
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnSceneUnloading(scene, sceneId);
        }

        /// <inheritdoc />
        public override void OnPlayBeginning()
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnPlayBeginning();
        }

        /// <inheritdoc />
        public override void OnPlayBegin()
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnPlayBegin();
        }

        /// <inheritdoc />
        public override void OnPlayEnding()
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnPlayEnding();
        }

        /// <inheritdoc />
        public override void OnPlayEnd()
        {
            for (int i = 0; i < Windows.Count; i++)
                Windows[i].OnPlayEnd();
        }

        #endregion
    }
}
