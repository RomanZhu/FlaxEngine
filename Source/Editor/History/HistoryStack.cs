// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Linq;
using FlaxEngine.Collections;

namespace FlaxEditor.History
{
    /// <summary>
    /// Reason why a history action was discarded from a history stack.
    /// </summary>
    public enum HistoryStackDiscardReason
    {
        /// <summary>
        /// Unknown or unspecified reason.
        /// </summary>
        Unknown,

        /// <summary>
        /// The history stack was cleared.
        /// </summary>
        Cleared,

        /// <summary>
        /// The action was discarded because the count limit was exceeded.
        /// </summary>
        CountLimit,

        /// <summary>
        /// The action was discarded because the size limit was exceeded.
        /// </summary>
        SizeLimit,

        /// <summary>
        /// The action was explicitly removed by a caller.
        /// </summary>
        Removed,

        /// <summary>
        /// The redo/forward side was invalidated by a new history action.
        /// </summary>
        ReverseInvalidated,
    }

    /// <summary>
    /// Controller for handling stack manipulations in history and reverse buffers.
    /// </summary>
    public sealed class HistoryStack
    {
        private int _historyActionsLimit;
        private long _historySizeLimitInBytes = -1;
        private long _historyActionsSizeInBytes;
        private long _reverseActionsSizeInBytes;
        private readonly Func<IHistoryAction, long> _actionSizeGetter;

        private readonly CircularBuffer<IHistoryAction> _historyActions;
        private readonly CircularBuffer<IHistoryAction> _reverseActions;

        /// <summary>
        /// Occurs when history stack disposes an action because it was removed from history.
        /// </summary>
        public event Action<IHistoryAction> ActionDisposed;

        /// <summary>
        /// Occurs when history stack discards an action and reports why it was removed.
        /// </summary>
        public event Action<IHistoryAction, HistoryStackDiscardReason> ActionDiscarded;

        /// <summary>
        /// Initializes a new instance of the <see cref="HistoryStack"/> class.
        /// </summary>
        /// <param name="historyActionsLimit">The history actions limit.</param>
        /// <param name="actionSizeGetter">Optional function that returns an approximate action size in bytes. Negative means unknown and is not counted.</param>
        public HistoryStack(int historyActionsLimit = 1000, Func<IHistoryAction, long> actionSizeGetter = null)
        {
            if (historyActionsLimit < 1)
                throw new ArgumentOutOfRangeException();

            _historyActionsLimit = historyActionsLimit;
            _actionSizeGetter = actionSizeGetter;
            _historyActions = new CircularBuffer<IHistoryAction>(_historyActionsLimit);
            _reverseActions = new CircularBuffer<IHistoryAction>(_historyActionsLimit);

            _historyActions.OnItemOverflown += OnItemOverflown;
            _reverseActions.OnItemOverflown += OnItemOverflown;
        }

        private void OnItemOverflown(object sender, CircularBuffer<IHistoryAction>.ItemOverflownEventArgs e)
        {
            if (ReferenceEquals(sender, _historyActions))
                RemoveActionSize(ref _historyActionsSizeInBytes, e.Item);
            else if (ReferenceEquals(sender, _reverseActions))
                RemoveActionSize(ref _reverseActionsSizeInBytes, e.Item);

            // Dispose item to prevent leaks
            DisposeAction(e.Item, HistoryStackDiscardReason.CountLimit);
        }

