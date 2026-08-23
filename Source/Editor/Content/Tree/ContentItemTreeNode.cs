// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Content.GUI;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Drag;
using FlaxEditor.GUI.Tree;
using FlaxEditor.Utilities;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Content;

/// <summary>
/// Tree node for non-folder content items.
/// </summary>
public sealed class ContentItemTreeNode : TreeNode, IContentItemOwner, ITooltipPreviewProvider
{
    private const float RenameDelay = 1.0f;

    private List<Rectangle> _highlights;
    private float _pendingRenameTime = -1.0f;
    private readonly bool _useCanonicalSubAssetName;

    /// <summary>
    /// The content item.
    /// </summary>
    public ContentItem Item { get; }

    /// <inheritdoc />
    public SpriteHandle TooltipPreview => Item?.Thumbnail ?? SpriteHandle.Invalid;

    /// <summary>
    /// Initializes a new instance of the <see cref="ContentItemTreeNode"/> class.
    /// </summary>
    /// <param name="item">The content item.</param>
    /// <param name="useCanonicalSubAssetName">Whether to display only the canonical subasset name.</param>
    public ContentItemTreeNode(ContentItem item, bool useCanonicalSubAssetName = false)
    : base(false, Editor.Instance.Icons.Document128, Editor.Instance.Icons.Document128)
    {
        Item = item ?? throw new ArgumentNullException(nameof(item));
        _useCanonicalSubAssetName = useCanonicalSubAssetName;
        UpdateDisplayedName();
        IconColor = Color.Transparent; // Reserve icon space but draw custom thumbnail.
        Item.AddReference(this);
    }

    private static SpriteHandle GetIcon(ContentItem item)
    {
        if (item == null)
            return SpriteHandle.Invalid;
        var icon = item.Thumbnail;
        if (!icon.IsValid)
            icon = item.DefaultThumbnail;
        if (!icon.IsValid)
            icon = Editor.Instance.Icons.Document128;
        return icon;
    }

    /// <summary>
    /// Updates the query search filter.
    /// </summary>
    /// <param name="filterText">The filter text.</param>
    public void UpdateFilter(string filterText)
    {
        bool noFilter = string.IsNullOrWhiteSpace(filterText);
        bool isVisible;
        if (noFilter)
        {
            _highlights?.Clear();
            isVisible = true;
        }
        else
        {
            var terms = filterText.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            var nameTerms = new List<string>(terms.Length);
            var isTypeMatch = true;
            for (int i = 0; i < terms.Length; i++)
            {
                var term = terms[i];
                if (term.StartsWith("t:", StringComparison.OrdinalIgnoreCase))
                {
                    var typeQuery = term.Substring(2);
                    if (typeQuery.Length == 0 && i + 1 < terms.Length && !terms[i + 1].Contains(":"))
                        typeQuery = terms[++i];
                    if (typeQuery.Length != 0)
                    {
                        var typeText = Item.TypeDescription + " " + Item.SearchFilter + " " + Item.GetType().Name;
                        isTypeMatch &= QueryFilterHelper.FuzzyMatch(typeQuery, typeText, out _, out _);
                    }
                }
                else
                {
                    nameTerms.Add(term);
                }
            }

            var text = Text;
            QueryFilterHelper.Range[] ranges = null;
            var isNameMatch = nameTerms.Count == 0 || QueryFilterHelper.Match(string.Join(" ", nameTerms), text, out ranges);
            if (isTypeMatch && isNameMatch)
            {
                if (_highlights == null)
                    _highlights = new List<Rectangle>(ranges?.Length ?? 0);
                else
                    _highlights.Clear();
                if (ranges != null)
                {
                    var style = Style.Current;
                    var font = style.FontSmall;
                    var textRect = TextRect;
                    for (int i = 0; i < ranges.Length; i++)
                    {
                        var start = font.GetCharPosition(text, ranges[i].StartIndex);
                        var end = font.GetCharPosition(text, ranges[i].EndIndex);
                        _highlights.Add(new Rectangle(start.X + textRect.X, textRect.Y, end.X - start.X, textRect.Height));
                    }
                }
                isVisible = true;
            }
            else
            {
                _highlights?.Clear();
                isVisible = false;
            }
        }

        bool isAnyChildVisible = false;
        for (int i = 0; i < _children.Count; i++)
        {
            if (_children[i] is ContentItemTreeNode child)
            {
                child.UpdateFilter(filterText);
                isAnyChildVisible |= child.Visible;
            }
        }

        if (!noFilter)
        {
            if (isAnyChildVisible)
                Expand(true);
            else if (HasChildren)
                Collapse(true);
        }

        Visible = isVisible || isAnyChildVisible;
    }

