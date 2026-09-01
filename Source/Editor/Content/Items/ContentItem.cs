// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using FlaxEditor.Content.GUI;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Drag;
using FlaxEngine;
using FlaxEngine.Assertions;
using FlaxEngine.GUI;

namespace FlaxEditor.Content
{
    /// <summary>
    /// Content item types.
    /// </summary>
    [HideInEditor]
    public enum ContentItemType
    {
        /// <summary>
        /// The binary or text asset.
        /// </summary>
        Asset,

        /// <summary>
        /// The directory.
        /// </summary>
        Folder,

        /// <summary>
        /// The script file.
        /// </summary>
        Script,

        /// <summary>
        /// The scene file.
        /// </summary>
        Scene,

        /// <summary>
        /// The other type.
        /// </summary>
        Other,
    }

    /// <summary>
    /// Content item filter types used for searching.
    /// </summary>
    [HideInEditor]
    public enum ContentItemSearchFilter
    {
        /// <summary>
        /// The model.
        /// </summary>
        Model,

        /// <summary>
        /// The skinned model.
        /// </summary>
        SkinnedModel,

        /// <summary>
        /// The material.
        /// </summary>
        Material,

        /// <summary>
        /// The texture.
        /// </summary>
        Texture,

        /// <summary>
        /// The scene.
        /// </summary>
        Scene,

        /// <summary>
        /// The prefab.
        /// </summary>
        Prefab,

        /// <summary>
        /// The script.
        /// </summary>
        Script,

        /// <summary>
        /// The audio.
        /// </summary>
        Audio,

        /// <summary>
        /// The animation.
        /// </summary>
        Animation,

        /// <summary>
        /// The json.
        /// </summary>
        Json,

        /// <summary>
        /// The particles.
        /// </summary>
        Particles,

        /// <summary>
        /// The shader source files.
        /// </summary>
        Shader,

        /// <summary>
        /// The other.
        /// </summary>
        Other,
    }

    /// <summary>
    /// Interface for objects that can reference the content items in order to receive events from them.
    /// </summary>
    [HideInEditor]
    public interface IContentItemOwner
    {
        /// <summary>
        /// Called when referenced item gets deleted (asset unloaded, file deleted, etc.).
        /// Item should not be used after that.
        /// </summary>
        /// <param name="item">The item.</param>
        void OnItemDeleted(ContentItem item);

        /// <summary>
        /// Called when referenced item gets renamed (filename change, path change, etc.)
        /// </summary>
        /// <param name="item">The item.</param>
        void OnItemRenamed(ContentItem item);

        /// <summary>
        /// Called when item gets reimported or reloaded.
        /// </summary>
        /// <param name="item">The item.</param>
        void OnItemReimported(ContentItem item);

        /// <summary>
        /// Called when referenced item gets disposed (editor closing, database internal changes, etc.).
        /// Item should not be used after that.
        /// </summary>
        /// <param name="item">The item.</param>
        void OnItemDispose(ContentItem item);
    }

    /// <summary>
    /// Base class for all content items.
    /// Item parent GUI control is always <see cref="ContentView"/> or null if not in a view.
    /// </summary>
    /// <seealso cref="FlaxEngine.GUI.Control" />
    [HideInEditor]
    public abstract class ContentItem : Control, ITooltipPreviewProvider
    {
        private const float TargetHighlightScale = 1.25f;
        private const float HighlightScaleAnimDuration = 0.85f;
        private const float ListVerticalInset = 1.0f;

        /// <summary>
        /// The default margin size.
        /// </summary>
        public const int DefaultMarginSize = 4;

        /// <summary>
        /// The default text height.
        /// </summary>
        public const int DefaultTextHeight = 42;

        /// <summary>
        /// The default thumbnail size.
        /// </summary>
        public const int DefaultThumbnailSize = 128;

