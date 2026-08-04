// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.SceneGraph;

namespace FlaxEditor.History
{
    /// <summary>
    /// Scene graph selection navigation action.
    /// </summary>
    /// <seealso cref="INavigationHistoryAction" />
    public sealed class SelectionNavigationAction : INavigationHistoryAction, INavigationHistoryDestination
    {
        private readonly SceneGraphNode[] _before;
        private readonly SceneGraphNode[] _after;
        private readonly string[] _contentBefore;
        private readonly string[] _contentAfter;
        private Action<SceneGraphNode[]> _callback;
        private Action<string[]> _contentSelectionCallback;

        /// <summary>
        /// Gets the owner object that produced this navigation action.
        /// </summary>
        public object Owner { get; }

        /// <summary>
        /// Initializes a new instance of the <see cref="SelectionNavigationAction"/> class.
        /// </summary>
        /// <param name="owner">The owner object that produced this action.</param>
        /// <param name="before">Previously selected nodes.</param>
        /// <param name="after">Newly selected nodes.</param>
        /// <param name="callback">Selection change callback.</param>
        /// <param name="contentBefore">Previously selected content items.</param>
        /// <param name="contentAfter">Newly selected content items.</param>
        /// <param name="contentSelectionCallback">Content selection change callback.</param>
        public SelectionNavigationAction(object owner, SceneGraphNode[] before, SceneGraphNode[] after, Action<SceneGraphNode[]> callback, string[] contentBefore = null, string[] contentAfter = null, Action<string[]> contentSelectionCallback = null)
        {
            Owner = owner ?? throw new ArgumentNullException(nameof(owner));
            _before = before ?? Array.Empty<SceneGraphNode>();
            _after = after ?? Array.Empty<SceneGraphNode>();
            _contentBefore = contentBefore ?? Array.Empty<string>();
            _contentAfter = contentAfter ?? Array.Empty<string>();
            _callback = callback ?? throw new ArgumentNullException(nameof(callback));
            _contentSelectionCallback = contentSelectionCallback;
        }

        /// <inheritdoc />
        public string ActionString => "Selection change";

        /// <inheritdoc />
        public bool IsSameDestination(INavigationHistoryAction other)
        {
            return other is SelectionNavigationAction action &&
                   Equals(Owner, action.Owner) &&
                   AreSameSelection(_after, action._after) &&
                   AreSameContentSelection(_contentAfter, action._contentAfter);
        }

        /// <inheritdoc />
        public void NavigateBack()
        {
            _callback?.Invoke(_before);
            _contentSelectionCallback?.Invoke(_contentBefore);
        }

        /// <inheritdoc />
        public void NavigateForward()
        {
            _callback?.Invoke(_after);
            _contentSelectionCallback?.Invoke(_contentAfter);
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _callback = null;
            _contentSelectionCallback = null;
        }

        private static bool AreSameSelection(SceneGraphNode[] a, SceneGraphNode[] b)
        {
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                if (a[i]?.ID != b[i]?.ID)
                    return false;
            }
            return true;
        }

        private static bool AreSameContentSelection(string[] a, string[] b)
        {
            a ??= Array.Empty<string>();
            b ??= Array.Empty<string>();
            if (a.Length != b.Length)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                if (!string.Equals(a[i], b[i], StringComparison.OrdinalIgnoreCase))
                    return false;
            }
            return true;
        }
    }
}
