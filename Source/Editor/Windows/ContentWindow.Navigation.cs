// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Content;
using FlaxEditor.Content.GUI;
using FlaxEditor.GUI;
using FlaxEditor.GUI.Tree;
using FlaxEditor.History;
using FlaxEditor.SceneGraph;

namespace FlaxEditor.Windows
{
    public partial class ContentWindow
    {
        private static readonly List<ContentFolderTreeNode> NavUpdateCache = new List<ContentFolderTreeNode>(8);

        private sealed class ContentFolderNavigationAction : INavigationHistoryAction, INavigationHistoryDestination
        {
            private readonly ContentWindow _window;
            private readonly ContentFolderTreeNode _source;
            private readonly ContentFolderTreeNode _target;

            public ContentFolderNavigationAction(ContentWindow window, ContentFolderTreeNode source, ContentFolderTreeNode target)
            {
                _window = window;
                _source = source;
                _target = target;
            }

            public object Owner => _window;

            public string ActionString => "Content folder change";

            public bool IsSameDestination(INavigationHistoryAction other)
            {
                return other is ContentFolderNavigationAction action &&
                       action.Owner == Owner &&
                       action._target == _target;
            }

            public bool Contains(ContentFolderTreeNode node)
            {
                return _source == node || _target == node;
            }

            public void NavigateBack()
            {
                _window.DoNavigateFromHistory(_source);
            }

            public void NavigateForward()
            {
                _window.DoNavigateFromHistory(_target);
            }

            public void Dispose()
            {
            }
        }

        private sealed class ContentSelectionNavigationAction : INavigationHistoryAction, INavigationHistoryDestination
        {
            private readonly ContentWindow _window;
            private readonly string[] _source;
            private readonly string[] _target;

            public ContentSelectionNavigationAction(ContentWindow window, string[] source, string[] target)
            {
                _window = window;
                _source = source ?? Array.Empty<string>();
                _target = target ?? Array.Empty<string>();
            }

            public object Owner => _window;

            public string ActionString => "Content selection change";

            public bool IsSameDestination(INavigationHistoryAction other)
            {
                return other is ContentSelectionNavigationAction action &&
                       action.Owner == Owner &&
                       AreSameContentSelection(action._target, _target);
            }

            public bool Contains(string path)
            {
                return Contains(_source, path) || Contains(_target, path);
            }

            public void NavigateBack()
            {
                _window.DoSelectContentItemsFromHistory(_source);
            }

            public void NavigateForward()
            {
                _window.DoSelectContentItemsFromHistory(_target);
            }

            public void Dispose()
            {
            }

