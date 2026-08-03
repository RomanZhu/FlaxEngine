// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Xml;
using FlaxEditor.Gizmo;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.Modules;
using FlaxEditor.Options;
using FlaxEditor.Viewport;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Render output control with content scaling support.
    /// </summary>
    public class ScaledRenderOutputControl : RenderOutputControl
    {
        /// <summary>
        /// Custom scale.
        /// </summary>
        public float ContentScale = 1.0f;

        /// <summary>
        /// Actual bounds size for content (incl. scale).
        /// </summary>
        public Float2 ContentSize => Size / ContentScale;

        /// <inheritdoc />
        public ScaledRenderOutputControl(SceneRenderTask task)
        : base(task)
        {
        }

        /// <inheritdoc />
        public override void Draw()
        {
            DrawSelf();

            // Draw children with scale
            var scaling = new Float3(ContentScale, ContentScale, 1);
            Matrix3x3.Scaling(ref scaling, out Matrix3x3 scale);
            Render2D.PushTransform(scale);
            if (ClipChildren)
            {
                GetDesireClientArea(out var clientArea);
                Render2D.PushClip(ref clientArea);
                DrawChildren();
                Render2D.PopClip();
            }
            else
            {
                DrawChildren();
            }

            Render2D.PopTransform();
        }

        /// <inheritdoc />
        public override void GetDesireClientArea(out Rectangle rect)
        {
            // Scale the area for the client controls
            rect = new Rectangle(Float2.Zero, Size / ContentScale);
        }

        /// <inheritdoc />
        public override bool IntersectsContent(ref Float2 locationParent, out Float2 location)
        {
            // Skip local PointFromParent but use base code
            location = base.PointFromParent(ref locationParent);
            return ContainsPoint(ref location);
        }

        /// <inheritdoc />
        public override bool IntersectsChildContent(Control child, Float2 location, out Float2 childSpaceLocation)
        {
            location /= ContentScale;
            return base.IntersectsChildContent(child, location, out childSpaceLocation);
        }

        /// <inheritdoc />
        public override bool ContainsPoint(ref Float2 location, bool precise = false)
        {
            if (precise) // Ignore as utility-only element
                return false;
            return base.ContainsPoint(ref location, precise);
        }

        /// <inheritdoc />
        public override bool RayCast(ref Float2 location, out Control hit)
        {
            var p = location / ContentScale;
            if (RayCastChildren(ref p, out hit))
                return true;
            return base.RayCast(ref location, out hit);
        }

        /// <inheritdoc />
        public override Float2 PointToParent(ref Float2 location)
        {
            var result = base.PointToParent(ref location);
            result *= ContentScale;
            return result;
        }

        /// <inheritdoc />
        public override Float2 PointFromParent(ref Float2 location)
        {
            var result = base.PointFromParent(ref location);
            result /= ContentScale;
            return result;
        }
    }

    /// <summary>
    /// Provides in-editor play mode simulation.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.EditorWindow" />
    public class GameWindow : EditorWindow
    {
        private readonly ScaledRenderOutputControl _viewport;
        private readonly GameRoot _guiRoot;
        private ToolStrip _toolStrip;
        private ToolStripButton _viewportSizeButton;
        private ToolStripButton _debugViewButton;
        private ToolStripButton _showGuiButton;
        private ToolStripButton _editGuiButton;
        private ToolStripButton _debugDrawButton;
        private ToolStripButton _vsyncButton;
        private ToolStripButton _muteButton;
        private ToolStripButton _shortcutsButton;
        private ToolStripButton _gizmosButton;
        private bool _showGUI = true, _editGUI = true;
        private bool _showDebugDraw = true;
        private bool _flaxShortcutsEnabled = true;
        private bool _showGameViewGizmos = false;
        private bool _audioMuted = false;
        private float _audioVolume = 1;
        private bool _isMaximized = false, _isFullscreen = false, _isUnlockingMouse = false;
        private bool _isFloating = false, _isBorderless = false;
        private bool _isMouseLockBlocked = false;
        private bool _gameViewActorPickMouseDown = false;
        private bool _forceHideToolStrip = false;
        private bool _cursorVisible = true;
        private float _gameStartTime;
        private Float2 _gameViewActorPickMouseDownLocation;
        private GUI.Docking.DockState _maximizeRestoreDockState;
        private GUI.Docking.DockPanel _maximizeRestoreDockTo;
        private int _maximizeRestoreTabIndex = -1;
        private ContainerControl _maximizeRestoreParent;
        private AnchorPresets _maximizeRestoreAnchorPreset;
        private Margin _maximizeRestoreOffsets;
        private CursorLockMode _cursorLockMode = CursorLockMode.None;

        // Viewport scaling variables
        private int _defaultScaleActiveIndex = 0;
        private int _customScaleActiveIndex = -1;
        private float _viewportAspectRatio = 1;
        private float _windowAspectRatio = 1;
        private bool _useAspect = false;
        private bool _freeAspect = true;

        private List<PlayModeFocusOptions> _focusOptions = new List<PlayModeFocusOptions>()
        {
            new PlayModeFocusOptions
            {
                Name = "None",
                Tooltip = "Don't change focus.",
                FocusOption = InterfaceOptions.PlayModeFocus.None,
            },
            new PlayModeFocusOptions
            {
                Name = "Game Window (Click to Focus)",
                Tooltip = "Show and select the Game Window. Click the Game view to focus it for gameplay input.",
                FocusOption = InterfaceOptions.PlayModeFocus.GameWindowNoFocus,
            },
            new PlayModeFocusOptions
            {
                Name = "Game Window",
                Tooltip = "Focus the Game Window.",
                FocusOption = InterfaceOptions.PlayModeFocus.GameWindow,
            },
            new PlayModeFocusOptions
            {
                Name = "Game Window Then Restore",
                Tooltip = "Focus the Game Window. On play mode end restore focus to the previous window.",
                FocusOption = InterfaceOptions.PlayModeFocus.GameWindowThenRestore,
            },
        };

        /// <summary>
        /// Fired when the game window audio is muted.
        /// </summary>
        public event Action MuteAudio;

        /// <summary>
        /// Fired when the game window master audio volume is changed.
        /// </summary>
        public event Action<float> MasterVolumeChanged;

        /// <summary>
        /// Gets the viewport.
        /// </summary>
        public ScaledRenderOutputControl Viewport => _viewport;

        /// <summary>
        /// Gets a value indicating whether mouse locking is blocked until the Game view is clicked.
        /// </summary>
        internal bool IsMouseLockBlocked => _isMouseLockBlocked;

        /// <summary>
        /// Gets or sets a value indicating whether show game GUI in the view or keep it hidden.
        /// </summary>
        public bool ShowGUI
        {
            get => _showGUI;
            set
            {
                if (value != _showGUI)
                {
                    _showGUI = value;
                    _guiRoot.Visible = value;
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether allow editing game GUI in the view or keep it visible-only.
        /// </summary>
        public bool EditGUI
        {
            get => _editGUI;
            set
            {
                if (value != _editGUI)
                {
                    _editGUI = value;
                    _guiRoot.Editable = value;
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether show Debug Draw shapes in the view or keep it hidden.
        /// </summary>
        public bool ShowDebugDraw
        {
            get => _showDebugDraw;
            set => _showDebugDraw = value;
        }


        /// <summary>
        /// Gets or set a value indicating whether Audio is muted.
        /// </summary>
        public bool AudioMuted
        {
            get => _audioMuted;
            set
            {
                Audio.MasterVolume = value ? 0 : AudioVolume;
                _audioMuted = value;
                MuteAudio?.Invoke();
                UpdateToolStrip();
            }
        }

        /// <summary>
        /// Gets or sets a value that set the audio volume.
        /// </summary>
        public float AudioVolume
        {
            get => _audioVolume;
            set
            {
                if (!AudioMuted)
                    Audio.MasterVolume = value;
                _audioVolume = value;
                MasterVolumeChanged?.Invoke(value);
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether the game window is maximized.
        /// </summary>
        private bool IsMaximized
        {
            get => _isMaximized;
            set
            {
                if (_isMaximized == value)
                    return;
                _isMaximized = value;
                if (value)
                {
                    ShowGameWindow();
                    IsFullscreen = false;
                    _maximizeRestoreParent = Parent;
                    _maximizeRestoreAnchorPreset = AnchorPreset;
                    _maximizeRestoreOffsets = Offsets;
                    Parent = Editor.Instance.UI.MasterPanel;
                    AnchorPreset = AnchorPresets.StretchAll;
                    Offsets = Margin.Zero;
                    FocusGameViewport();
                }
                else
                {
                    if (_maximizeRestoreParent != null)
                    {
                        AnchorPreset = _maximizeRestoreAnchorPreset;
                        Offsets = _maximizeRestoreOffsets;
                        Parent = _maximizeRestoreParent;
                        _maximizeRestoreParent = null;
                    }
                    if (_dockedTo != null)
                    {
                        _dockedTo.SelectTab(null, false);
                        _dockedTo.SelectTab(this, false);
                    }
                }
            }
        }

        /// <summary>
        /// Toggles the game view fullscreen mode.
        /// </summary>
        internal void ToggleFullscreen()
        {
            IsFullscreen = !_isFullscreen;
        }

        /// <summary>
        /// Gets or sets a value indicating whether the game window is shown in a borderless fullscreen floating window.
        /// </summary>
        private bool IsFullscreen
        {
            get => _isFullscreen;
            set
            {
                if (_isFullscreen == value)
                    return;
                _isFullscreen = value;
                if (value)
                {
                    IsMaximized = false;
                    if (_isBorderless)
                        IsBorderless = false;
                    var monitorBounds = Platform.GetMonitorBounds(PointToScreen(Size * 0.5f));
                    SetFullscreenToolStripHidden(true);
                    EditorViewport.SetFullscreenRenderingSuppression(_viewport, true);
                    IsFloating = true;
                    if (_dockedTo is GUI.Docking.FloatWindowDockPanel floatingPanel)
                        floatingPanel.HideDecorations = true;
                    var rootWindow = RootWindow;
                    if (rootWindow != null)
                    {
                        rootWindow.Window.Position = monitorBounds.Location;
                        rootWindow.Window.SetBorderless(true, true);
                        rootWindow.Window.ClientSize = monitorBounds.Size;
                        rootWindow.BringToFront(true);
                        FocusGameViewport();
                    }
                }
                else
                {
                    SetFullscreenToolStripHidden(false);
                    EditorViewport.SetFullscreenRenderingSuppression(_viewport, false);
                    if (_dockedTo is GUI.Docking.FloatWindowDockPanel floatingPanel)
                        floatingPanel.HideDecorations = false;
                    var rootWindow = RootWindow;
                    rootWindow?.Window.SetBorderless(false);
                    IsFloating = false;
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether the game window is floating (popup, only in play mode).
        /// </summary>
        private bool IsFloating
        {
            get => _isFloating;
            set
            {
                if (_isFloating == value)
                    return;
                _isFloating = value;
                var rootWindow = RootWindow;
                if (value)
                {
                    _maximizeRestoreDockTo = _dockedTo;
                    _maximizeRestoreDockState = _dockedTo?.TryGetDockState(out _) ?? GUI.Docking.DockState.Unknown;
                    _maximizeRestoreTabIndex = _dockedTo?.GetTabIndex(this) ?? -1;
                    if (_maximizeRestoreDockState != GUI.Docking.DockState.Float)
                    {
                        var monitorBounds = Platform.GetMonitorBounds(PointToScreen(Size * 0.5f));
                        var size = DefaultSize;
                        var location = monitorBounds.Location + monitorBounds.Size * 0.5f - size * 0.5f;
                        ShowFloating(location, size, WindowStartPosition.Manual);
                    }
                }
                else
                {
                    // Restore
                    if (rootWindow != null)
                        rootWindow.Restore();
                    if (_maximizeRestoreDockTo != null && _maximizeRestoreDockTo.IsDisposing)
                        _maximizeRestoreDockTo = null;
                    if (_maximizeRestoreDockTo != null && _maximizeRestoreDockState != GUI.Docking.DockState.Float)
                        _maximizeRestoreDockTo.DockWindowAt(this, _maximizeRestoreTabIndex, true);
                    else
                        Show(_maximizeRestoreDockState, _maximizeRestoreDockTo);
                    _maximizeRestoreTabIndex = -1;
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether the game window is borderless (only in play mode).
        /// </summary>
        private bool IsBorderless
        {
            get => _isBorderless;
            set
            {
                if (_isBorderless == value)
                    return;
                _isBorderless = value;
                if (value)
                {
                    SetFullscreenToolStripHidden(true);
                    EditorViewport.SetFullscreenRenderingSuppression(_viewport, true);
                    IsFloating = true;
                    if (_dockedTo is GUI.Docking.FloatWindowDockPanel floatingPanel)
                        floatingPanel.HideDecorations = true;
                    var rootWindow = RootWindow;
                    var monitorBounds = Platform.GetMonitorBounds(rootWindow.RootWindow.Window.ClientPosition);
                    rootWindow.Window.Position = monitorBounds.Location;
                    rootWindow.Window.SetBorderless(true, true);
                    rootWindow.Window.ClientSize = monitorBounds.Size;
                }
                else
                {
                    SetFullscreenToolStripHidden(false);
                    EditorViewport.SetFullscreenRenderingSuppression(_viewport, false);
                    if (_dockedTo is GUI.Docking.FloatWindowDockPanel floatingPanel)
                        floatingPanel.HideDecorations = false;
                    RootWindow?.Window.SetBorderless(false);
                    IsFloating = false;
                }
            }
        }

        /// <summary>
        /// Gets or sets a value indicating whether center mouse position on window focus in play mode. Helps when working with games that lock mouse cursor.
        /// </summary>
        public bool CenterMouseOnFocus { get; set; }

        /// <summary>
        /// Gets or sets a value indicating what panel should be shown and/or focused when play mode starts.
        /// </summary>
        public InterfaceOptions.PlayModeFocus FocusOnPlayOption { get; set; }

        private class PlayModeFocusOptions
        {
            /// <summary>
            /// The name.
            /// </summary>
            public string Name;

            /// <summary>
            /// The tooltip.
            /// </summary>
            public string Tooltip;

            /// <summary>
            /// The type of focus.
            /// </summary>
            public InterfaceOptions.PlayModeFocus FocusOption;

            /// <summary>
            /// If the option is active.
            /// </summary>
            public bool Active;
        }

        /// <summary>
        /// Root control for game UI preview in Editor. Supports basic UI editing via <see cref="UIEditorRoot"/>.
        /// </summary>
        private class GameRoot : UIEditorRoot
        {
            internal bool Editable = true;
            public override bool EnableInputs => !Time.GamePaused && Editor.IsPlayMode;
            public override bool EnableSelecting => (!Editor.IsPlayMode || Time.GamePaused) && Editable;
            public override TransformGizmo TransformGizmo => Editor.Instance.MainTransformGizmo;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="GameWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public GameWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Game";
            Icon = editor.Icons.Play64;
            AutoFocus = true;

            var task = MainRenderTask.Instance;

            // Setup viewport
            _viewport = new ScaledRenderOutputControl(task)
            {
                AnchorPreset = AnchorPresets.StretchAll,
                Offsets = Margin.Zero,
                AutoFocus = false,
                Parent = this
            };
            task.PostRender += OnPostRender;

            // Override the game GUI root
            _guiRoot = new GameRoot
            {
                Parent = _viewport
            };
            RootControl.GameRoot = _guiRoot.UIRoot;
            InitToolStrip();

            SizeChanged += control => { ResizeViewport(); };

            Editor.StateMachine.PlayingState.SceneDuplicating += PlayingStateOnSceneDuplicating;
            Editor.StateMachine.PlayingState.SceneRestored += PlayingStateOnSceneRestored;

            // Link editor options
            Editor.Options.OptionsChanged += OnOptionsChanged;
            OnOptionsChanged(Editor.Options.Options);

            InputActions.Add(options => options.TakeScreenshot, () => Screenshot.Capture(string.Empty));
            InputActions.Add(options => options.DebuggerUnlockMouse, UnlockMouseInPlay);
            InputActions.Add(options => Editor.StateMachine.IsPlayMode ? options.DebuggerUnlockMouseSecondary : new InputBinding(KeyboardKeys.None), UnlockMouseInPlay);
            InputActions.Add(options => options.ToggleFullscreen, ToggleFullscreen);
            InputActions.Add(options => options.Play, Editor.Instance.Simulation.DelegatePlayOrStopPlayInEditor);
            InputActions.Add(options => options.Pause, Editor.Instance.Simulation.RequestResumeOrPause);
            InputActions.Add(options => options.StepFrame, Editor.Instance.Simulation.RequestPlayOneFrame);
#if USE_PROFILER
            InputActions.Add(options => options.ProfilerStartStop, () =>
            {
                bool recording = !Editor.Instance.Windows.ProfilerWin.LiveRecording;
                Editor.Instance.Windows.ProfilerWin.LiveRecording = recording;
                Editor.Instance.UI.AddStatusMessage($"Profiling {(recording ? "started" : "stopped")}.");
            });
            InputActions.Add(options => options.ProfilerClear, () =>
            {
                Editor.Instance.Windows.ProfilerWin.Clear();
                Editor.Instance.UI.AddStatusMessage("Profiling results cleared.");
            });
#endif
            InputActions.Add(options => options.Save, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SaveAll();
            });
            InputActions.Add(options => options.Undo, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.PerformUndo();
                Focus();
            });
            InputActions.Add(options => options.Redo, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.PerformRedo();
                Focus();
            });
            InputActions.Add(options => options.Cut, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SceneEditing.Cut();
            });
            InputActions.Add(options => options.Copy, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SceneEditing.Copy();
            });
            InputActions.Add(options => options.Paste, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SceneEditing.Paste();
            });
            InputActions.Add(options => options.Duplicate, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SceneEditing.Duplicate();
            });
            InputActions.Add(options => options.Delete, () =>
            {
                if (Editor.IsPlayMode)
                    return;
                Editor.Instance.SceneEditing.Delete();
            });
            InputActions.Add(options => options.FocusConsoleCommand, () => Editor.Instance.Windows.OutputLogWin.FocusCommand());
        }

        private void InitToolStrip()
        {
            var interfaceOptions = Editor.Options.Options.Interface;
            _toolStrip = new ToolStrip(22.0f)
            {
                Parent = this,
                ItemsMargin = new Margin(2, 2, 1, 1),
                UseMenuSelection = true,
                UseItemContextMenu = true,
                UseOverlayStyle = true,
                BackgroundColor = Color.Transparent,
            };
            _toolStrip.ApplyLayout(interfaceOptions.GameViewToolStripLayout);
            _toolStrip.LayoutChanged += () =>
            {
                Editor.Options.Options.Interface.GameViewToolStripLayout = _toolStrip.CaptureLayout();
                Editor.Options.SaveOptions();
            };

            _viewportSizeButton = AddToolStripMenuButton(GetViewportSizeLabel(), SpriteHandle.Invalid, CreateViewportSizeMenu(), ToolStripAnchor.Left, "Flax.Game.Size");
            _viewportSizeButton.LinkTooltip("Viewport size.");
            AddToolStripMenuButton("View", SpriteHandle.Invalid, CreateGameViewOptionsMenu(), ToolStripAnchor.Left, "Flax.Game.View").LinkTooltip("Game view options.");
            _debugViewButton = AddToolStripMenuButton("Default", SpriteHandle.Invalid, EditorViewport.CreateViewModeContextMenu(_viewport.Task), ToolStripAnchor.Left, "Flax.Game.Debug");
            _debugViewButton.LinkTooltip("Debug view.");
            var flagsButton = AddToolStripMenuButton("Flags", SpriteHandle.Invalid, EditorViewport.CreateViewFlagsContextMenu(_viewport.Task, ViewFlags.DefaultGame), ToolStripAnchor.Left, "Flax.Game.Flags.Optional");
            flagsButton.LinkTooltip("View flags.");
            if (!_toolStrip.HasSavedState("Flax.Game.Flags.Optional"))
                _toolStrip.SetItemVisible(flagsButton, false, false);
            var layersButton = AddToolStripMenuButton("Layers", SpriteHandle.Invalid, EditorViewport.CreateViewLayersContextMenu(_viewport.Task), ToolStripAnchor.Left, "Flax.Game.Layers.Optional");
            layersButton.LinkTooltip("View layer visibility.");
            if (!_toolStrip.HasSavedState("Flax.Game.Layers.Optional"))
                _toolStrip.SetItemVisible(layersButton, false, false);
            _showGuiButton = AddToolStripButton("GUI", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Game.Gui", () => ShowGUI = !ShowGUI);
            _showGuiButton.LinkTooltip("Toggle game GUI.");
            _editGuiButton = AddToolStripButton("Edit GUI", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Game.EditGui", () => EditGUI = !EditGUI);
            _editGuiButton.LinkTooltip("Toggle game GUI editing.");
            _debugDrawButton = AddToolStripButton("Debug Draw", SpriteHandle.Invalid, ToolStripAnchor.Left, "Flax.Game.DebugDraw", () => ShowDebugDraw = !ShowDebugDraw);
            _debugDrawButton.LinkTooltip("Toggle debug draw.");
            AddToolStripMenuButton("Play Focus", SpriteHandle.Invalid, CreatePlayFocusMenu(), ToolStripAnchor.Center, "Flax.Game.Focus").LinkTooltip("Focus behavior when play mode starts.");
            AddToolStripMenuButton("Window", SpriteHandle.Invalid, CreateWindowModeMenu(), ToolStripAnchor.Center, "Flax.Game.WindowMode").LinkTooltip("Game window mode when play mode starts.");
            _vsyncButton = AddToolStripButton("VSync", SpriteHandle.Invalid, ToolStripAnchor.Right, "Flax.Game.VSync", () => Graphics.UseVSync = !Graphics.UseVSync);
            _vsyncButton.LinkTooltip("Toggle VSync.");
            _muteButton = _toolStrip.AddGlyphButton(ToolStripGlyph.Speaker, ToolStripAnchor.Right, "Flax.Game.Audio", () => AudioMuted = !AudioMuted);
            CompactToolStripButton(_muteButton, "Audio");
            _muteButton.LinkTooltip("Mute or unmute game audio.");
            _shortcutsButton = _toolStrip.AddGlyphButton(ToolStripGlyph.Keyboard, ToolStripAnchor.Right, "Flax.Game.Shortcuts", () => _flaxShortcutsEnabled = !_flaxShortcutsEnabled);
            CompactToolStripButton(_shortcutsButton, "Shortcuts");
            _shortcutsButton.LinkTooltip("Enable Flax editor shortcuts in Game View.");
            _gizmosButton = _toolStrip.AddGlyphButton(ToolStripGlyph.Eye, ToolStripAnchor.Right, "Flax.Game.Gizmos", () => _showGameViewGizmos = !_showGameViewGizmos);
            CompactToolStripButton(_gizmosButton, "Gizmos");
            _gizmosButton.LinkTooltip("Toggle editor gizmos in Game View.");

            UpdateToolStrip();
        }

        private ToolStripButton AddToolStripButton(string text, SpriteHandle icon, ToolStripAnchor anchor, string id, Action onClick = null)
        {
            var button = new ToolStripButton(_toolStrip.ItemsHeight, ref icon)
            {
                Text = text,
                Parent = _toolStrip,
                UseBlueCheckedStyle = true,
                ContentMargin = 4,
                MaxIconSize = 14.0f,
            };
            _toolStrip.SetItemPlacement(button, anchor, -1, id);
            button.CustomizationLabel = GetToolStripItemLabel(text, id);
            button.PerformLayout();
            if (onClick != null)
                button.Clicked += onClick;
            return button;
        }

        private static void CompactToolStripButton(ToolStripButton button, string label)
        {
            button.ContentMargin = 4;
            button.MaxIconSize = 14.0f;
            button.CustomizationLabel = label;
            button.PerformLayout();
        }

        private ToolStripButton AddToolStripMenuButton(string text, SpriteHandle icon, ContextMenu menu, ToolStripAnchor anchor, string id)
        {
            var button = AddToolStripButton(text, icon, anchor, id);
            button.ContextMenu = menu;
            if (!icon.IsValid)
            {
                button.DrawAsTextLabel = true;
                button.DrawTextShadow = true;
                button.UseBlueCheckedStyle = false;
            }
            button.Clicked += () => _toolStrip.SelectedMenuButton = button;
            return button;
        }

        private static string GetToolStripItemLabel(string text, string id)
        {
            if (!string.IsNullOrEmpty(text))
                return text;
            int lastDot = id.LastIndexOf('.');
            return lastDot != -1 && lastDot + 1 < id.Length ? id.Substring(lastDot + 1) : id;
        }

        private ContextMenu CreateViewportSizeMenu()
        {
            var menu = new ContextMenu();
            menu.VisibleChanged += control =>
            {
                if (!menu.IsOpened)
                    return;
                menu.DisposeAllItems();
                Editor.UI.CreateViewportSizingContextMenu(menu, _defaultScaleActiveIndex, _customScaleActiveIndex, false, v =>
                {
                    ChangeViewportRatio(v);
                    UpdateToolStrip();
                }, (a, b) =>
                {
                    _defaultScaleActiveIndex = a;
                    _customScaleActiveIndex = b;
                    UpdateToolStrip();
                });
                menu.PerformLayout();
            };
            return menu;
        }

        private ContextMenu CreateGameViewOptionsMenu()
        {
            var menu = new ContextMenu
            {
                MinimumWidth = 220,
            };
            AddCheckMenuItem(menu, "Show GUI", () => ShowGUI, value => ShowGUI = value);
            AddCheckMenuItem(menu, "Edit GUI", () => EditGUI, value => EditGUI = value);
            AddCheckMenuItem(menu, "Show Debug Draw", () => ShowDebugDraw, value => ShowDebugDraw = value);
            AddCheckMenuItem(menu, "Editor Gizmos", () => _showGameViewGizmos, value => _showGameViewGizmos = value);
            AddCheckMenuItem(menu, "Flax Shortcuts", () => _flaxShortcutsEnabled, value => _flaxShortcutsEnabled = value);
            AddCheckMenuItem(menu, "Mute Audio", () => AudioMuted, value => AudioMuted = value);
            AddCheckMenuItem(menu, "VSync", () => Graphics.UseVSync, value => Graphics.UseVSync = value);

            menu.AddSeparator();
            EditorViewport.PopulateViewFlagsContextMenu(menu.AddChildMenu("View Flags").ContextMenu, _viewport.Task, ViewFlags.DefaultGame);
            EditorViewport.PopulateViewLayersContextMenu(menu.AddChildMenu("View Layers").ContextMenu, _viewport.Task);

            menu.AddSeparator();

            var brightness = menu.AddButton("Viewport Brightness");
            brightness.CloseMenuOnClick = false;
            var brightnessValue = new FloatValueBox(_viewport.Brightness, 150, 2, 55.0f, 0.001f, 10.0f, 0.001f)
            {
                Parent = brightness
            };
            brightnessValue.ValueChanged += () => _viewport.Brightness = brightnessValue.Value;
            menu.VisibleChanged += control => brightnessValue.Value = _viewport.Brightness;

            var resolution = menu.AddButton("Viewport Resolution");
            resolution.CloseMenuOnClick = false;
            var resolutionValue = new FloatValueBox(_viewport.ResolutionScale, 150, 2, 55.0f, 0.1f, 4.0f, 0.001f)
            {
                Parent = resolution
            };
            resolutionValue.ValueChanged += () => _viewport.ResolutionScale = resolutionValue.Value;
            menu.VisibleChanged += control => resolutionValue.Value = _viewport.ResolutionScale;

            menu.AddSeparator();
            menu.AddButton("Take Screenshot", TakeScreenshot);
            var clearDebugDraw = menu.AddButton("Clear Debug Draw", () => DebugDraw.Clear());
            clearDebugDraw.CloseMenuOnClick = false;
            menu.VisibleChanged += control => clearDebugDraw.Visible = DebugDraw.CanClear();
            AddToolStripVisibilityItem(menu, "Game View Toolstrip");
            return menu;
        }

        private void AddCheckMenuItem(ContextMenu menu, string text, Func<bool> getter, Action<bool> setter)
        {
            var button = menu.AddButton(text);
            button.CloseMenuOnClick = false;
            var checkbox = new CheckBox(150, 2, getter())
            {
                Parent = button
            };
            checkbox.StateChanged += x =>
            {
                setter(x.Checked);
                UpdateToolStrip();
            };
            menu.VisibleChanged += control => checkbox.Checked = getter();
        }

        private ContextMenu CreatePlayFocusMenu()
        {
            var menu = new ContextMenu
            {
                MinimumWidth = 240,
            };
            GenerateFocusOptionsContextMenu(menu);
            menu.AddSeparator();
            var reset = menu.AddButton("Remove override");
            reset.TooltipText = "Reset the override to the value set in the editor options.";
            reset.Clicked += () => FocusOnPlayOption = Editor.Options.Options.Interface.FocusOnPlayMode;
            menu.VisibleChanged += UpdateFocusOptionsContextMenu;
            return menu;
        }

        private void UpdateFocusOptionsContextMenu(Control control)
        {
            var menu = (ContextMenu)control;
            foreach (var child in menu.Items)
            {
                if (child is ContextMenuButton button && button.Tag is PlayModeFocusOptions option)
                {
                    option.Active = option.FocusOption == FocusOnPlayOption;
                    button.Icon = option.Active ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
                }
            }
        }

        private ContextMenu CreateWindowModeMenu()
        {
            var menu = new ContextMenu
            {
                MinimumWidth = 220,
            };
            AddWindowModeButton(menu, "Docked", InterfaceOptions.GameWindowMode.Docked, "Shows the game window docked, inside the editor.");
            AddWindowModeButton(menu, "Popup", InterfaceOptions.GameWindowMode.PopupWindow, "Shows the game window as a popup.");
            AddWindowModeButton(menu, "Maximized", InterfaceOptions.GameWindowMode.MaximizedWindow, "Shows the game window maximized in the editor window.");
            AddWindowModeButton(menu, "Borderless", InterfaceOptions.GameWindowMode.BorderlessWindow, "Shows the game window borderless.");
            menu.VisibleChanged += UpdateWindowModeContextMenu;
            return menu;
        }

        private void AddWindowModeButton(ContextMenu menu, string text, InterfaceOptions.GameWindowMode mode, string tooltip)
        {
            var button = menu.AddButton(text);
            button.CloseMenuOnClick = false;
            button.Tag = mode;
            button.TooltipText = tooltip;
            button.Clicked += () =>
            {
                Editor.Options.Options.Interface.DefaultGameWindowMode = mode;
                Editor.Options.SaveOptions();
                UpdateWindowModeContextMenu(menu);
            };
        }

        private void UpdateWindowModeContextMenu(Control control)
        {
            var menu = (ContextMenu)control;
            var selected = Editor.Options.Options.Interface.DefaultGameWindowMode;
            foreach (var child in menu.Items)
            {
                if (child is ContextMenuButton button && button.Tag is InterfaceOptions.GameWindowMode mode)
                    button.Icon = mode == selected ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
            }
        }

        private ContextMenu CreateToolStripSettingsMenu()
        {
            var menu = new ContextMenu
            {
                MinimumWidth = 220,
            };
            AddToolStripVisibilityItem(menu, "Game View Toolstrip");
            return menu;
        }

        private void AddToolStripVisibilityItem(ContextMenu menu, string text)
        {
            var button = menu.AddButton(text);
            button.CloseMenuOnClick = false;
            var checkbox = new CheckBox(150, 2, Editor.Options.Options.Interface.ShowGameViewToolStrip)
            {
                Parent = button
            };
            checkbox.StateChanged += x => SetToolStripVisible(x.Checked);
            menu.VisibleChanged += control => checkbox.Checked = Editor.Options.Options.Interface.ShowGameViewToolStrip;
        }

        private void SetToolStripVisible(bool visible)
        {
            var interfaceOptions = Editor.Options.Options.Interface;
            if (interfaceOptions.ShowGameViewToolStrip == visible)
                return;
            interfaceOptions.ShowGameViewToolStrip = visible;
            Editor.Options.SaveOptions();
            UpdateToolStrip();
        }

        private void SetFullscreenToolStripHidden(bool value)
        {
            if (_forceHideToolStrip == value)
                return;
            _forceHideToolStrip = value;
            UpdateToolStrip();
            PerformLayout();
        }

        private void UpdateToolStrip(bool forceNotPlaying = false)
        {
            if (_toolStrip == null)
                return;
            var visible = !_forceHideToolStrip && Editor.Options.Options.Interface.ShowGameViewToolStrip && (forceNotPlaying || !Editor.IsPlayMode);
            _toolStrip.Visible = visible;
            _toolStrip.Enabled = visible;
            SetButtonText(_viewportSizeButton, GetViewportSizeLabel());
            SetButtonText(_debugViewButton, EditorViewport.GetViewModeDisplayName(_viewport.Task.ViewMode));
            if (_showGuiButton != null)
                _showGuiButton.Checked = ShowGUI;
            if (_editGuiButton != null)
                _editGuiButton.Checked = EditGUI;
            if (_debugDrawButton != null)
                _debugDrawButton.Checked = ShowDebugDraw;
            if (_vsyncButton != null)
                _vsyncButton.Checked = Graphics.UseVSync;
            if (_muteButton != null)
            {
                _muteButton.Checked = AudioMuted;
                var glyph = AudioMuted ? ToolStripGlyph.MutedSpeaker : ToolStripGlyph.Speaker;
                if (_muteButton.Glyph != glyph)
                    _muteButton.Glyph = glyph;
            }
            if (_shortcutsButton != null)
                _shortcutsButton.Checked = _flaxShortcutsEnabled;
            if (_gizmosButton != null)
                _gizmosButton.Checked = _showGameViewGizmos;
        }

        private static void SetButtonText(ToolStripButton button, string text)
        {
            if (button != null && button.Text != text)
                button.Text = text;
        }

        private string GetViewportSizeLabel()
        {
            if (_customScaleActiveIndex >= 0 && _customScaleActiveIndex < Editor.UI.CustomViewportScaleOptions.Count)
                return Editor.UI.CustomViewportScaleOptions[_customScaleActiveIndex].Label;
            if (_defaultScaleActiveIndex >= 0 && _defaultScaleActiveIndex < Editor.UI.DefaultViewportScaleOptions.Count)
                return Editor.UI.DefaultViewportScaleOptions[_defaultScaleActiveIndex].Label;
            return "Viewport Size";
        }

        private void ChangeViewportRatio(UIModule.ViewportScaleOption v)
        {
            if (v == null)
                return;
            if (v.Size.Y <= 0 || v.Size.X <= 0)
                return;

            if (string.Equals(v.Label, "Free Aspect", StringComparison.Ordinal) && v.Size == new Int2(1, 1))
            {
                _freeAspect = true;
                _useAspect = true;
            }
            else
            {
                switch (v.ScaleType)
                {
                case UIModule.ViewportScaleOption.ViewportScaleType.Aspect:
                    _useAspect = true;
                    _freeAspect = false;
                    break;
                case UIModule.ViewportScaleOption.ViewportScaleType.Resolution:
                    _useAspect = false;
                    _freeAspect = false;
                    break;
                }
            }

            _viewportAspectRatio = (float)v.Size.X / v.Size.Y;

            if (!_freeAspect)
            {
                if (!_useAspect)
                {
                    _viewport.KeepAspectRatio = true;
                    _viewport.CustomResolution = new Int2(v.Size.X, v.Size.Y);
                }
                else
                {
                    _viewport.CustomResolution = new Int2?();
                    _viewport.KeepAspectRatio = true;
                }
            }
            else
            {
                _viewport.CustomResolution = new Int2?();
                _viewport.KeepAspectRatio = false;
            }

            ResizeViewport();
            UpdateToolStrip();
        }

        private void ResizeViewport()
        {
            _windowAspectRatio = _freeAspect ? 1 : Width / Height;
            var scaleWidth = _viewportAspectRatio / _windowAspectRatio;
            var scaleHeight = _windowAspectRatio / _viewportAspectRatio;

            if (scaleHeight < 1)
            {
                _viewport.Bounds = new Rectangle(0, Height * (1 - scaleHeight) / 2, Width, Height * scaleHeight);
            }
            else
            {
                _viewport.Bounds = new Rectangle(Width * (1 - scaleWidth) / 2, 0, Width * scaleWidth, Height);
            }

            if (_viewport.KeepAspectRatio)
            {
                var resolution = _viewport.CustomResolution.HasValue ? (Float2)_viewport.CustomResolution.Value : Size;
                if (scaleHeight < 1)
                {
                    _viewport.ContentScale = _viewport.Width / resolution.X;
                }
                else
                {
                    _viewport.ContentScale = _viewport.Height / resolution.Y;
                }
            }
            else
            {
                _viewport.ContentScale = 1;
            }

            _viewport.SyncBackbufferSize();
            PerformLayout();
        }

        private void OnPostRender(GPUContext context, ref RenderContext renderContext)
        {
            // Debug Draw shapes
            if (_showDebugDraw)
            {
                var task = _viewport.Task;

                // Draw actors debug shapes manually if editor viewport is hidden (game viewport task is always rendered before editor viewports)
                var editWindowViewport = Editor.Windows.EditWin.Viewport;
                if (editWindowViewport.Task.LastUsedFrame != Engine.FrameCount)
                {
                    var drawDebugData = editWindowViewport.DebugDrawData;
                    drawDebugData.Clear();
                    var selectedParents = editWindowViewport.TransformGizmo.SelectedParents;
                    if (selectedParents.Count > 0)
                    {
                        for (int i = 0; i < selectedParents.Count; i++)
                        {
                            if (selectedParents[i].IsActiveInHierarchy)
                                selectedParents[i].OnDebugDraw(drawDebugData);
                        }
                    }
                    drawDebugData.DrawActors(true);
                }

                DebugDraw.Draw(ref renderContext, task.OutputView);
            }

            if (_showGameViewGizmos && !Editor.IsPlayMode)
            {
                var editWindowViewport = Editor.Windows.EditWin.Viewport;
                if (editWindowViewport.EditorPrimitives && editWindowViewport.EditorPrimitives.CanRender())
                    editWindowViewport.EditorPrimitives.Render(context, ref renderContext, _viewport.Task.Output, _viewport.Task.Output);
            }
        }

        private void OnOptionsChanged(EditorOptions options)
        {
            CenterMouseOnFocus = options.Interface.CenterMouseOnGameWinFocus;
            FocusOnPlayOption = options.Interface.FocusOnPlayMode;
            UpdateToolStrip();
        }

        private void PlayingStateOnSceneDuplicating()
        {
            // Remove remaining GUI controls so loaded scene can add own GUI
            //_guiRoot.DisposeChildren();

            // Show GUI
            //_guiRoot.Visible = _showGUI;
        }

        private void PlayingStateOnSceneRestored()
        {
            // Remove remaining GUI controls so loaded scene can add own GUI
            //_guiRoot.DisposeChildren();

            // Hide GUI
            //_guiRoot.Visible = false;
        }

        /// <inheritdoc />
        protected override bool CanOpenContentFinder => !Editor.IsPlayMode;

        /// <inheritdoc />
        protected override bool CanUseNavigation => false;

        /// <inheritdoc />
        public override void OnPlayBeginning()
        {
            base.OnPlayBeginning();
            _isMouseLockBlocked = true;
            _cursorVisible = true;
            _cursorLockMode = CursorLockMode.None;
            Screen.CursorVisible = true;
            Screen.CursorLock = CursorLockMode.None;
            UpdateToolStrip();
        }

        /// <inheritdoc />
        public override void OnPlayBegin()
        {
            _gameStartTime = Time.UnscaledGameTime;
        }

        /// <inheritdoc />
        public override void OnPlayEnd()
        {
            _isMouseLockBlocked = false;
            IsFullscreen = false;
            IsFloating = false;
            IsMaximized = false;
            IsBorderless = false;
            Cursor = CursorType.Default;
            Screen.CursorLock = CursorLockMode.None;
            if (Screen.MainWindow != null && Screen.MainWindow.IsMouseTracking)
                Screen.MainWindow.EndTrackingMouse();
            RootControl.GameRoot?.EndMouseCapture();
            UpdateToolStrip(true);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            base.OnMouseLeave();

            // Remove focus from game window when mouse moves out and the cursor is hidden during game
            if (ContainsFocus && Parent != null && Editor.IsPlayMode && !Screen.CursorVisible && Screen.CursorLock == CursorLockMode.None)
            {
                Parent.Focus();
            }
        }

        /// <inheritdoc />
        public override void OnShowContextMenu(ContextMenu menu)
        {
            base.OnShowContextMenu(menu);

            // Focus on play
            {
                var pfMenu = menu.AddChildMenu("Focus On Play Override").ContextMenu;

                GenerateFocusOptionsContextMenu(pfMenu);

                pfMenu.AddSeparator();

                var button = pfMenu.AddButton("Remove override");
                button.TooltipText = "Reset the override to the value set in the editor options.";
                button.Clicked += () => FocusOnPlayOption = Editor.Instance.Options.Options.Interface.FocusOnPlayMode;
            }

            menu.AddSeparator();

            // Viewport Brightness
            {
                var brightness = menu.AddButton("Viewport Brightness");
                brightness.CloseMenuOnClick = false;
                var brightnessValue = new FloatValueBox(_viewport.Brightness, 140, 2, 50.0f, 0.001f, 10.0f, 0.001f)
                {
                    Parent = brightness
                };
                brightnessValue.ValueChanged += () => _viewport.Brightness = brightnessValue.Value;
            }

            // Viewport Resolution
            {
                var resolution = menu.AddButton("Viewport Resolution");
                resolution.CloseMenuOnClick = false;
                var resolutionValue = new FloatValueBox(_viewport.ResolutionScale, 140, 2, 50.0f, 0.1f, 4.0f, 0.001f)
                {
                    Parent = resolution
                };
                resolutionValue.ValueChanged += () => _viewport.ResolutionScale = resolutionValue.Value;
            }

            // Viewport aspect ratio
            {
                var vsMenu = menu.AddChildMenu("Viewport Size").ContextMenu;
                Editor.UI.CreateViewportSizingContextMenu(vsMenu, _defaultScaleActiveIndex, _customScaleActiveIndex, false, ChangeViewportRatio, (a, b) =>
                {
                    _defaultScaleActiveIndex = a;
                    _customScaleActiveIndex = b;
                });
            }

            AddToolStripVisibilityItem(menu, "Game View Toolstrip");

            // Take Screenshot
            {
                var takeScreenshot = menu.AddButton("Take Screenshot");
                takeScreenshot.Clicked += TakeScreenshot;
            }

            menu.AddSeparator();

            // Show GUI
            {
                var button = menu.AddButton("Show GUI");
                button.CloseMenuOnClick = false;
                var checkbox = new CheckBox(140, 2, ShowGUI) { Parent = button };
                checkbox.StateChanged += x => ShowGUI = x.Checked;
            }

            // Edit GUI
            {
                var button = menu.AddButton("Edit GUI");
                var checkbox = new CheckBox(140, 2, EditGUI) { Parent = button };
                checkbox.StateChanged += x => EditGUI = x.Checked;
            }

            // Show Debug Draw
            {
                var button = menu.AddButton("Show Debug Draw");
                button.CloseMenuOnClick = false;
                var checkbox = new CheckBox(140, 2, ShowDebugDraw) { Parent = button };
                checkbox.StateChanged += x => ShowDebugDraw = x.Checked;
            }

            // Clear Debug Draw
            if (DebugDraw.CanClear())
            {
                var button = menu.AddButton("Clear Debug Draw");
                button.CloseMenuOnClick = false;
                button.Clicked += () => DebugDraw.Clear();
            }

            menu.AddSeparator();

            // Mute Audio
            {
                var button = menu.AddButton("Mute Audio");
                button.CloseMenuOnClick = false;
                var checkbox = new CheckBox(140, 2, AudioMuted) { Parent = button };
                checkbox.StateChanged += x => AudioMuted = x.Checked;
            }

            // Audio Volume
            {
                var button = menu.AddButton("Audio Volume");
                button.CloseMenuOnClick = false;
                var slider = new FloatValueBox(AudioVolume, 140, 2, 50, 0, 1) { Parent = button };
                slider.ValueChanged += () => AudioVolume = slider.Value;
            }

            menu.MinimumWidth = 200;
            menu.AddSeparator();
        }

        private void GenerateFocusOptionsContextMenu(ContextMenu pfMenu)
        {
            foreach (PlayModeFocusOptions f in _focusOptions)
            {
                f.Active = f.FocusOption == FocusOnPlayOption;

                var button = pfMenu.AddButton(f.Name);
                button.CloseMenuOnClick = false;
                button.Tag = f;
                button.TooltipText = f.Tooltip;
                button.Icon = f.Active ? Style.Current.CheckBoxTick : SpriteHandle.Invalid;
                button.Clicked += () =>
                {
                    foreach (var child in pfMenu.Items)
                    {
                        if (child is ContextMenuButton cmb && cmb.Tag is PlayModeFocusOptions p)
                        {
                            if (cmb == button)
                            {
                                p.Active = true;
                                button.Icon = Style.Current.CheckBoxTick;
                                FocusOnPlayOption = p.FocusOption;
                            }
                            else if (p.Active)
                            {
                                cmb.Icon = SpriteHandle.Invalid;
                                p.Active = false;
                            }
                        }
                    }
                };
            }
        }

        /// <inheritdoc />
        public override void Draw()
        {
            UpdateToolStrip();
            base.Draw();

            var mainRenderTask = MainRenderTask.Instance;
            if (Camera.MainCamera == null && (mainRenderTask == null || !mainRenderTask.IsCustomRendering))
            {
                var style = Style.Current;
                Render2D.DrawText(style.FontLarge, "No camera", new Rectangle(Float2.Zero, Size), style.ForegroundDisabled, TextAlignment.Center, TextAlignment.Center);
            }

            // Play mode hints and overlay
            if (Editor.StateMachine.IsPlayMode)
            {
                var style = Style.Current;
                float time = Time.UnscaledGameTime - _gameStartTime;
                float timeout = 3.0f;
                float fadeOutTime = 1.0f;
                float animTime = time - timeout;
                if (animTime < 0)
                {
                    var alpha = Mathf.Saturate(-animTime / fadeOutTime);
                    var rect = new Rectangle(new Float2(6), Size - 12);
                    var input = Editor.Options.Options.Input;
                    var text = $"Press {input.DebuggerUnlockMouse}";
                    if (input.DebuggerUnlockMouseSecondary.Key != KeyboardKeys.None)
                        text += $" or {input.DebuggerUnlockMouseSecondary}";
                    text += " to unlock the mouse";
                    Render2D.DrawText(style.FontSmall, text, rect + new Float2(1.0f), style.Background * alpha, TextAlignment.Near, TextAlignment.Far);
                    Render2D.DrawText(style.FontSmall, text, rect, style.Foreground * alpha, TextAlignment.Near, TextAlignment.Far);
                }

                timeout = 1.0f;
                fadeOutTime = 0.6f;
                animTime = time - timeout;
                if (animTime < 0)
                {
                    float alpha = Mathf.Saturate(-animTime / fadeOutTime);
                    Render2D.DrawRectangle(new Rectangle(new Float2(4), Size - 8), style.SelectionBorder * alpha);
                }

                // Add overlay during debugger breakpoint hang
                if (Editor.Simulation.IsDuringBreakpointHang)
                {
                    var bounds = new Rectangle(Float2.Zero, Size);
                    Render2D.FillRectangle(bounds, new Color(0.0f, 0.0f, 0.0f, 0.2f));
                    Render2D.DrawText(Style.Current.FontLarge, "Debugger breakpoint hit...", bounds, Color.White, TextAlignment.Center, TextAlignment.Center);
                }
            }
        }

        /// <summary>
        /// Unlocks the mouse if game window is focused during play mode.
        /// </summary>
        public void UnlockMouseInPlay()
        {
            if (Editor.StateMachine.IsPlayMode)
            {
                // Cache cursor
                _cursorVisible = Screen.CursorVisible;
                _cursorLockMode = Screen.CursorLock;
                Screen.CursorVisible = true;
                if (_cursorLockMode != CursorLockMode.None)
                    Screen.CursorLock = CursorLockMode.None;
                _isMouseLockBlocked = true;

                // Defocus
                _isUnlockingMouse = true;
                Focus(null);
                _isUnlockingMouse = false;
                Editor.Windows.MainWindow.Focus();
                if (Editor.Windows.PropertiesWin.IsDocked)
                    Editor.Windows.PropertiesWin.Focus();
            }
        }

        private void RestoreCursorState()
        {
            if (!Editor.StateMachine.IsPlayMode || Editor.StateMachine.PlayingState.IsPaused)
                return;
            if (_cursorLockMode != CursorLockMode.None)
                Screen.CursorLock = _cursorLockMode;
            if (!_cursorVisible)
                Screen.CursorVisible = false;
        }

        /// <summary>
        /// Shows the game window without focusing the game viewport.
        /// </summary>
        public void ShowGameWindow()
        {
            if (!IsDocked)
            {
                ShowFloating();
            }
            else if (!IsSelected)
            {
                SelectTab(false);
            }
            Focus(null);
        }

        /// <summary>
        /// Focuses the game viewport. Shows the window if hidden or unselected.
        /// </summary>
        public void FocusGameViewport()
        {
            if (!IsDocked)
            {
                ShowFloating();
            }
            else if (!IsSelected)
            {
                SelectTab(false);
                RootWindow?.Window?.Focus();
            }
            Focus();
        }

        /// <summary>
        /// Apply the selected window mode to the game window.
        /// </summary>
        /// <param name="mode"></param>
        public void SetWindowMode(InterfaceOptions.GameWindowMode mode)
        {
            switch (mode)
            {
            case InterfaceOptions.GameWindowMode.Docked:
                break;
            case InterfaceOptions.GameWindowMode.PopupWindow:
                IsFloating = true;
                break;
            case InterfaceOptions.GameWindowMode.MaximizedWindow:
                IsMaximized = true;
                break;
            case InterfaceOptions.GameWindowMode.BorderlessWindow:
                IsBorderless = true;
                break;
            default: throw new ArgumentOutOfRangeException(nameof(mode), mode, null);
            }
        }

        /// <summary>
        /// Takes the screenshot of the current viewport.
        /// </summary>
        public void TakeScreenshot()
        {
            Screenshot.Capture(_viewport.Task);
        }

        /// <summary>
        /// Takes the screenshot of the current viewport.
        /// </summary>
        /// <param name="path">The output file path.</param>
        public void TakeScreenshot(string path)
        {
            Screenshot.Capture(_viewport.Task, path);
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (Editor.StateMachine.IsPlayMode && Editor.Options.Options.Input.DebuggerUnlockMouseSecondary.Process(this, key))
            {
                UnlockMouseInPlay();
                return true;
            }

            if (!_flaxShortcutsEnabled)
            {
                for (int i = 0; i < _children.Count; i++)
                {
                    var child = _children[i];
                    if (child.Enabled && child.ContainsFocus && child.OnKeyDown(key))
                        return true;
                }
                return true;
            }

            // Prevent closing the game window tab during a play session
            if (Editor.StateMachine.IsPlayMode && Editor.Options.Options.Input.CloseTab.Process(this, key))
            {
                return true;
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override void OnStartContainsFocus()
        {
            base.OnStartContainsFocus();

            if (Editor.StateMachine.IsPlayMode && !Editor.StateMachine.PlayingState.IsPaused)
            {
                if (_isMouseLockBlocked)
                    return;
                // Make sure the cursor is always in the viewport when cursor is locked
                bool forceCenter = _cursorLockMode != CursorLockMode.None && !IsMouseOver;

                // Center mouse in play mode
                if (CenterMouseOnFocus || forceCenter)
                {
                    var center = PointToWindow(Size * 0.5f);
                    Root.MousePosition = center;
                }

                RestoreCursorState();
            }
        }

        /// <inheritdoc />
        public override void OnEndContainsFocus()
        {
            base.OnEndContainsFocus();

            if (!_isUnlockingMouse && !_isMouseLockBlocked)
            {
                // Cache cursor
                _cursorVisible = Screen.CursorVisible;
                _cursorLockMode = Screen.CursorLock;

                // Restore cursor state, could be hidden or locked by the game
                if (!_cursorVisible)
                    Screen.CursorVisible = true;
                Screen.CursorLock = CursorLockMode.None;

                if (Editor.IsPlayMode && IsDocked && IsSelected && RootWindow.FocusedControl == null)
                {
                    // Game UI cleared focus so regain it to maintain UI navigation just like game window does
                    FlaxEngine.Scripting.InvokeOnUpdate(Focus);
                }
            }
        }

        /// <inheritdoc />
        public override Control OnNavigate(NavDirection direction, Float2 location, Control caller, List<Control> visited)
        {
            // Block leaking UI navigation focus outside the game window
            if (IsFocused && caller != this)
            {
                // Pick the first UI control if game UI is not focused yet
                foreach (var child in _guiRoot.Children)
                {
                    if (child.Visible)
                        return child.OnNavigate(direction, Float2.Zero, this, visited);
                }
            }

            return null;
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (_isMouseLockBlocked)
            {
                _isMouseLockBlocked = false;
                if (ContainsFocus)
                    RestoreCursorState();
            }

            if (CanStartGameViewActorPick(ref location, button))
            {
                _gameViewActorPickMouseDown = true;
                _gameViewActorPickMouseDownLocation = location;
                StartMouseCapture();
                Focus();
                return true;
            }

            var result = base.OnMouseDown(location, button);

            // Catch user focus
            if (!ContainsFocus)
                Focus();

            return result;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (_gameViewActorPickMouseDown && button == MouseButton.Left)
            {
                _gameViewActorPickMouseDown = false;
                EndMouseCapture();

                if (Editor.StateMachine.IsPlayMode &&
                    Editor.StateMachine.PlayingState.IsPaused &&
                    (location - _gameViewActorPickMouseDownLocation).LengthSquared <= 16.0f &&
                    TryGetGameViewportLocation(ref location, out var viewportLocation))
                {
                    PickGameViewActor(viewportLocation);
                }
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnEndMouseCapture()
        {
            _gameViewActorPickMouseDown = false;
            base.OnEndMouseCapture();
        }

        private bool CanStartGameViewActorPick(ref Float2 location, MouseButton button)
        {
            if (button != MouseButton.Left ||
                !Editor.StateMachine.IsPlayMode ||
                !Editor.StateMachine.PlayingState.IsPaused ||
                !Root.GetKey(KeyboardKeys.Control))
                return false;

            if (_toolStrip != null && _toolStrip.Visible && _toolStrip.Enabled && IntersectsChildContent(_toolStrip, location, out _))
                return false;

            return TryGetGameViewportLocation(ref location, out _);
        }

        private bool TryGetGameViewportLocation(ref Float2 location, out Float2 viewportLocation)
        {
            if (!IntersectsChildContent(_viewport, location, out viewportLocation))
                return false;

            var viewportBounds = new Rectangle(Float2.Zero, _viewport.Size);
            return viewportBounds.Contains(ref viewportLocation);
        }

        private void PickGameViewActor(Float2 viewportLocation)
        {
            if (_viewport.Width < Mathf.Epsilon || _viewport.Height < Mathf.Epsilon)
                return;

            FlaxEngine.Profiler.BeginEvent("GameView.Pick");

            var renderView = _viewport.Task.View;
            var viewport = new FlaxEngine.Viewport(0, 0, _viewport.Width, _viewport.Height);
            var projection = renderView.NonJitteredProjection;
            Matrix.Multiply(ref renderView.View, ref projection, out var viewProjection);
            var ray = Ray.GetPickRay(viewportLocation.X, viewportLocation.Y, ref viewport, ref viewProjection);
            ray.Position += renderView.Origin;

            var view = new Ray(renderView.Origin + renderView.Position, renderView.Direction);
            Editor.MainTransformGizmo.Pick(ref ray, ref view, renderView.Flags, renderView.Mode, Root.GetKey(KeyboardKeys.Shift));

            FlaxEngine.Profiler.EndEvent();
        }

        /// <inheritdoc />
        public override bool UseLayoutData => true;

        /// <inheritdoc />
        public override void OnLayoutSerialize(XmlWriter writer)
        {
            writer.WriteAttributeString("ShowGUI", ShowGUI.ToString());
            writer.WriteAttributeString("EditGUI", EditGUI.ToString());
            writer.WriteAttributeString("ShowDebugDraw", ShowDebugDraw.ToString());
            writer.WriteAttributeString("FlaxShortcutsEnabled", _flaxShortcutsEnabled.ToString());
            writer.WriteAttributeString("ShowGameViewGizmos", _showGameViewGizmos.ToString());
            writer.WriteAttributeString("DefaultViewportScalingIndex", _defaultScaleActiveIndex.ToString());
            writer.WriteAttributeString("CustomViewportScalingIndex", _customScaleActiveIndex.ToString());
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            if (bool.TryParse(node.GetAttribute("ShowGUI"), out bool value1))
                ShowGUI = value1;
            if (bool.TryParse(node.GetAttribute("EditGUI"), out value1))
                EditGUI = value1;
            if (bool.TryParse(node.GetAttribute("ShowDebugDraw"), out value1))
                ShowDebugDraw = value1;
            if (bool.TryParse(node.GetAttribute("FlaxShortcutsEnabled"), out value1))
                _flaxShortcutsEnabled = value1;
            if (bool.TryParse(node.GetAttribute("ShowGameViewGizmos"), out value1))
                _showGameViewGizmos = value1;
            if (int.TryParse(node.GetAttribute("DefaultViewportScalingIndex"), out int value2))
                _defaultScaleActiveIndex = value2;
            if (int.TryParse(node.GetAttribute("CustomViewportScalingIndex"), out value2))
                _customScaleActiveIndex = value2;

            if (_defaultScaleActiveIndex != -1)
            {
                var options = Editor.UI.DefaultViewportScaleOptions;
                if (options.Count > _defaultScaleActiveIndex)
                    ChangeViewportRatio(options[_defaultScaleActiveIndex]);
                else
                    _defaultScaleActiveIndex = 0;
            }

            if (_customScaleActiveIndex != -1)
            {
                var options = Editor.UI.CustomViewportScaleOptions;
                if (options.Count > _customScaleActiveIndex)
                    ChangeViewportRatio(options[_customScaleActiveIndex]);
                else
                {
                    _defaultScaleActiveIndex = 0;
                    _customScaleActiveIndex = -1;
                }
            }

            UpdateToolStrip();
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize()
        {
            ShowGUI = true;
            EditGUI = true;
            ShowDebugDraw = false;
            _flaxShortcutsEnabled = true;
            _showGameViewGizmos = false;
            UpdateToolStrip();
        }
    }
}