        /// <summary>
        /// Gets the history actions limit.
        /// </summary>
        /// <value>
        /// The history actions limit.
        /// </value>
        public int HistoryActionsLimit
        {
            get => _historyActionsLimit;
            set
            {
                if (value < 1)
                    throw new ArgumentOutOfRangeException();
                if (_historyActionsLimit == value)
                    return;

                // Cache actions
                var history = _historyActions.ToArray();
                var reverse = _reverseActions.ToArray();

                // Resize buffers
                _historyActionsLimit = value;
                _historyActions.Clear(_historyActionsLimit);
                _reverseActions.Clear(_historyActionsLimit);
                _historyActionsSizeInBytes = 0;
                _reverseActionsSizeInBytes = 0;

                // Add actions back
                for (int i = 0; i < _historyActionsLimit && i < history.Length; i++)
                {
                    _historyActions.PushBack(history[i]);
                    AddActionSize(ref _historyActionsSizeInBytes, history[i]);
                }
                for (int i = 0; i < _historyActionsLimit && i < reverse.Length; i++)
                {
                    _reverseActions.PushBack(reverse[i]);
                    AddActionSize(ref _reverseActionsSizeInBytes, reverse[i]);
                }

                // Cleanup remaining actions
                for (int i = _historyActionsLimit; i < history.Length; i++)
                {
                    DisposeAction(history[i], HistoryStackDiscardReason.CountLimit);
                }
                for (int i = _historyActionsLimit; i < reverse.Length; i++)
                {
                    DisposeAction(reverse[i], HistoryStackDiscardReason.CountLimit);
                }
                EnforceHistorySizeLimit();
            }
        }

        /// <summary>
        /// Gets or sets the approximate history size limit in bytes. Negative value disables size-based pruning.
        /// </summary>
        public long HistorySizeLimitInBytes
        {
            get => _historySizeLimitInBytes;
            set
            {
                if (value < -1)
                    throw new ArgumentOutOfRangeException();
                if (_historySizeLimitInBytes == value)
                    return;

                _historySizeLimitInBytes = value;
                EnforceHistorySizeLimit();
            }
        }

        /// <summary>
        /// Gets the approximate size of both history buffers in bytes. Actions with unknown size are not counted.
        /// </summary>
        public long HistorySizeInBytes => _historyActionsSizeInBytes + _reverseActionsSizeInBytes;

        /// <summary>
        /// Gets the history count.
        /// </summary>
        /// <value>
        /// The history count.
        /// </value>
        public int HistoryCount => _historyActions.Count;

        /// <summary>
        /// Gets the reverse count.
        /// </summary>
        /// <value>
        /// The reverse count.
        /// </value>
        public int ReverseCount => _reverseActions.Count;

        /// <summary>
        /// Gets a snapshot of history actions, ordered from the top-most action to the oldest action.
        /// </summary>
        /// <returns>The history actions.</returns>
        public IHistoryAction[] GetHistoryActions()
        {
            var result = _historyActions.ToArray();
            Array.Reverse(result);
            return result;
        }

        /// <summary>
        /// Gets a snapshot of reverse actions, ordered from the top-most action to the oldest action.
        /// </summary>
        /// <returns>The reverse actions.</returns>
        public IHistoryAction[] GetReverseActions()
        {
            var result = _reverseActions.ToArray();
            Array.Reverse(result);
            return result;
        }

        /// <summary>
        /// Adds new history element at top of history stack, and drops reverse stack
        /// </summary>
        /// <param name="item">Item to add</param>
        public void Push(IHistoryAction item)
        {
            _historyActions.PushFront(item);
            AddActionSize(ref _historyActionsSizeInBytes, item);
            ClearReverse(HistoryStackDiscardReason.ReverseInvalidated);
            EnforceHistorySizeLimit();
        }

        /// <summary>
        /// Gets top-most item in history stack
        /// </summary>
        /// <returns>Found element or null</returns>
        public IHistoryAction PeekHistory()
        {
            return _historyActions.Count == 0 ? null : _historyActions[_historyActions.Count - 1];
        }

        /// <summary>
        /// Gets top-most item in reverse stack
        /// </summary>
        /// <returns>Found element or null</returns>
        public IHistoryAction PeekReverse()
        {
            return _reverseActions.Count == 0 ? null : _reverseActions[_reverseActions.Count - 1];
        }

