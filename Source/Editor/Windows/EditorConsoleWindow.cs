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
    public sealed class EditorConsoleWindow : EditorWindow
    {
        private enum EntryKind
        {
            Info = 1,
            Warning = 2,
            Error = 4,
            Fatal = 8,
            Command = 16,
            Result = 32,
        }

        /// <summary>
        /// The single log message entry.
        /// </summary>
        private struct Entry
        {
            /// <summary>
            /// The log entry kind.
            /// </summary>
            public EntryKind Kind;

            /// <summary>
            /// The log time (in UTC local format).
            /// </summary>
            public DateTime Time;

            /// <summary>
            /// The message contents.
            /// </summary>
            public string Message;

            /// <summary>
            /// The optional stack trace.
            /// </summary>
            public string StackTrace;

            /// <summary>
            /// The source thread identifier.
            /// </summary>
            public ulong ThreadId;
        };

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
            private enum SelectionMode
            {
                Character,
                Word,
                Line,
            }

            private const float SelectionAutoScrollEdgeSize = 28.0f;
            private const float SelectionAutoScrollMinSpeed = 140.0f;
            private const float SelectionAutoScrollMaxSpeed = 560.0f;
            private const double TripleClickTime = 0.5;
            private const float TripleClickDistanceSquared = 64.0f;
            private const float MiddleScrollDeadZone = 12.0f;
            private const float MiddleScrollSpeedScale = 8.0f;
            private const float MiddleScrollMaxSpeed = 6000.0f;

            private SelectionMode _selectionMode;
            private int _selectionAnchorStart;
            private int _selectionAnchorEnd;
            private DateTime _lastDoubleClickTime;
            private Float2 _lastDoubleClickLocation;
            private bool _isMiddleScrolling;
            private Float2 _middleScrollOrigin;
            private Float2 _middleScrollRemainder;

            /// <summary>
            /// The parent window.
            /// </summary>
            public EditorConsoleWindow Window;

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

            /// <summary>
            /// The command text style.
            /// </summary>
            public TextBlockStyle CommandStyle;

            /// <summary>
            /// The command result text style.
            /// </summary>
            public TextBlockStyle ResultStyle;

            public OutputTextBox()
            {
                _consumeAllKeyDownEvents = false;
            }

            public bool IsSelectingText => _isSelecting;

            public bool IsMiddleScrolling => _isMiddleScrolling;

            public override int CharIndexAtPoint(ref Float2 location)
            {
                var clampedLocation = ClampSelectionPoint(location);
                if (_textBlocks.Count == 0)
                    return 0;

                float contentY = clampedLocation.Y + _viewOffset.Y;
                TextBlock previous = default;
                bool hasPrevious = false;
                for (int i = 0; i < _textBlocks.Count; i++)
                {
                    var block = _textBlocks[i];
                    if (contentY < block.Bounds.Top)
                    {
                        if (!hasPrevious)
                            return block.Range.StartIndex;
                        float distanceAbove = contentY - previous.Bounds.Bottom;
                        float distanceBelow = block.Bounds.Top - contentY;
                        return distanceAbove <= distanceBelow ? previous.Range.EndIndex : block.Range.StartIndex;
                    }
                    if (contentY < block.Bounds.Bottom)
                        return base.CharIndexAtPoint(ref clampedLocation);
                    previous = block;
                    hasPrevious = true;
                }

                return _text.Length;
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
                if (button == MouseButton.Left)
                {
                    int index = CharIndexAtPoint(ref location);
                    if (GetTextBlock(index, out TextBlock block) && block.Tag is TextBlockTag tag &&
                        tag.Type == TextBlockTag.Types.CodeLocation && File.Exists(tag.Url))
                    {
                        Editor.Instance.CodeEditing.OpenFile(tag.Url, tag.Line);
                        return true;
                    }

                    _lastDoubleClickTime = DateTime.UtcNow;
                    _lastDoubleClickLocation = location;

                    bool result = base.OnMouseDoubleClick(location, button);
                    if (TextLength != 0 && IsSelectable)
                    {
                        _selectionMode = SelectionMode.Word;
                        _selectionAnchorStart = SelectionLeft;
                        _selectionAnchorEnd = SelectionRight;
                        OnSelectingBegin();
                    }
                    return result;
                }

                return base.OnMouseDoubleClick(location, button);
            }

            /// <inheritdoc />
            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Middle &&
                    (_textSize.Y > Height || (!Window._wrapLogLines && _textSize.X > Width)))
                {
                    _isMiddleScrolling = true;
                    _middleScrollOrigin = location;
                    _middleScrollRemainder = Float2.Zero;
                    StartMouseCapture();
                    Cursor = CursorType.SizeAll;
                    return true;
                }

                if (button == MouseButton.Left && IsSelectable && IsTripleClick(location))
                {
                    _lastDoubleClickTime = DateTime.MinValue;
                    Focus();
                    OnSelectingBegin();

                    int index = CharIndexAtPoint(ref location);
                    var line = GetLineRange(index);
                    SetSelection(line.StartIndex, line.EndIndex, false);
                    _selectionMode = SelectionMode.Line;
                    _selectionAnchorStart = line.StartIndex;
                    _selectionAnchorEnd = line.EndIndex;

                    if (Cursor == CursorType.Default && _changeCursor)
                        Cursor = CursorType.IBeam;
                    return true;
                }

                if (button == MouseButton.Left)
                {
                    _lastDoubleClickTime = DateTime.MinValue;
                    _selectionMode = SelectionMode.Character;
                }
                return base.OnMouseDown(location, button);
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Middle && _isMiddleScrolling)
                {
                    EndMouseCapture();
                    return true;
                }
                return base.OnMouseUp(location, button);
            }

            /// <inheritdoc />
            public override void OnEndMouseCapture()
            {
                _isMiddleScrolling = false;
                _middleScrollRemainder = Float2.Zero;
                if (Cursor == CursorType.SizeAll)
                    Cursor = CursorType.Default;
                base.OnEndMouseCapture();
            }

            /// <inheritdoc />
            public override bool OnKeyDown(KeyboardKeys key)
            {
                if (key == KeyboardKeys.C && HasSelection && Root != null && Root.GetKey(KeyboardKeys.Control))
                {
                    Copy();
                    return true;
                }

                return base.OnKeyDown(key);
            }

            /// <inheritdoc />
            public override void OnMouseMove(Float2 location)
            {
                if (_isMiddleScrolling)
                    return;

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

                if (_isMiddleScrolling)
                {
                    if (Root == null || !Root.GetMouseButton(MouseButton.Middle))
                    {
                        EndMouseCapture();
                        return;
                    }

                    ScrollWithMiddleMouse(PointFromWindow(Root.MousePosition), deltaTime);
                    return;
                }

                if (!IsSelectingText || Root == null || !Root.GetMouseButton(MouseButton.Left))
                    return;

                var location = PointFromWindow(Root.MousePosition);
                if (ScrollDuringSelection(ref location, deltaTime))
                    ExtendTextSelection(ref location);
            }

            private Float2 ClampSelectionPoint(Float2 location)
            {
                location.X = Mathf.Clamp(location.X, 0.0f, Width);
                location.Y = Mathf.Clamp(location.Y, 0.0f, Height);
                return location;
            }

            private void ExtendTextSelection(ref Float2 location)
            {
                int currentIndex = CharIndexAtPoint(ref location);
                switch (_selectionMode)
                {
                case SelectionMode.Word:
                    ExtendTextSelection(GetWordRange(currentIndex));
                    break;
                case SelectionMode.Line:
                    ExtendTextSelection(GetLineRange(currentIndex));
                    break;
                default:
                    SetSelection(_selectionStart, currentIndex, false);
                    break;
                }
            }

            private void ExtendTextSelection(TextRange currentRange)
            {
                if (currentRange.EndIndex <= _selectionAnchorStart)
                    SetSelection(_selectionAnchorEnd, currentRange.StartIndex, false);
                else if (currentRange.StartIndex >= _selectionAnchorEnd)
                    SetSelection(_selectionAnchorStart, currentRange.EndIndex, false);
                else
                    SetSelection(_selectionAnchorStart, _selectionAnchorEnd, false);
            }

            private TextRange GetWordRange(int index)
            {
                int textLength = TextLength;
                if (textLength == 0)
                    return new TextRange(0, 0);

                index = Mathf.Clamp(index, 0, textLength);
                int searchIndex = Mathf.Min(index - 2, textLength - 1);
                int separatorIndex = searchIndex >= 0 ? _text.LastIndexOfAny(Separators, searchIndex) : -1;
                int left = separatorIndex == -1 ? 0 : separatorIndex + 1;
                searchIndex = Mathf.Min(index + 1, textLength);
                separatorIndex = searchIndex < textLength ? _text.IndexOfAny(Separators, searchIndex) : -1;
                int right = separatorIndex == -1 ? textLength : separatorIndex;
                return new TextRange(left, right);
            }

            private TextRange GetLineRange(int index)
            {
                int textLength = TextLength;
                if (textLength == 0)
                    return new TextRange(0, 0);

                index = Mathf.Clamp(index, 0, textLength);
                int searchIndex = Mathf.Min(index - 1, textLength - 1);
                int lineStart = searchIndex >= 0 ? _text.LastIndexOf('\n', searchIndex) + 1 : 0;
                int lineEnd = _text.IndexOf('\n', Mathf.Min(index, textLength - 1));
                lineEnd = lineEnd == -1 ? textLength : lineEnd + 1;
                return new TextRange(lineStart, lineEnd);
            }

            private bool IsTripleClick(Float2 location)
            {
                if (_lastDoubleClickTime == DateTime.MinValue ||
                    (DateTime.UtcNow - _lastDoubleClickTime).TotalSeconds > TripleClickTime)
                    return false;
                return Float2.DistanceSquared(location, _lastDoubleClickLocation) <= TripleClickDistanceSquared;
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

            private void ScrollWithMiddleMouse(Float2 location, float deltaTime)
            {
                var distance = location - _middleScrollOrigin;
                var speed = new Float2(GetMiddleScrollSpeed(distance.X), GetMiddleScrollSpeed(distance.Y));
                if (speed.IsZero)
                {
                    _middleScrollRemainder = Float2.Zero;
                    return;
                }

                var maxViewOffset = Float2.Max(_textSize - Size, Float2.Zero);
                if (Window._wrapLogLines)
                    maxViewOffset.X = 0.0f;
                var viewOffset = TargetViewOffset + speed * deltaTime + _middleScrollRemainder;
                viewOffset = Float2.Clamp(viewOffset, Float2.Zero, maxViewOffset);
                TargetViewOffset = viewOffset;
                _middleScrollRemainder = viewOffset - TargetViewOffset;
            }

            private static float GetMiddleScrollSpeed(float distance)
            {
                float speed = Mathf.Abs(distance) - MiddleScrollDeadZone;
                if (speed <= 0.0f)
                    return 0.0f;
                return Mathf.Sign(distance) * Mathf.Min(speed * MiddleScrollSpeedScale, MiddleScrollMaxSpeed);
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
                public string InsertText;

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
                    InsertText = null;
                    base.OnDestroy();
                }
            }

            private EditorConsoleWindow _window;
            private ItemsListContextMenu _searchPopup;
            private ItemsListContextMenu _historyPopup;
            private bool _isSettingText;

            public CommandLineBox(float x, float y, float width, EditorConsoleWindow window)
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
                    if (RootWindow?.Window != null)
                        RootWindow.Window.LostFocus -= OnRootWindowLostFocus;
                    _searchPopup.Dispose();
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
                    lastItem.TintColor = Style.Current.Foreground;
                    lastItem.ItemFocused += item =>
                    {
                        // Set command
                        Set(((Item)item).InsertText ?? item.Name);
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

            private void ShowSuggestionPopup(IReadOnlyList<EditorCommandSuggestion> suggestions)
            {
                HideSearch();
                _searchPopup = new ItemsListContextMenu(360, 220, false)
                {
                    UseVisibilityControl = false,
                    UseInput = false,
                };

                ItemsListContextMenu.Item lastItem = null;
                var itemFont = Style.Current.FontSmall;
                float maxWidth = 0.0f;
                for (int i = 0; i < suggestions.Count; i++)
                {
                    EditorCommandSuggestion suggestion = suggestions[i];
                    string label = string.IsNullOrEmpty(suggestion.Detail)
                        ? suggestion.Display
                        : suggestion.Display + "    " + suggestion.Detail;
                    var item = new Item
                    {
                        Name = label,
                        InsertText = suggestion.Text + (suggestion.IsCommand ? " " : string.Empty),
                        Owner = this,
                        TintColor = Style.Current.Foreground,
                    };
                    _searchPopup.AddItem(lastItem = item);
                    item.ItemFocused += focused => Set(((Item)focused).InsertText);
                    maxWidth = Mathf.Max(maxWidth, itemFont.MeasureText(label).X);
                }

                _searchPopup.ItemClicked += item =>
                {
                    Set(((Item)item).InsertText);
                    HideSearch();
                    Focus();
                };
                float totalHeight = suggestions.Count * lastItem.Height + _searchPopup.ItemsPanel.Margin.Height +
                                    _searchPopup.ItemsPanel.Spacing * (suggestions.Count - 1);
                _searchPopup.Height = Mathf.Min(220.0f, totalHeight);
                _searchPopup.Width = Mathf.Max(_searchPopup.Width, maxWidth + 8.0f + ScrollBar.DefaultSize);
                _searchPopup.Show(this, Float2.Zero, ContextMenuDirection.RightUp);
                RootWindow.Window.LostFocus += OnRootWindowLostFocus;
            }

            private void OnRootWindowLostFocus()
            {
                // Prevent popup from staying active when editor window looses focus
                HideSearch();
            }

            /// <inheritdoc />
            public override void OnGotFocus()
            {
                EditorConsole.Initialize();
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
                string text = Text ?? string.Empty;
                if (!string.IsNullOrEmpty(text))
                {
                    IReadOnlyList<EditorCommandSuggestion> suggestions = EditorConsole.GetAutoComplete(text);
                    if (suggestions.Count != 0)
                    {
                        HideHistory();
                        ShowSuggestionPopup(suggestions);
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
                    EditorConsole.Execute(command);
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
                    IReadOnlyList<EditorCommandSuggestion> suggestions = EditorConsole.GetAutoComplete(Text);
                    if (suggestions.Count != 0)
                    {
                        EditorCommandSuggestion suggestion = suggestions[0];
                        Set(suggestion.Text + (suggestion.IsCommand ? " " : string.Empty));
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
        private readonly int[] _logTypeCounts = new int[3];
        private bool _isDirty;
        private int _logTypeShowMask = (int)LogType.Info | (int)LogType.Warning | (int)LogType.Error | (int)LogType.Fatal;
        private float _scrollSize = ScrollBar.DefaultSize;
        private const int OutCapacity = 64;
        private string[] _outMessages = new string[OutCapacity];
        private string[] _outStackTraces = new string[OutCapacity];
        private byte[] _outLogTypes = new byte[OutCapacity];
        private long[] _outLogTimes = new long[OutCapacity];
        private ulong[] _outThreadIds = new ulong[OutCapacity];
        private int _textBufferCount;
        private StringBuilder _textBuffer = new StringBuilder();
        private List<TextBlock> _textBlocks = new List<TextBlock>();
        private DateTime _startupTime;
        private Regex _compileRegex = new Regex("(?<path>^(?:[a-zA-Z]\\:|\\\\\\\\[ \\-\\.\\w\\.]+\\\\[ \\-\\.\\w.$]+)\\\\(?:[ \\-\\.\\w]+\\\\)*\\w([ \\w.])+)\\((?<line>\\d{1,}),\\d{1,},\\d{1,},\\d{1,}\\): (?<level>error|warning) (?<message>.*)", RegexOptions.Compiled | RegexOptions.Multiline);
        private List<string> _commandHistory;
        private const string CommandHistoryKey = "CommandHistory";
        private const int CommandHistoryLimit = 30;
        private const float OutputPadding = 2.0f;
        private const float EntrySpacing = 1.0f;

        private Button _clearButton;
        private Button _viewDropdown;
        private ToolStripButton _clearOnPlayButton;
        private readonly ToolStripButton[] _logTypeButtons = new ToolStripButton[3];
        private ToolStripButton _stackTraceButton;
        private TextBox _searchBox;
        private HScrollBar _hScroll;
        private VScrollBar _vScroll;
        private OutputTextBox _output;
        private CommandLineBox _commandLineBox;
        private ContextMenu _contextMenu;
        private float _lastOutputWrapWidth = -1.0f;
        private bool _wrapLogLines = true;
        private bool _showStackTrace;
        private bool _outputSelectionBlockedAutoScroll;
        private EditorConsoleHtmlLog _htmlLog;

        /// <summary>
        /// Initializes a new instance of the <see cref="EditorConsoleWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public EditorConsoleWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "Editor Console";
            Icon = editor.Icons.Info64;
            ClipChildren = false;
            FlaxEditor.Utilities.Utils.SetupCommonInputActions(this);

            // Setup UI
            _clearButton = new Button(2, 2, 44.0f, TextBoxBase.DefaultHeight)
            {
                TooltipText = "Clear editor console",
                Text = "Clear",
                Parent = this,
            };
            _clearButton.Clicked += Clear;

            float toolbarX = _clearButton.Right + 2;
            var clearOnPlayIcon = SpriteHandle.Invalid;
            _clearOnPlayButton = new ToolStripButton(TextBoxBase.DefaultHeight, ref clearOnPlayIcon)
            {
                Parent = this,
                Location = new Float2(toolbarX, 2),
                Text = "Clear on Play",
                AutoCheck = true,
                UseBlueCheckedStyle = true,
                TooltipText = "Clear the editor console when entering play mode",
            };
            _clearOnPlayButton.Clicked += ToggleClearOnPlay;
            toolbarX = _clearOnPlayButton.Right + 2;
            var infoIcon = editor.Icons.Info32;
            _logTypeButtons[0] = new ToolStripButton(TextBoxBase.DefaultHeight, ref infoIcon)
            {
                Parent = this,
                Location = new Float2(toolbarX, 2),
                AutoCheck = true,
                Checked = true,
                Text = "0",
                UseBlueCheckedStyle = true,
                TooltipText = "Show info messages",
            };
            _logTypeButtons[0].Clicked += () => ToggleLogTypeShow(LogType.Info);
            toolbarX = _logTypeButtons[0].Right + 2;
            var warningIcon = editor.Icons.Warning32;
            _logTypeButtons[1] = new ToolStripButton(TextBoxBase.DefaultHeight, ref warningIcon)
            {
                Parent = this,
                Location = new Float2(toolbarX, 2),
                AutoCheck = true,
                Checked = true,
                Text = "0",
                UseBlueCheckedStyle = true,
                TooltipText = "Show warning messages",
            };
            _logTypeButtons[1].Clicked += () => ToggleLogTypeShow(LogType.Warning);
            toolbarX = _logTypeButtons[1].Right + 2;
            var errorIcon = editor.Icons.Error32;
            _logTypeButtons[2] = new ToolStripButton(TextBoxBase.DefaultHeight, ref errorIcon)
            {
                Parent = this,
                Location = new Float2(toolbarX, 2),
                AutoCheck = true,
                Checked = true,
                Text = "0",
                UseBlueCheckedStyle = true,
                TooltipText = "Show error and fatal messages",
            };
            _logTypeButtons[2].Clicked += () => ToggleLogTypeShow(LogType.Error);
            toolbarX = _logTypeButtons[2].Right + 2;
            var stackIcon = SpriteHandle.Invalid;
            _stackTraceButton = new ToolStripButton(TextBoxBase.DefaultHeight, ref stackIcon)
            {
                Parent = this,
                Location = new Float2(toolbarX, 2),
                Text = "Stacktrace",
                AutoCheck = true,
                UseBlueCheckedStyle = true,
                TooltipText = "Show full stack traces",
            };
            _stackTraceButton.Clicked += ToggleStackTrace;
            toolbarX = _stackTraceButton.Right + 2;

            _searchBox = new SearchBox(false, toolbarX, 2, Width - toolbarX - 46.0f)
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
                WatermarkText = "Enter editor command",
            };

            // Setup context menu
            _htmlLog = new EditorConsoleHtmlLog();
            _contextMenu = new ContextMenu();
            _contextMenu.AddButton("Clear console", Clear);
            _contextMenu.AddButton("Copy selection", _output.Copy);
            _contextMenu.AddButton("Select All", _output.SelectAll);
            _contextMenu.AddButton("Open HTML log", _htmlLog.Open);
            _contextMenu.AddButton(Utilities.Constants.ShowInExplorer, _htmlLog.OpenFolder);
            _contextMenu.AddButton("Scroll to bottom", () => { _vScroll.TargetValue = _vScroll.Maximum; }).Icon = Editor.Icons.ArrowDown12;

            // Setup editor options
            Editor.Options.OptionsChanged += OnEditorOptionsChanged;
            OnEditorOptionsChanged(Editor.Options.Options);

            InputActions.Add(options => options.Search, _searchBox.Focus);

            GameCooker.Event += OnGameCookerEvent;
            ScriptsBuilder.CompilationFailed += OnScriptsCompilationFailed;

            EditorConsole.MessageWritten += OnEditorConsoleMessage;
            EditorConsole.ClearRequested += Clear;
            EditorConsole.OpenRequested += FocusOrShow;
            EditorConsole.CloseRequested += CloseConsole;
            EditorConsole.OpenHtmlRequested += _htmlLog.Open;
            EditorConsole.OpenLogFolderRequested += _htmlLog.OpenFolder;
            EditorConsole.HtmlLogPathProvider += GetHtmlLogPath;
            Editor.InitializationEnd += OnEditorInitializationEnd;
            EditorConsole.Initialize();
        }

        private void OnViewButtonClicked()
        {
            var menu = new ContextMenu();

            var wrapButton = menu.AddButton("Wrap");
            wrapButton.AutoCheck = true;
            wrapButton.Checked = _wrapLogLines;
            wrapButton.Clicked += ToggleWrapLogLines;

            menu.AddSeparator();

            menu.AddButton("Open HTML log", _htmlLog.Open);
            menu.AddButton(Utilities.Constants.ShowInExplorer, _htmlLog.OpenFolder);

            menu.Show(_viewDropdown.Parent, _viewDropdown.BottomLeft);
        }

        private void ToggleLogTypeShow(LogType type)
        {
            _logTypeShowMask ^= (int)type;
            if (type == LogType.Error)
                _logTypeShowMask ^= (int)LogType.Fatal;
            UpdateToolbarChecks();
            Refresh();
        }

        private void ToggleClearOnPlay()
        {
            Editor.Options.Options.Interface.DebugLogClearOnPlay = _clearOnPlayButton.Checked;
            Editor.Options.Apply(Editor.Options.Options);
        }

        private void ToggleWrapLogLines()
        {
            _wrapLogLines = !_wrapLogLines;
            if (_wrapLogLines)
                _output.TargetViewOffset = new Float2(0.0f, _output.TargetViewOffset.Y);
            UpdateToolbarChecks();
            Refresh();
        }

        private void UpdateToolbarChecks()
        {
            _logTypeButtons[0].Checked = (_logTypeShowMask & (int)LogType.Info) != 0;
            _logTypeButtons[1].Checked = (_logTypeShowMask & (int)LogType.Warning) != 0;
            _logTypeButtons[2].Checked = (_logTypeShowMask & (int)LogType.Error) != 0;
            _stackTraceButton.Checked = _showStackTrace;
        }

        private void ToggleStackTrace()
        {
            _showStackTrace = !_showStackTrace;
            UpdateToolbarChecks();
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
            _clearOnPlayButton.Checked = options.Interface.DebugLogClearOnPlay;
            _logTypeButtons[1].IconColor = options.Visual.LogWarningColor;
            _logTypeButtons[2].IconColor = options.Visual.LogErrorColor;
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
                _output.ErrorStyle.Color == options.Visual.LogErrorColor &&
                _output.CommandStyle.Color == style.BorderSelected &&
                _output.ResultStyle.Color == style.ProgressNormal)
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
            _output.CommandStyle = _output.DefaultStyle;
            _output.CommandStyle.Color = style.BorderSelected;
            _output.ResultStyle = _output.DefaultStyle;
            _output.ResultStyle.Color = style.ProgressNormal;

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
            _lastOutputWrapWidth = -1.0f;
            _isDirty = true;
        }

        private static string SanitizeMessage(string message)
        {
            if (string.IsNullOrEmpty(message))
                return string.Empty;
            return message.IndexOf('\r') != -1 ? message.Replace("\r", "") : message;
        }

        private TextBlockStyle GetEntryStyle(EntryKind kind)
        {
            switch (kind)
            {
            case EntryKind.Info:
                return _output.DefaultStyle;
            case EntryKind.Warning:
                return _output.WarningStyle;
            case EntryKind.Error:
            case EntryKind.Fatal:
                return _output.ErrorStyle;
            case EntryKind.Command:
                return _output.CommandStyle;
            case EntryKind.Result:
                return _output.ResultStyle;
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
                _textBuffer.AppendFormat("[{0}] ", entry.Kind);
            }
            return _textBuffer.Length;
        }

        private float GetOutputWrapWidth()
        {
            return _wrapLogLines ? Mathf.Max(_output != null ? _output.Width : 0.0f, 1.0f) : float.MaxValue;
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

        private void AppendVisibleEntry(ref Entry entry, float wrapWidth)
        {
            entry.Message = SanitizeMessage(entry.Message);
            entry.StackTrace = SanitizeMessage(entry.StackTrace);

            var entryStart = _textBuffer.Length;
            var y = _textBlocks.Count == 0 ? 0.0f : _textBlocks[_textBlocks.Count - 1].Bounds.Bottom + EntrySpacing;
            var style = GetEntryStyle(entry.Kind);
            var messageStart = AppendEntryPrefix(ref entry);
            var prefixLength = messageStart - entryStart;
            _textBuffer.Append(entry.Message);
            if (!string.IsNullOrWhiteSpace(entry.StackTrace))
            {
                _textBuffer.Append('\n');
                _textBuffer.Append(_showStackTrace ? entry.StackTrace : GetFirstLine(entry.StackTrace));
            }
            var entryEnd = _textBuffer.Length;
            _textBuffer.Append('\n');
            AddWrappedTextBlocks(_textBuffer.ToString(entryStart, entryEnd - entryStart), entryStart, style, ref y, wrapWidth, prefixLength);

        }

        private static string GetFirstLine(string text)
        {
            int lineEnd = text.IndexOf('\n');
            return lineEnd >= 0 ? text.Substring(0, lineEnd) : text;
        }

        private string GetHtmlLogPath()
        {
            return _htmlLog.FilePath;
        }

        private void CloseConsole()
        {
            Close();
        }

        private void OnEditorConsoleMessage(EditorConsoleMessageKind kind, string message)
        {
            EntryKind entryKind = kind switch
            {
                EditorConsoleMessageKind.Command => EntryKind.Command,
                EditorConsoleMessageKind.Result => EntryKind.Result,
                _ => EntryKind.Error,
            };
            AddEntry(new Entry
            {
                Kind = entryKind,
                Time = DateTime.Now,
                Message = message ?? string.Empty,
                ThreadId = Platform.CurrentThreadID,
            }, false);
        }

        private void AddEntry(Entry entry, bool allowPause = true)
        {
            _entries.Add(entry);
            switch (entry.Kind)
            {
            case EntryKind.Info:
                _logTypeCounts[0]++;
                break;
            case EntryKind.Warning:
                _logTypeCounts[1]++;
                break;
            case EntryKind.Error:
            case EntryKind.Fatal:
                _logTypeCounts[2]++;
                break;
            }
            UpdateLogTypeCounts();
            _htmlLog.Append(entry.Time, entry.ThreadId, entry.Kind.ToString(), entry.Message, entry.StackTrace);
            _isDirty = true;

            int maximum = Math.Max(100, EditorConsole.Lines);
            if (_entries.Count > maximum)
            {
                _entries.RemoveRange(0, _entries.Count - maximum);
                Refresh();
            }

            if (allowPause && (entry.Kind == EntryKind.Error || entry.Kind == EntryKind.Fatal) &&
                Editor.Options.Options.Interface.DebugLogPauseOnError &&
                Editor.StateMachine.CurrentState == Editor.StateMachine.PlayingState)
            {
                Editor.Simulation.RequestPausePlay();
            }
        }

        /// <summary>
        /// Clears the log.
        /// </summary>
        public void Clear()
        {
            _entries?.Clear();
            Array.Clear(_logTypeCounts, 0, _logTypeCounts.Length);
            UpdateLogTypeCounts();
            Refresh();
        }

        private void UpdateLogTypeCounts()
        {
            bool layoutChanged = false;
            for (int i = 0; i < _logTypeButtons.Length; i++)
            {
                string text = Math.Min(_logTypeCounts[i], 999).ToString();
                if (_logTypeButtons[i].Text == text)
                    continue;
                float previousWidth = _logTypeButtons[i].Width;
                _logTypeButtons[i].Text = text;
                layoutChanged |= !Mathf.NearEqual(previousWidth, _logTypeButtons[i].Width);
            }
            if (layoutChanged)
                PerformLayout();
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            base.PerformLayoutBeforeChildren();

            if (_output != null)
            {
                _viewDropdown.X = Width - _viewDropdown.Width - 2;
                _stackTraceButton.X = _viewDropdown.X - _stackTraceButton.Width - 2;
                _logTypeButtons[2].X = _stackTraceButton.X - _logTypeButtons[2].Width - 2;
                _logTypeButtons[1].X = _logTypeButtons[2].X - _logTypeButtons[1].Width - 2;
                _logTypeButtons[0].X = _logTypeButtons[1].X - _logTypeButtons[0].Width - 2;
                _searchBox.X = _clearOnPlayButton.Right + 2;
                _searchBox.Width = Mathf.Max(_logTypeButtons[0].X - _searchBox.X - 2, 0.0f);
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
            FlaxEngine.Profiler.BeginEvent("EditorConsoleWindow.Update");

            // Read the incoming log messages
            int logCount;
            do
            {
                logCount = Editor.Internal_ReadOutputLogs(ref _outMessages, ref _outStackTraces, ref _outLogTypes,
                    ref _outLogTimes, ref _outThreadIds, OutCapacity);

                for (int i = 0; i < logCount; i++)
                {
                    var entry = new Entry
                    {
                        Kind = (EntryKind)_outLogTypes[i],
                        Time = new DateTime(_outLogTimes[i], DateTimeKind.Utc),
                        Message = _outMessages[i],
                        StackTrace = _outStackTraces[i],
                        ThreadId = _outThreadIds[i],
                    };
                    AddEntry(entry);
                    _outMessages[i] = null;
                    _outStackTraces[i] = null;
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
                _output.CommandStyle.Font.GetFont();
                _output.ResultStyle.Font.GetFont();

                // Generate the output log
                Span<Entry> entries = CollectionsMarshal.AsSpan(_entries);
                var searchQuery = _searchBox.Text;
                var wrapWidth = GetOutputWrapWidth();
                _lastOutputWrapWidth = wrapWidth;
                for (int i = _textBufferCount; i < _entries.Count; i++)
                {
                    ref var entry = ref entries[i];
                    if ((entry.Kind == EntryKind.Info || entry.Kind == EntryKind.Warning ||
                         entry.Kind == EntryKind.Error || entry.Kind == EntryKind.Fatal) &&
                        ((int)entry.Kind & _logTypeShowMask) == 0)
                        continue;

                    entry.Message = SanitizeMessage(entry.Message);
                    entry.StackTrace = SanitizeMessage(entry.StackTrace);
                    if (searchQuery.Length != 0 &&
                        entry.Message.IndexOf(searchQuery, StringComparison.OrdinalIgnoreCase) == -1 &&
                        (entry.StackTrace?.IndexOf(searchQuery, StringComparison.OrdinalIgnoreCase) ?? -1) == -1)
                        continue;

                    AppendVisibleEntry(ref entry, wrapWidth);
                }

                // Update the output
                var cachedScrollValue = _vScroll.Value;
                var cachedSelection = _output.SelectionRange;
                var cachedOutputTargetViewOffset = _output.TargetViewOffset;
                var isBottomScroll = Editor.Options.Options.Interface.OutputLogScrollToBottom &&
                                     !_outputSelectionBlockedAutoScroll &&
                                     !_output.IsMiddleScrolling &&
                                     (_vScroll.Value >= _vScroll.Maximum - (_scrollSize * 2) || wasEmpty);
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
                _output.SelectionRange = cachedSelection;
                _outputSelectionBlockedAutoScroll = false;
            }

            base.Update(deltaTime);

            FlaxEngine.Profiler.EndEvent();
        }

        /// <inheritdoc />
        public override void OnPlayBeginning()
        {
            if (Editor.Options.Options.Interface.DebugLogClearOnPlay)
                Clear();
        }

        private void OnEditorInitializationEnd()
        {
            Editor.InitializationEnd -= OnEditorInitializationEnd;
            var panel = ParentDockPanel;
            if (panel != null && panel.TabsCount == 1 && panel.SelectedTab == null)
                panel.SelectTab(this, false);
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
            writer.WriteAttributeString("ShowStackTrace", _showStackTrace.ToString());
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize(XmlElement node)
        {
            if (int.TryParse(node.GetAttribute("LogTypeShowMask"), out int value1))
                _logTypeShowMask = value1;
            if (bool.TryParse(node.GetAttribute("WrapLogLines"), out var value2))
                _wrapLogLines = value2;
            if (bool.TryParse(node.GetAttribute("ShowStackTrace"), out var value3))
                _showStackTrace = value3;
            UpdateToolbarChecks();
        }

        /// <inheritdoc />
        public override void OnLayoutDeserialize()
        {
            _wrapLogLines = true;
            _showStackTrace = false;
            UpdateToolbarChecks();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (IsDisposing)
                return;

            // Unbind events
            Editor.Options.OptionsChanged -= OnEditorOptionsChanged;
            Editor.InitializationEnd -= OnEditorInitializationEnd;
            GameCooker.Event -= OnGameCookerEvent;
            ScriptsBuilder.CompilationFailed -= OnScriptsCompilationFailed;
            EditorConsole.MessageWritten -= OnEditorConsoleMessage;
            EditorConsole.ClearRequested -= Clear;
            EditorConsole.OpenRequested -= FocusOrShow;
            EditorConsole.CloseRequested -= CloseConsole;
            EditorConsole.OpenHtmlRequested -= _htmlLog.Open;
            EditorConsole.OpenLogFolderRequested -= _htmlLog.OpenFolder;
            EditorConsole.HtmlLogPathProvider -= GetHtmlLogPath;
            EditorConsole.Shutdown();

            // Cleanup
            _textBuffer.Clear();
            _textBuffer = null;
            _textBlocks.Clear();
            _textBlocks = null;
            _entries.Clear();
            _entries = null;
            _outMessages = null;
            _outStackTraces = null;
            _outLogTypes = null;
            _outLogTimes = null;
            _outThreadIds = null;
            _compileRegex = null;
            _commandHistory = null;
            _htmlLog.Dispose();
            _htmlLog = null;

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