            private static bool Contains(string[] paths, string path)
            {
                for (int i = 0; i < paths.Length; i++)
                {
                    if (paths[i].Equals(path, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                return false;
            }
        }

        private sealed class ContentSelectionUndoAction : IUndoAction, IUndoActionMetadata
        {
            private ContentWindow _window;
            private readonly string[] _source;
            private readonly string[] _target;
            private readonly SceneGraphNodeReference[] _sourceSceneSelection;
            private readonly SceneGraphNodeReference[] _targetSceneSelection;

            public ContentSelectionUndoAction(ContentWindow window, string[] source, string[] target, SceneGraphNode[] sourceSceneSelection, SceneGraphNode[] targetSceneSelection)
            {
                _window = window;
                _source = source ?? Array.Empty<string>();
                _target = target ?? Array.Empty<string>();
                _sourceSceneSelection = SceneGraphNodeReference.Capture(sourceSceneSelection);
                _targetSceneSelection = SceneGraphNodeReference.Capture(targetSceneSelection);
            }

            public string ActionString => "Content selection change";

            public bool IsSameTransition(ContentWindow window, string[] source, string[] target, SceneGraphNode[] sourceSceneSelection, SceneGraphNode[] targetSceneSelection)
            {
                return _window == window &&
                       AreSameContentSelection(_source, source) &&
                       AreSameContentSelection(_target, target) &&
                       AreSameSceneSelection(_sourceSceneSelection, SceneGraphNodeReference.Capture(sourceSceneSelection)) &&
                       AreSameSceneSelection(_targetSceneSelection, SceneGraphNodeReference.Capture(targetSceneSelection));
            }

            public UndoActionInfo ActionInfo => new UndoActionInfo
            {
                Operation = ActionString,
                TargetType = GetSelectionTargetType(_target),
                TargetName = DescribeSelection(_source) + " -> " + DescribeSelection(_target),
                TargetPath = _target.Length == 1 ? _target[0] : null,
                SecondaryTargetPath = _source.Length == 1 ? _source[0] : null,
                DisplayEditorTypeName = typeof(ContentWindow).FullName,
                Flags = UndoActionFlags.SelectionOnly,
                SizeInBytes = 0,
            };

            public void Do()
            {
                _window?.RestoreContentSelectionFromUndo(_target, _targetSceneSelection);
            }

            public void Undo()
            {
                _window?.RestoreContentSelectionFromUndo(_source, _sourceSceneSelection);
            }

            public void Dispose()
            {
                _window = null;
            }

            private static bool AreSameSceneSelection(SceneGraphNodeReference[] a, SceneGraphNodeReference[] b)
            {
                a ??= Array.Empty<SceneGraphNodeReference>();
                b ??= Array.Empty<SceneGraphNodeReference>();
                if (a.Length != b.Length)
                    return false;
                for (int i = 0; i < a.Length; i++)
                {
                    if (!a[i].Equals(b[i]))
                        return false;
                }
                return true;
            }

            private static UndoActionTargetType GetSelectionTargetType(string[] paths)
            {
                if (paths == null || paths.Length == 0)
                    return UndoActionTargetType.Unknown;
                return paths.Length == 1 ? UndoActionTargetType.ContentItem : UndoActionTargetType.Multiple;
            }

            private static string DescribeSelection(string[] paths)
            {
                if (paths == null || paths.Length == 0)
                    return "<empty>";
                return paths.Length == 1 ? paths[0] : paths.Length + " content items";
            }
        }

        private sealed class ContentOpenNavigationAction : INavigationHistoryAction, INavigationHistoryDestination
        {
            private readonly ContentWindow _window;
            private readonly string[] _sourceSelection;
            private readonly string _targetPath;

            public ContentOpenNavigationAction(ContentWindow window, string[] sourceSelection, string targetPath)
            {
                _window = window;
                _sourceSelection = sourceSelection ?? Array.Empty<string>();
                _targetPath = targetPath;
            }

            public object Owner => _window;

            public string ActionString => "Open content item";

            public bool IsSameDestination(INavigationHistoryAction other)
            {
                return other is ContentOpenNavigationAction action &&
                       action.Owner == Owner &&
                       action._targetPath.Equals(_targetPath, StringComparison.OrdinalIgnoreCase);
            }

            public bool Contains(string path)
            {
                return _targetPath.Equals(path, StringComparison.OrdinalIgnoreCase);
            }

            public void NavigateBack()
            {
                _window.DoSelectContentItemsFromHistory(_sourceSelection);
            }

            public void NavigateForward()
            {
                _window.DoOpenContentItemFromHistory(_targetPath);
            }

            public void Dispose()
            {
            }
        }

        private void OnTreeSelectionChanged(List<TreeNode> from, List<TreeNode> to)
        {
            if (_isClearingSelection)
            {
                UpdateUI();
                _lastContentSelectionPaths = GetContentSelectionPaths();
                SelectionChanged?.Invoke();
                return;
            }

            bool setLastViewFolder = !IsLayoutLocked;
            if (!_showAllContentInTree && to.Count > 1)
            {
                _tree.Select(to[^1]);
                return;
            }
            if (_showAllContentInTree && to.Count > 1)
            {
                if (setLastViewFolder)
                {
                    var activeNode = GetActiveTreeSelection(to);
                    if (activeNode is ContentItemTreeNode itemNode)
                        SaveLastViewedFolder(itemNode.Item?.ParentFolder?.Node);
                    else
                        SaveLastViewedFolder(activeNode as ContentFolderTreeNode);
                }
                UpdateUI();
                NotifyTreeContentSelectionChanged();
                return;
            }

            // Navigate
            var source = from.Count > 0 ? from[0] as ContentFolderTreeNode : null;
            var targetNode = GetActiveTreeSelection(to);
            if (targetNode is ContentItemTreeNode itemNode2)
            {
                if (setLastViewFolder)
                    SaveLastViewedFolder(itemNode2.Item?.ParentFolder?.Node);
                UpdateUI();
                itemNode2.Focus();
                NotifyTreeContentSelectionChanged();
                return;
            }

            var target = targetNode as ContentFolderTreeNode;
            Navigate(source, target);

            if (setLastViewFolder)
                SaveLastViewedFolder(target);
            target?.Focus();
            if (_showAllContentInTree)
                NotifyTreeContentSelectionChanged();
        }

        /// <summary>
        /// Navigates to the specified target content location.
        /// </summary>
        /// <param name="target">The target.</param>
        public void Navigate(ContentFolderTreeNode target)
        {
            Navigate(SelectedNode, target);
        }

        private void Navigate(ContentFolderTreeNode source, ContentFolderTreeNode target)
        {
            if (target == null || target == _root)
                target = GetFirstVisibleRootFolder();
            if (target == null)
                return;

            // Check if can do this action
            if (_navigationUnlocked && source != target)
            {
                // Lock navigation
                _navigationUnlocked = false;

                // Check if already added to the Undo on the top
                if (source != null && (_navigationUndo.Count == 0 || _navigationUndo.Peek() != source))
                {
                    // Add to Undo list
                    _navigationUndo.Push(source);
                }

                // Clear redo list
                _navigationRedo.Clear();

                if (source != null)
                    Editor.NavigationHistory.AddAction(new ContentFolderNavigationAction(this, source, target));

                DoNavigate(target);
            }
        }

        /// <summary>
        /// Navigates backward.
        /// </summary>
        public void NavigateBackward()
        {
            // Check if navigation is unlocked
            if (_navigationUnlocked && _navigationUndo.Count > 0)
            {
                // Pop node
                ContentFolderTreeNode node = _navigationUndo.Pop();

                // Lock navigation
                _navigationUnlocked = false;

                // Add to Redo list
                _navigationRedo.Push(SelectedNode);

                DoNavigate(node);
                if (!_showAllContentInTree)
                    RunWithContentSelectionHistorySuppressed(() => _view.SelectFirstItem());
            }
        }

        /// <summary>
        /// Navigates forward.
        /// </summary>
        public void NavigateForward()
        {
            // Check if navigation is unlocked
            if (_navigationUnlocked && _navigationRedo.Count > 0)
            {
                // Pop node
                ContentFolderTreeNode node = _navigationRedo.Pop();

                // Lock navigation
                _navigationUnlocked = false;

                // Add to Undo list
                _navigationUndo.Push(SelectedNode);

                DoNavigate(node);
                if (!_showAllContentInTree)
                    RunWithContentSelectionHistorySuppressed(() => _view.SelectFirstItem());
            }
        }

        /// <summary>
        /// Navigates directory up.
        /// </summary>
        public void NavigateUp()
        {
            ContentFolderTreeNode target = _root;
            ContentFolderTreeNode current = SelectedNode;

            if (current?.Folder.ParentFolder != null)
            {
                target = current.Folder.ParentFolder.Node;
            }

            Navigate(target);
        }

        /// <summary>
        /// Clears the navigation history.
        /// </summary>
        public void NavigationClearHistory()
        {
            _navigationUndo.Clear();
            _navigationRedo.Clear();
            Editor.NavigationHistory.RemoveActions(x =>
                x is ContentFolderNavigationAction folderAction && folderAction.Owner == this ||
                x is ContentSelectionNavigationAction selectionAction && selectionAction.Owner == this ||
                x is ContentOpenNavigationAction openAction && openAction.Owner == this);
            UpdateUI();
        }

        private void DoNavigateFromHistory(ContentFolderTreeNode node)
        {
            if (node == null)
                return;

            var wasSuppressed = _suppressContentSelectionNavigation;
            _suppressContentSelectionNavigation = true;
            try
            {
                _navigationUndo.Clear();
                _navigationRedo.Clear();
                _navigationUnlocked = false;
                DoNavigate(node);
                if (!_showAllContentInTree)
                    _view.SelectFirstItem();
            }
            finally
            {
                _lastContentSelectionPaths = GetContentSelectionPaths();
                _suppressContentSelectionNavigation = wasSuppressed;
            }
        }

        private void DoNavigate(ContentFolderTreeNode node)
        {
            // Select node
            if (!_showAllContentInTree)
                RefreshView(node);
            _tree.Select(node);
            node.ExpandAllParents();

            // Set valid sizes for stacks
            //RedoList.SetSize(32);
            //UndoList.SetSize(32);

            // Update search
            if (!_showAllContentInTree)
                UpdateItemsSearch();

            // Unlock navigation
            _navigationUnlocked = true;

            UpdateUI();

            // Clear auto-select cache for new/imported files
            _newFilesCache?.Clear();
            _newFilesCacheSize = 0;
        }

        private void RecordContentSelectionNavigation()
        {
            var source = _lastContentSelectionPaths ?? Array.Empty<string>();
            var target = GetContentSelectionPaths();
            _lastContentSelectionPaths = target;
            if (IsContentSelectionHistorySuppressed || AreSameContentSelection(source, target))
                return;

            var currentSceneSelection = Editor.SceneEditing.Selection.ToArray();
            var sourceSceneSelection = currentSceneSelection;
            var targetSceneSelection = currentSceneSelection;
            if (target.Length != 0)
                targetSceneSelection = Array.Empty<SceneGraphNode>();
            if (source.Length != 0 && target.Length == 0 && currentSceneSelection.Length != 0)
                sourceSceneSelection = Array.Empty<SceneGraphNode>();

            var previousSelectionAction = Editor.Undo.UndoOperationsStack.PeekHistory() as ContentSelectionUndoAction;
            if (previousSelectionAction == null || !previousSelectionAction.IsSameTransition(this, source, target, sourceSceneSelection, targetSceneSelection))
                Editor.Undo.AddAction(new ContentSelectionUndoAction(this, source, target, sourceSceneSelection, targetSceneSelection));
            Editor.NavigationHistory.AddAction(new ContentSelectionNavigationAction(this, source, target));
        }

        private void DoSelectContentItemsFromHistory(string[] paths, bool focusWindow = true)
        {
            var wasSuppressed = _suppressContentSelectionNavigation;
            _suppressContentSelectionNavigation = true;
            try
            {
                if (focusWindow)
                    FocusOrShow();
                if (paths == null || paths.Length == 0)
                {
                    ClearSelection(false);
                    _lastContentSelectionPaths = Array.Empty<string>();
                    return;
                }

                var items = new List<ContentItem>(paths.Length);
                ContentFolder folder = null;
                for (int i = 0; i < paths.Length; i++)
                {
                    var item = Editor.ContentDatabase.Find(paths[i]);
                    if (item == null)
                        continue;
                    items.Add(item);
                    folder ??= item as ContentFolder ?? item.ParentFolder;
                }
                if (items.Count == 0)
                    return;

                if (_showAllContentInTree)
                {
                    if (focusWindow)
                    {
                        for (int i = 0; i < items.Count; i++)
                            Select(items[i], true, i != 0);
                    }
                    else
                    {
                        for (int i = 0; i < items.Count; i++)
                            SelectInTreeWithoutFocus(items[i], i != 0);
                    }
                }
                else
                {
                    if (folder?.Node != null && SelectedNode != folder.Node)
                    {
                        _navigationUnlocked = false;
                        DoNavigate(folder.Node);
                    }
                    _view.Select(items);
                    if (focusWindow)
                    {
                        _contentViewPanel.ScrollViewTo(items[0], true);
                        _view.Focus();
                    }
                }
            }
            finally
            {
                _lastContentSelectionPaths = GetContentSelectionPaths();
                _suppressContentSelectionNavigation = wasSuppressed;
            }
        }

        internal string[] GetSelectionPathsForSceneUndo()
        {
            return GetContentSelectionPaths();
        }

        internal void RestoreSelectionFromSceneUndo(string[] paths)
        {
            DoSelectContentItemsFromHistory(paths, paths != null && paths.Length != 0);
        }

        private void RestoreContentSelectionFromUndo(string[] paths, SceneGraphNodeReference[] sceneSelection)
        {
            var hasContentSelection = paths != null && paths.Length != 0;
            var hasSceneSelection = sceneSelection != null && sceneSelection.Length != 0;
            DoSelectContentItemsFromHistory(paths, hasContentSelection || !hasSceneSelection);
            RestoreSceneSelectionFromContentUndo(sceneSelection);
        }

        private void RestoreSceneSelectionFromContentUndo(SceneGraphNodeReference[] sceneSelection)
        {
            if (sceneSelection == null || sceneSelection.Length == 0)
            {
                Editor.SceneEditing.Deselect(false);
                return;
            }

            var nodes = new List<SceneGraphNode>(SceneGraphNodeReference.Resolve(sceneSelection));
            if (nodes.Count != 0)
                Editor.SceneEditing.Select(nodes, false, false);
            else
                Editor.SceneEditing.Deselect(false);
        }

        private void NotifyTreeContentSelectionChanged()
        {
            if (_showAllContentInTree)
            {
                RecordContentSelectionNavigation();
                if (!IsContentSelectionHistorySuppressed)
                    ClearSceneSelection();
            }
            SelectionChanged?.Invoke();
        }

        private bool IsContentSelectionHistorySuppressed => _suppressContentSelectionNavigation || _isClearingSelection || (Editor.Undo != null && Editor.Undo.IsPerformingUndoRedo);

        private void RunWithContentSelectionHistorySuppressed(Action action)
        {
            var wasSuppressed = _suppressContentSelectionNavigation;
            _suppressContentSelectionNavigation = true;
            try
            {
                action();
            }
            finally
            {
                _lastContentSelectionPaths = GetContentSelectionPaths();
                _suppressContentSelectionNavigation = wasSuppressed;
            }
        }

        private void SelectInTreeWithoutFocus(ContentItem item, bool additive)
        {
            var parent = item.ParentFolder;
            if (parent == null || !parent.Visible)
                return;

            var targetNode = item is ContentFolder folder ? folder.Node : parent.Node;
            if (targetNode == null)
                return;

            targetNode.ExpandAllParents();
            if (item is ContentFolder)
            {
                _tree.Select(targetNode, additive);
                return;
            }

            var itemNode = FindTreeItemNode(targetNode, item);
            TreeNode nodeToSelect = itemNode != null ? itemNode : targetNode;
            _tree.Select(nodeToSelect, additive);
        }

        private void RecordContentOpenNavigation(ContentItem item)
        {
            if (_suppressContentOpenNavigation || item == null || item.IsFolder)
                return;

            var targetPath = item.Path;
            if (_lastContentOpenPath != null && _lastContentOpenPath.Equals(targetPath, StringComparison.OrdinalIgnoreCase))
                return;

            _lastContentOpenPath = targetPath;
            Editor.NavigationHistory.AddAction(new ContentOpenNavigationAction(this, _lastContentSelectionPaths, targetPath));
        }

        private void DoOpenContentItemFromHistory(string path)
        {
            if (string.IsNullOrEmpty(path))
                return;

            var item = Editor.ContentDatabase.Find(path);
            if (item == null)
                return;

            _suppressContentOpenNavigation = true;
            try
            {
                Open(item);
            }
            finally
            {
                _lastContentOpenPath = path;
                _suppressContentOpenNavigation = false;
            }
        }

        private string[] GetContentSelectionPaths()
        {
            var selection = Selection;
            if (selection.Count == 0)
                return Array.Empty<string>();

            var result = new List<string>(selection.Count);
            for (int i = 0; i < selection.Count; i++)
            {
                if (selection[i] != null)
                    result.Add(selection[i].Path);
            }
            return result.ToArray();
        }

        private static bool AreSameContentSelection(string[] a, string[] b)
        {
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                if (!a[i].Equals(b[i], StringComparison.OrdinalIgnoreCase))
                    return false;
            }
            return true;
        }

        private void UpdateNavigationBar()
        {
            if (_navigationBar == null)
                return;

            bool wasLayoutLocked = _navigationBar.IsLayoutLocked;
            _navigationBar.IsLayoutLocked = true;

            // Remove previous buttons
            _navigationBar.DisposeChildren();

            // Spawn buttons
            var nodes = NavUpdateCache;
            nodes.Clear();
            ContentFolderTreeNode node = SelectedNode;
            while (node != null)
            {
                nodes.Add(node);
                node = node.ParentNode;
            }
            float margin = 1;
            float x = NavigationBar.DefaultButtonsMargin;
            float h = _toolStrip.ItemsHeight - 2 * margin;
            for (int i = nodes.Count - 1; i >= 0; i--)
            {
                var button = new ContentNavigationButton(nodes[i], x, margin, h);
                button.PerformLayout();
                x += button.Width + NavigationBar.DefaultButtonsMargin;
                _navigationBar.AddChild(button);
                if (i > 0)
                {
                    var separator = new ContentNavigationSeparator(button, x, margin, h);
                    separator.PerformLayout();
                    x += separator.Width + NavigationBar.DefaultButtonsMargin;
                    _navigationBar.AddChild(separator);
                }
            }
            nodes.Clear();

            // Update
            _navigationBar.IsLayoutLocked = wasLayoutLocked;
            _navigationBar.PerformLayout();
            UpdateNavigationBarBounds();
        }

        /// <summary>
        /// Gets the selected tree node.
        /// </summary>
        public ContentFolderTreeNode SelectedNode
        {
            get
            {
                var selected = GetActiveTreeSelection(_tree.Selection);
                if (selected is ContentItemTreeNode itemNode)
                    return itemNode.Item?.ParentFolder?.Node;
                return selected as ContentFolderTreeNode;
            }
        }

        /// <summary>
        /// Gets the current view folder.
        /// </summary>
        public ContentFolder CurrentViewFolder => SelectedNode?.Folder;

        private TreeNode GetActiveTreeSelection(List<TreeNode> selection)
        {
            if (selection == null || selection.Count == 0)
                return null;
            return _showAllContentInTree && selection.Count > 1
                ? selection[^1]
                : selection[0];
        }

        private ContentFolderTreeNode GetFirstVisibleRootFolder()
        {
            if (_root == null)
                return null;
            for (int i = 0; i < _root.ChildrenCount; i++)
            {
                if (_root.GetChild(i) is ContentFolderTreeNode node && node.Visible && node.IsSelectable)
                    return node;
            }
            return null;
        }

        /// <summary>
        /// Shows the first visible root folder.
        /// </summary>
        public void ShowRoot()
        {
            var node = GetFirstVisibleRootFolder();
            if (node != null)
                _tree.Select(node);
            else
                _tree.Deselect();
        }

        private void SaveLastViewedFolder(ContentFolderTreeNode node)
        {
            Editor.ProjectCache.SetCustomData(ProjectDataLastViewedFolder, node?.Path ?? string.Empty);
        }
    }
}
