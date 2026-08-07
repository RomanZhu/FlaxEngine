// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Windows;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Horizontal placement zone for an editor tool strip item.
    /// </summary>
    public enum ToolStripAnchor
    {
        /// <summary>
        /// Places the item against the left edge.
        /// </summary>
        Left,

        /// <summary>
        /// Places the item in the centered command group.
        /// </summary>
        Center,

        /// <summary>
        /// Places the item against the right edge.
        /// </summary>
        Right,
    }

    /// <summary>
    /// Tool strip with child items.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.ContainerControl" />
    public class ToolStrip : ContainerControl
    {
        /// <summary>
        /// Standard compact toolstrip height used by viewport-style overlays.
        /// </summary>
        public const float CompactToolStripHeight = 28.0f;

        /// <summary>
        /// Standard vertical padding for compact header toolstrips with search fields.
        /// </summary>
        public const float CompactHeaderPadding = 5.0f;

        private Margin _itemsMargin;
        private readonly List<Control>[] _anchorItems =
        {
            new List<Control>(),
            new List<Control>(),
            new List<Control>(),
        };
        private readonly Dictionary<Control, string> _itemIds = new Dictionary<Control, string>();
        private readonly Dictionary<Control, ItemGroup> _itemGroups = new Dictionary<Control, ItemGroup>();
        private readonly Dictionary<string, SavedPlacement> _savedPlacements = new Dictionary<string, SavedPlacement>();
        private readonly HashSet<string> _hiddenItemIds = new HashSet<string>();
        private Control _draggedItem;
        private ToolStripAnchor _dragTargetAnchor;
        private int _dragTargetIndex = -1;
        private float _dragPreviewX;
        private ToolStripButton _selectedMenuButton;

        private struct SavedPlacement
        {
            public ToolStripAnchor Anchor;
            public int Index;
        }

        private sealed class ItemGroup
        {
            public readonly Control[] Items;

            public ItemGroup(Control[] items)
            {
                Items = items;
            }
        }

        /// <summary>
        /// Event fired when button gets clicked with the primary mouse button.
        /// </summary>
        public Action<ToolStripButton> ButtonClicked;

        /// <summary>
        /// Event fired when button gets clicked with the secondary mouse button.
        /// </summary>
        public Action<ToolStripButton> SecondaryButtonClicked;

        /// <summary>
        /// Event fired when the strip background is clicked with the secondary mouse button.
        /// </summary>
        public Action<Float2> SecondaryClicked;

        /// <summary>
        /// Event fired after the user rearranges items with Ctrl+drag.
        /// </summary>
        public Action LayoutChanged;

        /// <summary>
        /// True if this strip should open and switch dropdown menus like the main editor menu.
        /// </summary>
        public bool UseMenuSelection;

        /// <summary>
        /// True if right-click opens item visibility customization instead of the item's normal context menu.
        /// </summary>
        public bool UseItemContextMenu;

        /// <summary>
        /// True if use the viewport overlay visual style.
        /// </summary>
        public bool UseOverlayStyle;

        /// <summary>
        /// True if the strip should draw rounded background frames behind anchored item groups.
        /// </summary>
        public bool UseGroupFrames;

        /// <summary>
        /// True if the strip should apply compact sizing to newly added toolbar buttons.
        /// </summary>
        public bool UseCompactButtonStyle;

        /// <summary>
        /// The viewport overlay background color.
        /// </summary>
        public Color OverlayBackgroundColor = new Color(0.06f, 0.06f, 0.06f, 0.5f);

        /// <summary>
        /// Gets or sets the selected menu button.
        /// </summary>
        public ToolStripButton SelectedMenuButton
        {
            get => _selectedMenuButton;
            set
            {
                if (_selectedMenuButton == value)
                    return;

                if (_selectedMenuButton != null && _selectedMenuButton.ContextMenu != null)
                {
                    _selectedMenuButton.ContextMenu.VisibleChanged -= OnSelectedMenuVisibleChanged;
                    _selectedMenuButton.ContextMenu.Hide();
                }

                _selectedMenuButton = value;

                if (_selectedMenuButton != null && _selectedMenuButton.ContextMenu != null)
                {
                    _selectedMenuButton.ContextMenu.Show(_selectedMenuButton, new Float2(0, _selectedMenuButton.Height));
                    _selectedMenuButton.ContextMenu.VisibleChanged += OnSelectedMenuVisibleChanged;
                }
            }
        }

        /// <summary>
        /// Tries to get the last button.
        /// </summary>
        public ToolStripButton LastButton
        {
            get
            {
                for (int i = _children.Count - 1; i >= 0; i--)
                {
                    if (_children[i] is ToolStripButton button)
                        return button;
                }
                return null;
            }
        }

        /// <summary>
        /// Gets amount of buttons that has been added
        /// </summary>
        public int ButtonsCount
        {
            get
            {
                int result = 0;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is ToolStripButton)
                        result++;
                }
                return result;
            }
        }

        /// <summary>
        /// Gets or sets the space around items.
        /// </summary>
        public Margin ItemsMargin
        {
            get => _itemsMargin;
            set
            {
                if (_itemsMargin != value)
                {
                    _itemsMargin = value;
                    PerformLayout();
                }
            }
        }

        /// <summary>
        /// Gets the height for the items.
        /// </summary>
        public float ItemsHeight => Height - _itemsMargin.Height;

        /// <summary>
        /// Gets the standard item margin for compact header toolstrips.
        /// </summary>
        public static Margin CompactHeaderItemsMargin => new Margin(2, 2, CompactHeaderPadding, CompactHeaderPadding);

        /// <summary>
        /// Gets the standard compact header toolstrip height for a given item height.
        /// </summary>
        /// <param name="itemHeight">The desired control/item height.</param>
        public static float GetCompactHeaderHeight(float itemHeight)
        {
            return itemHeight + CompactHeaderPadding * 2.0f;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ToolStrip"/> class.
        /// </summary>
        /// <param name="height">The toolstrip height.</param>
        /// <param name="y">The toolstrip Y position.</param>
        public ToolStrip(float height = 32.0f, float y = 0)
        {
            AutoFocus = false;
            AnchorPreset = AnchorPresets.HorizontalStretchTop;
            BackgroundColor = Style.Current.SecondaryBackground;
            Offsets = new Margin(0, 0, y, height * Editor.Instance.Options.Options.Interface.IconsScale);
            _itemsMargin = new Margin(2, 2, 1, 1);
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="sprite">The icon sprite.</param>
        /// <param name="onClick">The custom action to call on button clicked.</param>
        /// <returns>The button.</returns>
        public ToolStripButton AddButton(SpriteHandle sprite, Action onClick = null)
        {
            var button = new ToolStripButton(ItemsHeight, ref sprite)
            {
                Parent = this,
                UseBlueCheckedStyle = UseOverlayStyle,
            };
            if (UseCompactButtonStyle)
                button.SetCompactStyle();
            SetItemPlacement(button, ToolStripAnchor.Left);
            if (onClick != null)
                button.Clicked += onClick;
            return button;
        }

        /// <summary>
        /// Adds a button to a specific placement zone.
        /// </summary>
        /// <param name="sprite">The icon sprite.</param>
        /// <param name="anchor">The placement zone.</param>
        /// <param name="id">Stable item identifier used by saved layouts.</param>
        /// <param name="onClick">The custom action to call on button clicked.</param>
        /// <returns>The button.</returns>
        public ToolStripButton AddButton(SpriteHandle sprite, ToolStripAnchor anchor, string id, Action onClick = null)
        {
            var button = AddButton(sprite, onClick);
            SetItemPlacement(button, anchor, -1, id);
            return button;
        }

        /// <summary>
        /// Adds a compact solid-glyph button to a specific placement zone.
        /// </summary>
        public ToolStripButton AddGlyphButton(ToolStripGlyph glyph, ToolStripAnchor anchor, string id, Action onClick = null)
        {
            var button = new ToolStripButton(ItemsHeight, ref SpriteHandle.Invalid)
            {
                Glyph = glyph,
                Parent = this,
                UseBlueCheckedStyle = UseOverlayStyle,
            };
            if (UseCompactButtonStyle)
                button.SetCompactStyle();
            SetItemPlacement(button, anchor, -1, id);
            if (onClick != null)
                button.Clicked += onClick;
            return button;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="sprite">The icon sprite.</param>
        /// <param name="text">The text.</param>
        /// <param name="onClick">The custom action to call on button clicked.</param>
        /// <returns>The button.</returns>
        public ToolStripButton AddButton(SpriteHandle sprite, string text, Action onClick = null)
        {
            var button = new ToolStripButton(ItemsHeight, ref sprite)
            {
                Text = text,
                Parent = this,
                UseBlueCheckedStyle = UseOverlayStyle,
            };
            if (UseCompactButtonStyle)
                button.SetCompactStyle();
            SetItemPlacement(button, ToolStripAnchor.Left);
            if (onClick != null)
                button.Clicked += onClick;
            return button;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="onClick">The custom action to call on button clicked.</param>
        /// <returns>The button.</returns>
        public ToolStripButton AddButton(string text, Action onClick = null)
        {
            var button = new ToolStripButton(ItemsHeight, ref SpriteHandle.Invalid)
            {
                Text = text,
                Parent = this,
                UseBlueCheckedStyle = UseOverlayStyle,
            };
            if (UseCompactButtonStyle)
                button.SetCompactStyle();
            SetItemPlacement(button, ToolStripAnchor.Left);
            if (onClick != null)
                button.Clicked += onClick;
            return button;
        }

        /// <summary>
        /// Adds the separator.
        /// </summary>
        /// <returns>The separator.</returns>
        public ToolStripSeparator AddSeparator()
        {
            var separator = AddChild(new ToolStripSeparator(ItemsHeight));
            SetItemPlacement(separator, ToolStripAnchor.Left);
            return separator;
        }

        /// <summary>
        /// Adds any control to a placement zone. Game editor extensions can use this method to
        /// contribute custom controls without depending on the built-in toolbar composition.
        /// </summary>
        /// <typeparam name="T">Control type.</typeparam>
        /// <param name="control">Control to add.</param>
        /// <param name="anchor">The placement zone.</param>
        /// <param name="id">Stable item identifier used by saved layouts.</param>
        /// <param name="index">Optional insertion index within the zone.</param>
        /// <returns>The added control.</returns>
        public T AddItem<T>(T control, ToolStripAnchor anchor, string id = null, int index = -1)
            where T : Control
        {
            if (control == null)
                throw new ArgumentNullException(nameof(control));
            control.Parent = this;
            SetItemPlacement(control, anchor, index, id);
            return control;
        }

        /// <summary>
        /// Changes an item's anchor or order within the tool strip.
        /// </summary>
        /// <param name="control">The item.</param>
        /// <param name="anchor">The target placement zone.</param>
        /// <param name="index">Optional insertion index within the zone.</param>
        /// <param name="id">Optional stable item identifier used by saved layouts.</param>
        public void SetItemPlacement(Control control, ToolStripAnchor anchor, int index = -1, string id = null)
        {
            if (control == null || control.Parent != this)
                throw new ArgumentException("Tool strip items must be children of this tool strip.", nameof(control));

            RemoveFromPlacement(control);
            if (!string.IsNullOrEmpty(id))
            {
                _itemIds[control] = id;
                control.Visible = !_hiddenItemIds.Contains(id);
                control.Enabled = control.Visible;
                if (_savedPlacements.TryGetValue(id, out var saved))
                {
                    anchor = saved.Anchor;
                    index = saved.Index;
                }
            }
            var items = _anchorItems[(int)anchor];
            if (index < 0 || index > items.Count)
                index = items.Count;
            items.Insert(index, control);
            PerformLayout();
        }

        /// <summary>
        /// Groups items into one fixed-order layout and drag unit.
        /// </summary>
        /// <param name="controls">Items in their fixed display order.</param>
        public void SetItemGroup(params Control[] controls)
        {
            if (controls == null || controls.Length < 2)
                throw new ArgumentException("Tool strip groups require at least two items.", nameof(controls));

            for (int i = 0; i < controls.Length; i++)
            {
                if (controls[i] == null || controls[i].Parent != this)
                    throw new ArgumentException("Tool strip group items must be children of this tool strip.", nameof(controls));
                if (Array.IndexOf(controls, controls[i]) != i)
                    throw new ArgumentException("Tool strip group items must be unique.", nameof(controls));
            }

            var anchor = GetItemAnchor(controls[0]);
            var anchorItems = _anchorItems[(int)anchor];
            var firstIndex = anchorItems.IndexOf(controls[0]);
            var insertIndex = 0;
            for (int i = 0; i < firstIndex; i++)
            {
                if (Array.IndexOf(controls, anchorItems[i]) == -1)
                    insertIndex++;
            }

            for (int i = 0; i < controls.Length; i++)
            {
                if (_itemGroups.TryGetValue(controls[i], out var oldGroup))
                {
                    for (int j = 0; j < oldGroup.Items.Length; j++)
                        _itemGroups.Remove(oldGroup.Items[j]);
                }
                RemoveFromPlacement(controls[i]);
            }

            var group = new ItemGroup((Control[])controls.Clone());
            for (int i = 0; i < group.Items.Length; i++)
            {
                _itemGroups[group.Items[i]] = group;
                anchorItems.Insert(insertIndex + i, group.Items[i]);
            }
            PerformLayout();
        }

        /// <summary>
        /// Captures stable item identifiers and their current anchor/order.
        /// </summary>
        /// <returns>A compact toolbar layout string.</returns>
        public string CaptureLayout()
        {
            var entries = new List<string>();
            for (int anchor = 0; anchor < _anchorItems.Length; anchor++)
            {
                var items = _anchorItems[anchor];
                for (int index = 0; index < items.Count; index++)
                {
                    if (_itemIds.TryGetValue(items[index], out var id) && !string.IsNullOrEmpty(id))
                        entries.Add(id + "@" + anchor + "@" + index + (items[index].Visible ? string.Empty : "@h"));
                }
            }
            return string.Join("|", entries);
        }

        /// <summary>
        /// Applies a previously captured layout. Placements for extension items that have not
        /// been registered yet are retained and applied when those items are added.
        /// </summary>
        /// <param name="layout">The layout string.</param>
        public void ApplyLayout(string layout)
        {
            _savedPlacements.Clear();
            _hiddenItemIds.Clear();
            if (!string.IsNullOrEmpty(layout))
            {
                var entries = layout.Split('|');
                for (int i = 0; i < entries.Length; i++)
                {
                    var parts = entries[i].Split('@');
                    if (parts.Length >= 3 && int.TryParse(parts[1], out var anchor) && int.TryParse(parts[2], out var index) && anchor >= 0 && anchor < _anchorItems.Length)
                    {
                        _savedPlacements[parts[0]] = new SavedPlacement
                        {
                            Anchor = (ToolStripAnchor)anchor,
                            Index = Mathf.Max(0, index),
                        };
                        if (parts.Length >= 4 && parts[3] == "h")
                            _hiddenItemIds.Add(parts[0]);
                    }
                }
            }

            var controls = new List<Control>(_itemIds.Keys);
            for (int i = 0; i < controls.Count; i++)
            {
                var control = controls[i];
                var id = _itemIds[control];
                control.Visible = !_hiddenItemIds.Contains(id);
                control.Enabled = control.Visible;
                if (_savedPlacements.TryGetValue(id, out var saved))
                    SetItemPlacement(control, saved.Anchor, saved.Index, id);
            }
            PerformLayout();
        }

        /// <summary>
        /// Returns true if the saved layout contains state for the given item id.
        /// </summary>
        public bool HasSavedState(string id)
        {
            return _savedPlacements.ContainsKey(id) || _hiddenItemIds.Contains(id);
        }

        /// <summary>
        /// Sets item visibility and stores it in captured layout.
        /// </summary>
        public void SetItemVisible(Control control, bool visible, bool notify = true)
        {
            if (control == null || !_itemIds.TryGetValue(control, out var id))
                return;

            control.Visible = visible;
            control.Enabled = visible;
            if (visible)
                _hiddenItemIds.Remove(id);
            else
                _hiddenItemIds.Add(id);
            if (_selectedMenuButton == control)
                SelectedMenuButton = null;
            PerformLayout();
            if (notify)
                LayoutChanged?.Invoke();
        }

        /// <summary>
        /// Gets the current placement zone for an item.
        /// </summary>
        public ToolStripAnchor GetItemAnchor(Control control)
        {
            for (int i = 0; i < _anchorItems.Length; i++)
            {
                if (_anchorItems[i].Contains(control))
                    return (ToolStripAnchor)i;
            }
            return ToolStripAnchor.Left;
        }

        private void RemoveFromPlacement(Control control)
        {
            for (int i = 0; i < _anchorItems.Length; i++)
                _anchorItems[i].Remove(control);
        }

        private void RegisterUntrackedChildren()
        {
            var left = _anchorItems[(int)ToolStripAnchor.Left];
            for (int i = 0; i < _children.Count; i++)
            {
                var child = _children[i];
                bool tracked = false;
                for (int anchor = 0; anchor < _anchorItems.Length && !tracked; anchor++)
                    tracked = _anchorItems[anchor].Contains(child);
                if (!tracked)
                    left.Add(child);
            }

            for (int anchor = 0; anchor < _anchorItems.Length; anchor++)
                _anchorItems[anchor].RemoveAll(x => x.Parent != this);
        }

        internal void OnButtonClicked(ToolStripButton button)
        {
            ButtonClicked?.Invoke(button);
        }

        internal void OnSecondaryButtonClicked(ToolStripButton button)
        {
            SecondaryButtonClicked?.Invoke(button);
        }

        internal void ShowItemContextMenu(ToolStripButton button, Float2 location)
        {
            var menu = new ContextMenu.ContextMenu
            {
                MinimumWidth = 190,
            };
            if (button != null && _itemIds.ContainsKey(button))
            {
                var remove = menu.AddButton("Remove from Toolstrip", () => SetItemVisible(button, false));
                remove.Enabled = CountVisibleItems() > 1;
                menu.AddSeparator();
            }

            var addMenu = menu.AddChildMenu("Add").ContextMenu;
            bool hasHidden = false;
            foreach (var pair in _itemIds)
            {
                var control = pair.Key;
                if (control.Visible)
                    continue;
                hasHidden = true;
                addMenu.AddButton(GetCustomizationLabel(control, pair.Value), () => SetItemVisible(control, true));
            }
            if (!hasHidden)
                addMenu.AddButton("No hidden items").Enabled = false;

            menu.AddButton("Show All", ShowAllItems).Enabled = _hiddenItemIds.Count != 0;
            menu.Show((Control)button ?? this, location);
        }

        private int CountVisibleItems()
        {
            int result = 0;
            foreach (var pair in _itemIds)
            {
                if (pair.Key.Visible)
                    result++;
            }
            return result;
        }

        private void ShowAllItems()
        {
            var controls = new List<Control>(_itemIds.Keys);
            for (int i = 0; i < controls.Count; i++)
            {
                controls[i].Visible = true;
                controls[i].Enabled = true;
            }
            _hiddenItemIds.Clear();
            PerformLayout();
            LayoutChanged?.Invoke();
        }

        private static string GetCustomizationLabel(Control control, string id)
        {
            if (control is ToolStripButton button)
            {
                if (!string.IsNullOrEmpty(button.CustomizationLabel))
                    return button.CustomizationLabel;
                if (!string.IsNullOrEmpty(button.Text))
                    return button.Text;
            }

            int lastDot = id.LastIndexOf('.');
            return lastDot != -1 && lastDot + 1 < id.Length ? id.Substring(lastDot + 1) : id;
        }

        private void OnSelectedMenuVisibleChanged(Control control)
        {
            if (_selectedMenuButton != null && _selectedMenuButton.ContextMenu == control && !control.Visible)
                SelectedMenuButton = null;
        }

        /// <inheritdoc />
        protected override void PerformLayoutBeforeChildren()
        {
            RegisterUntrackedChildren();
            float h = ItemsHeight;
            float leftWidth = MeasureGroup(_anchorItems[(int)ToolStripAnchor.Left]);
            float centerWidth = MeasureGroup(_anchorItems[(int)ToolStripAnchor.Center]);
            float rightWidth = MeasureGroup(_anchorItems[(int)ToolStripAnchor.Right]);
            float leftX = _itemsMargin.Left;
            float rightX = Width - _itemsMargin.Right - rightWidth;
            float minimumCenterX = leftX + leftWidth + _itemsMargin.Width;
            float maximumCenterX = rightX - centerWidth - _itemsMargin.Width;
            float centerX = (Width - centerWidth) * 0.5f;
            if (maximumCenterX >= minimumCenterX)
                centerX = Mathf.Clamp(centerX, minimumCenterX, maximumCenterX);
            else
                centerX = Mathf.Max(_itemsMargin.Left, Mathf.Min(centerX, Width - _itemsMargin.Right - centerWidth));

            LayoutGroup(_anchorItems[(int)ToolStripAnchor.Left], leftX, h);
            LayoutGroup(_anchorItems[(int)ToolStripAnchor.Center], centerX, h);
            LayoutGroup(_anchorItems[(int)ToolStripAnchor.Right], rightX, h);
        }

        private float MeasureGroup(List<Control> items)
        {
            float width = 0.0f;
            Control previous = null;
            for (int i = 0; i < items.Count; i++)
            {
                if (!items[i].Visible)
                    continue;
                if (previous != null && !AreGrouped(previous, items[i]))
                    width += _itemsMargin.Width;
                width += items[i].Width;
                previous = items[i];
            }
            return width;
        }

        private void LayoutGroup(List<Control> items, float x, float height)
        {
            Control previous = null;
            for (int i = 0; i < items.Count; i++)
            {
                var control = items[i];
                if (!control.Visible)
                    continue;
                if (previous != null && !AreGrouped(previous, control))
                    x += _itemsMargin.Width;
                control.Bounds = new Rectangle(x, _itemsMargin.Top, control.Width, height);
                x += control.Width;
                previous = control;
            }
        }

        private bool AreGrouped(Control first, Control second)
        {
            return _itemGroups.TryGetValue(first, out var firstGroup) &&
                   _itemGroups.TryGetValue(second, out var secondGroup) &&
                   ReferenceEquals(firstGroup, secondGroup);
        }

        /// <inheritdoc />
        public override void DrawSelf()
        {
            if (UseOverlayStyle)
            {
                Render2D.FillRectangle(new Rectangle(Float2.Zero, Size), OverlayBackgroundColor);
                Render2D.FillRectangle(new Rectangle(0.0f, Height - 1.0f, Width, 1.0f), Color.White.AlphaMultiplied(0.08f));
                return;
            }

            base.DrawSelf();
            if (!UseGroupFrames)
                return;

            var style = Style.Current;
            var drawnGroups = new HashSet<ItemGroup>();
            for (int anchor = 0; anchor < _anchorItems.Length; anchor++)
            {
                var items = _anchorItems[anchor];
                for (int i = 0; i < items.Count; i++)
                {
                    var item = items[i];
                    if (!item.Visible)
                        continue;
                    var first = item;
                    var last = item;
                    if (_itemGroups.TryGetValue(item, out var group))
                    {
                        if (!drawnGroups.Add(group))
                            continue;
                        for (int j = 0; j < group.Items.Length; j++)
                        {
                            if (!group.Items[j].Visible)
                                continue;
                            if (group.Items[j].Left < first.Left)
                                first = group.Items[j];
                            if (group.Items[j].Right > last.Right)
                                last = group.Items[j];
                        }
                    }
                    var frame = new Rectangle(first.Left - 2.0f, first.Top - 1.0f, last.Right - first.Left + 4.0f, first.Height + 2.0f);
                    StyleRendering.DrawRoundedRectangle(frame, style.BackgroundNormal, style.BorderNormal.AlphaMultiplied(0.72f), 1.0f, style.GetToolStripGroupCornerRadius());
                }
            }
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (base.OnMouseDown(location, button))
                return true;

            if (button == MouseButton.Right && (UseItemContextMenu || SecondaryClicked != null))
            {
                Focus();
                return true;
            }

            return false;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (base.OnMouseUp(location, button))
                return true;

            if (button == MouseButton.Right && UseItemContextMenu)
            {
                ShowItemContextMenu(null, location);
                return true;
            }
            if (button == MouseButton.Right && SecondaryClicked != null)
            {
                SecondaryClicked(location);
                return true;
            }

            return false;
        }

        internal void UpdateItemDrag(Control control, Float2 location)
        {
            if (control == null || control.Parent != this)
                return;

            _draggedItem = control;
            var draggedItems = GetItemUnit(control);
            float normalizedX = Width > 0.0f ? location.X / Width : 0.5f;
            _dragTargetAnchor = normalizedX < 0.33f ? ToolStripAnchor.Left : normalizedX > 0.67f ? ToolStripAnchor.Right : ToolStripAnchor.Center;
            var items = _anchorItems[(int)_dragTargetAnchor];
            var candidates = new List<Control>(items.Count);
            for (int i = 0; i < items.Count; i++)
            {
                if (Array.IndexOf(draggedItems, items[i]) == -1)
                    candidates.Add(items[i]);
            }
            _dragTargetIndex = 0;
            _dragPreviewX = location.X;
            for (int i = 0; i < candidates.Count;)
            {
                var item = candidates[i];
                var unit = GetItemUnit(item);
                var unitCount = 1;
                Control firstVisible = item.Visible ? item : null;
                Control lastVisible = firstVisible;
                if (unit.Length > 1)
                {
                    unitCount = 0;
                    while (i + unitCount < candidates.Count && Array.IndexOf(unit, candidates[i + unitCount]) != -1)
                    {
                        var unitItem = candidates[i + unitCount];
                        if (unitItem.Visible)
                        {
                            firstVisible ??= unitItem;
                            lastVisible = unitItem;
                        }
                        unitCount++;
                    }
                }
                if (firstVisible != null)
                {
                    if (location.X < (firstVisible.Left + lastVisible.Right) * 0.5f)
                    {
                        _dragTargetIndex = i;
                        _dragPreviewX = firstVisible.Left - _itemsMargin.Width * 0.5f;
                        return;
                    }
                    _dragTargetIndex = i + unitCount;
                    _dragPreviewX = lastVisible.Right + _itemsMargin.Width * 0.5f;
                }
                i += unitCount;
            }
        }

        internal void EndItemDrag(Control control, bool commit)
        {
            if (_draggedItem != control)
                return;

            if (commit && _dragTargetIndex >= 0)
            {
                var draggedItems = GetItemUnit(control);
                for (int i = 0; i < draggedItems.Length; i++)
                    RemoveFromPlacement(draggedItems[i]);
                var targetItems = _anchorItems[(int)_dragTargetAnchor];
                int targetIndex = Mathf.Clamp(_dragTargetIndex, 0, targetItems.Count);
                for (int i = 0; i < draggedItems.Length; i++)
                    targetItems.Insert(targetIndex + i, draggedItems[i]);
                LayoutChanged?.Invoke();
                PerformLayout();
            }

            _draggedItem = null;
            _dragTargetIndex = -1;
        }

        private Control[] GetItemUnit(Control control)
        {
            return _itemGroups.TryGetValue(control, out var group) ? group.Items : new[] { control };
        }

        /// <inheritdoc />
        protected override void DrawChildren()
        {
            base.DrawChildren();
            if (_draggedItem != null && _dragTargetIndex >= 0)
            {
                var style = Style.Current;
                var preview = new Rectangle(_dragPreviewX - 1.0f, 5.0f, 2.0f, Height - 10.0f);
                StyleRendering.FillRoundedRectangle(preview, style.BorderSelected.AlphaMultiplied(0.8f), 1.0f);
            }
        }

        /// <inheritdoc />
        public override void OnChildResized(Control control)
        {
            base.OnChildResized(control);

            PerformLayout();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (base.OnKeyDown(key))
                return true;

            // Fallback to the owning window for shortcuts
            EditorWindow editorWindow = null;
            ContainerControl c = Parent;
            while (c != null && editorWindow == null)
            {
                editorWindow = c as EditorWindow;
                c = c.Parent;
            }
            var editor = Editor.Instance;
            if (editorWindow == null)
                editorWindow = editor.Windows.EditWin; // Fallback to main editor window
            return editorWindow.InputActions.Process(editor, this, key);
        }
    }
}
