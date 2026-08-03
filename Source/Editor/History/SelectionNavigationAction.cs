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
        private Action<SceneGraphNode[]> _callback;

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
        public SelectionNavigationAction(object owner, SceneGraphNode[] before, SceneGraphNode[] after, Action<SceneGraphNode[]> callback)
        {
            Owner = owner ?? throw new ArgumentNullException(nameof(owner));
            _before = before ?? Array.Empty<SceneGraphNode>();
            _after = after ?? Array.Empty<SceneGraphNode>();
            _callback = callback ?? throw new ArgumentNullException(nameof(callback));
        }

        /// <inheritdoc />
        public string ActionString => "Selection change";

        /// <inheritdoc />
        public bool IsSameDestination(INavigationHistoryAction other)
        {
            return other is SelectionNavigationAction action &&
                   Equals(Owner, action.Owner) &&
                   AreSameSelection(_after, action._after);
        }

        /// <inheritdoc />
        public void NavigateBack()
        {
            _callback?.Invoke(_before);
        }

        /// <inheritdoc />
        public void NavigateForward()
        {
            _callback?.Invoke(_after);
        }

        /// <inheritdoc />
        public void Dispose()
        {
            _callback = null;
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
    }
}
