// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.Utilities;
using FlaxEngine;
using FlaxEngine.GUI;
using LegacyContextMenu = FlaxEditor.GUI.ContextMenu.ContextMenu;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Searchable adapter that preserves the source context menu hierarchy. With an empty
    /// query it behaves as a normal cascading menu; while searching it shows ranked leaves.
    /// </summary>
    public class SearchableContextMenu : LegacyContextMenu
    {
        private sealed class SearchInput : SearchBox
        {
            public event Action<int> Navigation;
            public event Action Submitted;
            public event Action Canceled;

            public override bool OnKeyDown(KeyboardKeys key)
            {
                switch (key)
                {
                case KeyboardKeys.ArrowUp:
                    Navigation?.Invoke(-1);
                    return true;
                case KeyboardKeys.ArrowDown:
                    Navigation?.Invoke(1);
                    return true;
                case KeyboardKeys.Tab:
                    Navigation?.Invoke(Root.GetKey(KeyboardKeys.Shift) ? -1 : 1);
                    return true;
                case KeyboardKeys.Return:
                {
                    var wasEditing = IsEditing;
                    if (!base.OnKeyDown(key))
                        return false;
                    if (wasEditing)
                        Submitted?.Invoke();
                    return true;
                }
                case KeyboardKeys.Escape:
                    Canceled?.Invoke();
                    return true;
                }

                return base.OnKeyDown(key);
            }
        }

        private sealed class SearchHeader : ContextMenuItem
        {
            private readonly Label _title;
            public readonly SearchInput SearchBox;

            public SearchHeader(LegacyContextMenu parent, string title, float minimumWidth)
            : base(parent, minimumWidth - 20.0f, 50.0f)
            {
                _title = new Label
                {
                    Parent = this,
                    Text = title,
                    Font = new FontReference(Style.Current.FontMedium),
                    TextColor = Style.Current.Foreground,
                    HorizontalAlignment = TextAlignment.Near,
                    VerticalAlignment = TextAlignment.Center,
                };
                SearchBox = new SearchInput
                {
                    Parent = this,
                    TooltipText = "Type to fuzzy-search every action. Use t: as an optional type prefix.",
                };
            }

            public override float MinimumWidth => Width;

            protected override void PerformLayoutAfterChildren()
            {
                base.PerformLayoutAfterChildren();
                _title.Bounds = new Rectangle(0.0f, 0.0f, Width, 18.0f);
                SearchBox.Bounds = new Rectangle(0.0f, 21.0f, Width, 24.0f);
            }
        }

        private sealed class SearchEntry
        {
            public string Name;
            public string SearchText;
            public ContextMenuButton Source;
            public SearchResultButton Result;
            public float Score;
        }

        private sealed class SearchResultButton : ContextMenuButton
        {
            public bool IsKeyboardSelected;

            public SearchResultButton(LegacyContextMenu parent, string text)
            : base(parent, text)
            {
            }

            public override void Draw()
            {
                base.Draw();

                if (IsKeyboardSelected)
                {
                    var style = Style.Current;
                    StyleRendering.DrawRoundedRectangleBorder(new Rectangle(Float2.Zero, Size).MakeExpanded(-1.0f), style.BorderSelected, 1.0f, style.GetSelectionCornerRadius());
                }
            }
        }

        private readonly LegacyContextMenu _source;
        private readonly SearchHeader _header;
        private readonly List<ContextMenuItem> _rootItems = new List<ContextMenuItem>();
        private readonly List<SearchEntry> _entries = new List<SearchEntry>();
        private readonly List<SearchEntry> _searchResults = new List<SearchEntry>();
        private int _selectedSearchResult = -1;

        /// <summary>
        /// Creates a searchable, hierarchy-preserving adapter for the source menu.
        /// </summary>
        public SearchableContextMenu(LegacyContextMenu source, string title = "New", float width = 360.0f, float height = 460.0f)
        {
            _source = source ?? throw new ArgumentNullException(nameof(source));
            MinimumWidth = width;
            MaximumItemsInViewCount = Mathf.Max(8, Mathf.FloorToInt(height / 24.0f) - 2);
            ItemsAreaMargin = new Margin(4.0f, 4.0f, 4.0f, 4.0f);
            ItemsMargin = new Margin(16.0f, 4.0f, 1.0f, 0.0f);

            _header = new SearchHeader(this, title, width)
            {
                Parent = ItemsContainer,
            };
            _header.SearchBox.TextChanged += OnSearchChanged;
            _header.SearchBox.Navigation += OnSearchNavigation;
            _header.SearchBox.Submitted += OnSearchSubmitted;
            _header.SearchBox.Canceled += OnSearchCanceled;

            CopyMenu(source, this, null, true);
            for (int i = 0; i < _entries.Count; i++)
            {
                var entry = _entries[i];
                var result = new SearchResultButton(this, entry.Name)
                {
                    Parent = ItemsContainer,
                };
                result.Clicked += entry.Source.Click;
                CopyButtonState(entry.Source, result);
                result.Visible = false;
                entry.Result = result;
            }
            _header.IndexInParent = 0;
        }

        private void CopyMenu(LegacyContextMenu source, LegacyContextMenu destination, string category, bool isRoot)
        {
            foreach (var item in source.Items)
            {
                ContextMenuItem copy;
                if (item is ContextMenuChildMenu child)
                {
                    var copyChild = destination.AddChildMenu(child.Text);
                    copyChild.Enabled = child.Enabled;
                    copyChild.Icon = child.Icon;
                    var childCategory = string.IsNullOrWhiteSpace(category) ? child.Text : category + " / " + child.Text;
                    CopyMenu(child.ContextMenu, copyChild.ContextMenu, childCategory, false);
                    copy = copyChild;
                }
                else if (item is ContextMenuSeparator)
                {
                    int index = destination.ItemsContainer.ChildrenCount;
                    destination.AddSeparator();
                    copy = destination.ItemsContainer.Children[index] as ContextMenuItem;
                }
                else if (item is ContextMenuButton button && !string.IsNullOrWhiteSpace(button.Text))
                {
                    var copyButton = destination.AddButton(button.Text, button.Click);
                    CopyButtonState(button, copyButton);
                    copy = copyButton;
                    _entries.Add(new SearchEntry
                    {
                        Name = button.Text,
                        SearchText = button.Text + " " + category + " " + button.TooltipText,
                        Source = button,
                    });
                }
                else
                {
                    continue;
                }

                if (isRoot && copy != null)
                    _rootItems.Add(copy);
            }
        }

        private static void CopyButtonState(ContextMenuButton source, ContextMenuButton destination)
        {
            destination.Enabled = source.Enabled;
            destination.Icon = source.Icon;
            destination.Checked = source.Checked;
            destination.AutoCheck = source.AutoCheck;
            destination.CloseMenuOnClick = source.CloseMenuOnClick;
            destination.ShortKeys = source.ShortKeys;
            destination.TooltipText = source.TooltipText;
        }

        private void OnSearchChanged()
        {
            var query = _header.SearchBox.Text?.Trim() ?? string.Empty;
            if (query.StartsWith("t:", StringComparison.OrdinalIgnoreCase))
                query = query.Substring(2).TrimStart();

            bool searching = !string.IsNullOrWhiteSpace(query);
            _searchResults.Clear();
            _selectedSearchResult = -1;
            for (int i = 0; i < _rootItems.Count; i++)
                _rootItems[i].Visible = !searching;
            for (int i = 0; i < _entries.Count; i++)
            {
                _entries[i].Result.Visible = false;
                _entries[i].Result.IsKeyboardSelected = false;
            }

            if (searching)
            {
                var matches = new List<SearchEntry>();
                for (int i = 0; i < _entries.Count; i++)
                {
                    var entry = _entries[i];
                    if (!QueryFilterHelper.FuzzyMatch(query, entry.SearchText, out var score, out _))
                        continue;
                    entry.Score = score;
                    matches.Add(entry);
                }
                matches.Sort((a, b) => b.Score.CompareTo(a.Score));
                int count = Mathf.Min(matches.Count, MaximumItemsInViewCount - 1);
                for (int i = 0; i < count; i++)
                {
                    var entry = matches[i];
                    var result = entry.Result;
                    result.Visible = true;
                    result.IndexInParent = i + 1;
                    _searchResults.Add(entry);
                }

                if (_searchResults.Count != 0)
                {
                    _selectedSearchResult = 0;
                    UpdateSearchResultSelection();
                }
            }

            // Results are re-indexed on every search, so re-pin the input header first.
            _header.IndexInParent = 0;
            PerformLayout(true);
        }

        private void OnSearchNavigation(int direction)
        {
            if (_searchResults.Count == 0)
                return;

            _selectedSearchResult = (_selectedSearchResult + direction + _searchResults.Count) % _searchResults.Count;
            UpdateSearchResultSelection();
            ItemsContainer.ScrollViewTo(_searchResults[_selectedSearchResult].Result);
        }

        private void OnSearchSubmitted()
        {
            if (_selectedSearchResult >= 0 && _selectedSearchResult < _searchResults.Count)
                _searchResults[_selectedSearchResult].Result.Click();
        }

        private void OnSearchCanceled()
        {
            Hide();
        }

        private void UpdateSearchResultSelection()
        {
            for (int i = 0; i < _searchResults.Count; i++)
                _searchResults[i].Result.IsKeyboardSelected = i == _selectedSearchResult;
        }

        /// <inheritdoc />
        public override void Show(Control parent, Float2 location, ContextMenuDirection? direction = null)
        {
            base.Show(parent, location, direction);
            OnSearchChanged();
            _header.SearchBox.Focus();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _source.Dispose();
            base.OnDestroy();
        }
    }
}
