// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.ContextMenu
{
    /// <summary>
    /// Popup menu control.
    /// </summary>
    /// <seealso cref="ContextMenuBase" />
    [HideInEditor]
    public class ContextMenu : ContextMenuBase
    {
        private const float ScrollIndicatorArea = 8.0f;
        private const int ScrollIndicatorRows = 4;

        private ContextMenuItem _pendingAimItem;
        private float _pendingAimUntil;
        private bool _hasScrollIndicators;
        private readonly Dictionary<ContextMenuButton, int> _accessKeyIndices = new Dictionary<ContextMenuButton, int>();

        /// <summary>
        /// The items container.
        /// </summary>
        /// <seealso cref="FlaxEngine.GUI.Panel" />
        [HideInEditor]
        protected class ItemsPanel : Panel
        {
            private readonly ContextMenu _menu;

            /// <summary>
            /// Initializes a new instance of the <see cref="ItemsPanel"/> class.
            /// </summary>
            /// <param name="menu">The menu.</param>
            public ItemsPanel(ContextMenu menu)
            : base(ScrollBars.Vertical)
            {
                _menu = menu;
                ScrollBarsSize = 0.0f;
                ScrollbarTrackColor = Color.Transparent;
                ScrollbarThumbColor = Color.Transparent;
                ScrollbarThumbSelectedColor = Color.Transparent;
            }

            /// <inheritdoc />
            protected override void Arrange()
            {
                base.Arrange();

                // Arrange controls
                Margin margin = _menu._itemsMargin;
                float y = 0;
                float width = Width - margin.Width;
                for (int i = 0; i < _children.Count; i++)
                {
                    if (_children[i] is ContextMenuItem item && item.Visible)
                    {
                        var height = item.Height;
                        item.Bounds = new Rectangle(margin.Left, y, width, height);
                        y += height + margin.Height;
                    }
                }
            }
        }

        /// <summary>
        /// The items area margin.
        /// </summary>
        protected Margin _itemsAreaMargin = new Margin(0, 0, 3, 3);

        /// <summary>
        /// The items margin.
        /// </summary>
        protected Margin _itemsMargin = new Margin(28, 0, 2, 0);

        /// <summary>
        /// The items panel.
        /// </summary>
        protected ItemsPanel _panel;

        /// <summary>
        /// Gets or sets the items area margin (items container area margin).
        /// </summary>
        public Margin ItemsAreaMargin
        {
            get => _itemsAreaMargin;
            set
            {
                _itemsAreaMargin = value;
                PerformLayout();
            }
        }

        internal Margin EffectiveItemsAreaMargin => GetItemsAreaMargin(_hasScrollIndicators);

        /// <summary>
        /// Gets or sets the items margin.
        /// </summary>
        public Margin ItemsMargin
        {
            get => _itemsMargin;
            set
            {
                _itemsMargin = value;
                PerformLayout();
            }
        }

        /// <summary>
        /// Gets or sets the minimum popup width.
        /// </summary>
        public float MinimumWidth { get; set; }

        /// <summary>
        /// Gets or sets the maximum amount of items in the view. If popup has more items to show it uses a additional scroll panel.
        /// </summary>
        public int MaximumItemsInViewCount { get; set; }

        /// <summary>
        /// Gets the items (readonly).
        /// </summary>
        public IEnumerable<ContextMenuItem> Items => _panel.Children.OfType<ContextMenuItem>();

        /// <summary>
        /// Event fired when user clicks on the button.
        /// </summary>
        public event Action<ContextMenuButton> ButtonClicked;

        /// <summary>
        /// Gets the context menu items container control.
        /// </summary>
        public Panel ItemsContainer => _panel;

        /// <summary>
        /// The auto sort.
        /// </summary>
        private bool _autosort;

        /// <summary>
        /// The auto sort property.
        /// </summary>
        public bool AutoSort
        {
            get => _autosort;
            set
            {
                _autosort = value;
                if (_autosort)
                    SortButtons();
            }
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="ContextMenu"/> class.
        /// </summary>
        public ContextMenu()
        {
            MinimumWidth = 10;
            MaximumItemsInViewCount = 20;

            _panel = new ItemsPanel(this)
            {
                ClipChildren = true,
                Parent = this,
            };
        }

        private Margin GetItemsAreaMargin(bool hasScrollIndicators)
        {
            if (!hasScrollIndicators)
                return _itemsAreaMargin;

            return new Margin(_itemsAreaMargin.Left, _itemsAreaMargin.Right, _itemsAreaMargin.Top + ScrollIndicatorArea, _itemsAreaMargin.Bottom + ScrollIndicatorArea);
        }

        internal bool OnItemMouseEnter(ContextMenuItem item, Float2 screenLocation)
        {
            if (HasChildCMOpened && IsPointerInsideSubmenuAim(screenLocation))
            {
                _pendingAimItem = item;
                _pendingAimUntil = Time.UnscaledGameTime + 0.22f;
                return true;
            }

            _pendingAimItem = null;
            HideChild();
            return false;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            base.Update(deltaTime);

            if (_pendingAimItem == null)
                return;

            // If the pointer reached the already open submenu, preserve it. Otherwise
            // switch after the short geometry guard expires or the pointer leaves its aim.
            if (!_pendingAimItem.IsMouseOver)
            {
                _pendingAimItem = null;
                return;
            }
            if (Time.UnscaledGameTime < _pendingAimUntil && IsPointerInsideSubmenuAim(FlaxEngine.Input.MouseScreenPosition))
                return;

            var item = _pendingAimItem;
            _pendingAimItem = null;
            HideChild();
            item.OnMenuAimReleased();
        }

        /// <summary>
        /// Sorts all <see cref="ContextMenuButton"/> alphabetically.
        /// </summary>
        /// <param name="force">Overrides <see cref="AutoSort"/> property.</param>
        public void SortButtons(bool force = false)
        {
            if (!_autosort && !force)
                return;
            _panel.Children.Sort((control, control1) =>
            {
                if (control is ContextMenuButton cmb && control1 is ContextMenuButton cmb1)
                    return string.Compare(cmb.Text, cmb1.Text, StringComparison.OrdinalIgnoreCase);
                if (!(control is ContextMenuButton))
                    return 1;
                return -1;
            });
        }

        /// <summary>
        /// Removes all the added items (buttons, separators, etc.).
        /// </summary>
        public void DisposeAllItems()
        {
            for (int i = _panel.ChildrenCount - 1; _panel.ChildrenCount > 0 && i >= 0; i--)
            {
                if (_panel.Children[i] is ContextMenuItem)
                    _panel.Children[i].Dispose();
            }
            _accessKeyIndices.Clear();
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text)
        {
            var item = new ContextMenuButton(this, text)
            {
                Parent = _panel
            };
            SortButtons();
            return item;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="shortKeys">The short keys.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text, string shortKeys)
        {
            var item = new ContextMenuButton(this, text, shortKeys)
            {
                Parent = _panel
            };
            SortButtons();
            return item;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="clicked">On button clicked event.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text, Action clicked)
        {
            var item = new ContextMenuButton(this, text)
            {
                Parent = _panel
            };
            item.Clicked += clicked;
            SortButtons();
            return item;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="clicked">On button clicked event.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text, Action<ContextMenuButton> clicked)
        {
            var item = new ContextMenuButton(this, text)
            {
                Parent = _panel
            };
            item.ButtonClicked += clicked;
            SortButtons();
            return item;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="shortKeys">The shortKeys.</param>
        /// <param name="clicked">On button clicked event.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text, string shortKeys, Action clicked)
        {
            var item = new ContextMenuButton(this, text, shortKeys)
            {
                Parent = _panel
            };
            item.Clicked += clicked;
            SortButtons();
            return item;
        }

        /// <summary>
        /// Adds the button.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <param name="binding">The input binding.</param>
        /// <param name="clicked">On button clicked event.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuButton AddButton(string text, InputBinding binding, Action clicked)
        {
            var item = new ContextMenuButton(this, text, binding.ToString())
            {
                Parent = _panel
            };
            item.Clicked += clicked;
            SortButtons();
            return item;
        }

        /// <summary>
        /// Gets the child menu (with that name).
        /// </summary>
        /// <param name="text">The text.</param>
        /// <returns>Created context menu item control or null if missing.</returns>
        public ContextMenuChildMenu GetChildMenu(string text)
        {
            for (int i = 0; i < _panel.ChildrenCount; i++)
            {
                if (_panel.Children[i] is ContextMenuChildMenu menu && menu.Text == text)
                    return menu;
            }
            return null;
        }

        /// <summary>
        /// Adds the child menu or gets it if already created (with that name).
        /// </summary>
        /// <param name="text">The text.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuChildMenu GetOrAddChildMenu(string text)
        {
            var item = GetChildMenu(text);
            if (item == null)
            {
                item = new ContextMenuChildMenu(this, text)
                {
                    Parent = _panel
                };
            }
            return item;
        }

        /// <summary>
        /// Adds the child menu.
        /// </summary>
        /// <param name="text">The text.</param>
        /// <returns>Created context menu item control.</returns>
        public ContextMenuChildMenu AddChildMenu(string text)
        {
            var item = new ContextMenuChildMenu(this, text)
            {
                Parent = _panel
            };
            return item;
        }

        /// <summary>
        /// Adds the separator.
        /// </summary>
        public void AddSeparator()
        {
            var item = new ContextMenuSeparator(this)
            {
                Parent = _panel
            };
        }

        /// <summary>
        /// Called when button get clicked.
        /// </summary>
        /// <param name="button">The button.</param>
        public virtual void OnButtonClicked(ContextMenuButton button)
        {
            ButtonClicked?.Invoke(button);
        }

        internal int GetAccessKeyIndex(ContextMenuButton button)
        {
            if (!_accessKeyIndices.TryGetValue(button, out var index))
                UpdateAccessKeys();
            return _accessKeyIndices.TryGetValue(button, out index) ? index : -1;
        }

        private void UpdateAccessKeys()
        {
            _accessKeyIndices.Clear();
            var used = new HashSet<char>();
            for (int i = 0; i < _panel.Children.Count; i++)
            {
                if (!(_panel.Children[i] is ContextMenuButton item) || !item.Visible)
                    continue;

                int index = FindAccessKeyIndex(item.Text, used);
                _accessKeyIndices[item] = index;

                if (index >= 0)
                    used.Add(NormalizeAccessKey(item.Text[index]));
            }
        }

        private static int FindAccessKeyIndex(string text, HashSet<char> used)
        {
            if (string.IsNullOrEmpty(text))
                return -1;

            int fallback = -1;
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (!char.IsLetterOrDigit(c))
                    continue;

                if (fallback == -1)
                    fallback = i;

                if (!used.Contains(NormalizeAccessKey(c)))
                    return i;
            }

            return fallback;
        }

        private static char NormalizeAccessKey(char c)
        {
            return char.ToUpperInvariant(c);
        }

        private bool ActivateAccessKey(char c)
        {
            if (!char.IsLetterOrDigit(c))
                return false;

            char key = NormalizeAccessKey(c);
            for (int i = 0; i < _panel.Children.Count; i++)
            {
                if (!(_panel.Children[i] is ContextMenuButton item) || !item.Visible || !item.Enabled)
                    continue;

                int accessKeyIndex = GetAccessKeyIndex(item);
                if (accessKeyIndex < 0 || NormalizeAccessKey(item.Text[accessKeyIndex]) != key)
                    continue;

                item.Focus();
                _panel.ScrollViewTo(item);
                if (item is ContextMenuChildMenu childMenu && childMenu.ContextMenu.HasChildren)
                {
                    childMenu.ShowChild(this);
                }
                else
                {
                    item.Click();
                }
                return true;
            }
            return false;
        }

        /// <inheritdoc />
        public override void Show(Control parent, Float2 location, ContextMenuDirection? direction = null)
        {
            // Remove last separator to make context menu look better
            int lastIndex = _panel.Children.Count - 1;
            if (lastIndex >= 0 && _panel.Children[lastIndex] is ContextMenuSeparator separator)
                separator.Dispose();

            base.Show(parent, location, direction);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            DrawScrollIndicators();
        }

        private void DrawScrollIndicators()
        {
            var scrollBar = _panel?.VScrollBar;
            if (!_hasScrollIndicators || scrollBar == null || !scrollBar.Enabled)
                return;

            var style = Style.Current;
            var color = style.ForegroundGrey.AlphaMultiplied(0.85f);
            var margin = EffectiveItemsAreaMargin;
            if (scrollBar.TargetValue > scrollBar.Minimum + 0.5f)
                DrawScrollIndicator(new Float2(Width * 0.5f, Mathf.Max(1.0f, margin.Top - ScrollIndicatorArea + 1.0f)), false, color);
            if (scrollBar.TargetValue < scrollBar.Maximum - 0.5f)
                DrawScrollIndicator(new Float2(Width * 0.5f, Height - margin.Bottom + 2.0f), true, color);
        }

        private static void DrawScrollIndicator(Float2 center, bool down, Color color)
        {
            for (int row = 0; row < ScrollIndicatorRows; row++)
            {
                int width = down ? ScrollIndicatorRows * 2 - 1 - row * 2 : row * 2 + 1;
                float y = center.Y + row;
                Render2D.FillRectangle(new Rectangle(center.X - width * 0.5f, y, width, 1.0f), color);
            }
        }

        /// <inheritdoc />
        public override bool ContainsPoint(ref Float2 location, bool precise)
        {
            if (base.ContainsPoint(ref location, precise))
                return true;

            var cLocation = location - Location;
            for (int i = 0; i < _panel.Children.Count; i++)
            {
                if (_panel.Children[i].ContainsPoint(ref cLocation, precise))
                    return true;
            }

            return false;
        }

        /// <inheritdoc />
        protected override void PerformLayoutAfterChildren()
        {
            var prevSize = Size;

            // Calculate size of the context menu (items only)
            float maxWidth = 0;
            float height = 0;
            int itemsLeft = MaximumItemsInViewCount;
            int overflowItemCount = 0;
            int itemsCount = 0;
            for (int i = 0; i < _panel.Children.Count; i++)
            {
                if (_panel.Children[i] is ContextMenuItem item && item.Visible)
                {
                    itemsCount++;
                    if (itemsLeft > 0)
                    {
                        height += item.Height + _itemsMargin.Height;
                        itemsLeft--;
                    }
                    else
                    {
                        overflowItemCount++;
                    }
                    maxWidth = Mathf.Max(maxWidth, item.MinimumWidth);
                }
            }
            if (itemsCount != 0)
                height -= _itemsMargin.Height; // Remove item margin from top and bottom
            _hasScrollIndicators = overflowItemCount > 0;
            var itemsAreaMargin = GetItemsAreaMargin(_hasScrollIndicators);
            height += itemsAreaMargin.Height;
            maxWidth = Mathf.Max(maxWidth + _itemsMargin.Width + 8.0f, MinimumWidth);

            // Move child arrows to accommodate scroll bar showing 
            foreach (var child in _panel.Children)
            {
                if (child is ContextMenuButton item && item.Visible)
                {
                    item.ExtraAdjustmentAmount = overflowItemCount > 0 ? -_panel.VScrollBar.Width : 0.0f;
                }
            }
            UpdateAccessKeys();

            // Resize container
            Size = new Float2(Mathf.Ceil(maxWidth), Mathf.Ceil(height));

            // Arrange items view panel
            var panelBounds = new Rectangle(Float2.Zero, Size);
            itemsAreaMargin.ShrinkRectangle(ref panelBounds);
            _panel.Bounds = panelBounds;

            // Check if is visible size get changed
            if (Visible && prevSize != Size)
            {
                // Update window dimensions
                UpdateWindowSize();
            }
        }

        /// <inheritdoc />
        public override bool OnCharInput(char c)
        {
            if (base.OnCharInput(c))
                return true;

            if (ActivateAccessKey(c))
                return true;

            // Find the item that starts with that character
            if (char.IsLetterOrDigit(c))
            {
                int startIndex = 0;
                for (int i = 0; i < _panel.Children.Count; i++)
                {
                    if (_panel.Children[i] is ContextMenuButton item && item.Visible && item.IsFocused)
                    {
                        // Start searching from the last hit item
                        startIndex = i + 1;
                        break;
                    }
                }
                for (int i = startIndex; i < _panel.Children.Count; i++)
                {
                    if (_panel.Children[i] is ContextMenuButton item && item.Visible)
                    {
                        bool startsWith = false;
                        for (int j = 0; j < item.Text.Length; j++)
                        {
                            var k = item.Text[j];
                            if (char.ToLower(k) == char.ToLower(c))
                            {
                                startsWith = true;
                                break;
                            }
                            if (!char.IsWhiteSpace(k) && k != '>')
                                break;
                        }
                        if (startsWith)
                        {
                            // Focus found item
                            item.Focus();
                            _panel.ScrollViewTo(item);
                            return true;
                        }
                    }
                }
                if (startIndex > 0 && startIndex <= _panel.Children.Count)
                {
                    // No more items found so start from the top if there are matching items
                    _panel.Children[startIndex - 1].Defocus();
                    return OnCharInput(c);
                }
            }

            return false;
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (base.OnKeyDown(key))
                return true;

            // Keyboard navigation around the menu
            switch (key)
            {
            case KeyboardKeys.ArrowDown:
                for (int i = 0; i < _panel.Children.Count; i++)
                {
                    if (_panel.Children[i] is ContextMenuButton item && item.Visible && item.Enabled)
                    {
                        item.Focus();
                        _panel.ScrollViewTo(item);
                        return true;
                    }
                }
                break;
            case KeyboardKeys.ArrowUp:
                for (int i = _panel.Children.Count - 1; i >= 0; i--)
                {
                    if (_panel.Children[i] is ContextMenuButton item && item.Visible && item.Enabled)
                    {
                        item.Focus();
                        _panel.ScrollViewTo(item);
                        return true;
                    }
                }
                break;
            case KeyboardKeys.ArrowRight:
                for (int i = 0; i < _panel.Children.Count; i++)
                {
                    if (_panel.Children[i] is ContextMenuChildMenu item && item.Visible && item.IsFocused && !item.ContextMenu.IsOpened)
                    {
                        item.ShowChild(this);
                        item.ContextMenu._panel.Children.FirstOrDefault(x => x is ContextMenuButton && x.Visible)?.Focus();
                        break;
                    }
                }
                break;
            case KeyboardKeys.ArrowLeft:
                ParentCM?.RootWindow.Focus();
                break;
            }

            return false;
        }
    }
}
