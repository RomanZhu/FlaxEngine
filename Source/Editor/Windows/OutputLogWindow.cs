// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Editor window used to show engine output logs.
    /// </summary>
    /// <seealso cref="FlaxEditor.Windows.EditorWindow" />
    public sealed class OutputLogWindow : EditorWindow
    {
        /// <summary>
        /// The single log message entry.
        /// </summary>
        private struct Entry
        {
            /// <summary>
            /// The log level.
            /// </summary>
            public LogType Level;

            /// <summary>
            /// The log time (in UTC local format).
            /// </summary>
            public DateTime Time;

            /// <summary>
            /// The message contents.
            /// </summary>
            public string Message;
        };

        private struct EntryRange
        {
            public int EntryIndex;
            public int StartIndex;
            public int EndIndex;
        }

        private struct TextBlockTag
        {
            internal enum Types
            {
                CodeLocation
            };

            public Types Type;
            public string Url;
            public int Line;
        }

        /// <summary>
        /// The output log textbox.
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.RichTextBoxBase" />
        private sealed class OutputTextBox : RichTextBoxBase
        {
            private const float SelectionAutoScrollEdgeSize = 28.0f;
            private const float SelectionAutoScrollMinSpeed = 140.0f;
            private const float SelectionAutoScrollMaxSpeed = 560.0f;

            private bool _isSelectingEntryBlocks;
            private int _entrySelectionAnchor = -1;

            /// <summary>
            /// The parent window.
            /// </summary>
            public OutputLogWindow Window;

            /// <summary>
            /// The default text style.
            /// </summary>
            public TextBlockStyle DefaultStyle;

            /// <summary>
            /// The warning text style.
            /// </summary>
            public TextBlockStyle WarningStyle;

            /// <summary>
            /// The error text style.
            /// </summary>
            public TextBlockStyle ErrorStyle;

            public OutputTextBox()
            {
                _consumeAllKeyDownEvents = false;
            }

            public bool IsSelectingText => _isSelecting || _isSelectingEntryBlocks;

            public int CharIndexAtSelectionPoint(ref Float2 location)
            {
                var clampedLocation = ClampSelectionPoint(location);
                return CharIndexAtPoint(ref clampedLocation);
            }

            /// <inheritdoc />
            protected override void OnParseTextBlocks()
            {
                if (ParseTextBlocks != null)
                {
                    ParseTextBlocks(_text, _textBlocks);
                    return;
                }

                // Use cached text blocks
                _textBlocks.Clear();
                _textBlocks.AddRange(Window._textBlocks);
            }

            public void RefreshTextLayout()
            {
                UpdateTextBlocks();
                _textSize = GetTextSize();
            }

            /// <inheritdoc />
            protected override void OnSizeChanged()
            {
                base.OnSizeChanged();

                Window?.OnOutputBoundsChanged();
            }

            /// <inheritdoc />
            public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left && Window != null && Window.SelectEntryBlockAt(ref location, out _entrySelectionAnchor))
                {
                    _isSelectingEntryBlocks = true;
                    StartMouseCapture();
                    return true;
                }

                return base.OnMouseDoubleClick(location, button);
            }

            /// <inheritdoc />
            public override void OnMouseMove(Float2 location)
            {
                if (_isSelectingEntryBlocks)
                {
                    Window?.ExtendEntryBlockSelection(_entrySelectionAnchor, ref location);
                    return;
                }
                if (_isSelecting)
                {
                    ExtendTextSelection(ref location);
                    return;
                }

                base.OnMouseMove(location);
            }

            /// <inheritdoc />
            public override void Update(float deltaTime)
            {
                base.Update(deltaTime);

                if (!IsSelectingText || Root == null || !Root.GetMouseButton(MouseButton.Left))
                    return;

                var location = PointFromWindow(Root.MousePosition);
                if (ScrollDuringSelection(ref location, deltaTime))
                {
                    if (_isSelectingEntryBlocks)
                        Window?.ExtendEntryBlockSelection(_entrySelectionAnchor, ref location);
                    else if (_isSelecting)
                        ExtendTextSelection(ref location);
                }
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (_isSelectingEntryBlocks && button == MouseButton.Left)
                {
                    _isSelectingEntryBlocks = false;
                    EndMouseCapture();
                    return true;
                }

                return base.OnMouseUp(location, button);
            }

            /// <inheritdoc />
            public override void OnEndMouseCapture()
            {
                _isSelectingEntryBlocks = false;
                base.OnEndMouseCapture();
            }

            private Float2 ClampSelectionPoint(Float2 location)
            {
                location.X = Mathf.Clamp(location.X, 0.0f, Width);
                location.Y = Mathf.Clamp(location.Y, 0.0f, Height);
                return location;
            }

            private void ExtendTextSelection(ref Float2 location)
            {
                int currentIndex = CharIndexAtSelectionPoint(ref location);
                SetSelection(_selectionStart, currentIndex, false);
            }

            private bool ScrollDuringSelection(ref Float2 location, float deltaTime)
            {
                float direction = 0.0f;
                float distance = 0.0f;
                if (location.Y < SelectionAutoScrollEdgeSize)
                {
                    direction = -1.0f;
                    distance = SelectionAutoScrollEdgeSize - location.Y;
                }
                else if (location.Y > Height - SelectionAutoScrollEdgeSize)
                {
                    direction = 1.0f;
                    distance = location.Y - (Height - SelectionAutoScrollEdgeSize);
                }
                if (Mathf.IsZero(direction))
                    return false;

                float maxOffsetY = Mathf.Max(0.0f, _textSize.Y - Height);
                float t = Mathf.Saturate(distance / SelectionAutoScrollEdgeSize);
                float speed = Mathf.Lerp(SelectionAutoScrollMinSpeed, SelectionAutoScrollMaxSpeed, t);
                var viewOffset = TargetViewOffset;
                float previousY = viewOffset.Y;
                viewOffset.Y = Mathf.Clamp(viewOffset.Y + direction * speed * deltaTime, 0.0f, maxOffsetY);
                TargetViewOffset = viewOffset;
                return !Mathf.NearEqual(previousY, viewOffset.Y);
            }
        }

        /// <summary>
        /// Command line input textbox control which can execute debug commands.
        /// </summary>
        private class CommandLineBox : TextBox
        {
            private sealed class Item : ItemsListContextMenu.Item
            {
                public CommandLineBox Owner;

                public Item()
                {
                }

                protected override void GetTextRect(out Rectangle rect)
                {
                    rect = new Rectangle(2, 0, Width - 4, Height);
                }

                public override bool OnCharInput(char c)
                {
                    if (Owner != null && (!Owner._searchPopup?.Visible ?? true))
                    {
                        // Redirect input into search textbox while typing and using command history
                        Owner.Set(Owner.Text + c);
                        return true;
                    }
                    else if (Owner != null && Owner._searchPopup != null && Owner._searchPopup.Visible)
                    {
                        // Redirect input into search textbox while typing and using command history
                        Owner.OnCharInput(c);
                        return true;
                    }
                    return false;
                }

                public override bool OnKeyDown(KeyboardKeys key)
                {
                    switch (key)
                    {
                    case KeyboardKeys.Delete:
                    case KeyboardKeys.Backspace:
                        if (Owner != null && (!Owner._searchPopup?.Visible ?? true))
                        {
                            // Redirect input into search textbox while typing and using command history
                            Owner.OnKeyDown(key);
                            return true;
                        }
                        break;
                    case KeyboardKeys.ArrowLeft:
                        if (Owner != null && (!Owner._searchPopup?.Visible ?? true))
                        {
                            // Focus back the input field as user want to modify command from history
                            Owner.HideHistory();
                            Owner.HideSearch();
                            Owner.RootWindow.Focus();
                            Owner.Focus();
                            Owner.OnKeyDown(key);
                            return true;
                        }
                        break;
                    case KeyboardKeys.ArrowDown:
                    case KeyboardKeys.ArrowUp:
                        // UI navigation
                        return base.OnKeyDown(key);
                    default:
                        if (Owner != null && (Owner._searchPopup?.Visible ?? false))
                        {
                            // Redirect input into search textbox while typing and using command history
                            Owner.OnKeyDown(key);
                            return true;
                        }
                        break;
                    }

                    return base.OnKeyDown(key);
                }

                public override void OnDestroy()
                {
                    Owner = null;
                    base.OnDestroy();
                }
            }

            private OutputLogWindow _window;
            private ItemsListContextMenu _searchPopup;
            private ItemsListContextMenu _historyPopup;
            private bool _isSettingText;

            public CommandLineBox(float x, float y, float width, OutputLogWindow window)
            : base(false, x, y, width)
            {
                WatermarkText = ">";
                _window = window;
            }

            private void Set(string command)
            {
                _isSettingText = true;
                SetText(command);
                SetSelection(command.Length);
                _isSettingText = false;
            }

            private void HideSearch()
            {
                if (_searchPopup != null)
                {
                    _searchPopup.Hide();
                    _searchPopup = null;
                }
            }

            private void HideHistory()
            {
                if (_historyPopup != null)
                {
                    _historyPopup.Dispose();
                    _historyPopup = null;
                }
            }

            private void ShowPopup(ref ItemsListContextMenu cm, IEnumerable<string> commands, string searchText = null)
            {
                if (cm == null)
                    cm = new ItemsListContextMenu(180, 220, false);
                else
                    cm.ClearItems();

                // Add items
                ItemsListContextMenu.Item lastItem = null;
                var itemFont = Style.Current.FontSmall;
                var maxWidth = 0.0f;
                foreach (var command in commands)
                {
                    cm.AddItem(lastItem = new Item
                    {
                        Name = command,
                        Owner = this,
                    });
                    var flags = DebugCommands.GetCommandFlags(command);
                    if (flags.HasFlag(DebugCommands.CommandFlags.Exec))
                        lastItem.TintColor = new Color(0.75f, 0.75f, 1.0f, 1.0f);
                    else if (flags.HasFlag(DebugCommands.CommandFlags.Read) && !flags.HasFlag(DebugCommands.CommandFlags.Write))
                        lastItem.TintColor = new Color(0.85f, 0.85f, 0.85f, 1.0f);
                    lastItem.ItemFocused += item =>
                    {
                        // Set command
                        Set(item.Name);
                    };
                    maxWidth = Mathf.Max(maxWidth, itemFont.MeasureText(command).X);
                }
                cm.ItemClicked += item =>
                {
                    // Execute command
                    OnKeyDown(KeyboardKeys.Return);
                };

                // Setup popup
                var count = commands.Count();
                var totalHeight = count * lastItem.Height + cm.ItemsPanel.Margin.Height + cm.ItemsPanel.Spacing * (count - 1);
                cm.Height = 220;
                if (cm.Height > totalHeight)
                    cm.Height = totalHeight; // Limit popup height if list is small
                maxWidth += 8.0f + ScrollBar.DefaultSize; // Margin
                if (cm.Width < maxWidth)
                    cm.Width = maxWidth;
                if (searchText != null)
                {
                    cm.SortItems();
                    cm.Search(searchText);
                    cm.UseVisibilityControl = false;
                    cm.UseInput = false;
                }

                // Show popup
                cm.Show(this, Float2.Zero, ContextMenuDirection.RightUp);
                cm.ScrollViewTo(lastItem);
                if (searchText != null)
                {
                    RootWindow.Window.LostFocus += OnRootWindowLostFocus;
                }
                else
                {
                    lastItem.Focus();
                }
            }

            private void OnRootWindowLostFocus()
            {
                // Prevent popup from staying active when editor window looses focus
                HideSearch();
                if (RootWindow?.Window != null)
                    RootWindow.Window.LostFocus -= OnRootWindowLostFocus;
            }

            /// <inheritdoc />
            public override void OnGotFocus()
            {
                // Precache debug commands to reduce time-to-interactive
                DebugCommands.InitAsync();

                base.OnGotFocus();
            }

            /// <inheritdoc />
            protected override void OnTextChanged()
            {
                base.OnTextChanged();

                // Skip when editing text from code
                if (_isSettingText)
                    return;

                // Show commands search popup based on current text input
                var text = Text.Trim();
                bool isWhitespaceOnly = string.IsNullOrWhiteSpace(Text) && !string.IsNullOrEmpty(Text);
                if (text.Length != 0 || isWhitespaceOnly)
                {
                    DebugCommands.Search(text, out var matches);
                    if (matches.Length != 0 || isWhitespaceOnly)
                    {
                        string[] commands = [];
                        if (isWhitespaceOnly)
                            DebugCommands.GetAllCommands(out commands);

                        HideHistory();
                        ShowPopup(ref _searchPopup, isWhitespaceOnly ? commands : matches, text);
                        
                        if (isWhitespaceOnly)
                        {
                            // Scroll to and select first item for consistent behaviour
                            var firstItem = _searchPopup.ItemsPanel.Children[0] as Item;
                            _searchPopup.ScrollToAndHighlightItemByName(firstItem.Name);
                        }

                        return;
                    }
                }
                HideSearch();
            }

            /// <inheritdoc />
            public override bool OnKeyDown(KeyboardKeys key)
            {
                switch (key)
                {
                case KeyboardKeys.Return:
                {
                    // Run command
                    HideSearch();
                    HideHistory();
                    var command = Text.Trim();
                    if (command.Length == 0)
                        return true;
                    DebugCommands.Execute(command);
                    SetText(string.Empty);

                    // Update history buffer
                    if (_window._commandHistory == null)
                        _window._commandHistory = new List<string>();
                    else if (_window._commandHistory.Count != 0 && _window._commandHistory.Contains(command))
                        _window._commandHistory.Remove(command);
                    _window._commandHistory.Add(command);
                    if (_window._commandHistory.Count > CommandHistoryLimit)
                        _window._commandHistory.RemoveAt(0);
                    _window.SaveHistory();

                    return true;
                }
                case KeyboardKeys.Tab:
                {
                    // Auto-complete
                    DebugCommands.Search(Text, out var matches, true);
                    if (matches.Length == 0)
                    {
                        // Nothing found
                    }
                    else if (matches.Length == 1)
                    {
                        // Exact match
                        Set(matches[0]);
                    }
                    else
                    {
                        // Find the most common part
                        Array.Sort(matches);
                        int minLength = Text.Length;
                        int maxLength = matches[0].Length;
                        int sharedLength = minLength + 1;
                        bool allMatch = true;
                        for (; allMatch && sharedLength < maxLength; sharedLength++)
                        {
                            var shared = matches[0].Substring(0, sharedLength);
                            for (int i = 1; i < matches.Length; i++)
                            {
                                if (!matches[i].StartsWith(shared, StringComparison.OrdinalIgnoreCase))
                                {
                                    sharedLength -= 2;
                                    allMatch = false;
                                    break;
                                }
                            }
                        }
                        if (sharedLength > minLength)
                        {
                            // Use the largest shared part of all matches
                            Set(matches[0].Substring(0, sharedLength));
                        }
                    }
                    return true;
                }
                case KeyboardKeys.ArrowUp:
                {
                    if (_searchPopup != null && _searchPopup.Visible)
                    {
                        // Route navigation to active popup
                        var focusedItem = _searchPopup.RootWindow.FocusedControl as Item;
                        if (focusedItem == null)
                            _searchPopup.SelectItem((Item)_searchPopup.ItemsPanel.Children.Last());
                        else
                            _searchPopup.OnKeyDown(key);
                    }
                    else if (TextLength == 0)
                    {
                        if (_window._commandHistory != null && _window._commandHistory.Count != 0)
                        {
                            // Show command history popup
                            HideSearch();
                            ShowPopup(ref _historyPopup, _window._commandHistory);
                        }
                    }
                    return true;
                }
                case KeyboardKeys.ArrowDown:
                {
                    if (_searchPopup != null && _searchPopup.Visible)
                    {
                        // Route navigation to active popup
                        var focusedItem = _searchPopup.RootWindow.FocusedControl as Item;
                        if (focusedItem == null)
                            _searchPopup.SelectItem((Item)_searchPopup.ItemsPanel.Children.First());
                        else
                            _searchPopup.OnKeyDown(key);
                    }
                    return true;
                }
                }

                return base.OnKeyDown(key);
            }

            /// <inheritdoc />
            public override void OnDestroy()
            {
                _searchPopup?.Dispose();
                _searchPopup = null;

                base.OnDestroy();
            }
        }

        private InterfaceOptions.TimestampsFormats _timestampsFormats;
        private bool _showLogType;

        private List<Entry> _entries = new List<Entry>(1024);
        private bool _isDirty;
        private int _logTypeShowMask = (int)LogType.Info | (int)LogType.Warning | (int)LogType.Error | (int)LogType.Fatal;
        private float _scrollSize = ScrollBar.DefaultSize;
        private const int OutCapacity = 64;
        private string[] _outMessages = new string[OutCapacity];
        private byte[] _outLogTypes = new byte[OutCapacity];
        private long[] _outLogTimes = new long[OutCapacity];
        private int _textBufferCount;
        private StringBuilder _textBuffer = new StringBuilder();
        private List<TextBlock> _textBlocks = new List<TextBlock>();
        private List<EntryRange> _entryRanges = new List<EntryRange>(1024);
        private DateTime _startupTime;
        private Regex _compileRegex = new Regex("(?<path>^(?:[a-zA-Z]\\:|\\\\\\\\[ \\-\\.\\w\\.]+\\\\[ \\-\\.\\w.$]+)\\\\(?:[ \\-\\.\\w]+\\\\)*\\w([ \\w.])+)\\((?<line>\\d{1,}),\\d{1,},\\d{1,},\\d{1,}\\): (?<level>error|warning) (?<message>.*)", RegexOptions.Compiled | RegexOptions.Multiline);
        private List<string> _commandHistory;
        private const string CommandHistoryKey = "CommandHistory";
        private const int CommandHistoryLimit = 30;
        private const float OutputPadding = 2.0f;
        private const float EntrySpacing = 1.0f;

        private Button _clearButton;
        private Button _viewDropdown;
        private TextBox _searchBox;
        private HScrollBar _hScroll;
        private VScrollBar _vScroll;
        private OutputTextBox _output;
        private CommandLineBox _commandLineBox;
        private ContextMenu _contextMenu;
        private int _selectedEntryIndex = -1;
        private float _lastOutputWrapWidth = -1.0f;
        private bool _wrapLogLines = true;
        private bool _outputSelectionBlockedAutoScroll;

        /// <summary>
        /// Initializes a new instance of the <see cref="DebugLogWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public OutputLogWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Output Log";
            Icon = editor.Icons.Info64;
            ClipChildren = false;
            FlaxEditor.Utilities.Utils.SetupCommonInputActions(this);

            // Setup UI
            _clearButton = new Button(2, 2, 44.0f, TextBoxBase.DefaultHeight)
            {
                TooltipText = "Clear output log",
                Text = "Clear",
                Parent = this,
            };
            _clearButton.Clicked += Clear;
            _searchBox = new SearchBox(false, _clearButton.Right + 2, 2, Width - _clearButton.Right - 46.0f)
            {
                Parent = this,
            };
            _searchBox.TextChanged += Refresh;
            _viewDropdown = new Button(_searchBox.Right + 2, 2, 40.0f, TextBoxBase.DefaultHeight)
            {
                TooltipText = "Change output log view options",
                Text = "View",
                Parent = this,
            };
            _viewDropdown.Clicked += OnViewButtonClicked;
            _hScroll = new HScrollBar(this, Height - _scrollSize - TextBox.DefaultHeight - 2, Width - _scrollSize, _scrollSize)
            {
                Maximum = 0,
            };
            _hScroll.ValueChanged += OnHScrollValueChanged;
            _vScroll = new VScrollBar(this, Width - _scrollSize, Height - _viewDropdown.Height - 4 - TextBox.DefaultHeight, _scrollSize)
            {
                Maximum = 0,
            };
            _vScroll.Y += _viewDropdown.Height + 2;
            _vScroll.ValueChanged += OnVScrollValueChanged;
            _output = new OutputTextBox
            {
                Window = this,
                IsReadOnly = true,
                IsMultiline = true,
                BackgroundSelectedFlashSpeed = 0.0f,
                BackgroundColor = Style.Current.Background,
                BackgroundSelectedColor = Style.Current.Background,
                BorderColor = Color.Transparent,
                BorderSelectedColor = Color.Transparent,
                Location = new Float2(OutputPadding, _viewDropdown.Bottom + 2),
                Parent = this,
            };
            _output.TargetViewOffsetChanged += OnOutputTargetViewOffsetChanged;
            _output.TextChanged += OnOutputTextChanged;
            _commandLineBox = new CommandLineBox(2, Height - 2 - TextBox.DefaultHeight, Width - 4, this)
            {
                Parent = this,
            };

            // Setup context menu
            _contextMenu = new ContextMenu();
            _contextMenu.AddButton("Clear log", Clear);
            _contextMenu.AddButton("Copy selection", _output.Copy);
            _contextMenu.AddButton("Select All", _output.SelectAll);
            _contextMenu.AddButton(Utilities.Constants.ShowInExplorer, () => FileSystem.ShowFileExplorer(Path.Combine(Globals.ProjectFolder, "Logs")));
            _contextMenu.AddButton("Scroll to bottom", () => { _vScroll.TargetValue = _vScroll.Maximum; }).Icon = Editor.Icons.ArrowDown12;

            // Setup editor options
            Editor.Options.OptionsChanged += OnEditorOptionsChanged;
            OnEditorOptionsChanged(Editor.Options.Options);

            InputActions.Add(options => options.Search, _searchBox.Focus);

            GameCooker.Event += OnGameCookerEvent;
            ScriptsBuilder.CompilationFailed += OnScriptsCompilationFailed;
        }

        private void OnViewButtonClicked()
        {
            var menu = new ContextMenu();

            var infoLogButton = menu.AddButton("Info");
            infoLogButton.AutoCheck = true;
            infoLogButton.Checked = (_logTypeShowMask & (int)LogType.Info) != 0;
            infoLogButton.Clicked += () => ToggleLogTypeShow(LogType.Info);

            var warningLogButton = menu.AddButton("Warning");
            warningLogButton.AutoCheck = true;
            warningLogButton.Checked = (_logTypeShowMask & (int)LogType.Warning) != 0;
            warningLogButton.Clicked += () => ToggleLogTypeShow(LogType.Warning);

            var errorLogButton = menu.AddButton("Error");
            errorLogButton.AutoCheck = true;
            errorLogButton.Checked = (_logTypeShowMask & (int)LogType.Error) != 0;
            errorLogButton.Clicked += () => ToggleLogTypeShow(LogType.Error);

            menu.AddSeparator();

            var wrapButton = menu.AddButton("Wrap");
            wrapButton.AutoCheck = true;
            wrapButton.Checked = _wrapLogLines;
            wrapButton.Clicked += ToggleWrapLogLines;

            menu.AddSeparator();

            menu.AddButton("Load log file...", LoadLogFile);

            menu.Show(_viewDropdown.Parent, _viewDropdown.BottomLeft);
        }

        private void ToggleLogTypeShow(LogType type)
        {
            _logTypeShowMask ^= (int)type;
            Refresh();
        }

        private void ToggleWrapLogLines()
        {
            _wrapLogLines = !_wrapLogLines;
            if (_wrapLogLines)
                _output.TargetViewOffset = new Float2(0.0f, _output.TargetViewOffset.Y);
            Refresh();
        }

        private void OnHScrollValueChanged()
        {
            if (_wrapLogLines)
                return;

            var viewOffset = _output.ViewOffset;
            viewOffset.X = _hScroll.Value;
            _output.TargetViewOffset = viewOffset;
        }

        private void OnVScrollValueChanged()
        {
            var viewOffset = _output.ViewOffset;
            viewOffset.Y = _vScroll.Value;
            _output.TargetViewOffset = viewOffset;
        }

        private void OnOutputTargetViewOffsetChanged()
        {
            if (!_hScroll.IsThumbClicked)
                _hScroll.TargetValue = _output.TargetViewOffset.X;
            if (!_vScroll.IsThumbClicked)
                _vScroll.TargetValue = _output.TargetViewOffset.Y;
        }

        private void OnOutputTextChanged()
        {
            if (IsLayoutLocked || _output == null || _hScroll == null || _vScroll == null)
                return;

            _hScroll.Maximum = _wrapLogLines ? _hScroll.Minimum : Mathf.Max(_output.TextSize.X - _output.Width, _hScroll.Minimum);
            _vScroll.Maximum = Mathf.Max(_output.TextSize.Y - _output.Height, _vScroll.Minimum);
        }

        private void OnOutputBoundsChanged()
        {
            if (_output == null)
                return;

            if (_wrapLogLines)
            {
                var wrapWidth = GetOutputWrapWidth();
                if (!_isDirty && !Mathf.NearEqual(_lastOutputWrapWidth, wrapWidth))
                {
                    _output.TargetViewOffset = new Float2(0.0f, _output.TargetViewOffset.Y);
                    Refresh();
                }
            }
            OnOutputTextChanged();
        }

        private void OnEditorOptionsChanged(EditorOptions options)
        {
            var style = Style.Current;
            _output.BackgroundColor = style.Background;
            _output.BackgroundSelectedColor = style.Background;
            _output.BorderColor = Color.Transparent;
            _output.BorderSelectedColor = Color.Transparent;
            var selectionBrush = _output.DefaultStyle.BackgroundSelectedBrush as SolidColorBrush;

            if (options.Interface.OutputLogTimestampsFormat == _timestampsFormats &&
                options.Interface.OutputLogShowLogType == _showLogType &&
                _output.DefaultStyle.Font == options.Interface.OutputLogTextFont &&
                _output.DefaultStyle.Color == options.Visual.LogInfoColor &&
                _output.DefaultStyle.ShadowColor == options.Interface.OutputLogTextShadowColor &&
                _output.DefaultStyle.ShadowOffset == options.Interface.OutputLogTextShadowOffset &&
                selectionBrush != null &&
                selectionBrush.Color == style.BorderSelected &&
                _output.WarningStyle.Color == options.Visual.LogWarningColor &&
                _output.ErrorStyle.Color == options.Visual.LogErrorColor)
                return;

            _output.DefaultStyle = new TextBlockStyle
            {
                Font = options.Interface.OutputLogTextFont,
                Color = options.Visual.LogInfoColor,
                ShadowColor = options.Interface.OutputLogTextShadowColor,
                ShadowOffset = options.Interface.OutputLogTextShadowOffset,
                BackgroundSelectedBrush = new SolidColorBrush(style.BorderSelected),
            };

            _output.WarningStyle = _output.DefaultStyle;
            _output.WarningStyle.Color = options.Visual.LogWarningColor;
            _output.ErrorStyle = _output.DefaultStyle;
            _output.ErrorStyle.Color = options.Visual.LogErrorColor;

            _timestampsFormats = options.Interface.OutputLogTimestampsFormat;
            _showLogType = options.Interface.OutputLogShowLogType;

            Refresh();
        }

        private void OnGameCookerEvent(GameCooker.EventType eventType)
        {
            if (eventType == GameCooker.EventType.BuildFailed && !Editor.IsHeadlessMode && Editor.Options.Options.Interface.FocusOutputLogOnGameBuildError)
                FocusOrShow();
        }

        private void OnScriptsCompilationFailed()
        {
            if (!Editor.IsHeadlessMode && Editor.Options.Options.Interface.FocusOutputLogOnCompilationError)
                FocusOrShow();
        }

        private void SaveHistory()
        {
            if (_commandHistory == null || _commandHistory.Count == 0)
                Editor.ProjectCache.RemoveCustomData(CommandHistoryKey);
            else
                Editor.ProjectCache.SetCustomData(CommandHistoryKey, FlaxEngine.Json.JsonSerializer.Serialize(_commandHistory));
        }

        /// <summary>
        /// Refreshes the log output.
        /// </summary>
        private void Refresh()
        {
            _textBufferCount = 0;
            _textBuffer.Clear();
            _textBlocks.Clear();
            _entryRanges.Clear();
            _lastOutputWrapWidth = -1.0f;
            _isDirty = true;
        }

        private static string SanitizeMessage(string message)
        {
            if (string.IsNullOrEmpty(message))
                return string.Empty;
            return message.IndexOf('\r') != -1 ? message.Replace("\r", "") : message;
        }

        private TextBlockStyle GetEntryStyle(LogType level)
        {
            switch (level)
            {
            case LogType.Info:
                return _output.DefaultStyle;
            case LogType.Warning:
                return _output.WarningStyle;
            case LogType.Error:
            case LogType.Fatal:
                return _output.ErrorStyle;
            default: throw new ArgumentOutOfRangeException();
            }
        }

        private int AppendEntryPrefix(ref Entry entry)
        {
            switch (_timestampsFormats)
            {
            case InterfaceOptions.TimestampsFormats.Utc:
                _textBuffer.AppendFormat("[ {0} ]: ", entry.Time.ToUniversalTime());
                break;
            case InterfaceOptions.TimestampsFormats.LocalTime:
                _textBuffer.AppendFormat("[ {0} ]: ", entry.Time);
                break;
            case InterfaceOptions.TimestampsFormats.TimeSinceStartup:
                var diff = entry.Time - _startupTime;
                _textBuffer.AppendFormat("[ {0:00}:{1:00}:{2:00}.{3:000} ]: ", diff.Hours, diff.Minutes, diff.Seconds, diff.Milliseconds);
                break;
            }
            if (_showLogType)
            {
                _textBuffer.AppendFormat("[{0}] ", entry.Level);
            }
            return _textBuffer.Length;
        }

        private float GetOutputWrapWidth()
        {
            return _wrapLogLines ? Mathf.Max(_output != null ? _output.Width : 0.0f, 1.0f) : float.MaxValue;
        }

        private bool FindEntryRange(int entryIndex, out EntryRange range)
        {
            for (int i = 0; i < _entryRanges.Count; i++)
            {
                range = _entryRanges[i];
                if (range.EntryIndex == entryIndex)
                    return true;
            }

            range = new EntryRange();
            return false;
        }

        private bool FindEntryRangeAt(int charIndex, out EntryRange range)
        {
            for (int i = 0; i < _entryRanges.Count; i++)
            {
                range = _entryRanges[i];
                if (charIndex >= range.StartIndex && charIndex <= range.EndIndex)
                    return true;
            }

            range = new EntryRange();
            return false;
        }

        private void SelectEntryRange(EntryRange from, EntryRange to)
        {
            _selectedEntryIndex = to.EntryIndex;
            _output.Focus();
            _output.SelectionRange = new TextRange(Mathf.Min(from.StartIndex, to.StartIndex), Mathf.Max(from.EndIndex, to.EndIndex));
        }

        private bool SelectEntryBlockAt(ref Float2 location, out int entryIndex)
        {
            entryIndex = -1;
            if (_output == null || _output.TextLength == 0 || _entryRanges.Count == 0)
                return false;

            var hitPos = _output.CharIndexAtSelectionPoint(ref location);
            if (!FindEntryRangeAt(hitPos, out var range))
                return false;

            entryIndex = range.EntryIndex;
            SelectEntryRange(range, range);
            return true;
        }

        private void ExtendEntryBlockSelection(int anchorEntryIndex, ref Float2 location)
        {
            if (!FindEntryRange(anchorEntryIndex, out var anchor))
                return;

            var hitPos = _output.CharIndexAtSelectionPoint(ref location);
            if (FindEntryRangeAt(hitPos, out var target))
                SelectEntryRange(anchor, target);
        }

        private void AddWrappedTextBlocks(string text, int textStartIndex, TextBlockStyle style, ref float y, float wrapWidth, int firstLineParseOffset)
        {
            if (string.IsNullOrEmpty(text))
                return;

            var font = style.Font.GetFont();
            if (!font)
                return;

            var layout = TextLayoutOptions.Default;
            layout.Bounds = new Rectangle(0, 0, wrapWidth, 10000000.0f);
            layout.HorizontalAlignment = TextAlignment.Near;
            layout.VerticalAlignment = TextAlignment.Near;
            layout.TextWrapping = _wrapLogLines ? TextWrapping.WrapChars : TextWrapping.NoWrap;
            var lines = font.ProcessText(text, ref layout);
            var startY = y;
            var bottom = y;
            for (int i = 0; i < lines.Length; i++)
            {
                ref var line = ref lines[i];
                var lineStart = Mathf.Clamp(line.FirstCharIndex, 0, text.Length);
                var lineEnd = Mathf.Clamp(line.LastCharIndex + 1, lineStart, text.Length);
                var textBlock = new TextBlock
                {
                    Style = style,
                    Range = new TextRange(textStartIndex + lineStart, textStartIndex + lineEnd),
                    Bounds = new Rectangle(new Float2(line.Location.X, startY + line.Location.Y), line.Size),
                };

                if (lineEnd > lineStart)
                {
                    var rawLineStart = lineStart;
                    while (rawLineStart > 0 && text[rawLineStart - 1] != '\n')
                        rawLineStart--;
                    var rawLineEnd = lineEnd;
                    while (rawLineEnd < text.Length && text[rawLineEnd] != '\n')
                        rawLineEnd++;

                    var regexStart = rawLineStart == 0 ? Mathf.Min(firstLineParseOffset, rawLineEnd) : rawLineStart;
                    var regexLength = rawLineEnd - regexStart;
                    if (regexLength > 0)
                    {
                        var match = _compileRegex.Match(text, regexStart, regexLength);
                        if (match.Success)
                        {
                            switch (match.Groups["level"].Value)
                            {
                            case "error":
                                textBlock.Style = _output.ErrorStyle;
                                break;
                            case "warning":
                                textBlock.Style = _output.WarningStyle;
                                break;
                            }
                            textBlock.Tag = new TextBlockTag
                            {
                                Type = TextBlockTag.Types.CodeLocation,
                                Url = match.Groups["path"].Value,
                                Line = int.Parse(match.Groups["line"].Value),
                            };
                        }
                    }
                }

                _textBlocks.Add(textBlock);
                bottom = Mathf.Max(bottom, textBlock.Bounds.Bottom);
            }
            y = bottom;
        }

        private void AppendVisibleEntry(int entryIndex, ref Entry entry, float wrapWidth)
        {
            entry.Message = SanitizeMessage(entry.Message);

            var entryStart = _textBuffer.Length;
            var y = _textBlocks.Count == 0 ? 0.0f : _textBlocks[_textBlocks.Count - 1].Bounds.Bottom + EntrySpacing;
            var style = GetEntryStyle(entry.Level);
            var messageStart = AppendEntryPrefix(ref entry);
            var prefixLength = messageStart - entryStart;
            _textBuffer.Append(entry.Message);
            var entryEnd = _textBuffer.Length;
            _textBuffer.Append('\n');
            AddWrappedTextBlocks(_textBuffer.ToString(entryStart, entryEnd - entryStart), entryStart, style, ref y, wrapWidth, prefixLength);

            _entryRanges.Add(new EntryRange
            {
                EntryIndex = entryIndex,
                StartIndex = entryStart,
                EndIndex = entryEnd,
            });
        }

        /// <summary>
        /// Clears the log.
        /// </summary>
        public void Clear()
        {
            _entries?.Clear();
            _selectedEntryIndex = -1;
            Refresh();
        }

        /// <summary>
        /// Loads the log from the file selected by the user with the file pickup dialog.
        /// </summary>
        public void LoadLogFile()
        {
            if (FileSystem.ShowOpenFileDialog(null, Path.Combine(Globals.ProjectFolder, "Logs"), null, false, "Pick a log file to load", out var files))
                return;
            if (files != null && files.Length > 0)
            {
                LoadLogFile(files[0]);
            }
        }

        /// <summary>
        /// Loads the log file.
        /// </summary>
        /// <param name="path">The path.</param>
        public void LoadLogFile(string path)
        {
            using (var file = File.OpenRead(path))
            using (var stream = new StreamReader(file))
            {
                _entries.Clear();
                _selectedEntryIndex = -1;
                var regex = new Regex(@"\[ (\d\d:\d\d:\d\d.\d\d\d) \]\: \[(\w*)\]");

                while (!stream.EndOfStream)
                {
                    // Read next line
                    var line = stream.ReadLine();
                    if (string.IsNullOrEmpty(line))
                        continue;

                    // Parse with regex
                    var match = regex.Match(line);
                    if (!match.Success || match.Groups.Count != 3)
                    {
                        // Try to add the line for multi-line logs
                        if (_entries.Count != 0 && !line.StartsWith("======"))
                        {
                            ref var last = ref CollectionsMarshal.AsSpan(_entries)[_entries.Count - 1];
                            last.Message += '\n';
                            last.Message += line;
                        }

                        continue;
                    }

                    // Parse log time and type
                    var time = match.Groups[1].Value;
                    var level = match.Groups[2].Value;
                    if (time.Length != 12)
                        continue;
                    int hours = int.Parse(time.Substring(0, 2));
                    int minutes = int.Parse(time.Substring(3, 2));
                    int seconds = int.Parse(time.Substring(6, 2));
                    int milliseconds = int.Parse(time.Substring(9, 3));
                    var timeSinceStartup = new TimeSpan(0, hours, minutes, seconds, milliseconds);
                    var logType = (LogType)Enum.Parse(typeof(LogType), level);

                    // Add new entry
                    var e = new Entry
                    {
                        Time = _startupTime + timeSinceStartup,
                        Level = logType,
                        Message = line.Substring(match.Index + match.Length)
                    };
                    _entries.Add(e);
                }

                Refresh();
            }
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            base.PerformLayoutBeforeChildren();

            if (_output != null)
            {
                _viewDropdown.X = Width - _viewDropdown.Width - 2;
                _searchBox.X = _clearButton.Right + 2;
                _searchBox.Width = Mathf.Max(_viewDropdown.X - _searchBox.X - 2, 0.0f);
                _commandLineBox.Width = Width - 4;
                _commandLineBox.Y = Height - 2 - _commandLineBox.Height;
                var outputBottom = _wrapLogLines ? _commandLineBox.Y - OutputPadding : _commandLineBox.Y - _scrollSize;
                _hScroll.Visible = !_wrapLogLines;
                _hScroll.Enabled = !_wrapLogLines;
                _hScroll.Bounds = new Rectangle(0, _commandLineBox.Y - _scrollSize, Mathf.Max(Width - _scrollSize, 0.0f), _scrollSize);
                _vScroll.Bounds = new Rectangle(Mathf.Max(Width - _scrollSize, 0.0f), _viewDropdown.Bottom + 2, _scrollSize, Mathf.Max(outputBottom - _viewDropdown.Bottom - 2, 0.0f));
                _output.Bounds = new Rectangle(OutputPadding, _vScroll.Y, Mathf.Max(_vScroll.X - OutputPadding * 2, 0.0f), Mathf.Max(outputBottom - _vScroll.Y, 0.0f));
                OnOutputBoundsChanged();
            }
        }

        /// <inheritdoc/>
        public override void Draw()
        {
            base.Draw();

            bool showHint = (((int)LogType.Info & _logTypeShowMask) == 0 &&
                            ((int)LogType.Warning & _logTypeShowMask) == 0 &&
                            ((int)LogType.Error & _logTypeShowMask) == 0) ||
                            string.IsNullOrEmpty(_output.Text) ||
                            _entries.Count == 0;
            if (showHint)
            {
                var textRect = _output.Bounds;
                var style = Style.Current;
                var text = "No log level filter active or no entries that apply to the current filter exist";
                if (_entries.Count == 0)
                    text = "No log";
                Render2D.DrawText(style.FontMedium, text, textRect, style.ForegroundGrey, TextAlignment.Center, TextAlignment.Center, TextWrapping.WrapWords);
            }
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            var input = Editor.Options.Options.Input;
            if (input.Search.Process(this, key))
            {
                if (!_searchBox.ContainsFocus)
                {
                    _searchBox.Focus();
                    _searchBox.SelectAll();
                }
                return true;
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (base.OnMouseUp(location, button))
                return true;

            if (button == MouseButton.Right)
            {
                _contextMenu.Show(this, location);
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        protected override void OnSizeChanged()
        {
            base.OnSizeChanged();

            // Update scroll range
            OnOutputBoundsChanged();
        }

        /// <summary>
        /// Focus the debug command line and ensure that the output log window is visible.
        /// </summary>
        public void FocusCommand()
        {
            FocusOrShow();
            _commandLineBox.Focus();
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            FlaxEngine.Profiler.BeginEvent("OutputLogWindow.Update");

            // Read the incoming log messages
            int logCount;
            do
            {
                logCount = Editor.Internal_ReadOutputLogs(ref _outMessages, ref _outLogTypes, ref _outLogTimes, OutCapacity);

                for (int i = 0; i < logCount; i++)
                {
                    var entry = new Entry
                    {
                        Level = (LogType)_outLogTypes[i],
                        Time = new DateTime(_outLogTimes[i], DateTimeKind.Utc),
                        Message = _outMessages[i],
                    };
                    _entries.Add(entry);
                    _outMessages[i] = null;
                    _isDirty = true;
                }
            } while (logCount != 0);

            if (_isDirty && _output.IsSelectingText)
                _outputSelectionBlockedAutoScroll = true;

            if (_isDirty && !_output.IsSelectingText)
            {
                _isDirty = false;
                var wasEmpty = _output.TextLength == 0;

                // Cache fonts
                _output.DefaultStyle.Font.GetFont();
                _output.WarningStyle.Font.GetFont();
                _output.ErrorStyle.Font.GetFont();

                // Generate the output log
                Span<Entry> entries = CollectionsMarshal.AsSpan(_entries);
                var searchQuery = _searchBox.Text;
                var wrapWidth = GetOutputWrapWidth();
                _lastOutputWrapWidth = wrapWidth;
                for (int i = _textBufferCount; i < _entries.Count; i++)
                {
                    ref var entry = ref entries[i];
                    if (((int)entry.Level & _logTypeShowMask) == 0)
                        continue;

                    entry.Message = SanitizeMessage(entry.Message);
                    if (searchQuery.Length != 0 && entry.Message.IndexOf(searchQuery, StringComparison.OrdinalIgnoreCase) == -1)
                        continue;

                    AppendVisibleEntry(i, ref entry, wrapWidth);
                }

                // Update the output
                var cachedScrollValue = _vScroll.Value;
                var cachedSelection = _output.SelectionRange;
                var cachedOutputTargetViewOffset = _output.TargetViewOffset;
                var isBottomScroll = !_outputSelectionBlockedAutoScroll && (_vScroll.Value >= _vScroll.Maximum - (_scrollSize * 2) || wasEmpty);
                var outputText = _textBuffer.ToString();
                if (_output.Text.Equals(outputText, StringComparison.Ordinal))
                {
                    _output.RefreshTextLayout();
                    OnOutputTextChanged();
                }
                else
                {
                    _output.Text = outputText;
                }
                if (_hScroll.Maximum <= 0.0)
                    cachedOutputTargetViewOffset.X = 0;
                if (_vScroll.Maximum <= 0.0)
                    cachedOutputTargetViewOffset.Y = 0;
                _output.TargetViewOffset = cachedOutputTargetViewOffset;
                _textBufferCount = _entries.Count;
                if (!_vScroll.IsThumbClicked)
                    _vScroll.TargetValue = isBottomScroll ? _vScroll.Maximum : cachedScrollValue;
                if (_selectedEntryIndex != -1)
                {
                    if (FindEntryRange(_selectedEntryIndex, out var selectedRange))
                        _output.SelectionRange = new TextRange(selectedRange.StartIndex, selectedRange.EndIndex);
                    else
                    {
                        _selectedEntryIndex = -1;
                        _output.Deselect();
                    }
                }
                else
                {
                    _output.SelectionRange = cachedSelection;
                }
                _outputSelectionBlockedAutoScroll = false;
            }

            base.Update(deltaTime);

            FlaxEngine.Profiler.EndEvent();
        }

        /// <inheritdoc />
        public override void OnInit()
        {
            _startupTime = Time.StartupTime;

            // Load debug commands history
            if (Editor.ProjectCache.TryGetCustomData(CommandHistoryKey, out string history))
            {
                try
                {
                    _commandHistory = (List<string>)FlaxEngine.Json.JsonSerializer.Deserialize(history, typeof(List<string>));
                    for (int i = _commandHistory.Count - 1; i >= 0; i--)
                    {
                        if (string.IsNullOrEmpty(_commandHistory[i]))
                            _commandHistory.RemoveAt(i);
                    }
                }
                catch
                {
                    // Ignore errors
                    _commandHistory = null;
                }
            }
        }

        /// <inheritdoc />
        public override bool UseLayoutData => true;

        /// <inheritdoc />
        public override void OnLayoutSerialize(XmlWriter writer)
        {
            writer.WriteAttributeString("LogTypeShowMask", _logTypeShowMask.ToString());
            writer.WriteAttributeString("WrapLogLines", _wrapLogLines.ToString());
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            if (int.TryParse(node.GetAttribute("LogTypeShowMask"), out int value1))
                _logTypeShowMask = value1;
            if (bool.TryParse(node.GetAttribute("WrapLogLines"), out var value2))
                _wrapLogLines = value2;
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize()
        {
            _wrapLogLines = true;
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;

            // Unbind events
            Editor.Options.OptionsChanged -= OnEditorOptionsChanged;
            GameCooker.Event -= OnGameCookerEvent;
            ScriptsBuilder.CompilationFailed -= OnScriptsCompilationFailed;

            // Cleanup
            _textBuffer.Clear();
            _textBuffer = null;
            _textBlocks.Clear();
            _textBlocks = null;
            _entryRanges.Clear();
            _entryRanges = null;
            _entries.Clear();
            _entries = null;
            _outMessages = null;
            _outLogTypes = null;
            _outLogTimes = null;
            _compileRegex = null;
            _commandHistory = null;

            // Unlink controls
            _viewDropdown = null;
            _searchBox = null;
            _hScroll = null;
            _vScroll = null;
            _output = null;
            _commandLineBox = null;
            _contextMenu = null;

            base.OnDestroy();
        }
    }
}
