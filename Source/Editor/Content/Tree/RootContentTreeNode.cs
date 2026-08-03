// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEditor.Content
{
    /// <summary>
    /// Root tree node for the content workspace.
    /// </summary>
    /// <seealso cref="ContentFolderTreeNode" />
    public sealed class RootContentFolderTreeNode : ContentFolderTreeNode
    {
        /// <summary>
        /// Initializes a new instance of the <see cref="RootContentFolderTreeNode"/> class.
        /// </summary>
        public RootContentFolderTreeNode()
        : base(null, string.Empty)
        {
            AutoFocus = false;
            IsSelectable = false;
            ShowHeader = false;
        }

        /// <inheritdoc />
        public override string NavButtonLabel => " /";
    }
}
