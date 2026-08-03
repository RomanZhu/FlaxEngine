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
        private Margin _itemsMargin;
        private readonly List<Control>[] _anchorItems =
        {
            new List<Control>(),
            new List<Control>(),
            new List<Control>(),
        };
        private readonly Dictionary<Control, string> _itemIds = new Dictionary<Control, string>();
        private readonly Dictionary<string, SavedPlacement> _savedPlacements = new Dictionary<string, SavedPlacement>();
        private Control _draggedItem;
        private ToolStripAnchor _dragTargetAnchor;
        private int _dragTargetIndex = -1;
        private float _dragPreviewX;

        private struct SavedPlacement
        {
            public ToolStripAnchor Anchor;
            public int Index;
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
        /// Event fired after the user rearranges items with Ctrl+drag.
        /// </summary>
        public Action LayoutChanged;

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
            };
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
            };
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
            };
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
            };
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
                        entries.Add(id + "@" + anchor + "@" + index);
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
            if (!string.IsNullOrEmpty(layout))
            {
                var entries = layout.Split('|');
                for (int i = 0; i < entries.Length; i++)
                {
                    var parts = entries[i].Split('@');
                    if (parts.Length == 3 && int.TryParse(parts[1], out var anchor) && int.TryParse(parts[2], out var index) && anchor >= 0 && anchor < _anchorItems.Length)
                    {
                        _savedPlacements[parts[0]] = new SavedPlacement
                        {
                            Anchor = (ToolStripAnchor)anchor,
                            Index = Mathf.Max(0, index),
                        };
                    }
                }
            }

            var controls = new List<Control>(_itemIds.Keys);
            for (int i = 0; i < controls.Count; i++)
            {
                var control = controls[i];
                var id = _itemIds[control];
                if (_savedPlacements.TryGetValue(id, out var saved))
                    SetItemPlacement(control, saved.Anchor, saved.Index, id);
            }
            PerformLayout();
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
            int visibleCount = 0;
            for (int i = 0; i < items.Count; i++)
            {
                if (!items[i].Visible)
                    continue;
                width += items[i].Width;
                visibleCount++;
            }
            return width + Mathf.Max(0, visibleCount - 1) * _itemsMargin.Width;
        }

        private void LayoutGroup(List<Control> items, float x, float height)
        {
            for (int i = 0; i < items.Count; i++)
            {
                var control = items[i];
                if (!control.Visible)
                    continue;
                control.Bounds = new Rectangle(x, _itemsMargin.Top, control.Width, height);
                x += control.Width + _itemsMargin.Width;
            }
        }

        /// <inheritdoc />
        public override void DrawSelf()
        {
            base.DrawSelf();
            var style = Style.Current;
            for (int anchor = 0; anchor < _anchorItems.Length; anchor++)
            {
                var items = _anchorItems[anchor];
                Control first = null;
                Control last = null;
                for (int i = 0; i < items.Count; i++)
                {
                    if (!items[i].Visible)
                        continue;
                    first ??= items[i];
                    last = items[i];
                }
                if (first == null)
                    continue;
                var groupRect = new Rectangle(first.Left - 2.0f, first.Top - 1.0f, last.Right - first.Left + 4.0f, first.Height + 2.0f);
                StyleRendering.DrawRoundedRectangle(groupRect, style.BackgroundNormal, style.BorderNormal.AlphaMultiplied(0.72f), 1.0f, style.CornerRadius);
            }
        }

        internal void UpdateItemDrag(Control control, Float2 location)
        {
            if (control == null || control.Parent != this)
                return;

            _draggedItem = control;
            float normalizedX = Width > 0.0f ? location.X / Width : 0.5f;
            _dragTargetAnchor = normalizedX < 0.33f ? ToolStripAnchor.Left : normalizedX > 0.67f ? ToolStripAnchor.Right : ToolStripAnchor.Center;
            var items = _anchorItems[(int)_dragTargetAnchor];
            _dragTargetIndex = 0;
            _dragPreviewX = location.X;
            for (int i = 0; i < items.Count; i++)
            {
                var item = items[i];
                if (item == control || !item.Visible)
                    continue;
                if (location.X < item.X + item.Width * 0.5f)
                {
                    _dragPreviewX = item.X - _itemsMargin.Width * 0.5f;
                    return;
                }
                _dragTargetIndex++;
                _dragPreviewX = item.Right + _itemsMargin.Width * 0.5f;
            }
        }

        internal void EndItemDrag(Control control, bool commit)
        {
            if (_draggedItem != control)
                return;

            if (commit && _dragTargetIndex >= 0)
            {
                var oldAnchor = GetItemAnchor(control);
                var oldItems = _anchorItems[(int)oldAnchor];
                int oldIndex = oldItems.IndexOf(control);
                oldItems.Remove(control);
                var targetItems = _anchorItems[(int)_dragTargetAnchor];
                int targetIndex = Mathf.Clamp(_dragTargetIndex, 0, targetItems.Count);
                if (oldAnchor == _dragTargetAnchor && oldIndex >= 0 && oldIndex < _dragTargetIndex)
                    targetIndex = Mathf.Max(0, targetIndex - 1);
                targetItems.Insert(targetIndex, control);
                LayoutChanged?.Invoke();
                PerformLayout();
            }

            _draggedItem = null;
            _dragTargetIndex = -1;
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
