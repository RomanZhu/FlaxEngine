// Copyright (c) Wojciech Figat. All rights reserved.

using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.GUI.ContextMenu
{
    /// <summary>
    /// Context Menu separator control that visually separate chunks of the popup menu items.
    /// </summary>
    /// <seealso cref="ContextMenuItem" />
    [HideInEditor]
    public class ContextMenuSeparator : ContextMenuItem
    {
        private const float SeparatorSidePadding = 3.0f;
        private static readonly Color SeparatorColor = Color.FromBgra(0xFF434347);

        /// <summary>
        /// Initializes a new instance of the <see cref="ContextMenuSeparator"/> class.
        /// </summary>
        /// <param name="parent">The parent context menu.</param>
        public ContextMenuSeparator(ContextMenu parent)
        : base(parent, 8, 4)
        {
        }

        /// <inheritdoc />
        public override void Draw()
        {
            base.Draw();

            // Draw separator line
            Render2D.FillRectangle(new Rectangle(-X + SeparatorSidePadding, 1, Parent.Width - SeparatorSidePadding * 2.0f, 1), SeparatorColor);
        }
    }
}
