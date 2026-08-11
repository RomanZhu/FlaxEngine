// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.CustomEditors;
using FlaxEditor.GUI.ContextMenu;
using FlaxEngine;
using FlaxEngine.GUI;
using ContextMenuPopup = FlaxEditor.GUI.ContextMenu.ContextMenu;

namespace FlaxEditor.GUI
{
    /// <summary>
    /// The custom context menu that shows a tree of prefab diff items.
    /// </summary>
    /// <seealso cref="ContextMenuBase" />
    public class PrefabDiffContextMenu : ContextMenuBase
    {
        private readonly Label _title;
        private readonly Button _revertAll;
        private readonly Button _applyAll;

        /// <summary>
        /// The tree control where you should add your nodes.
        /// </summary>
        public readonly Tree.Tree Tree;

        /// <summary>
        /// The event called to revert all the changes applied.
        /// </summary>
        public event Action RevertAll;

        /// <summary>
        /// The event called to apply all the changes.
        /// </summary>
        public event Action ApplyAll;

        /// <summary>
        /// The event called when the popup is hidden.
        /// </summary>
        public event Action Closed;

        /// <summary>
        /// Initializes a new instance of the <see cref="PrefabDiffContextMenu"/> class.
        /// </summary>
        /// <param name="width">The control width.</param>
        /// <param name="height">The control height.</param>
        public PrefabDiffContextMenu(float width = 360, float height = 360)
        {
            // Context menu dimensions
            Size = new Float2(width, height);

            _title = new Label
            {
                Bounds = new Rectangle(10.0f, 5.0f, width - 20.0f, 22.0f),
                Text = "Review, Revert or Apply Overrides",
                Font = new FontReference(Style.Current.FontMedium),
                TextColor = Style.Current.Foreground,
                HorizontalAlignment = TextAlignment.Near,
                Parent = this,
            };

            const float padding = 8.0f;
            const float buttonsHeight = 22.0f;
            float buttonsWidth = (width - padding * 3.0f) * 0.5f;
            _revertAll = new Button(padding, height - buttonsHeight - padding, buttonsWidth, buttonsHeight)
            {
                Text = "Revert All",
                Parent = this
            };
            _revertAll.Clicked += OnRevertAllClicked;

            _applyAll = new Button(_revertAll.Right + padding, _revertAll.Y, buttonsWidth, buttonsHeight)
            {
                Text = "Apply All",
                Parent = this
            };
            _applyAll.Clicked += OnApplyAllClicked;

            // Actual panel
            var panel1 = new Panel(ScrollBars.Vertical)
            {
                Bounds = new Rectangle(4.0f, _title.Bottom + 2.0f, width - 8.0f, _revertAll.Top - _title.Bottom - 6.0f),
                Parent = this
            };

            Tree = new Tree.Tree
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                IsScrollable = true,
                Parent = panel1
            };
        }

        private void OnRevertAllClicked()
        {
            Hide();
            RevertAll?.Invoke();
        }

        private void OnApplyAllClicked()
        {
            Hide();
            ApplyAll?.Invoke();
        }

        /// <inheritdoc />
        protected override void OnShow()
        {
            // Prepare
            Focus();

            base.OnShow();
        }

        /// <inheritdoc />
        protected override void OnHide()
        {
            Closed?.Invoke();
            base.OnHide();
        }

        /// <inheritdoc />
        public override void Hide()
        {
            if (!Visible)
                return;

            Focus(null);

            base.Hide();
        }

