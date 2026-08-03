// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEditor.History
{
    /// <summary>
    /// Global editor navigation history used for user context changes such as selection and opened locations.
    /// </summary>
    public sealed class NavigationHistory : IDisposable
    {
        /// <summary>
        /// The default navigation history actions limit.
        /// </summary>
        public const int DefaultHistoryActionsLimit = 200;

        private readonly HistoryStack _historyStack;

        /// <summary>
        /// Occurs when navigation history action gets added.
        /// </summary>
        public event Action<INavigationHistoryAction> ActionDone;

        /// <summary>
        /// Occurs when navigation history travels back.
        /// </summary>
        public event Action<INavigationHistoryAction> BackDone;

        /// <summary>
        /// Occurs when navigation history travels forward.
        /// </summary>
        public event Action<INavigationHistoryAction> ForwardDone;

        /// <summary>
        /// Gets or sets a value indicating whether recording navigation history is enabled.
        /// </summary>
        public bool Enabled { get; set; } = true;

        /// <summary>
        /// Gets a value indicating whether navigation can travel back.
        /// </summary>
        public bool CanGoBack => _historyStack.HistoryCount > 0;

        /// <summary>
        /// Gets a value indicating whether navigation can travel forward.
        /// </summary>
        public bool CanGoForward => _historyStack.ReverseCount > 0;

        /// <summary>
        /// Gets or sets the navigation history actions limit.
        /// </summary>
        public int Capacity
        {
            get => _historyStack.HistoryActionsLimit;
            set => _historyStack.HistoryActionsLimit = value;
        }

        /// <summary>
        /// Gets a snapshot of back navigation actions, ordered from the next back action to the oldest back action.
        /// </summary>
        /// <returns>The back navigation actions.</returns>
        public INavigationHistoryAction[] GetBackActions()
        {
            var actions = _historyStack.GetHistoryActions();
            var result = new INavigationHistoryAction[actions.Length];
            for (int i = 0; i < actions.Length; i++)
                result[i] = (INavigationHistoryAction)actions[i];
            return result;
        }

        /// <summary>
        /// Gets a snapshot of forward navigation actions, ordered from the next forward action to the oldest forward action.
        /// </summary>
        /// <returns>The forward navigation actions.</returns>
        public INavigationHistoryAction[] GetForwardActions()
        {
            var actions = _historyStack.GetReverseActions();
            var result = new INavigationHistoryAction[actions.Length];
            for (int i = 0; i < actions.Length; i++)
                result[i] = (INavigationHistoryAction)actions[i];
            return result;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="NavigationHistory"/> class.
        /// </summary>
        /// <param name="historyActionsLimit">The history actions limit.</param>
        public NavigationHistory(int historyActionsLimit = DefaultHistoryActionsLimit)
        {
            _historyStack = new HistoryStack(historyActionsLimit);
        }

        /// <summary>
        /// Adds new navigation history action.
        /// </summary>
        /// <param name="action">The action.</param>
        public void AddAction(INavigationHistoryAction action)
        {
            if (action == null)
                throw new ArgumentNullException(nameof(action));

            if (!Enabled)
            {
                action.Dispose();
                return;
            }

            var previous = _historyStack.PeekHistory() as INavigationHistoryAction;
            if (previous != null && IsSameDestination(previous, action))
            {
                action.Dispose();
                return;
            }

            _historyStack.Push(action);
            ActionDone?.Invoke(action);
        }

        /// <summary>
        /// Navigates back to the previous editor context.
        /// </summary>
        public void GoBack()
        {
            var action = (INavigationHistoryAction)_historyStack.PopHistory();
            if (action == null)
                return;

            action.NavigateBack();
            BackDone?.Invoke(action);
        }

        /// <summary>
        /// Navigates forward to the next editor context.
        /// </summary>
        public void GoForward()
        {
            var action = (INavigationHistoryAction)_historyStack.PopReverse();
            if (action == null)
                return;

            action.NavigateForward();
            ForwardDone?.Invoke(action);
        }

        /// <summary>
        /// Clears the history.
        /// </summary>
        public void Clear()
        {
            _historyStack.Clear();
        }

        /// <summary>
        /// Removes all matching navigation history actions.
        /// </summary>
        /// <param name="match">The match predicate.</param>
        public void RemoveActions(Predicate<INavigationHistoryAction> match)
        {
            if (match == null)
                throw new ArgumentNullException(nameof(match));

            _historyStack.RemoveAll(x => x is INavigationHistoryAction action && match(action));
        }

        /// <inheritdoc />
        public void Dispose()
        {
            Clear();
            ActionDone = null;
            BackDone = null;
            ForwardDone = null;
        }

        private static bool IsSameDestination(INavigationHistoryAction previous, INavigationHistoryAction next)
        {
            if (next is INavigationHistoryDestination nextDestination && nextDestination.IsSameDestination(previous))
                return true;
            if (previous is INavigationHistoryDestination previousDestination && previousDestination.IsSameDestination(next))
                return true;
            return false;
        }
    }
}
