// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.IO;
using FlaxEditor.Content;
using FlaxEditor.GUI.Drag;
using FlaxEditor.Scripting;
using FlaxEditor.Windows;
using FlaxEngine;
using FlaxEngine.GUI;
using FlaxEngine.Utilities;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// Assets picking control.
    /// </summary>
    /// <seealso cref="Control" />
    /// <seealso cref="IContentItemOwner" />
    [HideInEditor]
    public class AssetPicker : Control, ITooltipPreviewProvider
    {
        private const float DefaultIconSize = 64;
        private const float ButtonsOffset = 2;
        private const float ButtonsSize = 12;

        /// <summary>
        /// The height of the compact single-line reference field.
        /// </summary>
        public const float CompactFieldHeight = 22;

        /// <summary>
        /// The height of the compact reference field when it shows an item preview.
        /// </summary>
        public const float CompactFieldWithPreviewHeight = 36;

        private const float CompactButtonSize = 16;
        private const float CompactIconSize = 12;
        private const float CompactPreviewSize = 32;
        private const float CompactPreviewSpacing = 4;

        private bool _isMouseDown;
        private Float2 _mouseDownPos;
        private Float2 _mousePos;
        private DragItems _dragOverElement;
        private bool _useCompactField;
        private bool _showCompactPreview;

        /// <summary>
        /// The asset validator. Used to ensure only appropriate items can be picked.
        /// </summary>
        public AssetPickerValidator Validator { get; }

        /// <summary>
        /// Occurs when selected item gets changed.
        /// </summary>
        public event Action SelectedItemChanged;

        /// <summary>
        /// False if changing selected item is disabled.
        /// </summary>
        public bool CanEdit = true;

        /// <summary>
        /// Utility flag used to indicate that there are different values assigned to this reference editor and user should be informed about it.
        /// </summary>
        public bool DifferentValues;

        /// <summary>
        /// Custom background color used by the compact reference field.
        /// </summary>
        public Color CompactBackgroundColor = Color.Transparent;

        /// <summary>
        /// True to render as a compact single-line reference field.
        /// </summary>
        public bool UseCompactField
        {
            get => _useCompactField;
            set
            {
                if (_useCompactField == value)
                    return;
                _useCompactField = value;
                UpdateCompactHeight();
            }
        }

        /// <summary>
        /// True to render selected item preview next to the compact reference field.
        /// </summary>
        public bool ShowCompactPreview
        {
            get => _showCompactPreview;
            set
            {
                if (_showCompactPreview == value)
                    return;
                _showCompactPreview = value;
                UpdateCompactHeight();
            }
        }

        /// <summary>
        /// Gets the compact field height for the current compact display options.
        /// </summary>
        public float CompactHeight => ShowCompactPreview ? CompactFieldWithPreviewHeight : CompactFieldHeight;

        /// <inheritdoc />
        public SpriteHandle TooltipPreview => GetSelectedContentItem()?.TooltipPreview ?? SpriteHandle.Invalid;

        /// <summary>
        /// Initializes a new instance of the <see cref="AssetPicker"/> class.
        /// </summary>
        public AssetPicker()
        : this(new ScriptType(typeof(Asset)), Float2.Zero)
        {
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="AssetPicker"/> class.
        /// </summary>
        /// <param name="assetType">The asset types that this picker accepts.</param>
        /// <param name="location">The control location.</param>
        public AssetPicker(ScriptType assetType, Float2 location)
        : base(location, new Float2(DefaultIconSize + ButtonsOffset + ButtonsSize, DefaultIconSize))
        {
            Validator = new AssetPickerValidator(assetType);
            Validator.SelectedItemChanged += OnSelectedItemChanged;
            _mousePos = Float2.Minimum;
        }

        /// <summary>
        /// Called when selected item gets changed.
        /// </summary>
        protected virtual void OnSelectedItemChanged()
        {
            if (IsDisposing)
                return;

            // Update tooltip
            string tooltip;
            if (Validator.SelectedItem is AssetItem assetItem)
                tooltip = assetItem.NamePath;
            else
                tooltip = Validator.SelectedPath;
            TooltipText = tooltip;

            SelectedItemChanged?.Invoke();
        }

        private void DoDrag()
        {
            // Do the drag drop operation if has selected element
            var dragRect = UseCompactField ? CompactTextRect : new Rectangle(Float2.Zero, Size);
            if (dragRect.Contains(ref _mouseDownPos))
            {
                if (Validator.SelectedAsset != null)
                    DoDragDrop(DragAssets.GetDragData(Validator.SelectedAsset));
                else if (Validator.SelectedItem != null)
                    DoDragDrop(DragItems.GetDragData(Validator.SelectedItem));
            }
        }

        private Rectangle IconRect => new Rectangle(0, 0, Height, Height);

        private Rectangle Button1Rect => new Rectangle(Height + ButtonsOffset, 0, ButtonsSize, ButtonsSize);

        private Rectangle Button2Rect => new Rectangle(Height + ButtonsOffset, ButtonsSize + 2, ButtonsSize, ButtonsSize);

        private Rectangle Button3Rect => new Rectangle(Height + ButtonsOffset, (ButtonsSize + 2) * 2, ButtonsSize, ButtonsSize);

        private bool HasCompactPreview => UseCompactField && ShowCompactPreview;

        private Rectangle CompactPreviewRect => new Rectangle(0, (Height - CompactPreviewSize) * 0.5f, CompactPreviewSize, CompactPreviewSize);

        private Rectangle CompactFieldRect
        {
            get
            {
                var x = HasCompactPreview ? CompactPreviewSize + CompactPreviewSpacing : 0.0f;
                return new Rectangle(x, (Height - CompactFieldHeight) * 0.5f, Mathf.Max(0.0f, Width - x), CompactFieldHeight);
            }
        }

        private Rectangle CompactPickerRect
        {
            get
            {
                var fieldRect = CompactFieldRect;
                return new Rectangle(fieldRect.X + 1, fieldRect.Y + (fieldRect.Height - CompactButtonSize) * 0.5f, CompactButtonSize, CompactButtonSize);
            }
        }

        private Rectangle CompactSearchRect
        {
            get
            {
                var fieldRect = CompactFieldRect;
                return new Rectangle(fieldRect.Right - CompactButtonSize - 1, fieldRect.Y + (fieldRect.Height - CompactButtonSize) * 0.5f, CompactButtonSize, CompactButtonSize);
            }
        }

        private Rectangle CompactTextRect
        {
            get
            {
                var fieldRect = CompactFieldRect;
                return new Rectangle(fieldRect.X + CompactButtonSize + 3, fieldRect.Y, Mathf.Max(0.0f, fieldRect.Width - CompactButtonSize * 2 - 6), fieldRect.Height);
            }
        }

        private bool HasSelection => Validator.SelectedItem != null || Validator.SelectedAsset != null;

        private void UpdateCompactHeight()
        {
            if (UseCompactField)
                Height = CompactHeight;
        }

        private static void DrawPickerIcon(Rectangle rect, Color color)
        {
            var center = new Float2(rect.X + rect.Width * 0.5f, rect.Y + rect.Height * 0.5f);
            Render2D.DrawLine(new Float2(rect.X + 3, center.Y), new Float2(center.X - 2, center.Y), color);
            Render2D.DrawLine(new Float2(center.X + 2, center.Y), new Float2(rect.Right - 3, center.Y), color);
            Render2D.DrawLine(new Float2(center.X, rect.Y + 3), new Float2(center.X, center.Y - 2), color);
            Render2D.DrawLine(new Float2(center.X, center.Y + 2), new Float2(center.X, rect.Bottom - 3), color);
            Render2D.DrawRectangle(new Rectangle(center.X - 2, center.Y - 2, 4, 4), color);
        }

        private string GetAcceptedTypeName()
        {
            if (!string.IsNullOrEmpty(Validator.FileExtension))
                return Validator.FileExtension;
            var type = Validator.AssetType.Type;
            return type != null ? type.GetTypeDisplayName() : "Asset";
        }

        private string GetCompactDisplayText()
        {
            var acceptedType = GetAcceptedTypeName();
            if (DifferentValues)
                return $"Multiple Values ({acceptedType})";
            if (Validator.SelectedItem != null)
                return $"{Validator.SelectedItem.ShortName} ({Validator.SelectedItem.TypeDescription})";
            if (Validator.SelectedAsset)
            {
                var name = Validator.SelectedAsset.GetType().Name;
                if (Validator.SelectedAsset.IsVirtual)
                    name += " (virtual)";
                return $"{name} ({acceptedType})";
            }
            return $"None ({acceptedType})";
        }

        private void DrawCompactPreview()
        {
            var style = Style.Current;
            var previewRect = CompactPreviewRect;
            var borderColor = VisuallyEnabledInHierarchy && (previewRect.Contains(_mousePos) || IsFocused || IsNavFocused) ? style.BorderHighlighted : style.BorderNormal;
            var backgroundColor = CompactBackgroundColor.A > 0.0f ? CompactBackgroundColor : style.TextBoxBackground;

            StyleRendering.DrawRoundedRectangle(previewRect, backgroundColor, borderColor, 1.0f, style.CornerRadius);

            if (DifferentValues)
            {
                Render2D.DrawText(style.FontMedium, "...", previewRect, style.ForegroundGrey, TextAlignment.Center, TextAlignment.Center);
                return;
            }

            var selectedItem = GetSelectedContentItem();
            if (selectedItem != null)
            {
                var thumbnailRect = new Rectangle(previewRect.X + 2, previewRect.Y + 2, previewRect.Width - 4, previewRect.Height - 4);
                selectedItem.DrawThumbnail(ref thumbnailRect, false);
            }
        }

        private void DrawCompactField()
        {
            var style = Style.Current;
            var visuallyEnabled = VisuallyEnabledInHierarchy;
            var textColor = visuallyEnabled && HasSelection && !DifferentValues ? style.Foreground : style.ForegroundGrey;
            var iconColor = visuallyEnabled ? style.ForegroundGrey : style.ForegroundDisabled;
            var fieldRect = CompactFieldRect;
            var pickerRect = CompactPickerRect;
            var searchRect = CompactSearchRect;
            var textRect = CompactTextRect;
            var borderColor = visuallyEnabled && (IsMouseOver || IsFocused || IsNavFocused) ? style.BorderHighlighted : style.BorderNormal;
            var backgroundColor = CompactBackgroundColor.A > 0.0f ? CompactBackgroundColor : style.TextBoxBackground;

            if (HasCompactPreview)
                DrawCompactPreview();

            StyleRendering.DrawRoundedRectangle(fieldRect, backgroundColor, borderColor, 1.0f, style.CornerRadius);

            if (CanEdit && visuallyEnabled)
            {
                var pickerIconRect = new Rectangle(pickerRect.X + (pickerRect.Width - CompactIconSize) * 0.5f, pickerRect.Y + (pickerRect.Height - CompactIconSize) * 0.5f, CompactIconSize, CompactIconSize);
                DrawPickerIcon(pickerIconRect, pickerRect.Contains(_mousePos) ? style.Foreground : iconColor);
                Render2D.DrawSprite(style.Search, searchRect, searchRect.Contains(_mousePos) ? style.Foreground : iconColor);
            }

            Render2D.PushClip(textRect);
            Render2D.DrawText(style.FontMedium, GetCompactDisplayText(), textRect, textColor, TextAlignment.Near, TextAlignment.Center);
            Render2D.PopClip();

            if (IsDragOver && _dragOverElement != null && _dragOverElement.HasValidDrag)
            {
                StyleRendering.DrawRoundedRectangle(new Rectangle(Float2.Zero, Size), style.Selection, style.SelectionBorder, 1.0f, style.CornerRadius);
            }

            if (IsNavFocused)
                StyleRendering.DrawRoundedRectangleBorder(new Rectangle(Float2.Zero, Size), style.BackgroundSelected, 1.0f, style.CornerRadius);
        }

        /// <inheritdoc />
        public override void Draw()
        {
            if (UseCompactField)
            {
                DrawCompactField();
                return;
            }

            var style = Style.Current;
            var iconRect = IconRect;
            var button1Rect = Button1Rect;
            var button2Rect = Button2Rect;
            var button3Rect = Button3Rect;
            var visuallyEnabled = VisuallyEnabledInHierarchy;
            var foreground = visuallyEnabled ? style.Foreground : style.ForegroundGrey;
            var foregroundGrey = visuallyEnabled ? style.ForegroundGrey : style.ForegroundDisabled;

            // Draw asset picker button
            if (CanEdit)
                Render2D.DrawSprite(style.ArrowDown, button1Rect, visuallyEnabled && button1Rect.Contains(_mousePos) ? style.Foreground : style.ForegroundGrey);

            if (DifferentValues)
            {
                // No element selected
                Render2D.FillRectangle(iconRect, style.BackgroundNormal);
                Render2D.DrawText(style.FontMedium, "Multiple\nValues", iconRect, foreground, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, Height / DefaultIconSize);
            }
            else if (Validator.SelectedItem != null)
            {
                // Draw item preview
                Validator.SelectedItem.DrawThumbnail(ref iconRect);

                // Draw buttons
                if (CanEdit)
                {
                    Render2D.DrawSprite(style.Search, button2Rect, visuallyEnabled && button2Rect.Contains(_mousePos) ? style.Foreground : style.ForegroundGrey);
                    Render2D.DrawSprite(style.Cross, button3Rect, visuallyEnabled && button3Rect.Contains(_mousePos) ? style.Foreground : style.ForegroundGrey);
                }
                else
                {
                    Render2D.DrawSprite(style.Search, button1Rect, visuallyEnabled && button1Rect.Contains(_mousePos) ? style.Foreground : style.ForegroundGrey);
                }

                // Draw name
                float sizeForTextLeft = Width - button1Rect.Right;
                if (sizeForTextLeft > 30)
                {
                    Render2D.DrawText(
                                      style.FontSmall,
                                      Validator.SelectedItem.ShortName,
                                      new Rectangle(button1Rect.Right + 2, 0, sizeForTextLeft, ButtonsSize),
                                      foreground,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                    Render2D.DrawText(
                                      style.FontSmall,
                                      $"{Validator.AssetType.Type.GetTypeDisplayName()}",
                                      new Rectangle(button1Rect.Right + 2, ButtonsSize + 2, sizeForTextLeft, ButtonsSize),
                                      foregroundGrey,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                }
            }
            // Check if has no item but has an asset (eg. virtual asset)
            else if (Validator.SelectedAsset)
            {
                // Draw remove button
                Render2D.DrawSprite(style.Cross, button3Rect, visuallyEnabled && button3Rect.Contains(_mousePos) ? style.Foreground : style.ForegroundGrey);

                // Draw name
                float sizeForTextLeft = Width - button1Rect.Right;
                if (sizeForTextLeft > 30)
                {
                    var name = Validator.SelectedAsset.GetType().Name;
                    if (Validator.SelectedAsset.IsVirtual)
                        name += " (virtual)";
                    Render2D.DrawText(
                                      style.FontSmall,
                                      name,
                                      new Rectangle(button1Rect.Right + 2, 0, sizeForTextLeft, ButtonsSize),
                                      foreground,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                    Render2D.DrawText(
                                      style.FontSmall,
                                      $"{Validator.AssetType.Type.GetTypeDisplayName()}",
                                      new Rectangle(button1Rect.Right + 2, ButtonsSize + 2, sizeForTextLeft, ButtonsSize),
                                      foregroundGrey,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                }
            }
            else
            {
                // No element selected
                Render2D.FillRectangle(iconRect, style.BackgroundNormal);
                Render2D.DrawText(style.FontMedium, "No asset\nselected", iconRect, visuallyEnabled ? Color.Orange : style.ForegroundGrey, TextAlignment.Center, TextAlignment.Center, TextWrapping.NoWrap, 1.0f, Height / DefaultIconSize);
                float sizeForTextLeft = Width - button1Rect.Right;
                if (sizeForTextLeft > 30)
                {
                    Render2D.DrawText(
                                      style.FontSmall,
                                      $"None",
                                      new Rectangle(button1Rect.Right + 2, 0, sizeForTextLeft, ButtonsSize),
                                      foreground,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                    Render2D.DrawText(
                                      style.FontSmall,
                                      $"{Validator.AssetType.Type.GetTypeDisplayName()}",
                                      new Rectangle(button1Rect.Right + 2, ButtonsSize + 2, sizeForTextLeft, ButtonsSize),
                                      foregroundGrey,
                                      TextAlignment.Near,
                                      TextAlignment.Center);
                }
            }

            // Check if drag is over
            if (IsDragOver && _dragOverElement != null && _dragOverElement.HasValidDrag)
            {
                var bounds = new Rectangle(Float2.Zero, Size);
                StyleRendering.DrawRoundedRectangle(bounds, style.Selection, style.SelectionBorder, 1.0f, style.CornerRadius);
            }

            // Navigation focus highlight
            if (IsNavFocused)
            {
                var bounds = new Rectangle(Float2.Zero, Size);
                StyleRendering.DrawRoundedRectangleBorder(bounds, style.BackgroundSelected, 1.0f, style.CornerRadius);
            }
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            Validator.OnDestroy();

            base.OnDestroy();
        }

        /// <inheritdoc />
        public override void OnMouseLeave()
        {
            _mousePos = Float2.Minimum;

            // Check if start drag drop
            if (_isMouseDown)
            {
                _isMouseDown = false;
                DoDrag();
            }

            base.OnMouseLeave();
        }

        /// <inheritdoc />
        public override void OnMouseEnter(Float2 location)
        {
            _mousePos = location;
            _mouseDownPos = Float2.Minimum;

            base.OnMouseEnter(location);
        }

        /// <inheritdoc />
        public override void OnMouseMove(Float2 location)
        {
            _mousePos = location;

            // Check if start drag drop
            var dragRect = UseCompactField ? CompactTextRect : IconRect;
            if (_isMouseDown && Float2.Distance(location, _mouseDownPos) > 10.0f && dragRect.Contains(_mouseDownPos))
            {
                _isMouseDown = false;
                DoDrag();
            }

            base.OnMouseMove(location);
        }

        /// <inheritdoc />
        public override bool OnMouseUp(Float2 location, MouseButton button)
        {
            if (UseCompactField)
                return OnCompactMouseUp(location, button);

            if (button == MouseButton.Left && _isMouseDown)
            {
                _isMouseDown = false;

                // Buttons logic
                if (!CanEdit)
                {
                    if (Button1Rect.Contains(location) && Validator.SelectedItem != null)
                    {
                        // Select asset
                        Editor.Instance.Windows.ContentWin.Select(Validator.SelectedItem);
                        Editor.Instance.Windows.ContentWin.ClearItemsSearch();
                    }
                }
                else if (Button1Rect.Contains(location))
                {
                    Focus();
                    OnSubmit();
                }
                else if (Validator.SelectedAsset != null || Validator.SelectedItem != null)
                {
                    if (Button2Rect.Contains(location) && Validator.SelectedItem != null)
                    {
                        // Select asset
                        Editor.Instance.Windows.ContentWin.Select(Validator.SelectedItem);
                        Editor.Instance.Windows.ContentWin.ClearItemsSearch();
                    }
                    else if (Button3Rect.Contains(location))
                    {
                        // Deselect asset
                        Focus();
                        Validator.SelectedItem = null;
                    }
                }
            }

            // Handled
            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseDown(Float2 location, MouseButton button)
        {
            if (button == MouseButton.Left)
            {
                _isMouseDown = true;
                _mouseDownPos = location;
            }

            // Handled
            return true;
        }

        /// <inheritdoc />
        public override bool OnMouseDoubleClick(Float2 location, MouseButton button)
        {
            Focus();

            if (UseCompactField)
            {
                if (button == MouseButton.Left && HasCompactPreview && CompactPreviewRect.Contains(ref location))
                    OpenSelectedContentItem();
                return true;
            }

            if (Validator.SelectedItem != null && IconRect.Contains(location))
            {
                // Open it
                Editor.Instance.ContentEditing.Open(Validator.SelectedItem);
            }

            // Handled
            return true;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragEnter(ref Float2 location, DragData data)
        {
            base.OnDragEnter(ref location, data);

            // Check if drop asset
            if (_dragOverElement == null)
                _dragOverElement = new DragItems(Validator.IsValid);
            if (CanEdit && _dragOverElement.OnDragEnter(data))
            {
            }

            return _dragOverElement.Effect;
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragMove(ref Float2 location, DragData data)
        {
            base.OnDragMove(ref location, data);

            return _dragOverElement.Effect;
        }

        /// <inheritdoc />
        public override void OnDragLeave()
        {
            // Clear cache
            _dragOverElement.OnDragLeave();

            base.OnDragLeave();
        }

        /// <inheritdoc />
        public override DragDropEffect OnDragDrop(ref Float2 location, DragData data)
        {
            base.OnDragDrop(ref location, data);

            if (CanEdit && _dragOverElement.HasValidDrag)
            {
                // Select element
                Validator.SelectedItem = _dragOverElement.Objects[0];
            }

            // Clear cache
            _dragOverElement.OnDragDrop();

            return DragDropEffect.Move;
        }

        /// <inheritdoc />
        public override void OnSubmit()
        {
            base.OnSubmit();

            if (Validator.AssetType != ScriptType.Null)
            {
                // Show asset picker popup
                var popup = AssetSearchPopup.Show(this, UseCompactField ? CompactFieldRect.BottomLeft : Button1Rect.BottomLeft, Validator.IsValid, item =>
                {
                    Validator.SelectedItem = item;
                    RootWindow.Focus();
                    Focus();
                });
                if (Validator.SelectedAsset != null)
                {
                    var selectedAssetName = Path.GetFileNameWithoutExtension(Validator.SelectedAsset.Path);
                    popup.ScrollToAndHighlightItemByName(selectedAssetName);
                }
            }
            else
            {
                // Show content item picker popup
                var popup = ContentSearchPopup.Show(this, UseCompactField ? CompactFieldRect.BottomLeft : Button1Rect.BottomLeft, Validator.IsValid, item =>
                {
                    Validator.SelectedItem = item;
                    RootWindow.Focus();
                    Focus();
                });
                if (Validator.SelectedItem != null)
                {
                    popup.ScrollToAndHighlightItemByName(Validator.SelectedItem.ShortName);
                }
            }
        }

        private bool OnCompactMouseUp(Float2 location, MouseButton button)
        {
            _isMouseDown = false;

            if (button == MouseButton.Right)
            {
                Focus();
                ShowContextMenu(location);
                return true;
            }

            if (button == MouseButton.Left)
            {
                Focus();

                if (HasCompactPreview && CompactPreviewRect.Contains(ref location))
                {
                    FocusSelectedSource();
                    return true;
                }

                if (CanEdit && CompactPickerRect.Contains(ref location))
                {
                    PickFromContentSelectionOrSearch();
                    return true;
                }

                if (CanEdit && CompactSearchRect.Contains(ref location))
                {
                    OnSubmit();
                    return true;
                }

                if (CompactTextRect.Contains(ref location))
                {
                    FocusSelectedSource();
                    return true;
                }
            }

            return true;
        }

        private ContentItem GetSelectedContentItem()
        {
            if (Validator.SelectedItem != null)
                return Validator.SelectedItem;
            return Validator.SelectedAsset ? Editor.Instance.ContentDatabase.FindAsset(Validator.SelectedAsset.ID) : null;
        }

        private bool FocusSelectedSource()
        {
            var selectedItem = GetSelectedContentItem();
            if (selectedItem == null)
                return false;

            Editor.Instance.Windows.ContentWin.Highlight(selectedItem, true);
            return true;
        }

        private bool OpenSelectedContentItem()
        {
            var selectedItem = GetSelectedContentItem();
            if (selectedItem == null)
                return false;

            Editor.Instance.ContentEditing.Open(selectedItem);
            return true;
        }

        /// <inheritdoc />
        public override bool OnShowTooltip(out string text, out Float2 location, out Rectangle area)
        {
            var selectedItem = GetSelectedContentItem();
            if (selectedItem != null)
            {
                selectedItem.UpdateTooltipText();
                TooltipText = selectedItem.TooltipText;
            }

            return base.OnShowTooltip(out text, out location, out area);
        }

        private void ShowContextMenu(Float2 location)
        {
            var selectedItem = GetSelectedContentItem();
            var menu = new ContextMenu.ContextMenu();

            var clear = menu.AddButton("Clear", () =>
            {
                Focus();
                Validator.SelectedItem = null;
            });
            clear.Enabled = CanEdit && HasSelection;

            if (selectedItem != null)
            {
                menu.AddSeparator();
                var findReferences = menu.AddButton("Find References", () =>
                {
                    if (selectedItem is AssetItem assetItem)
                        Editor.Instance.Windows.Open(new AssetReferencesGraphWindow(Editor.Instance, assetItem));
                });
                findReferences.Enabled = selectedItem is AssetItem;
                menu.AddButton("Open", () => OpenSelectedContentItem());
                menu.AddButton("Focus on Source", () => FocusSelectedSource());
            }

            menu.Show(this, location);
        }

        private bool TryAssignContentSelection()
        {
            var contentWindow = Editor.Instance.Windows.ContentWin;
            var selection = contentWindow.Selection;
            if (selection.Count == 1 && Validator.IsValid(selection[0]) && selection[0] != Validator.SelectedItem)
            {
                Validator.SelectedItem = selection[0];
                RootWindow.Focus();
                Focus();
                return true;
            }
            return false;
        }

        private void PickFromContentSelectionOrSearch()
        {
            if (!TryAssignContentSelection())
                OnSubmit();
        }
    }
}