        /// <summary>
        /// The default width.
        /// </summary>
        public const int DefaultWidth = (DefaultThumbnailSize + 2 * DefaultMarginSize);

        /// <summary>
        /// The default height.
        /// </summary>
        public const int DefaultHeight = (DefaultThumbnailSize + 2 * DefaultMarginSize + DefaultTextHeight);

        /// <summary>
        /// Whether the item is being but.
        /// </summary>
        public bool IsBeingCut;

        private ContentFolder _parentFolder;

        private bool _isMouseDown;
        private Float2 _mouseDownStartPos;
        private bool _isHighlighted;
        private float _targetHighlightTimeSec;
        private float _currentHighlightTimeSec;
        private float _debounceHighlightTime;
        private float _highlightScale;
        private static ContentItem _lastHighlightedItem;
        private readonly List<IContentItemOwner> _references = new List<IContentItemOwner>(4);

        private SpriteHandle _shadowIcon;

        /// <summary>
        /// Gets the type of the item.
        /// </summary>
        public abstract ContentItemType ItemType { get; }

        /// <summary>
        /// Gets the type of the item searching filter to use.
        /// </summary>
        public abstract ContentItemSearchFilter SearchFilter { get; }

        /// <summary>
        /// Gets a value indicating whether this instance is asset.
        /// </summary>
        public bool IsAsset => ItemType == ContentItemType.Asset;

        /// <summary>
        /// Gets a value indicating whether this instance is folder.
        /// </summary>
        public bool IsFolder => ItemType == ContentItemType.Folder;

        /// <summary>
        /// Gets a value indicating whether this instance can have children.
        /// </summary>
        public bool CanHaveChildren => ItemType == ContentItemType.Folder;

        /// <summary>
        /// Determines whether this item can be renamed.
        /// </summary>
        public virtual bool CanRename => true;

        /// <summary>
        /// Gets a value indicating whether this item can be dragged and dropped.
        /// </summary>
        public virtual bool CanDrag => Root != null;

        /// <summary>
        /// Gets a value indicating whether this <see cref="ContentItem"/> exists on drive.
        /// </summary>
        public virtual bool Exists => System.IO.File.Exists(Path);

        /// <summary>
        /// Gets the parent folder.
        /// </summary>
        public ContentFolder ParentFolder
        {
            get => _parentFolder;
            set
            {
                if (_parentFolder == value)
                    return;

                // Remove from old
                _parentFolder?.Children.Remove(this);

                // Link
                _parentFolder = value;

                // Add to new
                _parentFolder?.Children.Add(this);

                OnParentFolderChanged();
            }
        }

        /// <summary>
        /// Gets the path to the item.
        /// </summary>
        public string Path { get; private set; }

        /// <summary>
        /// Gets the item file name (filename with extension).
        /// </summary>
        public string FileName { get; internal set; }

        /// <summary>
        /// Gets the item short name (filename without extension).
        /// </summary>
        public string ShortName { get; internal set; }

        /// <summary>
        /// Gets the asset name relative to the project root folder (without asset file extension)
        /// </summary>
        public string NamePath => FlaxEditor.Utilities.Utils.GetAssetNamePath(Path);

        /// <summary>
        /// Gets the content item type description (for UI).
        /// </summary>
        public abstract string TypeDescription { get; }

        /// <summary>
        /// Gets the default static icon of the content item.
        /// </summary>
        public virtual SpriteHandle DefaultThumbnail => Editor.Instance.Icons.Document128;

        private SpriteHandle PresentationIcon => DefaultThumbnail.IsValid ? DefaultThumbnail : Editor.Instance.Icons.Document128;

        /// <inheritdoc />
        public SpriteHandle TooltipPreview => PresentationIcon;

        /// <summary>
        /// True if force show file extension.
        /// </summary>
        public bool ShowFileExtension;

