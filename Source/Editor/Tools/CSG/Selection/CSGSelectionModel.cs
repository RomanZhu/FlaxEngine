// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.SceneGraph;

namespace FlaxEditor.Tools.CSG.Selection
{
    /// <summary>
    /// Stores the CSG authoring selection independently from the ordinary Object-mode selection.
    /// </summary>
    public sealed class CSGSelectionModel
    {
        private readonly List<SceneGraphNode> _objectSelection = new List<SceneGraphNode>(16);
        private readonly List<SceneGraphNode> _csgSelection = new List<SceneGraphNode>(16);

        /// <summary>
        /// Gets the current remembered CSG selection.
        /// </summary>
        public IReadOnlyList<SceneGraphNode> CSGSelection => _csgSelection;

        /// <summary>
        /// Gets whether a node is a supported CSG selection target.
        /// </summary>
        public static bool IsCSGNode(SceneGraphNode node)
        {
            return node != null && node.CSGViewportSelection != CSGViewportSelectionKind.None;
        }

        /// <summary>
        /// Captures Object-mode selection and returns the still-valid remembered CSG selection.
        /// </summary>
        public void Enter(IReadOnlyList<SceneGraphNode> current, RootNode root, List<SceneGraphNode> result)
        {
            if (result == null)
                throw new ArgumentNullException(nameof(result));
            if (ContainsKind(current, false))
                Capture(current, _objectSelection, false);
            else if (ContainsKind(current, true))
                Capture(current, _csgSelection, true);
            CopyValid(_csgSelection, root, result);
            Replace(_csgSelection, result);
        }

        /// <summary>
        /// Captures CSG selection and returns the still-valid remembered Object-mode selection.
        /// </summary>
        public void Leave(IReadOnlyList<SceneGraphNode> current, RootNode root, List<SceneGraphNode> result)
        {
            if (result == null)
                throw new ArgumentNullException(nameof(result));
            if (ContainsKind(current, true))
                Capture(current, _csgSelection, true);
            else if (ContainsKind(current, false))
                Capture(current, _objectSelection, false);
            else
                _csgSelection.Clear();
            CopyValid(_objectSelection, root, result);
            Replace(_objectSelection, result);
        }

        /// <summary>
        /// Observes a selection change caused by the viewport or Scene Tree without losing the other mode's memory.
        /// </summary>
        public void Observe(IReadOnlyList<SceneGraphNode> selection)
        {
            bool hasCSG = false;
            bool hasObject = false;
            for (int i = 0; selection != null && i < selection.Count; i++)
            {
                if (IsCSGNode(selection[i]))
                    hasCSG = true;
                else if (selection[i] != null)
                    hasObject = true;
            }

            if (hasObject)
                Capture(selection, _objectSelection, false);
            else if (hasCSG || selection?.Count == 0)
                Capture(selection, _csgSelection, true);
        }

        /// <summary>
        /// Applies a click candidate using replace, additive, or toggle behavior.
        /// </summary>
        public void ApplyClick(SceneGraphNode candidate, bool additive, bool toggle, List<SceneGraphNode> result)
        {
            if (result == null)
                throw new ArgumentNullException(nameof(result));

            result.Clear();
            result.AddRange(_csgSelection);
            if (candidate == null)
            {
                if (!additive && !toggle)
                    result.Clear();
            }
            else if (toggle)
            {
                if (!result.Remove(candidate))
                    result.Add(candidate);
            }
            else if (additive)
            {
                if (!result.Contains(candidate))
                    result.Add(candidate);
            }
            else
            {
                result.Clear();
                result.Add(candidate);
            }
            Replace(_csgSelection, result);
        }

        /// <summary>
        /// Clears the remembered CSG selection.
        /// </summary>
        public void Clear()
        {
            _csgSelection.Clear();
        }

        private static void Capture(IReadOnlyList<SceneGraphNode> source, List<SceneGraphNode> target, bool csg)
        {
            target.Clear();
            for (int i = 0; source != null && i < source.Count; i++)
            {
                var node = source[i];
                if (node != null && IsCSGNode(node) == csg && !target.Contains(node))
                    target.Add(node);
            }
        }

        private static bool ContainsKind(IReadOnlyList<SceneGraphNode> source, bool csg)
        {
            for (int i = 0; source != null && i < source.Count; i++)
            {
                var node = source[i];
                if (node != null && IsCSGNode(node) == csg)
                    return true;
            }
            return false;
        }

        private static void CopyValid(List<SceneGraphNode> source, RootNode root, List<SceneGraphNode> target)
        {
            target.Clear();
            for (int i = 0; i < source.Count; i++)
            {
                var node = source[i];
                if (node != null && node.Root == root)
                    target.Add(node);
            }
        }

        private static void Replace(List<SceneGraphNode> target, List<SceneGraphNode> source)
        {
            if (ReferenceEquals(target, source))
                return;
            target.Clear();
            target.AddRange(source);
        }
    }
}