        /// <summary>
        /// Gets top-most item in history stack, and removes it from history stack. Adds forgot element in reverse stack.
        /// </summary>
        /// <returns>Found element or null</returns>
        public IHistoryAction PopHistory()
        {
            var item = PeekHistory();
            if (item == null)
                return null;
            _historyActions.PopFront();
            RemoveActionSize(ref _historyActionsSizeInBytes, item);
            _reverseActions.PushFront(item);
            AddActionSize(ref _reverseActionsSizeInBytes, item);
            return item;
        }

        /// <summary>
        /// Gets top-most item in reverse stack, and removes it from reverse stack. Adds forgot element in history stack.
        /// </summary>
        /// <returns>Found element or null</returns>
        public IHistoryAction PopReverse()
        {
            var item = PeekReverse();
            if (item == null)
                return null;
            _reverseActions.PopFront();
            RemoveActionSize(ref _reverseActionsSizeInBytes, item);
            _historyActions.PushFront(item);
            AddActionSize(ref _historyActionsSizeInBytes, item);
            return item;
        }

        /// <summary>
        /// Gets element at given index from top of history stack, and adds all skipped elements to reverse stack
        /// </summary>
        /// <remarks>If skipElements is bigger, then amount of elements in history, returns null, clears history and pushes all to reverse stack</remarks>
        /// <param name="skipElements">Amount of elements to skip from history stack</param>
        /// <returns>>Found element or null</returns>
        public IHistoryAction TravelBack(int skipElements)
        {
            if (skipElements <= 0)
            {
                throw new ArgumentOutOfRangeException(nameof(skipElements), "argument cannot be smaller or equal to 0");
            }

            if (_historyActions.Count - skipElements <= 0)
            {
                var result = _historyActions.Back();
                var historyActions = _historyActions.ToArray();
                foreach (var historyAction in historyActions)
                {
                    _reverseActions.PushFront(historyAction);
                    AddActionSize(ref _reverseActionsSizeInBytes, historyAction);
                }
                _historyActions.Clear();
                _historyActionsSizeInBytes = 0;
                EnforceHistorySizeLimit();
                return result;
            }

            // Iterate all but one elements to skip. Last element is handled exclusively
            for (int i = 0; i < skipElements - 1; i++)
            {
                PopHistory();
            }

            return PopHistory();
        }

        /// <summary>
        /// Gets element at given index from top of reverse stack, and adds all skipped elements to history stack
        /// </summary>
        /// <remarks>If skipElements is bigger, then amount of elements in reverse, returns null, clears reverse and pushes all to history stack</remarks>
        /// <param name="skipElements">Amount of elements to skip from reverse stack</param>
        /// <returns>>Found element or null</returns>
        public IHistoryAction TravelReverse(int skipElements)
        {
            if (skipElements <= 0)
            {
                throw new ArgumentOutOfRangeException(nameof(skipElements), "skipElement cannot be smaller or equal to 0");
            }

            if (_reverseActions.Count - skipElements <= 0)
            {
                var reverseActions = _reverseActions.Reverse().ToArray();
                foreach (var reverseAction in reverseActions)
                {
                    _historyActions.PushFront(reverseAction);
                    AddActionSize(ref _historyActionsSizeInBytes, reverseAction);
                }
                _reverseActions.Clear();
                _reverseActionsSizeInBytes = 0;
                EnforceHistorySizeLimit();
                return PeekHistory();
            }

            // iterate all but one elements to skip. Last element is handled exclusively
            for (int i = 0; i < skipElements - 1; i++)
            {
                PopReverse();
            }

            return PopReverse();
        }

        /// <summary>
        /// Clears whole history (back and front).
        /// </summary>
        public void Clear()
        {
            ClearHistory(HistoryStackDiscardReason.Cleared);
            ClearReverse(HistoryStackDiscardReason.Cleared);
        }