        /// <summary>
        /// Initializes a new instance of the <see cref="ContentItem"/> class.
        /// </summary>
        /// <param name="path">The path to the item.</param>
        protected ContentItem(string path)
        : base(0, 0, DefaultWidth, DefaultHeight)
        {
            // Set path
            Path = path;
            FileName = System.IO.Path.GetFileName(path);
            ShortName = System.IO.Path.GetFileNameWithoutExtension(path);
        }

        /// <summary>
        /// Updates the item path. Use with caution or even don't use it. It's dangerous.
        /// </summary>
        /// <param name="value">The new path.</param>
        internal virtual void UpdatePath(string value)
        {
            // Set path
            Path = StringUtils.NormalizePath(value);
            FileName = System.IO.Path.GetFileName(value);
            ShortName = System.IO.Path.GetFileNameWithoutExtension(value);

            // Fire event
            OnPathChanged();
            for (int i = 0; i < _references.Count; i++)
            {
                _references[i].OnItemRenamed(this);
            }
        }

        /// <summary>
        /// Updates the tooltip text text.
        /// </summary>
        public virtual void UpdateTooltipText()
        {
            var sb = new StringBuilder();
            OnBuildTooltipText(sb);
            if (sb.Length != 0 && sb[sb.Length - 1] == '\n')
            {
                // Remove new-line from end
                int sub = 1;
                if (sb.Length != 1 && sb[sb.Length - 2] == '\r')
                    sub = 2;
                sb.Length -= sub;
            }
            TooltipText = sb.ToString();
        }

        /// <summary>
        /// Called when building tooltip text.
        /// </summary>
        /// <param name="sb">The output string builder.</param>
        protected virtual void OnBuildTooltipText(StringBuilder sb)
        {
            sb.Append("Type: ").Append(TypeDescription).AppendLine();
            if (File.Exists(Path))
                sb.Append("Size: ").Append(Utilities.Utils.FormatBytesCount((ulong)new FileInfo(Path).Length)).AppendLine();
            sb.Append("Path: ").Append(Utilities.Utils.GetAssetNamePathWithExt(Path)).AppendLine();
        }

        /// <summary>
        /// Tries to find the item at the specified path.
        /// </summary>
        /// <param name="path">The path.</param>
        /// <returns>Found item or null if missing.</returns>
        public virtual ContentItem Find(string path)
        {
            return Path == path ? this : null;
        }

        /// <summary>
        /// Tries to find a specified item in the assets tree.
        /// </summary>
        /// <param name="item">The item.</param>
        /// <returns>True if has been found, otherwise false.</returns>
        public virtual bool Find(ContentItem item)
        {
            return this == item;
        }

        /// <summary>
        /// Tries to find the item with the specified id.
        /// </summary>
        /// <param name="id">The id.</param>
        /// <returns>Found item or null if missing.</returns>
        public virtual ContentItem Find(Guid id)
        {
            return null;
        }

        /// <summary>
        /// Tries to find script with the given name.
        /// </summary>
        /// <param name="scriptName">Name of the script.</param>
        /// <returns>Found script or null if missing.</returns>
        public virtual ScriptItem FindScriptWitScriptName(string scriptName)
        {
            return null;
        }

        /// <summary>
        /// Gets a value indicating whether draw item shadow.
        /// </summary>
        protected virtual bool DrawShadow => false;

        /// <summary>
        /// Gets the local space rectangle for element name text area.
        /// </summary>
        public Rectangle TextRectangle
        {
            get
            {
                // Skip when hidden
                if (!Visible)
                    return Rectangle.Empty;
                var view = Parent as ContentView;
                var size = Size;
                switch (view?.ViewType ?? ContentViewType.Tiles)
                {
                case ContentViewType.Tiles:
                {
                    var textHeight = DefaultTextHeight * size.X / DefaultWidth;
                    return new Rectangle(0, size.Y - textHeight, size.X, textHeight);
                }
                case ContentViewType.List:
                {
                    var thumbnailSize = Mathf.Max(0.0f, size.Y - ListVerticalInset * 2.0f);
                    var textHeight = Mathf.Min(size.Y, 24.0f);
                    var semanticIconSize = Mathf.Clamp(thumbnailSize * 0.55f, 12.0f, 20.0f);
                    var textX = DefaultMarginSize + semanticIconSize + 3.0f + thumbnailSize + DefaultMarginSize;
                    return new Rectangle(textX, (size.Y - textHeight) * 0.5f, Mathf.Max(0.0f, size.X - textX - DefaultMarginSize), textHeight);
                }
                default: throw new ArgumentOutOfRangeException();
                }
            }
        }

