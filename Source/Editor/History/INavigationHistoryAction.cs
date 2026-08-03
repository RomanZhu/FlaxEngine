// Copyright (c) Wojciech Figat. All rights reserved.

namespace FlaxEditor.History
{
    /// <summary>
    /// Interface for editor navigation history actions.
    /// </summary>
    /// <seealso cref="IHistoryAction" />
    public interface INavigationHistoryAction : IHistoryAction
    {
        /// <summary>
        /// Navigates back to the previous editor context.
        /// </summary>
        void NavigateBack();

        /// <summary>
        /// Navigates forward to the next editor context.
        /// </summary>
        void NavigateForward();
    }

    /// <summary>
    /// Optional interface for navigation actions that can compare destination identity.
    /// </summary>
    public interface INavigationHistoryDestination
    {
        /// <summary>
        /// Checks if this action has the same forward destination as another navigation action.
        /// </summary>
        /// <param name="other">The other navigation action.</param>
        /// <returns>True if both actions point at the same destination.</returns>
        bool IsSameDestination(INavigationHistoryAction other);
    }
}