        /// <summary>
        /// Removes all matching actions from history and reverse buffers.
        /// </summary>
        /// <param name="match">The match predicate.</param>
        public void RemoveAll(Predicate<IHistoryAction> match)
        {
            if (match == null)
                throw new ArgumentNullException(nameof(match));

            RemoveAll(_historyActions, match);
            RemoveAll(_reverseActions, match);
        }

        private void ClearHistory(HistoryStackDiscardReason reason = HistoryStackDiscardReason.Cleared)
        {
            if (_historyActions.Count > 0)
            {
                var actions = _historyActions.ToArray();
                for (int i = 0; i < actions.Length; i++)
                    DisposeAction(actions[i], reason);
                _historyActions.Clear();
                _historyActionsSizeInBytes = 0;
            }
        }

        private void ClearReverse(HistoryStackDiscardReason reason = HistoryStackDiscardReason.Cleared)
        {
            if (_reverseActions.Count > 0)
            {
                var actions = _reverseActions.ToArray();
                for (int i = 0; i < actions.Length; i++)
                    DisposeAction(actions[i], reason);
                _reverseActions.Clear();
                _reverseActionsSizeInBytes = 0;
            }
        }

        private void RemoveAll(CircularBuffer<IHistoryAction> actions, Predicate<IHistoryAction> match)
        {
            if (actions.Count == 0)
                return;

            var cached = actions.ToArray();
            actions.Clear();
            var sizeInBytes = 0L;
            for (int i = 0; i < cached.Length; i++)
            {
                if (match(cached[i]))
                    DisposeAction(cached[i], HistoryStackDiscardReason.Removed);
                else
                {
                    // ToArray returns oldest-to-newest. Rebuild at the front so
                    // filtering does not reverse the replay order.
                    actions.PushFront(cached[i]);
                    AddActionSize(ref sizeInBytes, cached[i]);
                }
            }

            if (ReferenceEquals(actions, _historyActions))
                _historyActionsSizeInBytes = sizeInBytes;
            else if (ReferenceEquals(actions, _reverseActions))
                _reverseActionsSizeInBytes = sizeInBytes;

            EnforceHistorySizeLimit();
        }

        private void DisposeAction(IHistoryAction action, HistoryStackDiscardReason reason = HistoryStackDiscardReason.Unknown)
        {
            if (action == null)
                return;

            ActionDiscarded?.Invoke(action, reason);
            ActionDisposed?.Invoke(action);
            action.Dispose();
        }

        private void EnforceHistorySizeLimit()
        {
            if (_historySizeLimitInBytes < 0)
                return;

            while (HistorySizeInBytes > _historySizeLimitInBytes && _historyActions.Count + _reverseActions.Count > 1)
            {
                if (_historyActions.Count > 1)
                {
                    DisposeOldestAction(_historyActions, ref _historyActionsSizeInBytes);
                }
                else if (_reverseActions.Count > 0)
                {
                    DisposeOldestAction(_reverseActions, ref _reverseActionsSizeInBytes);
                }
                else
                {
                    break;
                }
            }
        }

        private void DisposeOldestAction(CircularBuffer<IHistoryAction> actions, ref long sizeInBytes)
        {
            var action = actions.PopBack();
            RemoveActionSize(ref sizeInBytes, action);
            DisposeAction(action, HistoryStackDiscardReason.SizeLimit);
        }

        private long GetActionSize(IHistoryAction action)
        {
            if (_actionSizeGetter == null || action == null)
                return 0;

            var sizeInBytes = _actionSizeGetter(action);
            return sizeInBytes > 0 ? sizeInBytes : 0;
        }

        private void AddActionSize(ref long sizeInBytes, IHistoryAction action)
        {
            sizeInBytes += GetActionSize(action);
        }

        private void RemoveActionSize(ref long sizeInBytes, IHistoryAction action)
        {
            sizeInBytes -= GetActionSize(action);
            if (sizeInBytes < 0)
                sizeInBytes = 0;
        }
    }
}