        /// <summary>
        /// Draws the item thumbnail.
        /// </summary>
        /// <param name="rectangle">The thumbnail rectangle.</param>
        public void DrawThumbnail(ref Rectangle rectangle)
        {
            // Draw shadow
            if (DrawShadow)
            {
                const float thumbnailInShadowSize = 50.0f;
                var shadowRect = rectangle.MakeExpanded((DefaultThumbnailSize - thumbnailInShadowSize) * rectangle.Width / DefaultThumbnailSize * 1.3f);
                if (!_shadowIcon.IsValid)
                    _shadowIcon = Editor.Instance.Icons.AssetShadow128;
                Render2D.DrawSprite(_shadowIcon, shadowRect);
            }

            // Draw thumbnail
            var thumbnail = PresentationIcon;
            if (thumbnail.IsValid)
                Render2D.DrawSprite(thumbnail, rectangle);
            else
                Render2D.FillRectangle(rectangle, Color.Black);

            DrawThumbnailAccent(ref rectangle);
        }

        /// <summary>
        /// Draws the item thumbnail.
        /// </summary>
        /// <param name="rectangle">The thumbnail rectangle.</param>
        /// /// <param name="shadow">Whether or not to draw the shadow. Overrides DrawShadow.</param>
        public void DrawThumbnail(ref Rectangle rectangle, bool shadow)
        {
            // Draw shadow
            if (shadow)
            {
                const float thumbnailInShadowSize = 50.0f;
                var shadowRect = rectangle.MakeExpanded((DefaultThumbnailSize - thumbnailInShadowSize) * rectangle.Width / DefaultThumbnailSize * 1.3f);
                if (!_shadowIcon.IsValid)
                    _shadowIcon = Editor.Instance.Icons.AssetShadow128;
                Render2D.DrawSprite(_shadowIcon, shadowRect);
            }

            // Draw thumbnail
            var thumbnail = PresentationIcon;
            if (thumbnail.IsValid)
                Render2D.DrawSprite(thumbnail, rectangle);
            else
                Render2D.FillRectangle(rectangle, Color.Black);

            DrawThumbnailAccent(ref rectangle);
        }

        private void DrawThumbnailAccent(ref Rectangle rectangle)
        {
            if (SearchFilter != ContentItemSearchFilter.Prefab)
                return;

            var height = Mathf.Max(2.0f, rectangle.Height * 0.08f);
            var accentRect = new Rectangle(rectangle.X, rectangle.Bottom - height, rectangle.Width, height);
            Render2D.FillRectangle(accentRect, SemanticIcons.GetContentColor(SearchFilter, Style.Current));
        }

        /// <summary>
        /// Gets the amount of references to that item.
        /// </summary>
        public int ReferencesCount => _references.Count;

        /// <summary>
        /// Adds the reference to the item.
        /// </summary>
        /// <param name="obj">The object.</param>
        public void AddReference(IContentItemOwner obj)
        {
            Assert.IsNotNull(obj);
            Assert.IsFalse(_references.Contains(obj));

            _references.Add(obj);
        }

        /// <summary>
        /// Removes the reference from the item.
        /// </summary>
        /// <param name="obj">The object.</param>
        public void RemoveReference(IContentItemOwner obj)
        {
            _references.Remove(obj);
        }

