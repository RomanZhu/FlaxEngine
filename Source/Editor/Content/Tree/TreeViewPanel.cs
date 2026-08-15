using System.Collections.Generic;
using FlaxEditor.GUI.Tree;
using FlaxEditor.Options;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Content;

/// <summary>
/// The content tree view panel.
/// </summary>
public class TreeViewPanel : Panel
{
    /// <summary>
    /// The content tree assigned to this panel.
    /// </summary>
    public Tree ContentTree;
    
    private InputActionsContainer _inputActions;
    private bool _isCutting;
    private List<ContentItem> _cutItems = new List<ContentItem>();

    /// <summary>
    /// Gets the content items that can be moved, copied, or duplicated from the tree selection.
    /// </summary>
    /// <param name="tree">The content tree.</param>
    /// <returns>The selected content items.</returns>
    public static List<ContentItem> GetSelectedContentItems(Tree tree)
    {
        var items = new List<ContentItem>();
        if (tree == null)
            return items;

        foreach (var node in tree.Selection)
        {
            switch (node)
            {
            // In tree-only mode content items are represented by tree nodes and are not
            // attached to the GUI hierarchy themselves, so ContentItem.CanDrag is false.
            // The tree node is still a valid drag source (including multi-selection).
            case ContentItemTreeNode contentNode:
                items.Add(contentNode.Item);
                break;
            case ContentFolderTreeNode folderNode when !folderNode.IsRoot && folderNode.CanDelete && folderNode.Folder.CanDrag:
                items.Add(folderNode.Folder);
                break;
            }
        }
        return items;
    }

    /// <summary>
    /// Gets the content items to include in a drag operation started by the specified node.
    /// </summary>
    /// <param name="node">The node that started the drag operation.</param>
    /// <param name="item">The content item represented by the node.</param>
    /// <returns>The content items to drag.</returns>
    public static List<ContentItem> GetDragItems(TreeNode node, ContentItem item)
    {
        var tree = node.ParentTree;
        if (tree != null && tree.Selection.Contains(node))
        {
            var items = GetSelectedContentItems(tree);
            if (items.Count != 0)
                return items;
        }
        return new List<ContentItem> { item };
    }

    /// <summary>
    /// Initializes a new instance of the <see cref="TreeViewPanel"/> class.
    /// </summary>
    public TreeViewPanel()
    : base(ScrollBars.None)
    {
        // Setup input actions
        _inputActions = new InputActionsContainer(new[]
        {
            new InputActionsContainer.Binding(options => options.Rename, Rename),
            new InputActionsContainer.Binding(options => options.Delete, Delete),
            new InputActionsContainer.Binding(options => options.Duplicate, Duplicate),
            new InputActionsContainer.Binding(options => options.Copy, Copy),
            new InputActionsContainer.Binding(options => options.Paste, Paste),
            new InputActionsContainer.Binding(options => options.Cut, Cut),
        });
    }

    /// <summary>
    /// Renames the selected item.
    /// </summary>
    public void Rename()
    {
        if (ContentTree == null || !Visible)
            return;
        
        var selection = ContentTree.Selection;
        if (selection.Count > 0)
        {
            var node = selection[0];
            if (node is ContentItemTreeNode contentNode)
                Editor.Instance.Windows.ContentWin.Rename(contentNode.Item);
            else if (node is ContentFolderTreeNode folderNode)
                Editor.Instance.Windows.ContentWin.Rename(folderNode.Folder);
        }
    }

    /// <summary>
    /// Deletes the selected items.
    /// </summary>
    public void Delete()
    {
        if (ContentTree == null || !Visible)
            return;
        
        var selection = ContentTree.Selection;
        if (selection.Count > 0)
        {
            var items = new List<ContentItem>();
            foreach (var node in selection)
            {
                if (node is ContentItemTreeNode fileNode)
                {
                    items.Add(fileNode.Item);
                } else if (node is ContentFolderTreeNode folderNode)
                {
                    items.Add(folderNode.Folder);
                }
            }

            Editor.Instance.Windows.ContentWin.Delete(items);
        }
    }
    
    /// <summary>
    /// Duplicates the selected items.
    /// </summary>
    public void Duplicate()
    {
        if (ContentTree == null || !Visible)
            return;

        var selection = ContentTree.Selection;
        if (selection.Count > 0)
        {
            Editor.Instance.Windows.ContentWin.Duplicate(GetSelectedContentItems(ContentTree));
        }
    }
    
    /// <summary>
    /// Copies the items.
    /// </summary>
    public void Copy()
    {
        if (ContentTree == null || !Visible)
            return;

        var selection = ContentTree.Selection;
        if (selection.Count == 0)
            return;
        var filePaths = GetSelectedContentItems(ContentTree).ConvertAll(item => item.Path);

        Clipboard.Files = filePaths.ToArray();
        UpdateContentItemCut(false);
    }
    
    /// <summary>
    /// Pastes the items.
    /// </summary>
    public void Paste()
    {
        if (ContentTree == null || !Visible)
            return;
 
        var files = Clipboard.Files;
        if (files == null || files.Length == 0)
            return;

        if (Editor.Instance.Windows.ContentWin.Paste(files, _isCutting))
            UpdateContentItemCut(false);
    }

    /// <summary>
    /// Cuts the items.
    /// </summary>
    public void Cut()
    {
        if (ContentTree == null || !Visible)
            return;

        Copy();
        UpdateContentItemCut(true);
    }
    
    private void UpdateContentItemCut(bool cut)
    {
        _isCutting = cut;
        
        // Add selection to cut list
        if (cut)
        {
            _cutItems.AddRange(GetSelectedContentItems(ContentTree));
        }
            
        // Update item with if it is being cut.
        foreach (var item in _cutItems)
        {
            item.IsBeingCut = cut;
        }
            
        // Clean up cut items
        if (!cut)
            _cutItems.Clear();
    }

    /// <inheritdoc />
    public override bool OnKeyDown(KeyboardKeys key)
    {
        if (!Visible)
            return false;
        if (ContentTree == null)
            return base.OnKeyDown(key);

        if (_inputActions.Process(Editor.Instance, this, key))
            return true;
        
        var selection = ContentTree.Selection;
        if (selection.Count > 0)
        {
            if (key == KeyboardKeys.Return)
            {
                foreach (var node in selection)
                {
                    if (node is ContentItemTreeNode contentNode)
                        Editor.Instance.Windows.ContentWin.Open(contentNode.Item);
                }
            }
        }
        return base.OnKeyDown(key);
    }
}