    /// <inheritdoc />
    public override void Draw()
    {
        base.Draw();

        var style = Style.Current;
        var contentWindow = Editor.Instance.Windows.ContentWin;
        var scale = contentWindow != null && contentWindow.IsTreeOnlyMode ? contentWindow.View.ViewScale : 1.0f;
        var iconSize = Mathf.Min(Mathf.Max(0.0f, style.GetContentTreeIconSize() * scale), Mathf.Max(0.0f, HeaderHeight - 4.0f));
        var textRect = TextRect;

        var icon = GetIcon(Item);
        if (icon.IsValid && iconSize > 0.0f)
        {
            var iconRect = new Rectangle(textRect.Left - iconSize - 2.0f, (HeaderHeight - iconSize) * 0.5f, iconSize, iconSize);
            Render2D.DrawSprite(icon, iconRect);
        }

        var typeText = Item.TypeDescription;
        if (!string.IsNullOrWhiteSpace(typeText))
        {
            const float TypeGap = 8.0f;
            const float MinimumTypeWidth = 12.0f;
            const float PreferredTypeWidth = 112.0f;
            const float RightPadding = 6.0f;

            var titleWidth = TextFont.GetFont().MeasureText(Text ?? string.Empty).X;
            var preferredTypeX = Mathf.Max(textRect.X, Width - PreferredTypeWidth);
            var typeX = Mathf.Max(preferredTypeX, textRect.X + titleWidth + TypeGap);
            var typeRight = Width - RightPadding;
            if (typeX + MinimumTypeWidth <= typeRight)
            {
                var typeRect = new Rectangle(typeX, 0.0f, typeRight - typeX, HeaderHeight);
                var displayType = ContentItem.TruncateText(style.FontSmall, typeText, Mathf.Max(0.0f, typeRect.Width - 2.0f));
                var typeColor = Color.Lerp(style.Background, style.Foreground, 0.2f);
                Render2D.PushClip(ref typeRect);
                Render2D.DrawText(style.FontSmall, displayType, typeRect, typeColor, TextAlignment.Far, TextAlignment.Center, TextWrapping.NoWrap);
                Render2D.PopClip();
            }
        }

        if (_highlights != null)
        {
            var color = style.ProgressNormal * 0.6f;
            for (int i = 0; i < _highlights.Count; i++)
                Render2D.FillRectangle(_highlights[i], color);
        }
    }

    /// <inheritdoc />
    protected override bool OnMouseDoubleClickHeader(ref Float2 location, MouseButton button)
    {
        _pendingRenameTime = -1.0f;
        if (button == MouseButton.Left)
        {
            Editor.Instance.Windows.ContentWin.Open(Item);
            return true;
        }

        return base.OnMouseDoubleClickHeader(ref location, button);
    }

    /// <inheritdoc />
    protected override void OnSelectedClickHeader()
    {
        _pendingRenameTime = Item.CanRename ? Time.UnscaledGameTime + RenameDelay : -1.0f;
    }

    /// <inheritdoc />
    protected override void OnExpandedChanged()
    {
        base.OnExpandedChanged();
        if (HasChildren)
            Editor.Instance?.Windows?.ContentWin?.OnContentTreeItemNodeExpandedChanged(this, IsExpanded);
    }

    /// <inheritdoc />
    public override void Update(float deltaTime)
    {
        base.Update(deltaTime);

        if (_pendingRenameTime >= 0.0f && Time.UnscaledGameTime >= _pendingRenameTime)
        {
            _pendingRenameTime = -1.0f;
            var tree = ParentTree;
            if (tree != null && tree.ContainsFocus && tree.Selection.Count == 1 && tree.SelectedNode == this && Item.CanRename)
                Editor.Instance.Windows.ContentWin.Rename(Item);
        }
    }

    /// <inheritdoc />
    protected override void DoDragDrop()
    {
        DoDragDrop(DragItems.GetDragData(TreeViewPanel.GetDragItems(this, Item)));
    }

    /// <inheritdoc />
    protected override bool ShowTooltip => true;

    /// <inheritdoc />
    public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
    {
        Item.UpdateTooltipText();
        TooltipText = Item.TooltipText;
        return base.OnShowTooltip(out text, out location, out area);
    }

    /// <inheritdoc />
    void IContentItemOwner.OnItemDeleted(ContentItem item)
    {
    }

    /// <inheritdoc />
    void IContentItemOwner.OnItemRenamed(ContentItem item)
    {
        UpdateDisplayedName();
    }

    /// <inheritdoc />
    void IContentItemOwner.OnItemReimported(ContentItem item)
    {
    }

    /// <inheritdoc />
    void IContentItemOwner.OnItemDispose(ContentItem item)
    {
    }

    /// <inheritdoc />
    public override int Compare(Control other)
    {
        if (other is ContentFolderTreeNode)
            return 1;
        if (other is ContentItemTreeNode otherItem)
            return ApplySortOrder(string.Compare(Text, otherItem.Text, StringComparison.InvariantCulture));
        return base.Compare(other);
    }

    /// <inheritdoc />
    public override void OnDestroy()
    {
        Item.RemoveReference(this);
        base.OnDestroy();
    }

    /// <summary>
    /// Updates the text of the node.
    /// </summary>
    public void UpdateDisplayedName()
    {
        var contentWindow = Editor.Instance?.Windows?.ContentWin;
        var showExtensions = contentWindow?.View?.ShowFileExtensions ?? true;
        if (_useCanonicalSubAssetName && Item is AssetItem { IsCanonicalSubAsset: true } assetItem)
            Text = assetItem.CanonicalSubAssetName;
        else
            Text = Item.ShowFileExtension || showExtensions ? Item.FileName : Item.ShortName;
    }

    private static SortType GetSortType()
    {
        return Editor.Instance?.Windows?.ContentWin?.CurrentSortType ?? SortType.AlphabeticOrder;
    }

    private static int ApplySortOrder(int result)
    {
        return GetSortType() == SortType.AlphabeticReverse ? -result : result;
    }
}