        /// <summary>
        /// Called when context menu is being prepared to show. Can be used to add custom options.
        /// </summary>
        /// <param name="menu">The menu.</param>
        public virtual void OnContextMenu(FlaxEditor.GUI.ContextMenu.ContextMenu menu)
        {
        }

        /// <summary>
        /// Called when item gets renamed or location gets changed (path modification).
        /// </summary>
        public virtual void OnPathChanged()
        {
        }

        /// <summary>
        /// Called when content item gets removed (by the user or externally).
        /// </summary>
        public virtual void OnDelete()
        {
            // Fire event
            while (_references.Count > 0)
            {
                var reference = _references[0];
                reference.OnItemDeleted(this);
                RemoveReference(reference);
            }

        }

        /// <summary>
        /// Called when item parent folder gets changed.
        /// </summary>
        protected virtual void OnParentFolderChanged()
        {
        }

        /// <summary>
        /// Adds a temporary animated highlight around the item.
        /// </summary>
        /// <param name="durationSec">The duration of the highlight in seconds.</param>
        public void StartHighlight(float durationSec = 0.5f)
        {
            if (_lastHighlightedItem != null && _lastHighlightedItem != this && !_lastHighlightedItem.IsDisposing)
                _lastHighlightedItem.StopHighlight();

            _isHighlighted = true;
            _targetHighlightTimeSec = durationSec;
            _currentHighlightTimeSec = 0;
            _debounceHighlightTime = 0;
            _highlightScale = 2.0f;
            _lastHighlightedItem = this;
        }

        /// <summary>
        /// Stops any current highlight.
        /// </summary>
        public void StopHighlight()
        {
            _isHighlighted = false;
            _targetHighlightTimeSec = 0;
            _currentHighlightTimeSec = 0;
            _debounceHighlightTime = 0;
            if (_lastHighlightedItem == this)
                _lastHighlightedItem = null;
        }

        /// <inheritdoc />
        public override void Update(float deltaTime)
        {
            if (_isHighlighted)
            {
                _debounceHighlightTime += deltaTime;
                _currentHighlightTimeSec += deltaTime;

                if (_currentHighlightTimeSec < HighlightScaleAnimDuration)
                    _highlightScale = Mathf.Lerp(_highlightScale, TargetHighlightScale, _currentHighlightTimeSec);

                if (_currentHighlightTimeSec >= _targetHighlightTimeSec)
                    StopHighlight();
            }

            base.Update(deltaTime);
        }

        /// <summary>
        /// Called when item gets reimported or reloaded.
        /// </summary>
        protected virtual void OnReimport()
        {
            for (int i = 0; i < _references.Count; i++)
                _references[i].OnItemReimported(this);
        }

        /// <summary>
        /// Does the drag and drop operation with this asset.
        /// </summary>
        protected virtual void DoDrag()
        {
            if (!CanDrag)
                return;

            DragData data;

            // Check if is selected
            if (Parent is ContentView view && view.IsSelected(this))
            {
                // Drag selected item
                data = DragItems.GetDragData(view.Selection);
            }
            else
            {
                // Drag single item
                data = DragItems.GetDragData(this);
            }

            // Start drag operation
            DoDragDrop(data);
        }

        /// <inheritdoc />
        protected override bool ShowTooltip => true;

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            UpdateTooltipText();
            var result = base.OnShowTooltip(out text, out _, out area);
            location = Size * new Float2(0.9f, 0.5f);
            return result;
        }

        /// <inheritdoc />
        public override void NavigationFocus()
        {
            base.NavigationFocus();

            if (IsFocused)
                (Parent as ContentView)?.Select(this);
        }

