// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Reflection;
using FlaxEditor.CustomEditors;
using FlaxEditor.CustomEditors.Editors;
using FlaxEditor.CustomEditors.Elements;
using FlaxEditor.GUI.Docking;
using FlaxEditor.Utilities;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Windows
{
    /// <summary>
    /// Live, editor-only inspection and design tool for the existing Flax UI control tree.
    /// </summary>
    public sealed class UIDesignInspectorWindow : EditorWindow
    {
        private const float HeaderHeight = 58.0f;
        private const float FooterHeight = 22.0f;
        private const float ButtonHeight = 24.0f;

        private readonly Button _pickButton;
        private readonly Button _parentButton;
        private readonly Button _layoutTab;
        private readonly Button _controlTab;
        private readonly Button _themeTab;
        private readonly Button _undoButton;
        private readonly Button _redoButton;
        private readonly Button _resetButton;
        private readonly Label _selectionLabel;
        private readonly Label _hintLabel;
        private readonly Panel _layoutHost;
        private readonly Panel _controlHost;
        private readonly Panel _themeHost;
        private readonly CustomEditorPresenter _layoutPresenter;
        private readonly CustomEditorPresenter _controlPresenter;
        private readonly CustomEditorPresenter _themePresenter;
        private readonly DesignHistory _layoutHistory = new DesignHistory();
        private readonly DesignHistory _controlHistory = new DesignHistory();
        private readonly DesignHistory _themeHistory = new DesignHistory();

        private UIInspectorOverlay _overlay;
        private Control _selectedControl;
        private ControlLayoutProxy _layoutProxy;
        private ControlDesignProxy _controlProxy;
        private Style _selectedStyle;
        private InspectorMode _mode;
        private bool _applyingHistory;

        /// <inheritdoc />
        public override Float2 DefaultSize => new Float2(560, 720);

        /// <inheritdoc />
        protected override bool CanOpenContentFinder => false;

        /// <summary>
        /// Initializes a new instance of the <see cref="UIDesignInspectorWindow"/> class.
        /// </summary>
        /// <param name="editor">The editor.</param>
        public UIDesignInspectorWindow(Editor editor)
        : base(editor, true, ScrollBars.None)
        {
            Title = "UI Design Inspector";

            _pickButton = AddButton("Pick", BeginPicking);
            _pickButton.TooltipText = "Block normal UI input and click a control to inspect it. Escape cancels.";
            _parentButton = AddButton("Parent", SelectParent);
            _layoutTab = AddButton("Layout", () => SetMode(InspectorMode.Layout));
            _controlTab = AddButton("Control", () => SetMode(InspectorMode.Control));
            _themeTab = AddButton("Theme", () => SetMode(InspectorMode.Theme));
            _undoButton = AddButton("Undo", Undo);
            _redoButton = AddButton("Redo", Redo);
            _resetButton = AddButton("Reset", Reset);

            _selectionLabel = new Label
            {
                Parent = this,
                Text = "No control selected",
                TextColor = Style.Current.ForegroundGrey,
            };
            _hintLabel = new Label
            {
                Parent = this,
                Text = "Live session edits  •  colors: RGB / HSV / alpha / hex  •  arrows: 1, Shift 8, Ctrl 100, Alt 0.1",
                TextColor = Style.Current.ForegroundDisabled,
                TooltipText = "Generic reflected values appear here automatically. Custom-drawn or hardcoded values must be migrated into a style or control property before they can be edited here.",
            };

            _layoutHost = CreateHost();
            _controlHost = CreateHost();
            _themeHost = CreateHost();

            _layoutPresenter = new CustomEditorPresenter(null, "Pick a UI control to edit its hidden base layout properties.");
            _layoutPresenter.Panel.Parent = _layoutHost;
            _layoutPresenter.OverrideEditor = new SourceAwareGenericEditor(item => _layoutProxy?.GetSourceMember(item.Info.Name) ?? item.Info.Type);
            _layoutPresenter.Modified += OnLayoutModified;

            _controlPresenter = new CustomEditorPresenter(null, "Pick a UI control to inspect its existing properties.");
            _controlPresenter.Panel.Parent = _controlHost;
            _controlPresenter.OverrideEditor = new SourceAwareGenericEditor(item => _controlProxy?.GetSourceMember(item.Info.Name) ?? item.Info.Type);
            _controlPresenter.Modified += OnControlModified;

            _themePresenter = new CustomEditorPresenter(null);
            _themePresenter.Panel.Parent = _themeHost;
            _themePresenter.OverrideEditor = new SourceAwareGenericEditor(item => item.Info.Type);
            _themePresenter.Modified += OnThemeModified;

            BindCurrentStyle();
            SetMode(InspectorMode.Layout);
            LayoutControls();
            UpdateActions();
        }

        private Button AddButton(string text, Action clicked)
        {
            var button = new Button
            {
                Parent = this,
                Text = text,
            };
            button.Clicked += clicked;
            return button;
        }

        private Panel CreateHost()
        {
            return new Panel(ScrollBars.Vertical)
            {
                Parent = this,
                IsScrollable = true,
            };
        }

        private void BeginPicking()
        {
            if (_overlay == null || _overlay.IsDisposing)
            {
                var root = Editor.Windows.MainWindow?.GUI;
                if (root == null)
                    return;
                _overlay = new UIInspectorOverlay(root, SelectControl, OnPickingEnded);
            }

            _overlay.Activate();
            _pickButton.Text = "Picking...";
        }

        private void OnPickingEnded()
        {
            _pickButton.Text = "Pick";
        }

        private void SelectControl(Control control)
        {
            if (control == null || control.IsDisposing || control == _overlay)
                return;

            _selectedControl = control;
            _layoutProxy = new ControlLayoutProxy(control);
            _layoutPresenter.Select(_layoutProxy);
            _layoutHistory.Bind(_layoutProxy);
            _controlProxy = new ControlDesignProxy(control);
            _controlPresenter.Select(_controlProxy);
            _controlHistory.Bind(_controlProxy);
            _selectionLabel.Text = BuildControlPath(control);
            SetMode(InspectorMode.Layout);
            UpdateActions();
        }

        private void SelectParent()
        {
            var parent = _selectedControl?.Parent;
            if (parent != null && (Control)parent != _overlay && parent != Editor.Windows.MainWindow?.GUI)
                SelectControl(parent);
        }

        private static string BuildControlPath(Control control)
        {
            var path = new List<string>(8);
            for (var current = control; current != null && path.Count < 8; current = current.Parent)
                path.Add(GetControlName(current));
            path.Reverse();
            return string.Join("  /  ", path);
        }

        private static string GetControlName(Control control)
        {
            var type = control.GetType();
            if (control is DockWindow dockWindow && !string.IsNullOrWhiteSpace(dockWindow.Title))
                return type.Name + " \"" + dockWindow.Title + "\"";
            try
            {
                var textProperty = type.GetProperty("Text", BindingFlags.Instance | BindingFlags.Public);
                if (textProperty?.PropertyType == typeof(string))
                {
                    var text = textProperty.GetValue(control) as string;
                    if (!string.IsNullOrWhiteSpace(text))
                    {
                        if (text.Length > 28)
                            text = text.Substring(0, 25) + "...";
                        return type.Name + " \"" + text + "\"";
                    }
                }
            }
            catch
            {
                // A display name should never prevent control selection.
            }
            return type.Name;
        }

        private void SetMode(InspectorMode mode)
        {
            _mode = mode;
            if (mode == InspectorMode.Theme)
                BindCurrentStyle();
            _layoutHost.Visible = mode == InspectorMode.Layout;
            _controlHost.Visible = mode == InspectorMode.Control;
            _themeHost.Visible = mode == InspectorMode.Theme;
            _layoutTab.BackgroundColor = mode == InspectorMode.Layout ? Style.Current.Selection : Color.Transparent;
            _controlTab.BackgroundColor = mode == InspectorMode.Control ? Style.Current.Selection : Color.Transparent;
            _themeTab.BackgroundColor = mode == InspectorMode.Theme ? Style.Current.Selection : Color.Transparent;
            _selectionLabel.Text = mode == InspectorMode.Theme
                ? "Global theme  /  Style.Current"
                : (_selectedControl != null ? BuildControlPath(_selectedControl) : "No control selected");
            _selectionLabel.TextColor = mode == InspectorMode.Theme ? Style.Current.Foreground : Style.Current.ForegroundGrey;
            UpdateActions();
        }

        private void BindCurrentStyle()
        {
            if (_selectedStyle == Style.Current)
                return;
            _selectedStyle = Style.Current;
            _themePresenter?.Select(_selectedStyle);
            _themeHistory.Bind(_selectedStyle);
        }

        private DesignHistory ActiveHistory => _mode == InspectorMode.Theme ? _themeHistory : (_mode == InspectorMode.Layout ? _layoutHistory : _controlHistory);

        private CustomEditorPresenter ActivePresenter => _mode == InspectorMode.Theme ? _themePresenter : (_mode == InspectorMode.Layout ? _layoutPresenter : _controlPresenter);

        private void OnLayoutModified()
        {
            RecordModification(_layoutHistory);
            _selectedControl?.Parent?.PerformLayout();
        }

        private void OnControlModified()
        {
            RecordModification(_controlHistory);
            _selectedControl?.Parent?.PerformLayout();
        }

        private void OnThemeModified()
        {
            RecordModification(_themeHistory);
        }

        private void RecordModification(DesignHistory history)
        {
            if (_applyingHistory)
                return;
            history.Record();
            UpdateActions();
        }

        private void Undo()
        {
            ApplyHistory(ActiveHistory.Undo);
        }

        private void Redo()
        {
            ApplyHistory(ActiveHistory.Redo);
        }

        private void Reset()
        {
            ApplyHistory(ActiveHistory.Reset);
        }

        private void ApplyHistory(Func<bool> action)
        {
            _applyingHistory = true;
            try
            {
                if (action())
                {
                    ActivePresenter.BuildLayout();
                    _selectedControl?.Parent?.PerformLayout();
                }
            }
            finally
            {
                _applyingHistory = false;
            }
            UpdateActions();
        }

        private void UpdateActions()
        {
            var history = ActiveHistory;
            _parentButton.Enabled = _mode != InspectorMode.Theme && _selectedControl?.Parent != null && _selectedControl.Parent != Editor.Windows.MainWindow?.GUI;
            _undoButton.Enabled = history.CanUndo;
            _redoButton.Enabled = history.CanRedo;
            _resetButton.Enabled = history.IsBound && history.IsModified;
        }

        private void LayoutControls()
        {
            if (_pickButton == null)
                return;

            float x = 4.0f;
            _pickButton.Bounds = new Rectangle(x, 4, 58, ButtonHeight);
            x += 62;
            _parentButton.Bounds = new Rectangle(x, 4, 58, ButtonHeight);
            x += 68;
            _layoutTab.Bounds = new Rectangle(x, 4, 60, ButtonHeight);
            x += 64;
            _controlTab.Bounds = new Rectangle(x, 4, 66, ButtonHeight);
            x += 70;
            _themeTab.Bounds = new Rectangle(x, 4, 60, ButtonHeight);

            bool showHistory = Width >= 520.0f;
            _undoButton.Visible = showHistory;
            _redoButton.Visible = showHistory;
            _resetButton.Visible = showHistory;
            if (showHistory)
            {
                float right = Width - 4.0f;
                _resetButton.Bounds = new Rectangle(right - 54, 4, 54, ButtonHeight);
                right -= 58;
                _redoButton.Bounds = new Rectangle(right - 50, 4, 50, ButtonHeight);
                right -= 54;
                _undoButton.Bounds = new Rectangle(right - 50, 4, 50, ButtonHeight);
            }

            _selectionLabel.Bounds = new Rectangle(8, 32, Mathf.Max(0, Width - 16), 22);
            float hostHeight = Mathf.Max(0, Height - HeaderHeight - FooterHeight);
            var hostBounds = new Rectangle(0, HeaderHeight, Width, hostHeight);
            _layoutHost.Bounds = hostBounds;
            _controlHost.Bounds = hostBounds;
            _themeHost.Bounds = hostBounds;
            _layoutPresenter.Panel.Width = Mathf.Max(0, Width - 14.0f);
            _controlPresenter.Panel.Width = Mathf.Max(0, Width - 14.0f);
            _themePresenter.Panel.Width = Mathf.Max(0, Width - 14.0f);
            _hintLabel.Bounds = new Rectangle(8, Height - FooterHeight, Mathf.Max(0, Width - 16), FooterHeight);
        }

        /// <inheritdoc />
        protected override void OnSizeChanged()
        {
            base.OnSizeChanged();
            LayoutControls();
        }

        /// <inheritdoc />
        protected override void OnVisibleChanged()
        {
            base.OnVisibleChanged();
            if (!Visible)
                _overlay?.Deactivate();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (_overlay != null && !_overlay.IsDisposing)
                _overlay.Dispose();
            _overlay = null;
            base.OnDestroy();
        }

        private enum InspectorMode
        {
            Layout,
            Control,
            Theme,
        }

        /// <summary>
        /// Exposes the common layout members that Control deliberately hides from normal property editors.
        /// One adapter works for every Control; individual widget interfaces are not required.
        /// </summary>
        private sealed class ControlLayoutProxy
        {
            private readonly Control _control;

            public ControlLayoutProxy(Control control)
            {
                _control = control;
            }

            [EditorDisplay("Bounds"), EditorOrder(10)]
            public float X
            {
                get => _control.X;
                set => _control.X = value;
            }

            [EditorDisplay("Bounds"), EditorOrder(20)]
            public float Y
            {
                get => _control.Y;
                set => _control.Y = value;
            }

            [EditorDisplay("Bounds"), EditorOrder(30), Limit(0.0f)]
            public float Width
            {
                get => _control.Width;
                set => _control.Width = value;
            }

            [EditorDisplay("Bounds"), EditorOrder(40), Limit(0.0f)]
            public float Height
            {
                get => _control.Height;
                set => _control.Height = value;
            }

            [EditorDisplay("Anchoring"), EditorOrder(100)]
            public AnchorPresets AnchorPreset
            {
                get => _control.AnchorPreset;
                set => _control.SetAnchorPreset(value, true);
            }

            [EditorDisplay("Anchoring"), EditorOrder(110), Limit(0.0f, 1.0f, 0.01f)]
            public Float2 AnchorMin
            {
                get => _control.AnchorMin;
                set => _control.AnchorMin = value;
            }

            [EditorDisplay("Anchoring"), EditorOrder(120), Limit(0.0f, 1.0f, 0.01f)]
            public Float2 AnchorMax
            {
                get => _control.AnchorMax;
                set => _control.AnchorMax = value;
            }

            [EditorDisplay("Anchoring"), EditorOrder(130)]
            public Margin Offsets
            {
                get => _control.Offsets;
                set => _control.Offsets = value;
            }

            [EditorDisplay("Transform"), EditorOrder(200)]
            public Float2 Scale
            {
                get => _control.Scale;
                set => _control.Scale = value;
            }

            [EditorDisplay("Transform"), EditorOrder(210), Limit(0.0f, 1.0f, 0.1f)]
            public Float2 Pivot
            {
                get => _control.Pivot;
                set => _control.Pivot = value;
            }

            [EditorDisplay("Transform"), EditorOrder(220)]
            public float Rotation
            {
                get => _control.Rotation;
                set => _control.Rotation = value;
            }

            public MemberInfo GetSourceMember(string name)
            {
                return typeof(Control).GetProperty(name, BindingFlags.Instance | BindingFlags.Public);
            }
        }

        /// <summary>
        /// A deliberately bounded view of a Control. Arbitrary editor controls can expose indexers,
        /// service objects, and other members that are unsafe to evaluate as property rows.
        /// </summary>
        private sealed class ControlDesignProxy
        {
            private readonly Control _control;
            private readonly MemberAccessor _title;
            private readonly MemberAccessor _text;
            private readonly MemberAccessor _textColor;
            private readonly MemberAccessor _borderColor;
            private readonly MemberAccessor _highlightColor;
            private readonly MemberAccessor _selectionColor;
            private readonly MemberAccessor _backgroundSelected;
            private readonly MemberAccessor _backgroundHighlighted;
            private readonly MemberAccessor _margin;

            public ControlDesignProxy(Control control)
            {
                _control = control;
                _title = MemberAccessor.Find<string>(control, "Title");
                _text = MemberAccessor.Find<string>(control, "Text");
                _textColor = MemberAccessor.Find<Color>(control, "TextColor", "ForegroundColor");
                _borderColor = MemberAccessor.Find<Color>(control, "BorderColor");
                _highlightColor = MemberAccessor.Find<Color>(control, "HighlightColor");
                _selectionColor = MemberAccessor.Find<Color>(control, "SelectionColor", "SelectedColor");
                _backgroundSelected = MemberAccessor.Find<Color>(control, "BackgroundColorSelected");
                _backgroundHighlighted = MemberAccessor.Find<Color>(control, "BackgroundColorHighlighted");
                _margin = MemberAccessor.Find<Margin>(control, "Margin");
            }

            [ShowInEditor, EditorDisplay("Identity"), EditorOrder(0)]
            public string ControlType => _control.GetType().FullName;

            [HideInEditor]
            public bool HasTitle => _title != null;

            [HideInEditor]
            public bool HasText => _text != null;

            [HideInEditor]
            public bool HasTextColor => _textColor != null;

            [HideInEditor]
            public bool HasBorderColor => _borderColor != null;

            [HideInEditor]
            public bool HasHighlightColor => _highlightColor != null;

            [HideInEditor]
            public bool HasSelectionColor => _selectionColor != null;

            [HideInEditor]
            public bool HasBackgroundSelected => _backgroundSelected != null;

            [HideInEditor]
            public bool HasBackgroundHighlighted => _backgroundHighlighted != null;

            [HideInEditor]
            public bool HasMargin => _margin != null;

            [HideInEditor]
            public bool HasClipChildren => _control is ContainerControl;

            [VisibleIf(nameof(HasTitle)), EditorDisplay("Content"), EditorOrder(10)]
            public string Title
            {
                get => _title?.Get<string>();
                set => _title?.Set(value);
            }

            [VisibleIf(nameof(HasText)), EditorDisplay("Content"), EditorOrder(20)]
            public string Text
            {
                get => _text?.Get<string>();
                set => _text?.Set(value);
            }

            [EditorDisplay("Content"), EditorOrder(30)]
            public string Tooltip
            {
                get => _control.TooltipText;
                set => _control.TooltipText = value;
            }

            [EditorDisplay("State"), EditorOrder(100)]
            public bool Visible
            {
                get => _control.Visible;
                set => _control.Visible = value;
            }

            [EditorDisplay("State"), EditorOrder(110)]
            public bool Enabled
            {
                get => _control.Enabled;
                set => _control.Enabled = value;
            }

            [EditorDisplay("State"), EditorOrder(120)]
            public bool AutoFocus
            {
                get => _control.AutoFocus;
                set => _control.AutoFocus = value;
            }

            [EditorDisplay("State"), EditorOrder(130)]
            public bool IsScrollable
            {
                get => _control.IsScrollable;
                set => _control.IsScrollable = value;
            }

            [VisibleIf(nameof(HasClipChildren)), EditorDisplay("State"), EditorOrder(140)]
            public bool ClipChildren
            {
                get => _control is ContainerControl container && container.ClipChildren;
                set
                {
                    if (_control is ContainerControl container)
                        container.ClipChildren = value;
                }
            }

            [EditorDisplay("Appearance"), EditorOrder(200)]
            public Color BackgroundColor
            {
                get => _control.BackgroundColor;
                set => _control.BackgroundColor = value;
            }

            [VisibleIf(nameof(HasTextColor)), EditorDisplay("Appearance"), EditorOrder(210)]
            public Color TextColor
            {
                get => _textColor?.Get<Color>() ?? Color.Transparent;
                set => _textColor?.Set(value);
            }

            [VisibleIf(nameof(HasBorderColor)), EditorDisplay("Appearance"), EditorOrder(220)]
            public Color BorderColor
            {
                get => _borderColor?.Get<Color>() ?? Color.Transparent;
                set => _borderColor?.Set(value);
            }

            [VisibleIf(nameof(HasHighlightColor)), EditorDisplay("Appearance"), EditorOrder(230)]
            public Color HighlightColor
            {
                get => _highlightColor?.Get<Color>() ?? Color.Transparent;
                set => _highlightColor?.Set(value);
            }

            [VisibleIf(nameof(HasSelectionColor)), EditorDisplay("Appearance"), EditorOrder(240)]
            public Color SelectionColor
            {
                get => _selectionColor?.Get<Color>() ?? Color.Transparent;
                set => _selectionColor?.Set(value);
            }

            [VisibleIf(nameof(HasBackgroundSelected)), EditorDisplay("Appearance"), EditorOrder(250)]
            public Color BackgroundSelected
            {
                get => _backgroundSelected?.Get<Color>() ?? Color.Transparent;
                set => _backgroundSelected?.Set(value);
            }

            [VisibleIf(nameof(HasBackgroundHighlighted)), EditorDisplay("Appearance"), EditorOrder(260)]
            public Color BackgroundHighlighted
            {
                get => _backgroundHighlighted?.Get<Color>() ?? Color.Transparent;
                set => _backgroundHighlighted?.Set(value);
            }

            [VisibleIf(nameof(HasMargin)), EditorDisplay("Spacing"), EditorOrder(300)]
            public Margin Margin
            {
                get => _margin?.Get<Margin>() ?? Margin.Zero;
                set => _margin?.Set(value);
            }

            public MemberInfo GetSourceMember(string name)
            {
                switch (name)
                {
                case nameof(ControlType):
                    return _control.GetType();
                case nameof(Title):
                    return _title?.Member;
                case nameof(Text):
                    return _text?.Member;
                case nameof(Tooltip):
                    return typeof(Control).GetProperty(nameof(Control.TooltipText));
                case nameof(Visible):
                    return typeof(Control).GetProperty(nameof(Control.Visible));
                case nameof(Enabled):
                    return typeof(Control).GetProperty(nameof(Control.Enabled));
                case nameof(AutoFocus):
                    return typeof(Control).GetProperty(nameof(Control.AutoFocus));
                case nameof(IsScrollable):
                    return typeof(Control).GetProperty(nameof(Control.IsScrollable));
                case nameof(ClipChildren):
                    return typeof(ContainerControl).GetProperty(nameof(ContainerControl.ClipChildren));
                case nameof(BackgroundColor):
                    return typeof(Control).GetProperty(nameof(Control.BackgroundColor));
                case nameof(TextColor):
                    return _textColor?.Member;
                case nameof(BorderColor):
                    return _borderColor?.Member;
                case nameof(HighlightColor):
                    return _highlightColor?.Member;
                case nameof(SelectionColor):
                    return _selectionColor?.Member;
                case nameof(BackgroundSelected):
                    return _backgroundSelected?.Member;
                case nameof(BackgroundHighlighted):
                    return _backgroundHighlighted?.Member;
                case nameof(Margin):
                    return _margin?.Member;
                default:
                    return null;
                }
            }

            private sealed class MemberAccessor
            {
                private readonly object _target;
                private readonly MemberInfo _member;

                private MemberAccessor(object target, MemberInfo member)
                {
                    _target = target;
                    _member = member;
                }

                public MemberInfo Member => _member;

                public static MemberAccessor Find<T>(object target, params string[] names)
                {
                    const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public;
                    var type = target.GetType();
                    foreach (var name in names)
                    {
                        var property = type.GetProperty(name, flags);
                        if (property?.PropertyType == typeof(T) && property.CanRead && property.CanWrite && property.GetIndexParameters().Length == 0)
                            return new MemberAccessor(target, property);
                        var field = type.GetField(name, flags);
                        if (field?.FieldType == typeof(T) && !field.IsInitOnly)
                            return new MemberAccessor(target, field);
                    }
                    return null;
                }

                public T Get<T>()
                {
                    try
                    {
                        if (_member is PropertyInfo property)
                            return (T)property.GetValue(_target);
                        if (_member is FieldInfo field)
                            return (T)field.GetValue(_target);
                    }
                    catch
                    {
                    }
                    return default;
                }

                public void Set<T>(T value)
                {
                    try
                    {
                        if (_member is PropertyInfo property)
                            property.SetValue(_target, value);
                        else if (_member is FieldInfo field)
                            field.SetValue(_target, value);
                    }
                    catch
                    {
                    }
                }
            }
        }

        /// <summary>
        /// Generic property editor with a compact declaration link on every generated property row.
        /// </summary>
        private sealed class SourceAwareGenericEditor : GenericEditor
        {
            private const float SourceButtonSize = 16.0f;
            private readonly Func<ItemInfo, MemberInfo> _sourceResolver;

            public SourceAwareGenericEditor(Func<ItemInfo, MemberInfo> sourceResolver)
            {
                _sourceResolver = sourceResolver;
            }

            protected override void SpawnProperty(LayoutElementsContainer itemLayout, ValueContainer itemValues, ItemInfo item)
            {
                int labelIndex = GetLabelIndex(itemLayout, item);
                base.SpawnProperty(itemLayout, itemValues, item);

                if (_sourceResolver == null || itemLayout.Children.Count == 0 ||
                    !(itemLayout.Children[itemLayout.Children.Count - 1] is PropertiesListElement properties) ||
                    labelIndex < 0 || labelIndex >= properties.Labels.Count)
                {
                    return;
                }

                var member = _sourceResolver(item);
                if (member == null)
                    return;

                var style = FlaxEngine.GUI.Style.Current;
                var button = new Button
                {
                    Parent = properties.Labels[labelIndex],
                    AnchorPreset = AnchorPresets.MiddleRight,
                    Offsets = new Margin(-SourceButtonSize - 2.0f, SourceButtonSize, SourceButtonSize * -0.5f, SourceButtonSize),
                    BackgroundBrush = new SpriteBrush(Editor.Instance.Icons.Code64),
                    BackgroundColor = style.ForegroundGrey,
                    BackgroundColorHighlighted = style.Foreground,
                    BackgroundColorSelected = style.BackgroundSelected,
                    BorderColor = Color.Transparent,
                    BorderColorHighlighted = Color.Transparent,
                    BorderColorSelected = Color.Transparent,
                    HasBorder = false,
                    AutoFocus = false,
                    IsScrollable = false,
                    TooltipText = "Open this declaration in the configured code editor",
                };
                button.Clicked += () => MemberSourceNavigator.Open(member);
            }
        }

        private sealed class DesignHistory
        {
            private const int Capacity = 100;

            private readonly Stack<DesignSnapshot> _undo = new Stack<DesignSnapshot>();
            private readonly Stack<DesignSnapshot> _redo = new Stack<DesignSnapshot>();
            private object _target;
            private DesignSnapshot _baseline;
            private DesignSnapshot _current;

            public bool IsBound => _target != null;
            public bool CanUndo => _undo.Count != 0;
            public bool CanRedo => _redo.Count != 0;
            public bool IsModified => IsBound && !_current.Matches(_baseline);

            public void Bind(object target)
            {
                _target = target;
                _undo.Clear();
                _redo.Clear();
                _baseline = DesignSnapshot.Capture(target);
                _current = _baseline;
            }

            public void Record()
            {
                if (_target == null)
                    return;
                var next = DesignSnapshot.Capture(_target);
                if (next.Matches(_current))
                    return;
                _undo.Push(_current);
                Trim(_undo);
                _current = next;
                _redo.Clear();
            }

            public bool Undo()
            {
                if (!CanUndo)
                    return false;
                _redo.Push(_current);
                _current = _undo.Pop();
                _current.Apply(_target);
                return true;
            }

            public bool Redo()
            {
                if (!CanRedo)
                    return false;
                _undo.Push(_current);
                _current = _redo.Pop();
                _current.Apply(_target);
                return true;
            }

            public bool Reset()
            {
                if (!IsModified)
                    return false;
                _undo.Push(_current);
                Trim(_undo);
                _redo.Clear();
                _current = _baseline;
                _current.Apply(_target);
                return true;
            }

            private static void Trim(Stack<DesignSnapshot> stack)
            {
                if (stack.Count <= Capacity)
                    return;
                var values = stack.ToArray();
                stack.Clear();
                for (int i = Capacity - 1; i >= 0; i--)
                    stack.Push(values[i]);
            }
        }

        private sealed class DesignSnapshot
        {
            private readonly List<Entry> _entries;

            private DesignSnapshot(List<Entry> entries)
            {
                _entries = entries;
            }

            public static DesignSnapshot Capture(object target)
            {
                var entries = new List<Entry>();
                if (target == null)
                    return new DesignSnapshot(entries);

                const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public;
                var type = target.GetType();
                foreach (var field in type.GetFields(flags))
                {
                    if (field.IsInitOnly || !CanStore(field.FieldType))
                        continue;
                    try
                    {
                        entries.Add(new Entry(field, field.GetValue(target)));
                    }
                    catch
                    {
                    }
                }
                foreach (var property in type.GetProperties(flags))
                {
                    if (!property.CanRead || !property.CanWrite || property.GetIndexParameters().Length != 0 || !CanStore(property.PropertyType))
                        continue;
                    try
                    {
                        entries.Add(new Entry(property, property.GetValue(target)));
                    }
                    catch
                    {
                    }
                }
                return new DesignSnapshot(entries);
            }

            private static bool CanStore(Type type)
            {
                return type.IsValueType || type.IsEnum || type == typeof(string);
            }

            public bool Matches(DesignSnapshot other)
            {
                if (other == null || _entries.Count != other._entries.Count)
                    return false;
                for (int i = 0; i < _entries.Count; i++)
                {
                    var a = _entries[i];
                    var b = other._entries[i];
                    if (a.Member != b.Member || !Equals(a.Value, b.Value))
                        return false;
                }
                return true;
            }

            public void Apply(object target)
            {
                if (target == null)
                    return;
                foreach (var entry in _entries)
                {
                    try
                    {
                        if (entry.Member is FieldInfo field)
                            field.SetValue(target, entry.Value);
                        else if (entry.Member is PropertyInfo property)
                            property.SetValue(target, entry.Value);
                    }
                    catch
                    {
                        // Some runtime properties can become temporarily read-only. Apply the rest.
                    }
                }
            }

            private readonly struct Entry
            {
                public readonly MemberInfo Member;
                public readonly object Value;

                public Entry(MemberInfo member, object value)
                {
                    Member = member;
                    Value = value;
                }
            }
        }

        private sealed class UIInspectorOverlay : Control
        {
            private readonly ContainerControl _root;
            private readonly Action<Control> _selected;
            private readonly Action _ended;
            private InspectorHit _hovered;

            public UIInspectorOverlay(ContainerControl root, Action<Control> selected, Action ended)
            {
                _root = root;
                _selected = selected;
                _ended = ended;
                Parent = root;
                AnchorPreset = AnchorPresets.StretchAll;
                Offsets = Margin.Zero;
                Visible = false;
                AutoFocus = true;
            }

            public void Activate()
            {
                IndexInParent = _root.ChildrenCount - 1;
                Visible = true;
                Enabled = true;
                Focus();
            }

            public void Deactivate()
            {
                if (!Visible)
                    return;
                Visible = false;
                _hovered = null;
                _ended?.Invoke();
            }

            public override void OnMouseMove(Float2 location)
            {
                _hovered = FindHit(_root, PointToScreen(location));
                base.OnMouseMove(location);
            }

            public override void OnMouseEnter(Float2 location)
            {
                _hovered = FindHit(_root, PointToScreen(location));
                base.OnMouseEnter(location);
            }

            public override bool OnMouseDown(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left)
                {
                    _hovered = FindHit(_root, PointToScreen(location));
                    var result = _hovered?.Control;
                    Deactivate();
                    if (result != null)
                        _selected?.Invoke(result);
                    return true;
                }
                if (button == MouseButton.Right)
                {
                    Deactivate();
                    return true;
                }
                return true;
            }

            public override bool OnKeyDown(KeyboardKeys key)
            {
                if (key == KeyboardKeys.Escape)
                {
                    Deactivate();
                    return true;
                }
                return true;
            }

            public override void Draw()
            {
                Render2D.FillRectangle(new Rectangle(Float2.Zero, Size), Color.Black.AlphaMultiplied(0.035f));

                var style = Style.Current;
                if (_hovered?.Control != null && !_hovered.Control.IsDisposing)
                {
                    var min = PointFromScreen(_hovered.ScreenBounds.Location);
                    var max = PointFromScreen(_hovered.ScreenBounds.BottomRight);
                    var bounds = new Rectangle(
                        Mathf.Min(min.X, max.X),
                        Mathf.Min(min.Y, max.Y),
                        Mathf.Abs(max.X - min.X),
                        Mathf.Abs(max.Y - min.Y));
                    Render2D.FillRectangle(bounds, style.Selection.AlphaMultiplied(0.16f));
                    Render2D.DrawRectangle(bounds, style.SelectionBorder, 1.5f);
                }

                var banner = new Rectangle(8, 8, Mathf.Min(460.0f, Mathf.Max(0, Width - 16.0f)), 26);
                StyleRendering.DrawRoundedRectangle(banner, style.Background.AlphaMultiplied(0.96f), style.BorderNormal, 1.0f, style.CornerRadius);
                string name = _hovered?.Label ?? "No control";
                Render2D.DrawText(style.FontSmall, "UI PICKER  |  " + name + "  |  click to select, Esc/right-click to cancel", banner, style.Foreground, TextAlignment.Center, TextAlignment.Center);
            }

            private InspectorHit FindHit(ContainerControl parent, Float2 screenPosition)
            {
                for (int i = parent.ChildrenCount - 1; i >= 0; i--)
                {
                    var child = parent.Children[i];
                    if (child == this || child == null || child.IsDisposing || !child.Visible)
                        continue;

                    var local = child.PointFromScreen(screenPosition);
                    if (!child.ContainsPoint(ref local, false))
                        continue;

                    // Dock headers are drawn virtually by DockPanelProxy. Map the header rectangle
                    // back to the DockWindow so every visible tab can be picked independently.
                    if (child is DockPanelProxy dockProxy && dockProxy.TryGetTabAtPosition(local, out var dockTab, out var tabBounds))
                    {
                        return new InspectorHit(
                            dockTab,
                            ToScreenBounds(dockProxy, tabBounds),
                            "Dock tab \"" + dockTab.Title + "\"");
                    }

                    if (child is ContainerControl container)
                    {
                        var nested = FindHit(container, screenPosition);
                        if (nested != null)
                            return nested;
                    }
                    return new InspectorHit(child, GetScreenBounds(child), GetControlName(child));
                }
                return null;
            }

            private static Rectangle GetScreenBounds(Control control)
            {
                return ToScreenBounds(control, new Rectangle(Float2.Zero, control.Size));
            }

            private static Rectangle ToScreenBounds(Control control, Rectangle localBounds)
            {
                var min = control.PointToScreen(localBounds.Location);
                var max = control.PointToScreen(localBounds.BottomRight);
                return new Rectangle(
                    Mathf.Min(min.X, max.X),
                    Mathf.Min(min.Y, max.Y),
                    Mathf.Abs(max.X - min.X),
                    Mathf.Abs(max.Y - min.Y));
            }

            private sealed class InspectorHit
            {
                public readonly Control Control;
                public readonly Rectangle ScreenBounds;
                public readonly string Label;

                public InspectorHit(Control control, Rectangle screenBounds, string label)
                {
                    Control = control;
                    ScreenBounds = screenBounds;
                    Label = label;
                }
            }
        }
    }
}
