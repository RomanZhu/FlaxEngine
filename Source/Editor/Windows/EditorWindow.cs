// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content;
using FlaxEditor.Modules;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;
using DockWindow = FlaxEditor.GUI.Docking.DockWindow;

namespace FlaxEditor.Windows
{
    /// <summary>
    ///  Base class for all windows in Editor.
    /// </summary>
    /// <seealso cref="DockWindow" />
    public abstract class EditorWindow : DockWindow
    {
        /// <summary>
        /// Gets the editor object.
        /// </summary>
        public readonly Editor Editor;

        /// <summary>
        /// Gets a value indicating whether this window can open content finder popup.
        /// </summary>
        protected virtual bool CanOpenContentFinder => true;

        /// <summary>
        /// Gets a value indicating whether this window can use UI navigation (tab/enter).
        /// </summary>
        protected virtual bool CanUseNavigation => true;

        /// <summary>
        /// Initializes a new instance of the <see cref="EditorWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        /// <param name="hideOnClose">True if hide window on closing, otherwise it will be destroyed.</param>
        /// <param name="scrollBars">The scroll bars.</param>
        protected EditorWindow(Editor editor, bool hideOnClose, ScrollBars scrollBars)
        : base(editor.UI.MasterPanel, hideOnClose, scrollBars)
        {
            AutoFocus = true;
            Editor = editor;

            InputActions.Add(options => options.ContentFinder, () =>
            {
                if (CanOpenContentFinder)
                {
                    Editor.ContentFinding.ShowFinder(RootWindow);
                }
            });

            // Set up editor window shortcuts
            InputActions.Add(options => options.ContentWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.ContentWin.FocusOrShow();
            });
            InputActions.Add(options => options.SceneWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.SceneWin.FocusOrShow();
            });
            InputActions.Add(options => options.ToolboxWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.ToolboxWin.FocusOrShow();
            });
            InputActions.Add(options => options.PropertiesWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.PropertiesWin.FocusOrShow();
            });
            InputActions.Add(options => options.GameWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.GameWin.FocusOrShow();
            });
            InputActions.Add(options => options.EditorWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.EditWin.FocusOrShow();
            });
            InputActions.Add(options => options.DebugLogWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.EditorConsoleWin.FocusOrShow();
            });
            InputActions.Add(options => options.OutputLogWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.EditorConsoleWin.FocusOrShow();
            });
            InputActions.Add(options => options.GraphicsQualityWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.GraphicsQualityWin.FocusOrShow();
            });
            InputActions.Add(options => options.GameCookerWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.GameCookerWin.FocusOrShow();
            });
            InputActions.Add(options => options.ProfilerWindow, () =>
            {
                if (InputOptions.ProfilerShortcutAvaliable)
                    Editor.Windows.ProfilerWin.FocusOrShow();
            });
            InputActions.Add(options => options.ContentFinder, () =>
            {
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.ContentFinding.ShowSearch();
            });
            InputActions.Add(options => options.VisualScriptDebuggerWindow, () => 
            { 
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.VisualScriptDebuggerWin.FocusOrShow();
            });
            InputActions.Add(options => options.EditorOptionsWindow, () =>
            {
                if (InputOptions.WindowShortcutsAvaliable)
                    Editor.Windows.EditorOptionsWin.FocusOrShow();
            });

            // Register
            Editor.Windows.OnWindowAdd(this);
        }

        /// <summary>
        /// Determines whether this window is holding reference to the specified item.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <returns><c>true</c> if window is editing the specified item; otherwise, <c>false</c>.</returns>
        public virtual bool IsEditingItem(ContentItem item)
        {
            return false;
        }

        #region Window Events

        /// <summary>
        /// Fired when scene starts saving
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneSaving(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene gets saved
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneSaved(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene gets saving error
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneSaveError(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene starts loading
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneLoading(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene gets loaded
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneLoaded(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene cannot be loaded
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneLoadError(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene gets unloading
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneUnloading(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Fired when scene gets unloaded
        /// </summary>
        /// <param name="scene">The scene object. It may be null!</param>
        /// <param name="sceneId">The scene ID.</param>
        public virtual void OnSceneUnloaded(Scene scene, Guid sceneId)
        {
        }

        /// <summary>
        /// Called before Editor will enter play mode.
        /// </summary>
        public virtual void OnPlayBeginning()
        {
        }

        /// <summary>
        /// Called when Editor is entering play mode.
        /// </summary>
        public virtual void OnPlayBegin()
        {
        }

        /// <summary>
        /// Called when Editor will leave the play mode.
        /// </summary>
        public virtual void OnPlayEnding()
        {
        }

        /// <summary>
        /// Called when Editor leaves the play mode.
        /// </summary>
        public virtual void OnPlayEnd()
        {
        }

        /// <summary>
        /// Called when window should be initialized.
        /// At this point, main window, content database, default editor windows are ready.
        /// </summary>
        public virtual void OnInit()
        {
        }

        /// <summary>
        /// Called when every engine update.
        /// Note: <see cref="Control.Update"/> may be called at the lower frequency than the engine updates.
        /// </summary>
        public virtual void OnUpdate()
        {
        }

        /// <summary>
        /// Called when editor is being closed and window should perform release data operations.
        /// </summary>
        public virtual void OnExit()
        {
        }

        /// <summary>
        /// Called when Editor state gets changed.
        /// </summary>
        public virtual void OnEditorStateChanged()
        {
        }

        #endregion

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            // Prevent closing the editor window when using RMB + Ctrl + W to slow down the camera flight
            if (Editor.Options.Options.Input.CloseTab.Process(this, key))
            {
                if (Root.GetMouseButton(MouseButton.Right))
                    return true;
            }

            var input = Editor.Options.Options.Input;
            if (input.ToggleFullscreen.Process(this, key))
            {
                Editor.Windows.GameWin.ToggleFullscreen();
                return true;
            }
            if (input.ToggleSceneFullscreen.Process(this, key))
            {
                Editor.Windows.EditWin.ToggleFullscreen();
                return true;
            }
            var window = RootWindow?.Window;
            if (window != null && WindowsModule.IsNavigationBackInput(window, key))
            {
                Editor.Windows.NavigateBack();
                return true;
            }
            if (window != null && WindowsModule.IsNavigationForwardInput(window, key))
            {
                Editor.Windows.NavigateForward();
                return true;
            }

            if (base.OnKeyDown(key))
                return true;

            switch (key)
            {
            case KeyboardKeys.Return:
                if (CanUseNavigation && Root?.FocusedControl != null)
                {
                    Root.SubmitFocused();
                    return true;
                }
                break;
            case KeyboardKeys.Tab:
                if (Root != null)
                {
                    bool shortcutModifier = Root.GetKey(KeyboardKeys.Control) || Root.GetKey(KeyboardKeys.Alt);
                    if (!shortcutModifier)
                        return Editor.Windows.EditWin?.Viewport?.CycleContextualAuthoringMode() ?? false;
                }
                break;
            }
            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Extended1 || button == MouseButton.Extended2)
                return true;

            return base.OnMouseDown(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Extended1)
            {
                Editor.Windows.NavigateBack();
                return true;
            }
            if (button == MouseButton.Extended2)
            {
                Editor.Windows.NavigateForward();
                return true;
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override void OnStartContainsFocus()
        {
            base.OnStartContainsFocus();

            Editor.Windows.OnWindowFocused(this);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;
            OnExit();

            // Unregister
            Editor.Windows.OnWindowRemove(this);

            base.OnDestroy();
        }
    }
}