        private Rectangle GetHighlightRect(ContentView view, Rectangle clientRect, Rectangle textRect, string displayName)
        {
            Rectangle result;
            if (view.ViewType == ContentViewType.List)
            {
                var nameWidth = Style.Current.FontMedium.MeasureText(displayName).X;
                var right = Mathf.Min(clientRect.Right, textRect.X + nameWidth + DefaultMarginSize);
                result = new Rectangle(clientRect.X, clientRect.Y, Mathf.Max(textRect.X + 12.0f, right), clientRect.Height);
            }
            else
            {
                result = clientRect;
            }
            result.Scale(_highlightScale);
            return result;
        }

        private void DrawHighlight(Rectangle rect, bool border)
        {
            if (!_isHighlighted || _debounceHighlightTime <= 0.1f)
                return;

            var color = Editor.Instance.Options.Options.Visual.HighlightColor;
            if (border)
                Render2D.DrawRectangle(rect, color, 3);
            else
                Render2D.FillRectangle(rect, color.AlphaMultiplied(0.3f));
        }

        /// <inheritdoc />
        public override void Draw()
        {
            var size = Size;
            var style = Style.Current;
            var view = Parent as ContentView;
            var isSelected = view.IsSelected(this);
            var clientRect = new Rectangle(Float2.Zero, size);
            var textRect = TextRectangle;
            var displayName = ShowFileExtension || view.ShowFileExtensions ? FileName : ShortName;
            var highlightRect = GetHighlightRect(view, clientRect, textRect, displayName);
            Rectangle thumbnailRect;
            TextAlignment nameAlignment;
            switch (view.ViewType)
            {
            case ContentViewType.Tiles:
            {
                var thumbnailSize = size.X;
                thumbnailRect = new Rectangle(0, 0, thumbnailSize, thumbnailSize);
                nameAlignment = TextAlignment.Center;

                if (this is ContentFolder)
                {
                    // Small shadow
                    var shadowRect = new Rectangle(2, 2, clientRect.Width + 1, clientRect.Height + 1);
                    var color = Color.Black.AlphaMultiplied(0.2f);
                    Render2D.FillRectangle(shadowRect, color);
                    Render2D.FillRectangle(clientRect, style.Background.RGBMultiplied(1.25f));

                    if (isSelected)
                        Render2D.FillRectangle(clientRect, Parent.ContainsFocus ? style.BackgroundSelected : style.SecondaryBackground);
                    else if (IsMouseOver)
                        Render2D.FillRectangle(clientRect, style.BackgroundHighlighted);

                    DrawThumbnail(ref thumbnailRect, false);
                }
                else
                {
                    // Small shadow
                    var shadowRect = new Rectangle(2, 2, clientRect.Width + 1, clientRect.Height + 1);
                    var color = Color.Black.AlphaMultiplied(0.2f);
                    Render2D.FillRectangle(shadowRect, color);

                    Render2D.FillRectangle(clientRect, style.Background.RGBMultiplied(1.25f));
                    Render2D.FillRectangle(TextRectangle, style.SecondaryBackground);

                    var accentHeight = 2 * view.ViewScale;
                    var barRect = new Rectangle(0, thumbnailRect.Height - accentHeight, clientRect.Width, accentHeight);
                    Render2D.FillRectangle(barRect, Color.DimGray);

                    DrawThumbnail(ref thumbnailRect, false);
                    if (isSelected)
                    {
                        Render2D.FillRectangle(textRect, Parent.ContainsFocus ? style.BackgroundSelected : style.SecondaryBackground);
                        StyleRendering.DrawRoundedRectangleBorder(clientRect, Parent.ContainsFocus ? style.BackgroundSelected : style.SecondaryBackground, 1.0f, style.CornerRadius);
                    }
                    else if (IsMouseOver)
                    {
                        Render2D.FillRectangle(textRect, style.BackgroundHighlighted);
                        StyleRendering.DrawRoundedRectangleBorder(clientRect, style.BackgroundHighlighted, 1.0f, style.CornerRadius);
                    }
                }
                break;
            }
            case ContentViewType.List:
            {
                var thumbnailSize = Mathf.Max(0.0f, size.Y - ListVerticalInset * 2.0f);
                var semanticIconSize = Mathf.Clamp(thumbnailSize * 0.55f, 12.0f, 20.0f);
                thumbnailRect = new Rectangle(DefaultMarginSize + semanticIconSize + 3.0f, ListVerticalInset, thumbnailSize, thumbnailSize);
                nameAlignment = TextAlignment.Near;

                if (!isSelected && !IsMouseOver && Editor.Instance.Options.Options.Interface.AlternatingTreeRows && (IndexInParent & 1) != 0)
                    Render2D.FillRectangle(clientRect, Color.Lerp(style.Background, style.Foreground, 0.02f));
                if (isSelected)
                    Render2D.FillRectangle(clientRect, Parent.ContainsFocus ? style.BackgroundSelected : style.SecondaryBackground);
                else if (IsMouseOver)
                    Render2D.FillRectangle(clientRect, style.BackgroundHighlighted);

                var semanticIconRect = new Rectangle(DefaultMarginSize, (size.Y - semanticIconSize) * 0.5f, semanticIconSize, semanticIconSize);
                SemanticIcons.Draw(SemanticIcons.ForContent(SearchFilter), semanticIconRect, SemanticIcons.GetContentColor(SearchFilter, style));
                DrawThumbnail(ref thumbnailRect);
                break;
            }
            default: throw new ArgumentOutOfRangeException();
            }

            DrawHighlight(highlightRect, false);

            // Draw a type label directly after the short name when there is room for both.
            if (view.ViewType == ContentViewType.List && !string.IsNullOrWhiteSpace(TypeDescription))
            {
                var typeX = textRect.X + style.FontMedium.MeasureText(displayName).X + 8.0f;
                if (typeX + 12.0f <= textRect.Right)
                {
                    var typeRect = new Rectangle(typeX, textRect.Y, textRect.Right - typeX, textRect.Height);
                    var typeText = TruncateText(style.FontSmall, TypeDescription, Mathf.Max(0.0f, typeRect.Width - 2.0f));
                    var typeColor = Color.Lerp(style.Background, style.Foreground, 0.2f);
                    Render2D.PushClip(ref typeRect);
                    Render2D.DrawText(style.FontSmall, typeText, typeRect, typeColor, TextAlignment.Near, TextAlignment.Center, TextWrapping.NoWrap);
                    Render2D.PopClip();
                }
            }
            Render2D.PushClip(ref textRect);
            Render2D.DrawText(style.FontMedium, displayName, textRect, style.Foreground, nameAlignment, TextAlignment.Center, view.ViewType == ContentViewType.List ? TextWrapping.NoWrap : TextWrapping.WrapWords);
            Render2D.PopClip();

            DrawHighlight(highlightRect, true);

            if (IsBeingCut)
            {
                var color = style.SecondaryBackground.AlphaMultiplied(0.5f);
                Render2D.FillRectangle(clientRect, color);
            }
        }

