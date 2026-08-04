// Copyright (c) Wojciech Figat. All rights reserved.

using System.Collections.Generic;
using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.GUI.ContextMenu;
using FlaxEditor.GUI.Input;
using FlaxEditor.Utilities;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Json;

namespace FlaxEditor.Windows
{
    public sealed partial class ContentWindow
    {
        private FlaxEngine.Window _searchHintsWindow;

        private sealed class ContentSearchBox : SearchBox
        {
            public event System.Action SearchFocused;
            public event System.Action SearchSubmitted;
            public event System.Action SearchCanceled;
            public event System.Action<KeyboardKeys> SearchNavigation;

            public override void OnGotFocus()
            {
                base.OnGotFocus();
                SearchFocused?.Invoke();
            }

            public override bool OnKeyDown(KeyboardKeys key)
            {
                switch (key)
                {
                case KeyboardKeys.Escape:
                    SearchCanceled?.Invoke();
                    return true;
                case KeyboardKeys.ArrowUp:
                case KeyboardKeys.ArrowDown:
                case KeyboardKeys.Tab:
                    SearchNavigation?.Invoke(key);
                    return true;
                }
                if (key == KeyboardKeys.Return)
                {
                    SearchSubmitted?.Invoke();
                    return true;
                }
                return base.OnKeyDown(key);
            }
        }

        private sealed class SearchHintsPanel : ContextMenuBase
        {
            private readonly List<TypeSuggestion> _typeSuggestions = new List<TypeSuggestion>();
            private int _hoveredSuggestion = -1;
            private int _selectedSuggestion = -1;
            private string _typeQuery;

            public event System.Action<ContentItemSearchFilter> TypeSelected;

            private readonly struct TypeSuggestion
            {
                public readonly ContentItemSearchFilter Type;
                public readonly float Score;

                public TypeSuggestion(ContentItemSearchFilter type, float score)
                {
                    Type = type;
                    Score = score;
                }
            }

            public SearchHintsPanel()
            : base()
            {
                Size = new Float2(280.0f, 116.0f);
                AutoFocus = false;
                // Keep the content search box focused after showing, while still allowing
                // suggestions to be selected with the mouse.
                UseVisibilityControl = false;
            }

            /// <inheritdoc />
            protected override void OnWindowCreating(ref CreateWindowSettings settings)
            {
                // Suggestions are interactive, but must never become the active text-input target.
                settings.ActivateWhenFirstShown = false;
            }

            public void SetQuery(string query)
            {
                _typeSuggestions.Clear();
                _hoveredSuggestion = -1;
                _selectedSuggestion = -1;
                var typeQuery = GetActiveTypeQuery(query);
                _typeQuery = typeQuery;
                if (typeQuery == null)
                {
                    Height = 116.0f;
                    return;
                }

                for (int i = 0; i <= (int)ContentItemSearchFilter.Other; i++)
                {
                    var type = (ContentItemSearchFilter)i;
                    float score;
                    if (typeQuery.Length == 0)
                        score = 1.0f - i * 0.001f;
                    else if (!QueryFilterHelper.FuzzyMatch(typeQuery, GetTypeSearchText(type), out score, out _))
                        continue;
                    _typeSuggestions.Add(new TypeSuggestion(type, score));
                }
                _typeSuggestions.Sort((a, b) => b.Score.CompareTo(a.Score));
                if (_typeSuggestions.Count > 7)
                    _typeSuggestions.RemoveRange(7, _typeSuggestions.Count - 7);
                if (_typeSuggestions.Count != 0)
                    _selectedSuggestion = 0;
                Height = 38.0f + _typeSuggestions.Count * 24.0f + 8.0f;
            }

            public bool MoveSelection(KeyboardKeys key)
            {
                if (_typeSuggestions.Count == 0)
                    return false;

                var direction = key == KeyboardKeys.ArrowUp ? -1 : 1;
                _selectedSuggestion = (_selectedSuggestion + direction + _typeSuggestions.Count) % _typeSuggestions.Count;
                _hoveredSuggestion = -1;
                return true;
            }