        /// <inheritdoc />
        public override bool OnKeyDown(KeyboardKeys key)
        {
            if (key == KeyboardKeys.Escape)
            {
                Hide();
                return true;
            }

            return base.OnKeyDown(key);
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            RevertAll = null;
            ApplyAll = null;
            Closed = null;

            base.OnDestroy();
        }
    }

    /// <summary>
    /// Displays a native property-editor comparison for a single prefab override.
    /// </summary>
    [HideInEditor]
    public sealed class PrefabOverrideDetailsContextMenu : ContextMenuBase
    {
        private const float ModifiedWidth = 820.0f;
        private const float ModifiedHeight = 560.0f;
        private const float SingleColumnWidth = 480.0f;
        private const float SingleColumnHeight = 460.0f;

        private sealed class ApplyButton : Button
        {
            /// <inheritdoc />
            public override void DrawSelf()
            {
                base.DrawSelf();
                var iconRect = new Rectangle(Width - 14.0f, (Height - 8.0f) * 0.5f, 8.0f, 8.0f);
                Render2D.DrawSprite(Style.Current.ArrowDown, iconRect, Enabled ? Style.Current.Foreground : Style.Current.ForegroundDisabled);
            }
        }

        private readonly Label _title;
        private readonly Label _sourceHeader;
        private readonly Label _overrideHeader;
        private readonly Button _revert;
        private readonly ApplyButton _apply;
        private readonly Panel _sourcePanel;
        private readonly Panel _overridePanel;
        private readonly CustomEditorPresenter _sourcePresenter;
        private readonly CustomEditorPresenter _overridePresenter;
        private Action _revertAction;
        private Action<ContextMenuPopup> _setupApplyMenu;

        /// <summary>
        /// Gets or sets the direction used to open this comparison beside the overrides popup.
        /// </summary>
        public ContextMenuDirection OpenDirection { get; set; } = ContextMenuDirection.RightDown;

        /// <summary>
        /// Initializes a new instance of the <see cref="PrefabOverrideDetailsContextMenu"/> class.
        /// </summary>
        public PrefabOverrideDetailsContextMenu(Undo undo, IPresenterOwner owner, float width = 820.0f, float height = 560.0f)
        {
            Size = new Float2(width, height);

            const float padding = 8.0f;
            const float headerHeight = 28.0f;
            const float columnHeaderHeight = 24.0f;
            const float buttonWidth = 76.0f;
            const float buttonHeight = 22.0f;
            const float columnGap = 4.0f;
            float columnWidth = (width - columnGap) * 0.5f;

            _title = new Label
            {
                Bounds = new Rectangle(padding, 2.0f, width - padding * 2.0f - buttonWidth * 2.0f - 8.0f, headerHeight),
                Font = new FontReference(Style.Current.FontMedium),
                TextColor = Style.Current.Foreground,
                HorizontalAlignment = TextAlignment.Near,
                Parent = this,
            };
            _revert = new Button(width - padding - buttonWidth * 2.0f - 4.0f, 4.0f, buttonWidth, buttonHeight)
            {
                Text = "Revert",
                Parent = this,
            };
            _revert.Clicked += OnRevertClicked;
            _apply = new ApplyButton
            {
                Bounds = new Rectangle(width - padding - buttonWidth, 4.0f, buttonWidth, buttonHeight),
                Text = "Apply",
                Parent = this,
            };
            _apply.Clicked += OnApplyClicked;

            _sourceHeader = CreateColumnHeader("Prefab Source", new Rectangle(0.0f, headerHeight, columnWidth, columnHeaderHeight));
            _overrideHeader = CreateColumnHeader("Override", new Rectangle(columnWidth + columnGap, headerHeight, columnWidth, columnHeaderHeight));
            _sourcePanel = new Panel(ScrollBars.Vertical)
            {
                Bounds = new Rectangle(0.0f, _sourceHeader.Bottom, columnWidth, height - _sourceHeader.Bottom),
                BackgroundColor = Style.Current.Background,
                Parent = this,
            };
            _overridePanel = new Panel(ScrollBars.Vertical)
            {
                Bounds = new Rectangle(columnWidth + columnGap, _overrideHeader.Bottom, columnWidth, height - _overrideHeader.Bottom),
                BackgroundColor = Style.Current.Background,
                Parent = this,
            };

            _sourcePresenter = new CustomEditorPresenter(null)
            {
                Features = FeatureFlags.None,
                ReadOnly = true,
            };
            _sourcePresenter.Panel.Parent = _sourcePanel;
            _overridePresenter = new CustomEditorPresenter(undo, owner: owner)
            {
                Features = FeatureFlags.UsePrefab | FeatureFlags.UseDefault,
            };
            _overridePresenter.Panel.Parent = _overridePanel;
        }

        /// <inheritdoc />
        public override void Show(Control parent, Float2 location, ContextMenuDirection? direction = null)
        {
            base.Show(parent, location, OpenDirection);
        }

        private Label CreateColumnHeader(string text, Rectangle bounds)
        {
            return new Label
            {
                Bounds = bounds,
                Text = text,
                Bold = true,
                BackgroundColor = Style.Current.SecondaryBackground,
                TextColor = Style.Current.Foreground,
                HorizontalAlignment = TextAlignment.Near,
                Margin = new Margin(8.0f, 0.0f, 0.0f, 0.0f),
                Parent = this,
            };
        }

        /// <summary>
        /// Shows a modified component in a two-column prefab/override comparison.
        /// </summary>
        public void ShowModified(string title, object source, object current, Action revert, Action<ContextMenuPopup> setupApplyMenu)
        {
            ResizePopup(ModifiedWidth, ModifiedHeight);
            Setup(title, source, current, true, true, revert, setupApplyMenu);
        }

        /// <summary>
        /// Shows a component added to the prefab instance.
        /// </summary>
        public void ShowAdded(string title, object current, Action revert, Action<ContextMenuPopup> setupApplyMenu)
        {
            ResizePopup(SingleColumnWidth, SingleColumnHeight);
            Setup(title, null, current, false, true, revert, setupApplyMenu);
        }

        /// <summary>
        /// Shows a component removed from the prefab instance.
        /// </summary>
        public void ShowRemoved(string title, object source, Action revert, Action<ContextMenuPopup> setupApplyMenu)
        {
            ResizePopup(SingleColumnWidth, SingleColumnHeight);
            Setup(title, source, null, true, false, revert, setupApplyMenu);
        }

        private void ResizePopup(float width, float height)
        {
            Size = new Float2(width, height);
            const float padding = 8.0f;
            const float buttonWidth = 76.0f;
            const float buttonHeight = 22.0f;
            _title.Bounds = new Rectangle(padding, 2.0f, width - padding * 2.0f - buttonWidth * 2.0f - 8.0f, 28.0f);
            _revert.Bounds = new Rectangle(width - padding - buttonWidth * 2.0f - 4.0f, 4.0f, buttonWidth, buttonHeight);
            _apply.Bounds = new Rectangle(width - padding - buttonWidth, 4.0f, buttonWidth, buttonHeight);
            if (IsOpened)
                UpdateWindowSize();
        }

        private void Setup(string title, object source, object current, bool showSource, bool showOverride, Action revert, Action<ContextMenuPopup> setupApplyMenu)
        {
            _sourcePresenter.Deselect();
            _overridePresenter.Deselect();

            _revertAction = revert;
            _setupApplyMenu = setupApplyMenu;
            _revert.Enabled = revert != null;
            _apply.Enabled = setupApplyMenu != null;

            const float headerBottom = 52.0f;
            if (showSource && showOverride)
            {
                const float gap = 4.0f;
                float columnWidth = (Width - gap) * 0.5f;
                _title.Text = title;
                _sourceHeader.Text = "Prefab Source";
                _overrideHeader.Text = "Override";
                _sourceHeader.Bounds = new Rectangle(0.0f, 28.0f, columnWidth, 24.0f);
                _overrideHeader.Bounds = new Rectangle(columnWidth + gap, 28.0f, columnWidth, 24.0f);
                _sourcePanel.Bounds = new Rectangle(0.0f, headerBottom, columnWidth, Height - headerBottom);
                _overridePanel.Bounds = new Rectangle(columnWidth + gap, headerBottom, columnWidth, Height - headerBottom);
            }
            else if (showSource)
            {
                _title.Text = "Removed";
                _sourceHeader.Text = title;
                _sourceHeader.Bounds = new Rectangle(0.0f, 28.0f, Width, 24.0f);
                _sourcePanel.Bounds = new Rectangle(0.0f, headerBottom, Width, Height - headerBottom);
            }
            else
            {
                _title.Text = "Added";
                _overrideHeader.Text = title;
                _overrideHeader.Bounds = new Rectangle(0.0f, 28.0f, Width, 24.0f);
                _overridePanel.Bounds = new Rectangle(0.0f, headerBottom, Width, Height - headerBottom);
            }

            _sourceHeader.Visible = showSource;
            _sourcePanel.Visible = showSource;
            _overrideHeader.Visible = showOverride;
            _overridePanel.Visible = showOverride;

            if (showSource)
            {
                _sourcePresenter.Select(source);
                _sourcePresenter.BuildLayout();
            }
            if (showOverride)
            {
                _overridePresenter.Select(current);
                if (source != null)
                    _overridePresenter.Selection.SetReferenceValue(source);
                _overridePresenter.BuildLayout();
            }
        }

        private void OnRevertClicked()
        {
            _revertAction?.Invoke();
        }

        private void OnApplyClicked()
        {
            if (_setupApplyMenu == null)
                return;
            var menu = new ContextMenuPopup();
            _setupApplyMenu(menu);
            menu.Show(_apply, new Float2(0.0f, _apply.Height));
        }

        /// <inheritdoc />
        protected override void OnHide()
        {
            _sourcePresenter.Deselect();
            _overridePresenter.Deselect();
            _revertAction = null;
            _setupApplyMenu = null;
            base.OnHide();
        }

        /// <inheritdoc />
        public override void OnDestroy()
        {
            _revertAction = null;
            _setupApplyMenu = null;
            base.OnDestroy();
        }
    }
}
