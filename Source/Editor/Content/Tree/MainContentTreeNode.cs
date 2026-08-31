// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEditor.Content
{
    /// <summary>
    /// Content tree node used for main directories.
    /// </summary>
    /// <seealso cref="ContentFolderTreeNode" />
    public class MainContentFolderTreeNode : ContentFolderTreeNode
    {
        /// <inheritdoc />
        public override bool CanDelete => false;

        /// <inheritdoc />
        public override bool CanDuplicate => false;

        /// <summary>
        /// Initializes a new instance of the <see cref="MainContentFolderTreeNode"/> class.
        /// </summary>
        /// <param name="parent">The parent project.</param>
        /// <param name="type">The folder type.</param>
        /// <param name="path">The folder path.</param>
        public MainContentFolderTreeNode(ProjectFolderTreeNode parent, ContentFolderType type, string path)
        : base(parent, type, path)
        {
        }

        /// <inheritdoc />
        protected override void DoDragDrop()
        {
            // No drag for root nodes
        }

    }
}