            public bool SelectCurrent()
            {
                if (_selectedSuggestion < 0 || _selectedSuggestion >= _typeSuggestions.Count)
                    return false;

                TypeSelected?.Invoke(_typeSuggestions[_selectedSuggestion].Type);
                return true;
            }

            public override void Draw()
            {
                var style = Style.Current;
                var bounds = new Rectangle(Float2.Zero, Size);
                StyleRendering.DrawRoundedRectangle(bounds, style.Background, style.BorderNormal, 1.0f, style.CornerRadius);
                if (_typeSuggestions.Count != 0)
                {
                    Render2D.DrawText(style.FontMedium, "TYPE SUGGESTIONS", new Rectangle(12, 8, Width - 24, 20), style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
                    for (int i = 0; i < _typeSuggestions.Count; i++)
                    {
                        var row = new Rectangle(6, 34 + i * 24, Width - 12, 22);
                        if (_hoveredSuggestion == i || _selectedSuggestion == i)
                            StyleRendering.FillRoundedRectangle(row, style.BackgroundHighlighted, style.CornerRadius);
                        var type = _typeSuggestions[i].Type;
                        var iconRect = new Rectangle(row.X + 6.0f, row.Y + 4.0f, 14.0f, 14.0f);
                        SemanticIcons.Draw(SemanticIcons.ForContent(type), iconRect, SemanticIcons.GetContentColor(type, style));
                        var typeText = type.ToString();
                        var textRect = new Rectangle(row.X + 26.0f, row.Y, row.Width - 32.0f, row.Height);
                        if (!string.IsNullOrEmpty(_typeQuery) && QueryFilterHelper.FuzzyMatch(_typeQuery, typeText, out _, out QueryFilterHelper.Range[] ranges))
                        {
                            var font = style.FontMedium;
                            for (int j = 0; j < ranges.Length; j++)
                            {
                                var start = font.GetCharPosition(typeText, ranges[j].StartIndex);
                                var end = font.GetCharPosition(typeText, ranges[j].EndIndex);
                                Render2D.FillRectangle(new Rectangle(textRect.X + start.X, textRect.Y + 3.0f, end.X - start.X, textRect.Height - 6.0f), style.ProgressNormal * 0.55f);
                            }
                        }
                        Render2D.DrawText(style.FontMedium, typeText, textRect, style.Foreground, TextAlignment.Near, TextAlignment.Center);
                    }
                    return;
                }
                Render2D.DrawText(style.FontMedium, "FILTERS", new Rectangle(12, 8, Width - 24, 20), style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
                Render2D.DrawText(style.FontMedium, "t:  Type", new Rectangle(12, 34, Width - 24, 22), style.Foreground, TextAlignment.Near, TextAlignment.Center);
                Render2D.DrawText(style.FontSmall, "Fuzzy-match asset and file types", new Rectangle(42, 52, Width - 54, 18), style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
                Render2D.DrawText(style.FontMedium, "name  Asset name", new Rectangle(12, 76, Width - 24, 22), style.Foreground, TextAlignment.Near, TextAlignment.Center);
                Render2D.DrawText(style.FontSmall, "Press Enter to apply and close", new Rectangle(42, 94, Width - 54, 18), style.ForegroundGrey, TextAlignment.Near, TextAlignment.Center);
            }

            public override void OnMouseMove(Float2 location)
            {
                _hoveredSuggestion = location.Y >= 34.0f ? Mathf.FloorToInt((location.Y - 34.0f) / 24.0f) : -1;
                if (_hoveredSuggestion < 0 || _hoveredSuggestion >= _typeSuggestions.Count)
                    _hoveredSuggestion = -1;
                base.OnMouseMove(location);
            }

            public override void OnMouseLeave()
            {
                _hoveredSuggestion = -1;
                base.OnMouseLeave();
            }

            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                if (button == MouseButton.Left && _hoveredSuggestion >= 0 && _hoveredSuggestion < _typeSuggestions.Count)
                {
                    _selectedSuggestion = _hoveredSuggestion;
                    SelectCurrent();
                    return true;
                }
                return base.OnMouseUp(location, button);
            }
        }

        private class ViewDropdown : ComboBox
        {
            public void OnClicked(int index)
            {
                OnItemClicked(index);
            }

            /// <inheritdoc />
            public override void Draw()
            {
                // Cache data
                var clientRect = new Rectangle(Float2.Zero, Size);
                float margin = clientRect.Height * 0.2f;
                bool isOpened = IsPopupOpened;
                bool enabled = EnabledInHierarchy;
                Color backgroundColor = BackgroundColor;
                Color borderColor = BorderColor;
                if (!enabled)
                {
                    backgroundColor *= 0.5f;
                }
                else if (isOpened || _mouseDown)
                {
                    backgroundColor = BackgroundColorSelected;
                    borderColor = BorderColorSelected;
                }
                else if (IsMouseOver)
                {
                    backgroundColor = BackgroundColorHighlighted;
                    borderColor = BorderColorHighlighted;
                }

                // Background
                var cornerRadius = Style.Current.CornerRadius;
                if (cornerRadius > 0.0f)
                    StyleRendering.DrawRoundedRectangle(clientRect, backgroundColor, borderColor, 1.0f, cornerRadius);
                else
                {
                    Render2D.FillRectangle(clientRect, backgroundColor);
                    Render2D.DrawRectangle(clientRect, borderColor);
                }

                // Compact view menu glyph (three dots) to match the panel toolbar language.
                float textScale = Height / DefaultHeight;
                var textRect = clientRect;
                Render2D.PushClip(textRect);
                var textColor = TextColor;
                Render2D.DrawText(Font.GetFont(), "•••", textRect, enabled ? textColor : textColor * 0.5f, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, textScale);
                Render2D.PopClip();
            }

            /// <inheritdoc />
            public override bool OnMouseUp(Float2 location, MouseButton button)
            {
                // Check flags
                if (_mouseDown && !_blockPopup)
                {
                    // Clear flag
                    _mouseDown = false;

                    // Ensure to have valid menu
                    if (_popupMenu == null)
                    {
                        _popupMenu = OnCreatePopup();
                        _popupMenu.MaximumItemsInViewCount = MaximumItemsInViewCount;
                        _popupMenu.VisibleChanged += cm =>
                        {
                            var win = Root;
                            _blockPopup = win != null && new Rectangle(Float2.Zero, Size).Contains(PointFromWindow(win.MousePosition));
                            if (!_blockPopup)
                                Focus();
                        };
                    }

                    // Check if menu hs been already shown
                    if (_popupMenu.Visible)
                    {
                        // Hide
                        _popupMenu.Hide();
                        return true;
                    }

                    // Show
                    _popupMenu.Show(this, new Float2(1, Height));
                }
                else
                {
                    _blockPopup = false;
                }

                return true;
            }
        }

        private void OnFoldersSearchBoxTextChanged()
        {
            // Skip events during setup or init stuff
            if (IsLayoutLocked)
                return;

            var root = _root;
            root.LockChildrenRecursive();
            _suppressExpandedStateSave = true;
            PerformLayout();

            // Update tree
            var query = _foldersSearchBox.Text;
            _searchHintsPanel?.SetQuery(query);
            if (_foldersSearchBox.IsFocused)
                ShowSearchHints();
            root.UpdateFilter(query);

            _suppressExpandedStateSave = false;
            root.UnlockChildrenRecursive();
            PerformLayout();
            PerformLayout();
        }

        private void ShowSearchHints()
        {
            if (_searchHintsPanel == null || _foldersSearchBox == null)
                return;
            _searchHintsPanel.SetQuery(_foldersSearchBox.Text);
            var width = Mathf.Max(240.0f, _foldersSearchBox.Width);
            _searchHintsPanel.Size = new Float2(width, _searchHintsPanel.Height);
            if (_searchHintsPanel.IsOpened)
            {
                _searchHintsPanel.PerformLayout(true);
                _foldersSearchBox.Focus();
                return;
            }
            _searchHintsPanel.Show(_foldersSearchBox, new Float2(0.0f, _foldersSearchBox.Height + 2.0f));
            _foldersSearchBox.Focus();
            AttachSearchHintsDismissHandler();
        }

        private void HideSearchHints()
        {
            _searchHintsPanel?.Hide();
            DetachSearchHintsDismissHandler();
        }

        private void OnSearchSubmitted()
        {
            if (!_searchHintsPanel.SelectCurrent())
                HideSearchHints();
        }

        private void OnSearchNavigation(KeyboardKeys key)
        {
            if (!_searchHintsPanel.IsOpened)
                ShowSearchHints();
            _searchHintsPanel.MoveSelection(key);
            _foldersSearchBox.Focus();
        }

        private void AttachSearchHintsDismissHandler()
        {
            var window = _foldersSearchBox?.RootWindow?.Window;
            if (_searchHintsWindow == window)
                return;
            DetachSearchHintsDismissHandler();
            _searchHintsWindow = window;
            if (_searchHintsWindow != null)
                _searchHintsWindow.MouseDown += OnSearchHintsWindowMouseDown;
        }

        private void DetachSearchHintsDismissHandler()
        {
            if (_searchHintsWindow != null)
                _searchHintsWindow.MouseDown -= OnSearchHintsWindowMouseDown;
            _searchHintsWindow = null;
        }

        private void OnSearchHintsWindowMouseDown(ref Float2 mousePosition, MouseButton button, ref bool handled)
        {
            if (_searchHintsPanel == null || !_searchHintsPanel.IsOpened || _foldersSearchBox == null)
                return;

            var searchLocation = _foldersSearchBox.PointFromScreen(FlaxEngine.Input.MouseScreenPosition);
            if (!new Rectangle(Float2.Zero, _foldersSearchBox.Size).Contains(searchLocation))
                HideSearchHints();
        }

        private void OnTypeSuggestionSelected(ContentItemSearchFilter type)
        {
            var terms = _foldersSearchBox.Text.Split(new[] { ' ' }, System.StringSplitOptions.RemoveEmptyEntries);
            int typeTerm = -1;
            for (int i = terms.Length - 1; i >= 0; i--)
            {
                if (terms[i].StartsWith("t:", System.StringComparison.OrdinalIgnoreCase))
                {
                    typeTerm = i;
                    break;
                }
            }
            if (typeTerm >= 0)
            {
                var usesSeparateValue = terms[typeTerm].Length == 2 && typeTerm + 1 < terms.Length;
                terms[typeTerm] = "t:" + type;
                // Accept both t:Model and the more natural t: Model spelling.
                if (usesSeparateValue)
                {
                    var compactTerms = new List<string>(terms.Length - 1);
                    for (int i = 0; i < terms.Length; i++)
                    {
                        if (i != typeTerm + 1)
                            compactTerms.Add(terms[i]);
                    }
                    terms = compactTerms.ToArray();
                }
            }
            else
            {
                var expanded = new string[terms.Length + 1];
                System.Array.Copy(terms, expanded, terms.Length);
                expanded[terms.Length] = "t:" + type;
                terms = expanded;
            }
            _foldersSearchBox.Text = string.Join(" ", terms) + " ";
            _foldersSearchBox.Focus();
            HideSearchHints();
        }

        private static string GetActiveTypeQuery(string query)
        {
            if (string.IsNullOrEmpty(query))
                return null;
            var terms = query.Split(new[] { ' ' }, System.StringSplitOptions.RemoveEmptyEntries);
            for (int i = terms.Length - 1; i >= 0; i--)
            {
                if (terms[i].StartsWith("t:", System.StringComparison.OrdinalIgnoreCase))
                {
                    var typeQuery = terms[i].Substring(2);
                    if (typeQuery.Length == 0 && i + 1 < terms.Length && !terms[i + 1].Contains(":"))
                        typeQuery = terms[i + 1];
                    return typeQuery;
                }
            }
            return query.EndsWith("t:", System.StringComparison.OrdinalIgnoreCase) ? string.Empty : null;
        }

        private static string GetTypeSearchText(ContentItemSearchFilter type)
        {
            switch (type)
            {
            case ContentItemSearchFilter.Model: return "Model mesh static geometry 3d object";
            case ContentItemSearchFilter.SkinnedModel: return "SkinnedModel skinned skeletal character rig animated mesh";
            case ContentItemSearchFilter.Material: return "Material surface look shader appearance";
            case ContentItemSearchFilter.Texture: return "Texture image bitmap sprite picture";
            case ContentItemSearchFilter.Scene: return "Scene level map world stage";
            case ContentItemSearchFilter.Prefab: return "Prefab template actor object archetype";
            case ContentItemSearchFilter.Script: return "Script code CSharp C++ source class";
            case ContentItemSearchFilter.Audio: return "Audio sound music voice clip";
            case ContentItemSearchFilter.Animation: return "Animation motion clip pose";
            case ContentItemSearchFilter.Json: return "Json data settings configuration config";
            case ContentItemSearchFilter.Particles: return "Particles particle VFX effect emitter visual";
            case ContentItemSearchFilter.Shader: return "Shader HLSL GPU graphics material source";
            default: return "Other file document unknown";
            }
        }

        /// <summary>
        /// Clears the items searching query text and filters.
        /// </summary>
        public void ClearItemsSearch()
        {
            // Skip if already cleared
            if (_itemsSearchBox.TextLength == 0)
                return;

            IsLayoutLocked = true;

            _itemsSearchBox.Clear();
            _viewDropdown.SelectedIndex = -1;

            IsLayoutLocked = false;

            UpdateItemsSearch();
        }

        private bool TryParseAssetId(string text, out AssetItem item)
        {
            item = null;
            if (text.Length != 32)
                return false;

            JsonSerializer.ParseID(text, out var id);
            item = Editor.ContentDatabase.FindAsset(id);
            return item != null;
        }

        private void UpdateItemsSearch()
        {
            // Skip events during setup or init stuff
            if (IsLayoutLocked)
                return;
            if (_showAllContentInTree)
            {
                RunWithContentSelectionHistorySuppressed(RefreshTreeItems);
                return;
            }

            // Check if clear filters
            if (_itemsSearchBox.TextLength == 0 && !_viewDropdown.HasSelection)
            {
                _view.IsSearching = false;
                RefreshView();
                return;
            }

            // Apply filter
            var items = new List<ContentItem>(8);
            var query = _itemsSearchBox.Text;
            var filters = new bool[_viewDropdown.Items.Count];
            if (_viewDropdown.HasSelection)
            {
                // Update filters flags
                for (int i = 0; i < filters.Length; i++)
                {
                    filters[i] = _viewDropdown.Selection.Contains(i);
                }
            }
            else
            {
                // No filters
                for (int i = 0; i < filters.Length; i++)
                {
                    filters[i] = true;
                }
            }

            // Search by filter only
            bool showAllFiles = _showAllFiles;
            if (string.IsNullOrWhiteSpace(query))
            {
                if (SelectedNode == _root)
                {
                    // Special case for root folder
                    for (int i = 0; i < _root.ChildrenCount; i++)
                    {
                        if (_root.GetChild(i) is ContentFolderTreeNode node)
                            UpdateItemsSearchFilter(node.Folder, items, filters, showAllFiles);
                    }
                }
                else
                {
                    UpdateItemsSearchFilter(CurrentViewFolder, items, filters, showAllFiles);
                }
            }
            // Search by asset ID
            else if (TryParseAssetId(query, out var assetItem))
            {
                items.Add(assetItem);
            }
            // Search by custom query text
            else
            {
                if (SelectedNode == _root)
                {
                    // Special case for root folder
                    for (int i = 0; i < _root.ChildrenCount; i++)
                    {
                        if (_root.GetChild(i) is ContentFolderTreeNode node)
                            UpdateItemsSearchFilter(node.Folder, items, filters, showAllFiles, query);
                    }
                }
                else
                {
                    UpdateItemsSearchFilter(CurrentViewFolder, items, filters, showAllFiles, query);
                }
            }

            _view.IsSearching = true;
            if (!string.IsNullOrWhiteSpace(query))
            {
                var scores = new Dictionary<ContentItem, float>(items.Count);
                for (int i = 0; i < items.Count; i++)
                    scores[items[i]] = GetSearchScore(items[i], query);
                items.Sort((a, b) =>
                {
                    var scoreCompare = scores[b].CompareTo(scores[a]);
                    return scoreCompare != 0 ? scoreCompare : string.Compare(a.ShortName, b.ShortName, System.StringComparison.OrdinalIgnoreCase);
                });
                RunWithContentSelectionHistorySuppressed(() => _view.ShowItems(items, FlaxEditor.Content.GUI.SortType.Relevance));
            }
            else
            {
                RunWithContentSelectionHistorySuppressed(() => _view.ShowItems(items, _sortType));
            }
        }

        private void UpdateItemsSearchFilter(ContentFolder folder, List<ContentItem> items, bool[] filters, bool showAllFiles)
        {
            for (int i = 0; i < folder.Children.Count; i++)
            {
                var child = folder.Children[i];
                if (child is ContentFolder childFolder)
                {
                    UpdateItemsSearchFilter(childFolder, items, filters, showAllFiles);
                }
                else if (filters[(int)child.SearchFilter] && (showAllFiles || !(child is FileItem)))
                {
                    items.Add(child);
                }
            }
        }

        private void UpdateItemsSearchFilter(ContentFolder folder, List<ContentItem> items, bool[] filters, bool showAllFiles, string filterText)
        {
            for (int i = 0; i < folder.Children.Count; i++)
            {
                var child = folder.Children[i];
                if (child is ContentFolder childFolder)
                {
                    UpdateItemsSearchFilter(childFolder, items, filters, showAllFiles, filterText);
                }
                else if (filters[(int)child.SearchFilter] && (showAllFiles || !(child is FileItem)) && MatchesSearchQuery(child, filterText))
                {
                    items.Add(child);
                }
            }
        }

        private static bool MatchesSearchQuery(ContentItem item, string query)
        {
            var terms = query.Split(new[] { ' ' }, System.StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < terms.Length; i++)
            {
                var term = terms[i];
                if (term.StartsWith("t:", System.StringComparison.OrdinalIgnoreCase))
                {
                    var typeQuery = term.Substring(2);
                    if (string.IsNullOrEmpty(typeQuery))
                        continue;
                    var typeText = item.TypeDescription + " " + item.GetType().Name + " " + item.ItemType + " " + GetTypeSearchText(item.SearchFilter);
                    if (!QueryFilterHelper.FuzzyMatch(typeQuery, typeText, out _, out _))
                        return false;
                }
                else
                {
                    var searchable = item.ShortName + " " + item.Path;
                    if (!QueryFilterHelper.FuzzyMatch(term, searchable, out _, out _))
                        return false;
                }
            }
            return true;
        }

        private static float GetSearchScore(ContentItem item, string query)
        {
            float result = 0.0f;
            var terms = query.Split(new[] { ' ' }, System.StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < terms.Length; i++)
            {
                var term = terms[i];
                string searchable;
                if (term.StartsWith("t:", System.StringComparison.OrdinalIgnoreCase))
                {
                    term = term.Substring(2);
                    if (term.Length == 0)
                        continue;
                    searchable = item.TypeDescription + " " + item.GetType().Name + " " + item.ItemType + " " + GetTypeSearchText(item.SearchFilter);
                }
                else
                {
                    searchable = item.ShortName + " " + item.Path;
                }
                if (QueryFilterHelper.FuzzyMatch(term, searchable, out var score, out _))
                    result += score;
            }
            return result;
        }
    }
}