        internal static string TruncateText(Font font, string text, float width)
        {
            if (string.IsNullOrEmpty(text) || width <= 0.0f)
                return string.Empty;
            if (font.MeasureText(text).X <= width)
                return text;

            const string ellipsis = "...";
            var ellipsisWidth = font.MeasureText(ellipsis).X;
            if (ellipsisWidth >= width)
                return ellipsis;

            int low = 0;
            int high = text.Length;
            while (low < high)
            {
                int middle = (low + high + 1) / 2;
                if (font.MeasureText(text.Substring(0, middle)).X + ellipsisWidth <= width)
                    low = middle;
                else
                    high = middle - 1;
            }
            return text.Substring(0, low) + ellipsis;
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            ContentMutationDiagnostics.Log("input.item.mouse-down", $"button={button}; item='{Path}'; location={location}; selected={(Parent as ContentView)?.IsSelected(this)}");
            Focus();
            (Parent as ContentView)?.OnItemMouseDown();

            if (button == MouseButton.Left)
            {
                // Cache data
                _isMouseDown = true;
                _mouseDownStartPos = location;
            }

            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            ContentMutationDiagnostics.Log("input.item.mouse-up", $"button={button}; item='{Path}'; location={location}; armed={_isMouseDown}");
            if (button == MouseButton.Left && _isMouseDown)
            {
                // Clear flag
                _isMouseDown = false;

                // A click only completes over the same live item. This prevents a release
                // from mutating selection after a popup, navigation or asset editor changed UI.
                if (new Rectangle(Float2.Zero, Size).Contains(location) && Parent is ContentView view)
                    view.OnItemClick(this);
            }

            return base.OnMouseUp(location, button);
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            ContentMutationDiagnostics.Log("input.item.double-click", $"button={button}; item='{Path}'; location={location}");
            Focus();

            if (button == MouseButton.Left && new Rectangle(Float2.Zero, Size).Contains(location))
            {
                // The input backend sends a release after the double-click notification.
                // Disarm the ordinary click first so opening cannot be immediately followed
                // by a stale selection/click against the newly changed editor hierarchy.
                _isMouseDown = false;
                (Parent as ContentView)?.OnItemDoubleClick(this);
            }

            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseWheel(Float2 location, float delta)
        {
            // ContentItem fills each list row, so explicitly forward Ctrl+wheel instead of
            // relying on parent bubbling (which differs between platform input backends).
            if (Parent is ContentView view && Root.GetKey(KeyboardKeys.Control))
            {
                view.Zoom(delta);
                return true;
            }
            return base.OnMouseWheel(location, delta);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            // Check if start drag and drop
            if (_isMouseDown && Float2.Distance(_mouseDownStartPos, location) > 10.0f)
            {
                // Clear flag
                _isMouseDown = false;

                // Start drag drop
                ContentMutationDiagnostics.Log("input.item.drag-start", $"item='{Path}'; start={_mouseDownStartPos}; current={location}");
                DoDrag();
            }
        }

        /// <inheritdoc />
        public override void OnMouseEnter(Float2 location)
        {
            ContentMutationDiagnostics.Log("input.item.hover-enter", $"item='{Path}'; location={location}");
            base.OnMouseEnter(location);
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            ContentMutationDiagnostics.Log("input.item.hover-leave", $"item='{Path}'; armed={_isMouseDown}");
            // Check if start drag and drop
            if (_isMouseDown)
            {
                // Clear flag
                _isMouseDown = false;

                // Start drag drop
                DoDrag();
            }

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnGotFocus()
        {
            ContentMutationDiagnostics.Log("focus.item.gained", $"item='{Path}'; selected={(Parent as ContentView)?.IsSelected(this)}");
            base.OnGotFocus();
        }

        /// <inheritdoc />
        public override void OnLostFocus()
        {
            ContentMutationDiagnostics.Log("focus.item.lost", $"item='{Path}'; selected={(Parent as ContentView)?.IsSelected(this)}");
            base.OnLostFocus();
        }

        /// <inheritdoc />
        public override void OnSubmit()
        {
            // Open
            (Parent as ContentView).OnItemDoubleClick(this);

            base.OnSubmit();
        }

        /// <inheritdoc />
        public override int Compare(Control other)
        {
            if (other is ContentItem otherItem)
            {
                if (otherItem.IsFolder)
                    return 1;
                return string.Compare(ShortName, otherItem.ShortName, StringComparison.InvariantCulture);
            }

            return base.Compare(other);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            if (_lastHighlightedItem == this)
                _lastHighlightedItem = null;

            // Fire event
            while (_references.Count > 0)
            {
                var reference = _references[0];
                reference.OnItemDispose(this);
                RemoveReference(reference);
            }

            base.OnDestroy();
        }

        /// <inheritdoc />
        public override string ToString()
        {
            return Path;
        }
    }
}
